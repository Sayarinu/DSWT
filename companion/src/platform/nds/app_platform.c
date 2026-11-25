#include "platform/nds/app_platform.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <nds.h>
#include <fat.h>
#include <psa/crypto.h>

#include "app/model.h"
#include "platform/entropy_pool.h"
#include "platform/nds/entropy_nds.h"
#include "platform/nds/net.h"
#include "platform/nds/sd_file.h"
#include "platform/nds/wifi.h"
#include "platform/nds/worker.h"
#include "ui/app.h"

/* melonDS's DS-mode Wi-Fi can take close to a minute to associate. */
#define WIFI_TIMEOUT_MS 90000
/* A network chosen in the Wi-Fi settings: a few seconds to find it, then the
 * DSi driver associates in seconds; a wrong password should not keep the
 * player waiting a minute and a half. */
#define JOIN_SCAN_MS 8000
#define JOIN_TIMEOUT_MS 45000
#define LOG_LIMIT (256 * 1024)

static struct {
    int tried, mounted;
    char root[16];
} sCard;

static CpSdFile sSave;
static int sSaveOpen;
static FILE *sRom[CP_APP_ROM_SLOTS];
static uint32_t sRomSize[CP_APP_ROM_SLOTS];
static CpNetExchange sExchange;
static char sLog[96];
static CpAppPlatform sPlatform;
static CpApp sApp;

int CpNdsCard_Mount(void)
{
    if (!sCard.tried) {
        sCard.tried = 1;
        sCard.mounted = fatInitDefault();
        if (sCard.mounted) snprintf(sCard.root, sizeof(sCard.root), "%s", fatGetDefaultDrive());
    }
    return sCard.mounted;
}

const char *CpNdsCard_Root(void)
{
    return sCard.mounted ? sCard.root : "";
}

int CpNdsApp_DiagnosticRequested(void)
{
    char path[64];
    struct stat info;
    if (!CpNdsCard_Mount()) return 0;
    snprintf(path, sizeof(path), "%sdswt-companion/diag.txt", sCard.root);
    return stat(path, &info) == 0;
}

/* ---- The log ---------------------------------------------------------------- */

static void Log(void *arg, const char *line)
{
    FILE *file;
    struct stat info;
    (void)arg;
    if (!sLog[0]) return;
    /* Keep it small: past the limit, start again with the newest lines. */
    if (stat(sLog, &info) == 0 && info.st_size > LOG_LIMIT) remove(sLog);
    file = fopen(sLog, "a");
    if (!file) return;
    fprintf(file, "%6lu ", (unsigned long)(systemCounterTicksToMsec((u32)systemCounterGetTicks()) / 1000));
    fputs(line, file);
    fclose(file);
}

static void Logf(const char *format, ...)
{
    char line[160];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    Log(NULL, line);
}

/* ---- Files ------------------------------------------------------------------ */

static int List(void *arg, const char *path, CpAppListEntry entry, void *context)
{
    DIR *directory = opendir(path);
    struct dirent *item;
    (void)arg;
    if (!directory) return -1;
    while ((item = readdir(directory))) {
        char full[CP_PATH_MAX + 64];
        struct stat info;
        int isDirectory = item->d_type == DT_DIR;
        uint32_t size = 0;
        if (!isDirectory) {
            snprintf(full, sizeof(full), "%s%s", path, item->d_name);
            if (stat(full, &info) != 0) continue;
            isDirectory = S_ISDIR(info.st_mode);
            size = (uint32_t)info.st_size;
        }
        entry(context, item->d_name, isDirectory, size);
    }
    closedir(directory);
    return 0;
}

static int OpenSave(void *arg, const char *path, CpPlatformFile *out)
{
    int result;
    (void)arg;
    if (sSaveOpen) return -1;
    result = CpSdFile_Open(&sSave, path, 1);
    if (result != 0) { Logf("save: open failed %d\n", result); return -1; }
    sSaveOpen = 1;
    *out = CpSdFile_Platform(&sSave);
    Logf("save: opened, durable reads %s\n", CpSdFile_RawStatus(&sSave) == 0 ? "from sectors" : "by reopening");
    return 0;
}

static void CloseSave(void *arg)
{
    (void)arg;
    if (!sSaveOpen) return;
    CpSdFile_Close(&sSave);
    sSaveOpen = 0;
}

static int RomRead(void *context, uint32_t offset, void *destination, uint32_t size)
{
    FILE *file = context;
    return fseek(file, (long)offset, SEEK_SET) == 0 && fread(destination, 1, size, file) == size ? 0 : -1;
}

static uint32_t RomSize(void *context)
{
    unsigned slot;
    for (slot = 0; slot < CP_APP_ROM_SLOTS; ++slot) if (sRom[slot] == context) return sRomSize[slot];
    return 0;
}

static int OpenRom(void *arg, int slot, const char *path, CpReader *out)
{
    FILE *file;
    long size;
    (void)arg;
    if (slot < 0 || slot >= CP_APP_ROM_SLOTS || sRom[slot]) return -1;
    file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0) { fclose(file); return -1; }
    sRom[slot] = file;
    sRomSize[slot] = (uint32_t)size;
    out->context = file;
    out->readAt = RomRead;
    out->size = RomSize;
    return 0;
}

static void CloseRom(void *arg, int slot)
{
    (void)arg;
    if (slot < 0 || slot >= CP_APP_ROM_SLOTS || !sRom[slot]) return;
    fclose(sRom[slot]);
    sRom[slot] = NULL;
}

static int FreeSpace(void *context, const char *path, uint64_t *available)
{
    struct statvfs info;
    (void)context;
    if (statvfs(path, &info) != 0) return -1;
    *available = (uint64_t)info.f_bavail * info.f_frsize;
    return 0;
}

/* A backup counts only once the card itself holds it: raw sectors, past
 * FatFs and its cache (backup_save.h, readDurable). */
static int BackupDurable(void *context, const char *path, uint32_t offset, void *bytes, size_t size)
{
    int result;
    (void)context;
    result = CpSdFile_ReadDurablePath(path, offset, bytes, size);
    if (result != 0) Logf("backup: durable read failed %d\n", result);
    return result == 0 ? 0 : -1;
}

/* ---- The worker ----------------------------------------------------------- */

static int Start(void *arg, CpAppJob job, void *jobArg, WtSeed *journal)
{
    CpSeed seed = { journal ? CP_SEED_JOURNAL : CP_SEED_VOLATILE, journal };
    (void)arg;
    return CpWorker_Start(job, jobArg, seed);
}

static int Finished(void *arg, int *result)
{
    size_t peak = 0, leaked = 0;
    (void)arg;
    if (!CpWorker_Collect(result, &peak, &leaked)) return 0;
    if (peak) Logf("worker: result %d, tls peak %u, leak %u\n", *result, (unsigned)peak, (unsigned)leaked);
    return 1;
}

static void Cancel(void *arg)
{
    (void)arg;
    CpWorker_Cancel();
}

/* ---- Wi-Fi and the network ------------------------------------------------- */

/* An attempt already under way is joined rather than restarted. */
static int WifiConnect(void *arg)
{
    int status = CpWifi_Status();
    (void)arg;
    if (status == CP_WIFI_CONNECTED || status == CP_WIFI_CONNECTING) return 0;
    CpWifi_SetTimeout(WIFI_TIMEOUT_MS);
    return CpWifi_AutoConnect();
}

static int WifiJoin(void *arg, const char *ssid, const unsigned char *key, size_t keyLength)
{
    (void)arg;
    CpWifi_SetTimeout(JOIN_TIMEOUT_MS);
    return CpWifi_ConnectKey(ssid, key, keyLength, JOIN_SCAN_MS);
}

static int WifiScan(void *arg)
{
    (void)arg;
    return CpWifi_Scan();
}

static unsigned WifiNetworks(void *arg, CpWifiNetwork *out, unsigned max)
{
    (void)arg;
    return CpWifi_Networks(out, max);
}

static int WifiFailure(void *arg)
{
    (void)arg;
    return CpWifi_LastFailure();
}

static int WifiStatus(void *arg)
{
    static int last = -1;
    int status = CpWifi_Status();
    (void)arg;
    if (status != last && (status == CP_WIFI_FAILED || status == CP_WIFI_CONNECTED))
        Logf("wifi: %s (%s)\n", status == CP_WIFI_CONNECTED ? "connected" : "failed", CpWifi_StatusText());
    last = status;
    return status;
}

static int Post(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    int result;
    (void)arg;
    result = CpNet_Post(&sExchange, request, size, CpWorker_Cancelled());
    if (result != 0 || sExchange.exchange.bodySize > WT_WIRE_REPLY_MAX) {
        Logf("net: failed %d at %s (detail %d)\n", result, sExchange.stage ? sExchange.stage : "-", sExchange.detail);
        memset(&sExchange, 0, sizeof(sExchange));
        return -1;
    }
    Logf("net: op %u in %lu+%lu+%lu ms\n", request[5], (unsigned long)sExchange.connectMs,
        (unsigned long)sExchange.handshakeMs, (unsigned long)sExchange.httpMs);
    memcpy(reply, sExchange.exchange.body, sExchange.exchange.bodySize);
    *replySize = sExchange.exchange.bodySize;
    memset(&sExchange, 0, sizeof(sExchange));
    return 0;
}

/* While the server finishes a request: give the CPU to the screens. */
static void Wait(void *arg, unsigned milliseconds)
{
    uint64_t end = systemCounterGetTicks() + systemCounterMsecsToTicks(milliseconds);
    const volatile int *cancel = CpWorker_Cancelled();
    (void)arg;
    while (systemCounterGetTicks() < end && !*cancel) cothread_yield();
}

/* WtTls_Close frees PSA after every exchange. Starting it again here, before
 * the visit reads its journal entry, reads and advances the NV seed through
 * the open engine journal (worker.h CP_SEED_JOURNAL), as the game's RunTrade. */
static int Prepare(void *arg)
{
    (void)arg;
    return psa_crypto_init() == PSA_SUCCESS ? 0 : -1;
}

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    (void)arg;
    return psa_generate_random(bytes, size) == PSA_SUCCESS ? 0 : -1;
}

static int Seed(void *arg, unsigned char out[32])
{
    (void)arg;
    return CpEntropy_Seed(out);
}

static void Frame(void *arg)
{
    CpNdsEntropy_Frame();
    /* Follow the connection every frame: its timeout runs from its start, so
     * an attempt left unpolled would only fail late. */
    (void)WifiStatus(arg);
}

/* ---- The companion ----------------------------------------------------------- */

void CpNdsApp_Run(void)
{
    static char backups[64];
    unsigned bytes = 0;
    int microphone = 0;
    const char *root = CpNdsCard_Root();
    if (root[0]) {
        char folder[64];
        snprintf(folder, sizeof(folder), "%s_nds", root);
        mkdir(folder, 0777);
        snprintf(folder, sizeof(folder), "%s_nds/dswt", root);
        mkdir(folder, 0777);
        snprintf(sLog, sizeof(sLog), "%s_nds/dswt/companion-log.txt", root);
        snprintf(backups, sizeof(backups), "%s_nds/dswt/backups", root);
    }
    Logf("=== DSWT Companion, %s mode, drive %s, server %s\n", isDSiMode() ? "DSi" : "DS", root[0] ? root : "none",
        WONDER_TRADE_AUTHORITY);
    CpNdsEntropy_Jitter();
    if (CpWifi_Init() != 0) Logf("wifi: driver failed to start\n");
    CpWifi_SetTimeout(WIFI_TIMEOUT_MS);
    CpNdsEntropy_Wifi();
    if (CpNdsEntropy_Finish() != 0) Logf("entropy: pool failed to start\n");
    CpNdsEntropy_Report(&bytes, &microphone);
    Logf("entropy: boot %u bytes, mic %s, pool %s\n", bytes, microphone ? "varied" : "flat",
        CpEntropy_Ready() ? "ready" : "FAILED");

    memset(&sPlatform, 0, sizeof(sPlatform));
    sPlatform.root = root;
    sPlatform.list = List;
    sPlatform.openSave = OpenSave; sPlatform.closeSave = CloseSave;
    sPlatform.openRom = OpenRom; sPlatform.closeRom = CloseRom;
    sPlatform.start = Start; sPlatform.finished = Finished; sPlatform.cancel = Cancel;
    sPlatform.wifiConnect = WifiConnect; sPlatform.wifiStatus = WifiStatus;
    sPlatform.transport.post = Post;
    sPlatform.transport.wait = Wait;
    sPlatform.transport.cancel = CpWorker_Cancelled();
    sPlatform.transport.prepare = Prepare;
    sPlatform.random = Random;
    sPlatform.seed = Seed;
    sPlatform.hold = WtGame_HoldAwake;
    sPlatform.backup.root = backups[0] ? backups : NULL;
    sPlatform.backup.freeSpace = FreeSpace;
    sPlatform.backup.readDurable = BackupDurable;
    sPlatform.frame = Frame;
    sPlatform.log = Log;
    sPlatform.wifiScan = WifiScan;
    sPlatform.wifiNetworks = WifiNetworks;
    sPlatform.wifiJoin = WifiJoin;
    sPlatform.wifiFailure = WifiFailure;
    sPlatform.wifiWpa2 = CpWifi_DsiMode();

    CpApp_Init(&sApp, &sPlatform, CpUiApp_SystemLanguage());
    /* Join Wi-Fi while the player chooses a save (DSWiFi can take a while):
     * the network stored by the Wi-Fi settings, then the console's saved
     * connections. Connect then finds it ready or waits for it. */
    CpApp_StartWifi(&sApp);
    CpUiApp_Run(&sApp.model, NULL);
    Logf("exit\n");
    CpWifi_Disconnect();
}
