#include "ui/diagnostic.h"

#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <nds.h>
#include <fat.h>
#include <arpa/inet.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include "platform/entropy_pool.h"
#include "platform/journal_io.h"
#include "platform/nds/entropy_nds.h"
#include "platform/nds/net.h"
#include "platform/nds/sd_file.h"
#include "platform/nds/wifi.h"
#include "platform/nds/worker.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/wire.h"

#define MAX_ATTEMPTS 100

static CompanionScreens *sScreens;
static char sBase[48];
static unsigned sFrame;
static struct {
    char sav[CP_SD_PATH_MAX], ssid[40], passphrase[72];
    unsigned attempts;
    int writeTest;
} sConfig;

typedef struct Attempt {
    CpNetExchange net;
    unsigned char frame[WT_WIRE_REQUEST_MAX];
    size_t frameSize;
    WtWireReply reply;
    int stage, decoded;
} Attempt;
static Attempt sAttempt;
static unsigned char sCached[WT_JOURNAL_RECORD_SIZE], sDurable[WT_JOURNAL_RECORD_SIZE];

static const char *ResultName(unsigned result)
{
    static const char *names[] = { "?", "OK", "PROCESSING", "INVALID_REQUEST", "UNAUTHENTICATED", "CONFLICT",
        "NOT_FOUND", "CAPACITY", "ILLEGAL_POKEMON", "COUNTER_MISMATCH", "CURSOR_AHEAD", "UNAVAILABLE" };
    return result < sizeof(names) / sizeof(*names) ? names[result] : "?";
}

static void Log(const char *format, ...)
{
    char line[160], path[96];
    FILE *file;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    consoleSelect(&sScreens->bottom);
    fputs(line, stdout);
    if (!sBase[0]) return;
    snprintf(path, sizeof(path), "%sdiag-log.txt", sBase);
    file = fopen(path, "a");
    if (file) { fputs(line, file); fclose(file); }
}

/* One line of the top screen, padded to its full width. */
static void Top(int row, const char *format, ...)
{
    char line[40];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    consoleSelect(&sScreens->top);
    printf("\x1b[%d;0H%-32.32s", row, line);
}

/* Liveness that keeps going while the worker computes: the bottom screen's
 * backdrop pulses from the VBlank interrupt. */
static void Pulse(void)
{
    static unsigned tick;
    tick++;
    BG_PALETTE_SUB[0] = RGB15(0, 0, (tick >> 3) & 7);
}

/* One main-loop frame: input, entropy, and the worker's turn. */
static void Frame(void)
{
    static const char spinner[] = "|/-\\";
    scanKeys();
    CpNdsEntropy_Frame();
    consoleSelect(&sScreens->top);
    printf("\x1b[0;31H%c", spinner[(sFrame++ >> 3) & 3]);
    cothread_yield_irq(IRQ_VBLANK);
}

static void Trim(char *text)
{
    size_t length = strlen(text);
    while (length && strchr("\r\n \t", text[length - 1])) text[--length] = 0;
}

static void LoadConfig(void)
{
    char path[96], line[CP_SD_PATH_MAX + 16];
    FILE *file;
    memset(&sConfig, 0, sizeof(sConfig));
    sConfig.attempts = 1;
    snprintf(path, sizeof(path), "%sdiag.txt", sBase);
    file = fopen(path, "r");
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        char *value = strchr(line, '=');
        Trim(line);
        if (line[0] == '#' || !value) continue;
        *value++ = 0;
        if (!strcmp(line, "sav")) snprintf(sConfig.sav, sizeof(sConfig.sav), "%s", value);
        else if (!strcmp(line, "ssid")) snprintf(sConfig.ssid, sizeof(sConfig.ssid), "%s", value);
        else if (!strcmp(line, "passphrase")) snprintf(sConfig.passphrase, sizeof(sConfig.passphrase), "%s", value);
        else if (!strcmp(line, "attempts")) sConfig.attempts = (unsigned)strtoul(value, NULL, 10);
        else if (!strcmp(line, "write-test")) sConfig.writeTest = !strcmp(value, "1");
    }
    fclose(file);
    if (sConfig.attempts < 1 || sConfig.attempts > MAX_ATTEMPTS) sConfig.attempts = 1;
    mbedtls_platform_zeroize(line, sizeof(line));
}

/* The alphabetically first .sav file in the companion folder. */
static int FindSave(char *path, size_t size)
{
    DIR *directory = opendir(sBase);
    struct dirent *entry;
    char best[CP_SD_PATH_MAX] = "";
    if (!directory) return -1;
    while ((entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        if (length > 4 && !strcasecmp(entry->d_name + length - 4, ".sav")
            && (!best[0] || strcmp(entry->d_name, best) < 0))
            snprintf(best, sizeof(best), "%s", entry->d_name);
    }
    closedir(directory);
    if (!best[0]) return -1;
    snprintf(path, size, "%s%s", sBase, best);
    return 0;
}

/* Read-only: open the save in place, show its size, compare one journal bank
 * through FatFs and through raw sectors, and classify the journal. */
static void CheckSave(void)
{
    CpSdFile file;
    CpPlatformFile platform;
    CpJournalStore store;
    uint32_t size = 0;
    int result;
    if (!sConfig.sav[0] && FindSave(sConfig.sav, sizeof(sConfig.sav)) != 0) {
        Log("save: no .sav in %s\n", sBase);
        Top(3, "Save: none found");
        return;
    }
    Log("save: %s\n", sConfig.sav);
    result = CpSdFile_Open(&file, sConfig.sav, 0);
    if (result != 0) {
        Log("save: open failed %d (%s)\n", result, result == CP_SD_ERROR_SIZE ? "not 524288 bytes" : "cannot open");
        Top(3, "Save: open failed %d", result);
        return;
    }
    platform = CpSdFile_Platform(&file);
    platform.size(platform.arg, &size);
    Top(3, "Save: %lu bytes, read-only", (unsigned long)size);
    Log("save: size=%lu raw=%s\n", (unsigned long)size, CpSdFile_RawStatus(&file) == 0 ? "sectors" : "reopen (no FAT parse)");
    result = platform.readAt(platform.arg, CP_JOURNAL_BANK_0, sCached, sizeof(sCached)) == 0
        && platform.readDurable(platform.arg, CP_JOURNAL_BANK_0, sDurable, sizeof(sDurable)) == 0;
    Log("save: bank 0 FatFs vs card: %s\n", !result ? "READ ERROR" : memcmp(sCached, sDurable, sizeof(sCached)) ? "DIFFERENT" : "same");
    memset(&store, 0, sizeof(store));
    store.file = &platform;
    /* Recovery's check reads both banks and never writes. */
    result = WtRecovery_Check(CpJournal_IO(&store, WtFlash_Hash));
    Log("save: journal %s (check %d, io %d)\n", result == 0 ? "blank or empty" : "not usable here", result, store.error);
    platform.size(platform.arg, &size);
    Log("save: size after=%lu\n", (unsigned long)size);
    CpSdFile_Close(&file);
}

typedef struct WriteTest {
    CpJournalStore store;
    WtJournalEntry entry;
    int initialized, result;
} WriteTest;
static WriteTest sWriteTest;

/* Worker job: start the journal on a blank dummy save, or commit its next
 * generation, through the staged writes and raw read-back. */
static int WriteTestJob(void *arg)
{
    WriteTest *test = arg;
    WtJournalIO io = CpJournal_IO(&test->store, WtFlash_Hash);
    WtJournal journal;
    unsigned char seed[32];
    int result;
    memset(seed, 0, sizeof(seed));
    /* A test seed only: never use a seed made this way for a real save. */
    if (CpEntropy_Mix(seed) != 0) return -1;
    result = WtJournal_Initialize(&journal, io, seed);
    test->initialized = result == 0;
    WtJournal_Close(&journal);
    if (result != 0 && result != WT_JOURNAL_ERROR_UNKNOWN) return result;
    result = WtJournal_Open(&journal, io);
    if (result == 0) result = WtJournal_Read(&journal, &test->entry);
    if (result == 0 && !test->initialized) {
        test->entry.generation++;
        seed[0] ^= 1;
        memcpy(test->entry.seed, seed, sizeof(seed));
        result = WtJournal_Commit(&journal, &test->entry);
    }
    WtJournal_Close(&journal);
    mbedtls_platform_zeroize(seed, sizeof(seed));
    return result;
}

/* Opt-in (write-test=1), and only on a dummy named test.sav: write the
 * journal banks in place and read them back from the card. */
static void WriteTestSave(void)
{
    CpSdFile file;
    CpPlatformFile platform;
    CpSeed seed = { CP_SEED_VOLATILE, NULL };
    const char *name = strrchr(sConfig.sav, '/');
    uint32_t size = 0;
    int result = -1;
    if (!name || strcasecmp(name + 1, "test.sav") != 0) { Log("write test: only on a dummy test.sav\n"); return; }
    if (CpSdFile_Open(&file, sConfig.sav, 1) != 0) { Log("write test: open failed\n"); return; }
    platform = CpSdFile_Platform(&file);
    memset(&sWriteTest, 0, sizeof(sWriteTest));
    sWriteTest.store.file = &platform;
    if (CpWorker_Start(WriteTestJob, &sWriteTest, seed) == 0)
        while (!CpWorker_Collect(&result, NULL, NULL)) Frame();
    platform.size(platform.arg, &size);
    Log("write test: %s gen=%lu result=%d io=%d\n", sWriteTest.initialized ? "initialized" : "committed",
        (unsigned long)sWriteTest.entry.generation, result, sWriteTest.store.error);
    Log("write test: size=%lu raw=%s\n", (unsigned long)size, CpSdFile_RawStatus(&file) == 0 ? "sectors" : "reopen");
    Top(7, "Write test: %s", result == 0 ? "verified" : "FAILED");
    mbedtls_platform_zeroize(&sWriteTest, sizeof(sWriteTest));
    CpSdFile_Close(&file);
}

static int RandomNonzero(unsigned char *bytes, size_t size)
{
    size_t i;
    if (psa_generate_random(bytes, size) != PSA_SUCCESS) return -1;
    for (i = 0; i < size; i++) if (bytes[i]) return 0;
    return -1;
}

/* Worker job: STATUS for a fresh random save ID and credential. The server
 * finds no such save and answers UNAUTHENTICATED; nothing is enrolled. */
static int StatusJob(void *arg)
{
    Attempt *attempt = arg;
    WtWireRequest request;
    int result = -1;
    memset(&request, 0, sizeof(request));
    attempt->stage = 0;
    if (psa_crypto_init() != PSA_SUCCESS) goto done;
    request.operation = WT_OP_STATUS;
    attempt->stage = 1;
    if (RandomNonzero(request.requestID, 16) != 0 || RandomNonzero(request.saveID, 16) != 0
        || RandomNonzero(request.credential, 32) != 0
        || WtWire_Encode(&request, WtFlash_Hash, attempt->frame, sizeof(attempt->frame), &attempt->frameSize) != 0) goto done;
    attempt->stage = 2;
    result = CpNet_Post(&attempt->net, attempt->frame, attempt->frameSize, CpWorker_Cancelled());
    if (result != 0) goto done;
    attempt->stage = 3;
    attempt->decoded = WtWire_Decode(attempt->frame, attempt->frameSize, attempt->net.exchange.body,
        attempt->net.exchange.bodySize, WtFlash_Hash, &attempt->reply);
    result = attempt->decoded == 0 ? 0 : -1;
done:
    mbedtls_platform_zeroize(&request, sizeof(request));
    return result;
}

static int RunAttempt(unsigned number)
{
    static const char *stages[] = { "crypto-init", "encode", "network", "decode" };
    CpSeed seed = { CP_SEED_VOLATILE, NULL };
    size_t peak = 0, leaked = 0;
    int before = mallinfo().uordblks, after, result = -1, ok;
    memset(&sAttempt, 0, sizeof(sAttempt));
    sAttempt.decoded = -1;
    Top(6, "STATUS #%u: running", number);
    if (CpWorker_Start(StatusJob, &sAttempt, seed) != 0) { Log("#%02u worker busy\n", number); return 0; }
    while (!CpWorker_Collect(&result, &peak, &leaked)) Frame();
    after = mallinfo().uordblks;
    ok = result == 0 && sAttempt.reply.result == WT_RESULT_UNAUTHENTICATED;
    if (result == 0) {
        Log("#%02u %s %s\n    conn=%lums hs=%lums http=%lums\n", number, ok ? "OK" : "UNEXPECTED",
            ResultName(sAttempt.reply.result), (unsigned long)sAttempt.net.connectMs,
            (unsigned long)sAttempt.net.handshakeMs, (unsigned long)sAttempt.net.httpMs);
    } else {
        Log("#%02u FAIL stage=%s/%s err=%d detail=%d\n", number, stages[sAttempt.stage],
            sAttempt.net.stage ? sAttempt.net.stage : "-", result, sAttempt.net.detail);
    }
    Log("    heap %d->%d linger=%d\n    tls peak=%u leak=%u\n", before, after, sAttempt.net.lingerSet, (unsigned)peak, (unsigned)leaked);
    Top(6, "STATUS #%u: %s", number, ok ? ResultName(sAttempt.reply.result) : "failed");
    return ok;
}

static int ConnectWifi(void)
{
    enum CpWifiStatus status;
    unsigned frames = 0;
    if (sConfig.ssid[0]) {
        Log("wifi: connecting to %s with a passphrase\n", sConfig.ssid);
        if (CpWifi_ConnectPassphrase(sConfig.ssid, sConfig.passphrase, 10000) != 0) return -1;
    } else {
        Log("wifi: saved connections (AutoConnect)\n");
        if (CpWifi_AutoConnect() != 0) return -1;
    }
    mbedtls_platform_zeroize(sConfig.passphrase, sizeof(sConfig.passphrase));
    while ((status = CpWifi_Status()) == CP_WIFI_CONNECTING) {
        if ((frames++ & 15) == 0) Top(5, "Wi-Fi: %s %us", CpWifi_StatusText(), frames / 60);
        Frame();
    }
    if (status != CP_WIFI_CONNECTED) {
        Log("wifi: cannot connect (%s) after %us\n", CpWifi_StatusText(), frames / 60);
        Top(5, "Wi-Fi: cannot connect");
        return -1;
    }
    {
        struct in_addr address = { CpWifi_Address() };
        Log("wifi: connected in %us, ip %s\n", frames / 60, inet_ntoa(address));
        Top(5, "Wi-Fi: %s", inet_ntoa(address));
    }
    return 0;
}

void CpDiagnostic_Run(CompanionScreens *screens)
{
    unsigned bytes = 0, i, ok = 0, number = 0;
    int microphone = 0, fat;
    unsigned char mac[6];
    sScreens = screens;
    irqSet(IRQ_VBLANK, Pulse);
    Top(0, "DSWT Companion diagnostic");
    Top(1, "%s mode", isDSiMode() ? "DSi" : "DS");

    CpNdsEntropy_Start();
    CpNdsEntropy_Jitter();
    fat = fatInitDefault();
    CpNdsEntropy_Jitter();
    if (fat) snprintf(sBase, sizeof(sBase), "%sdswt-companion/", fatGetDefaultDrive());
    Log("\n=== DSWT Companion diagnostic, %s mode, drive %s\n", isDSiMode() ? "DSi" : "DS", fat ? fatGetDefaultDrive() : "none");
    Log("server %s (%s)\n", WONDER_TRADE_AUTHORITY, inet_ntoa((struct in_addr){ htonl((uint32_t)WONDER_TRADE_IPV4) }));
    Top(2, "Drive: %s", fat ? fatGetDefaultDrive() : "none");
    if (fat) { LoadConfig(); CheckSave(); }
    else { sConfig.attempts = 1; Top(3, "Save: no SD card"); }

    Top(5, "Wi-Fi: starting");
    if (CpWifi_Init() != 0) Log("wifi: Wifi_InitDefault failed\n");
    CpNdsEntropy_Wifi();
    if (CpNdsEntropy_Finish() != 0) Log("entropy: pool failed to start\n");
    CpNdsEntropy_Report(&bytes, &microphone);
    CpWifi_Mac(mac);
    Log("entropy: boot %u bytes, mic %s, pool %s\n", bytes, microphone ? "varied" : "flat",
        CpEntropy_Ready() ? "ready" : "FAILED");
    Log("wifi: %s driver, mac %02x:%02x:%02x:%02x:%02x:%02x\n", CpWifi_DsiMode() ? "DSi" : "DS",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Top(4, "Entropy: %u boot bytes", bytes);

    if (sConfig.writeTest && sConfig.sav[0] && CpEntropy_Ready()) WriteTestSave();
    if (CpEntropy_Ready() && ConnectWifi() == 0) {
        for (i = 0; i < sConfig.attempts; i++) {
            ok += RunAttempt(++number);
            for (unsigned wait = 0; wait < 60; wait++) Frame();
        }
        Log("SUMMARY unauthenticated=%u/%u\n    heap=%d\n", ok, sConfig.attempts, mallinfo().uordblks);
    }
    Top(9, "A: one more STATUS");
    Top(10, "START: exit");
    for (;;) {
        Frame();
        if ((keysDown() & KEY_A) && CpWifi_Status() == CP_WIFI_CONNECTED) RunAttempt(++number);
        if (keysDown() & KEY_START) break;
    }
    CpWifi_Disconnect();
}
