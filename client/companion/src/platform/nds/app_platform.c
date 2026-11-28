#include "platform/nds/app_platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <nds.h>
#include <fat.h>
#include <psa/crypto.h>

#include "app/model.h"
#include "game/twilight.h"
#include "game/gts_music.h"
#include "platform/nds/menu_audio.h"
#include "platform/nds/card_rom.h"
#include "platform/nds/card_save.h"
#include "platform/nds/card_store.h"
#include "platform/entropy_pool.h"
#include "platform/nds/entropy_nds.h"
#include "platform/nds/net.h"
#include "platform/nds/sd_file.h"
#include "platform/nds/wifi.h"
#include "platform/nds/worker.h"
#include "ui/app.h"
#include "wonder_trade/flash.h"

/* melonDS's DS-mode Wi-Fi can take close to a minute to associate. */
#define MILLISECONDS_PER_SECOND 1000
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

static CpCardRom sCardRom;
static CpReader sHandoffReader;
#if CP_CARD_SAVE_WRITES
static CpSdFile sCardCopy, sCardIntent;
static CpCardStore *sCardStore;
#endif
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
    fprintf(file, "%6lu ", (unsigned long)(systemCounterTicksToMsec((u32)systemCounterGetTicks()) / MILLISECONDS_PER_SECOND));
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
#if CP_CARD_SAVE_WRITES
    if (sCardStore) {
        CpCardStore_Close(sCardStore);
        sCardStore = NULL;
        CpSdFile_Close(&sCardCopy);
        CpSdFile_Close(&sCardIntent);
        CpCardRom_Close(&sCardRom);
        return;
    }
#endif
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

static int ChipRead(void *arg, uint32_t offset, void *data, size_t size)
{
    return CpCardSave_Read(arg, offset, data, size) == CP_CHIP_OK ? 0 : -1;
}

static int ChipSize(void *arg, uint32_t *size)
{
    *size = ((CpCardSave *)arg)->size;
    return 0;
}

static int IdentifyCard(CpRomInfo *info)
{
    if (CpNdsSlot_Handoff(&sCardRom, &sHandoffReader) == 0) {
        Logf("slot-1: verified %.4s rev %u launcher handoff\n",
            (const char *)sCardRom.header + 12, sCardRom.header[0x1e]);
        return CpRom_FromCode((const char *)sCardRom.header + 12, sCardRom.header[0x1e], info);
    }
    Logf("slot-1: no verified launcher handoff (ROM ID %08lx)\n", (unsigned long)sCardRom.id);
    uint8_t header[CP_CARD_PAGE] __attribute__((aligned(4)));
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        uint32_t chipId = 0;
        if (CpNdsSlot_Header(header, attempt == 2 ? &chipId : NULL) != 0) {
            Logf("slot-1: header transfer failed on attempt %u (SCFG_MC=%04x)\n", attempt, (unsigned)REG_SCFG_MC);
            if (attempt == 1 && CpNdsSlot_Present()) continue;
            return CP_ERR_IO;
        }
        if (attempt == 2) Logf("slot-1: cold reset chip ID %08lx, header code bytes %08lx\n",
            (unsigned long)chipId, (unsigned long)CpLe32(header + 12));
        unsigned expected = CpLe16(header + 0x15e);
        unsigned actual = swiCRC16(0xffff, header, 0x15e);
        if (actual == expected) break;
        Logf("slot-1: header CRC mismatch on attempt %u (read %04x, calculated %04x)\n",
            attempt, expected, actual);
        if (attempt == 2) return CP_ERR_IO;
    }
    int result = CpRom_FromCode((const char *)header + 12, header[0x1e], info);
    if (result != CP_OK) {
        Logf("slot-1: unsupported game code (hex %02x %02x %02x %02x), revision %u\n",
            header[12], header[13], header[14], header[15], header[0x1e]);
        return result;
    }
    return CP_OK;
}

static int PreviewCardRom(CpReader *reader)
{
    if (sCardRom.active && !CpCardRom_Check(&sCardRom)) {
        *reader = sHandoffReader;
        return CP_OK;
    }
#if !CP_CARD_SAVE_WRITES
    /* A cold boot can read retail artwork even with flash writes disabled. */
    if (!CpNdsSlot_Open(&sCardRom, reader)) return CP_OK;
#endif
    return CP_ERR_UNSUPPORTED;
}

static int GameCard(void *arg, CpRomInfo *info, CpPlatformFile *file)
{
    static CpCardSave chip;
    (void)arg;
    if (!CpNdsSlot_Present()) {
        Logf("slot-1: no card visible (SCFG_MC=%04x)\n", (unsigned)REG_SCFG_MC);
        return CP_ERR_NOT_FOUND;
    }
    int result = IdentifyCard(info);
    if (result != CP_OK) return result;
    Logf("slot-1: %.4s revision %u\n", info->gameCode, info->revision);
    if (!file) return CP_OK;
    CpChipResult detected = CpCardSave_Detect(info->gameCode, &chip);
    Logf("slot-1: save detection %d, %lu bytes\n", detected, (unsigned long)chip.size);
    if (detected != CP_CHIP_OK || chip.size != CP_SAVE_SIZE) return CP_ERR_IO;
    *file = (CpPlatformFile){ .arg = &chip, .size = ChipSize, .readAt = ChipRead, .readDurable = ChipRead };
    return CP_OK;
}

#if CP_CARD_SAVE_WRITES
static int OpenCardFile(CpSdFile *file, const char *path)
{
    static const uint8_t zeros[4096] = {0};
    struct stat st;
    if (stat(path, &st) == 0) return CpSdFile_Open(file, path, 1);
    if (errno != ENOENT) return -1;
    char temp[112];
    int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(temp)) return -1;
    /* A cut during allocation leaves only the temporary file. A live
     * transaction file is never truncated, even if its contents are bad. */
    int fd = open(temp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return -1;
    int ok = 1;
    for (unsigned i = 0; ok && i < CP_SAVE_SIZE; i += sizeof(zeros))
        ok = write(fd, zeros, sizeof(zeros)) == (ssize_t)sizeof(zeros);
    if (fsync(fd) != 0) ok = 0;
    if (close(fd) != 0 || !ok || rename(temp, path) != 0) return -1;
    return CpSdFile_Open(file, path, 1);
}

static int CheckCard(void *arg) { return CpCardRom_Check(arg); }

static int OpenCardTrade(void *arg, const CpRomInfo *info, CpReader *reader, CpPlatformFile *file)
{
    char path[96];
    CpCardSave chip;
    (void)arg;
    if (sCardStore || sSaveOpen) return CP_ERR_IO;
    int result;
    if (sCardRom.active && sCardRom.inherited) {
        if (CpCardRom_Check(&sCardRom) != 0) return CP_ERR_IO;
        *reader = sHandoffReader;
        result = 0;
    } else {
        result = CpNdsSlot_Open(&sCardRom, reader);
    }
    if (result != 0) {
        Logf("slot-1: ROM data open failed %d\n", result);
        if (result == CP_CARD_NTR_KEY_MISSING) Logf("slot-1: need full bios7.bin or bios9i.bin in sd:/dswt-companion\n");
        if (result == CP_CARD_TWL_KEY_MISSING) Logf("slot-1: need full bios7i.bin in sd:/dswt-companion\n");
        return CP_ERR_UNSUPPORTED; /* The previously identified card may still be previewed. */
    }
    if (memcmp(sCardRom.header + 12, info->gameCode, 4) || sCardRom.header[0x1e] != info->revision ||
        CpCardSave_Detect(info->gameCode, &chip) != CP_CHIP_OK) goto fail;
    Logf("slot-1: save chip %02x%02x%02x erase %lu\n", chip.jedec[0], chip.jedec[1],
        chip.jedec[2], (unsigned long)chip.eraseSize);
    if (chip.eraseSize != 256 && chip.eraseSize != 4096) {
        sHandoffReader = *reader;
        return CP_ERR_UNSUPPORTED;
    }
    const char *folder = "sd:/_nds/dswt/cards";
    if (mkdir(folder, 0777) != 0 && errno != EEXIST) goto fail;
    snprintf(path, sizeof(path), "%s/%.4s-%08lx.txn", folder, info->gameCode, (unsigned long)sCardRom.id);
    if (OpenCardFile(&sCardIntent, path)) goto fail;
    memcpy(path + strlen(path) - 3, "sav", 3);
    if (!OpenCardFile(&sCardCopy, path)) {
        sCardCopy.bulkVerify = sCardIntent.bulkVerify = 1;
        CpPlatformFile copy = CpSdFile_Platform(&sCardCopy), intent = CpSdFile_Platform(&sCardIntent);
        sCardStore = CpCardStore_Open(&chip, sCardRom.id, info->buildCode, &copy, &intent, CheckCard, &sCardRom, file);
        if (sCardStore) return CP_OK;
        CpSdFile_Close(&sCardCopy);
    }
    CpSdFile_Close(&sCardIntent);
fail:
    CpCardRom_Close(&sCardRom);
    return CP_ERR_IO;
}
#endif

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
    char path[64];
    snprintf(path, sizeof(path), "%s_nds/dswt/seeds.bin", CpNdsCard_Root());
    return CpEntropy_SeedFromFile(path, WtFlash_Hash, out);
}

static void Frame(void *arg)
{
    CpNdsEntropy_Frame();
    /* Follow the connection every frame: its timeout runs from its start, so
     * an attempt left unpolled would only fail late. */
    (void)WifiStatus(arg);
}

/* ---- The companion ----------------------------------------------------------- */

static CpGtsMusic music __attribute__((aligned(32)));

/* ARM7 acknowledges only after it has stopped using the previous buffers. */
static int MusicCommand(const CpGtsMusic *command)
{
    DC_FlushRange(command, sizeof(*command));
    while (!fifoSendAddress(FIFO_USER_03, (void *)command)) cothread_yield_irq(IRQ_VBLANK);
    while (!fifoCheckValue32(FIFO_USER_03)) cothread_yield_irq(IRQ_VBLANK);
    return fifoGetValue32(FIFO_USER_03);
}

static void MenuMusic(void)
{
    static CpGtsMusic menu __attribute__((aligned(32))) = { .menuPcm = CpMenuTheme };
    menu.bytes = CpMenuThemeBytes;
    MusicCommand(&menu);
}

static void LoadMusic(const CpReader *rom, CpFamily family)
{
    int result = CpGtsMusic_Load(rom, family, &music);
    if (result == CP_OK) {
        DC_FlushRange(music.sequence, music.bytes);
        int playing = MusicCommand(&music);
        Logf("GTS music: %lu bytes, playing %d\n", (unsigned long)music.bytes, playing);
    } else Logf("GTS archive unavailable (%d), using menu theme\n", result);
}

static void StopMusic(void)
{
    if (music.sequence) {
        MenuMusic();
        CpGtsMusic_Free(&music);
    }
}

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
    Logf("=== DSWT, %s mode, drive %s, server %s\n", isDSiMode() ? "DSi" : "DS", root[0] ? root : "none",
        WONDER_TRADE_AUTHORITY);
    soundEnable();
    DC_FlushRange(CpMenuTheme, CpMenuThemeBytes);
    MenuMusic();
    Logf("music: built-in menu theme started\n");
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
    /* Resetting Slot-1 while a flashcart supplies the filesystem would
     * disconnect the drive. Probe only when running from the internal SD. */
    if (isDSiMode() && !strcmp(root, "sd:/")) {
        sPlatform.gameCard = GameCard;
        sPlatform.previewCardRom = PreviewCardRom;
    }
#if CP_CARD_SAVE_WRITES
    if (sPlatform.gameCard) sPlatform.openCardTrade = OpenCardTrade;
#endif
    Logf("slot-1: retail header probe v3 %s\n", sPlatform.gameCard ? "enabled" : "disabled");
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
    sPlatform.loadMusic = LoadMusic; sPlatform.stopMusic = StopMusic;
    sPlatform.frame = Frame;
    sPlatform.log = Log;
    sPlatform.wifiScan = WifiScan;
    sPlatform.wifiNetworks = WifiNetworks;
    sPlatform.wifiJoin = WifiJoin;
    sPlatform.wifiFailure = WifiFailure;
    sPlatform.wifiWpa2 = CpWifi_DsiMode();

    CpApp_Init(&sApp, &sPlatform, Companion_Language_Count);
    /* Join Wi-Fi while the player chooses a save (DSWiFi can take a while):
     * the network stored by the Wi-Fi settings, then the console's saved
     * connections. Connect then finds it ready or waits for it. */
    CpApp_StartWifi(&sApp);
    CpUiApp_Run(&sApp.model, NULL);
    static CpGtsMusic silence __attribute__((aligned(32)));
    MusicCommand(&silence);
    Logf("exit\n");
    CpNdsApp_ReturnToMenu();
    CpWifi_Disconnect();
}

/* TWiLight's installed 3DS launcher accepts a CNLT title request across a
 * TWL warmboot. RSET bit 2 skips the last game so the menu stays open. */
void CpNdsApp_ReturnToMenu(void)
{
    if (!isDSiMode() || strcmp(CpNdsCard_Root(), "sd:/")) return;
    int console = CpTwilight_Setting("sd:/" CP_TWL_SETTINGS, "SRLOADER", "CONSOLE_MODEL", -1);
    if (console != 2 && console != 3) return;
    static const uint32_t titleRequest[8] = {
        0x434E4C54, 0xEB871801, 0x53524C41, 0x00030004,
        0x53524C41, 0x00030004, 0x17, 0
    };
    uint32_t *params = (uint32_t *)0x02000000;
    void *request = (void *)0x02000300;
    Logf("exit: returning to TWiLight Menu++ on 3DS\n");
    params[0] = BIT(2);
    params[1] = 0x54455352; /* RSET */
    memcpy(request, titleRequest, sizeof(titleRequest));
    DC_FlushRange(params, 8);
    DC_FlushRange(request, sizeof(titleRequest));
    if (fifoSendValue32(FIFO_USER_02, 1)) {
        uint64_t end = systemCounterGetTicks() + systemCounterMsecsToTicks(2000);
        while (systemCounterGetTicks() < end) swiDelay(1000);
    }
    Logf("exit: warmboot did not complete, returning to loader\n");
}
