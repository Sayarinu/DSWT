/* The session model (app/model.c) end to end on the host: saves and ROMs in a
 * temporary folder laid out like a TWiLight Menu++ SD card, a synchronous
 * worker, the in-memory server (session/fake_server.h) and a fake PSA that,
 * like the DS's, is freed after every exchange and must be started again
 * before the next one. Every save, ROM and Pokémon here is synthetic. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "../posix_reader.h"
#include "../session/fake_server.h"
#include "../synthetic_rom.h"
#include "../adapter/gen5_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/model.h"
#include "platform/wifi_status.h"
#include "ui/ui.h"
#include "wonder_trade/entropy.h"

/* ---- What the core expects from the platform ------------------------------ */

void mbedtls_platform_zeroize(void *buffer, size_t size)
{
    volatile unsigned char *p = buffer;
    while (size--) *p++ = 0;
}

void *WtGame_Alloc(WtGame *game, size_t size) { (void)game; return malloc(size); }
void WtGame_Free(WtGame *game, void *pointer) { (void)game; free(pointer); }

/* ---- The fake console ------------------------------------------------------ */

typedef struct Fake {
    char root[256];
    /* The worker runs a job when the model first asks whether it finished. */
    CpAppJob job;
    void *jobArg;
    int jobResult, pending;
    WtSeed *journal;
    volatile int cancel;
    /* Wi-Fi: polls before it answers, and whether it connects. */
    int wifiFails, wifiPolls, wifiConnected;
    /* PSA: freed by every exchange, as WtTls_Close does on the DS. */
    int cryptoReady, randomFailures;
    unsigned prepares, seedWrites, posts;
    int seedWait, corruptReply;
    uint64_t freeSpace;
    int saveFd;
    /* A write-back cache in front of the card: readAt sees it, readDurable
     * sees only the card. loseRetail drops retail writes on their way to the
     * card (a lying cache); failRetail fails them outright. */
    unsigned char cache[CP_SAVE_SIZE], cached[CP_SAVE_SIZE];
    int loseRetail, failRetail;
    PosixReader roms[CP_APP_ROM_SLOTS];
    int romOpen[CP_APP_ROM_SLOTS];
    unsigned logLines;
    /* Wi-Fi settings (LUNE-875): the networks in range, the one name and key
     * that join, and what each attempt asked for. A join answers after a
     * couple of polls. */
    CpWifiNetwork networks[CP_UI_NETWORKS_MAX + 4];
    unsigned networkCount;
    char goodSsid[CP_WIFI_SSID_MAX + 1];
    unsigned char goodKey[CP_WIFI_KEY_MAX];
    size_t goodKeyLength;
    int scanning, joining, joinPolls, joinWorks, joinFailure, noAddress, slotsActive;
    /* The driver keeps reporting the old association after a scan (as DSWiFi
     * does when Wifi_ScanMode is called without Wifi_DisconnectAP). */
    int staleAfterScan;
    unsigned scansStarted, joinsStarted, slotsStarted;
    char joinedSsid[CP_WIFI_SSID_MAX + 1];
    unsigned char joinedKey[CP_WIFI_KEY_MAX];
    size_t joinedKeyLength;
    char log[32768];
    size_t logUsed;
} Fake;

static Fake fake;
static CpAppPlatform platform;
static CpApp app;

static int List(void *arg, const char *path, CpAppListEntry entry, void *context)
{
    DIR *directory = opendir(path);
    struct dirent *item;
    (void)arg;
    if (!directory) return -1;
    while ((item = readdir(directory))) {
        char full[600];
        struct stat info;
        snprintf(full, sizeof(full), "%s%s", path, item->d_name);
        if (stat(full, &info) != 0) continue;
        entry(context, item->d_name, S_ISDIR(info.st_mode), (uint32_t)info.st_size);
    }
    closedir(directory);
    return 0;
}

static int SaveDurable(void *arg, uint32_t offset, void *bytes, size_t size)
{
    (void)arg;
    return offset + size <= CP_SAVE_SIZE && pread(fake.saveFd, bytes, size, offset) == (ssize_t)size ? 0 : -1;
}

static int SaveRead(void *arg, uint32_t offset, void *bytes, size_t size)
{
    unsigned char *out = bytes;
    size_t i;
    if (SaveDurable(arg, offset, bytes, size) != 0) return -1;
    for (i = 0; i < size; ++i) if (fake.cached[offset + i]) out[i] = fake.cache[offset + i];
    return 0;
}

static int RetailOffset(uint32_t offset) { return offset < 0x30000 || (offset >= 0x40000 && offset < 0x70000); }

static int SaveWrite(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    (void)arg;
    if (offset + size > CP_SAVE_SIZE) return -1;
    if (fake.failRetail && RetailOffset(offset)) return -1;
    memcpy(fake.cache + offset, bytes, size);
    memset(fake.cached + offset, 1, size);
    if (fake.loseRetail && RetailOffset(offset)) return 0;
    return pwrite(fake.saveFd, bytes, size, offset) == (ssize_t)size ? 0 : -1;
}

static int SaveSync(void *arg) { (void)arg; return fsync(fake.saveFd) == 0 ? 0 : -1; }

static int SaveSize(void *arg, uint32_t *size)
{
    struct stat info;
    (void)arg;
    if (fstat(fake.saveFd, &info) != 0) return -1;
    *size = (uint32_t)info.st_size;
    return 0;
}

static int OpenSave(void *arg, const char *path, CpPlatformFile *out)
{
    (void)arg;
    assert(fake.saveFd < 0);
    fake.saveFd = open(path, O_RDWR);
    if (fake.saveFd < 0) return -1;
    out->arg = &fake;
    out->readAt = SaveRead; out->writeAt = SaveWrite; out->sync = SaveSync; out->size = SaveSize;
    out->readDurable = SaveDurable;
    memset(fake.cached, 0, sizeof(fake.cached));
    return 0;
}

static void CloseSave(void *arg)
{
    (void)arg;
    assert(fake.saveFd >= 0);
    close(fake.saveFd);
    fake.saveFd = -1;
    memset(fake.cached, 0, sizeof(fake.cached));
}

static int OpenRom(void *arg, int slot, const char *path, CpReader *out)
{
    (void)arg;
    assert(!fake.romOpen[slot]);
    if (PosixReader_Open(&fake.roms[slot], path) != 0) return -1;
    fake.romOpen[slot] = 1;
    *out = fake.roms[slot].reader;
    return 0;
}

static void CloseRom(void *arg, int slot)
{
    (void)arg;
    assert(fake.romOpen[slot]);
    PosixReader_Close(&fake.roms[slot]);
    fake.romOpen[slot] = 0;
}

static int Start(void *arg, CpAppJob job, void *jobArg, WtSeed *journal)
{
    (void)arg;
    if (fake.pending) return -1;
    fake.job = job; fake.jobArg = jobArg; fake.journal = journal;
    fake.pending = 1; fake.cancel = 0;
    /* A job starts with PSA down, as the worker leaves it. */
    fake.cryptoReady = 0;
    return 0;
}

static int Finished(void *arg, int *result)
{
    (void)arg;
    if (!fake.pending) return 0;
    fake.jobResult = fake.job(fake.jobArg);
    fake.pending = 0; fake.journal = NULL;
    *result = fake.jobResult;
    return 1;
}

static void Cancel(void *arg) { (void)arg; fake.cancel = 1; }

static int WifiConnect(void *arg)
{
    (void)arg;
    fake.slotsStarted++;
    fake.slotsActive = 1;
    fake.scanning = fake.joining = 0;
    fake.wifiPolls = 2;
    return 0;
}

static int WifiStatus(void *arg)
{
    (void)arg;
    if (fake.wifiConnected) return CP_WIFI_CONNECTED;
    if (fake.joining) {
        if (--fake.joinPolls > 0) return CP_WIFI_CONNECTING;
        fake.joining = 0;
        if (!fake.joinWorks) return CP_WIFI_FAILED;
        fake.wifiConnected = 1;
        return CP_WIFI_CONNECTED;
    }
    /* Nothing started: not connected. */
    if (fake.scanning || !fake.slotsActive) return CP_WIFI_IDLE;
    if (fake.wifiPolls && --fake.wifiPolls) return CP_WIFI_CONNECTING;
    if (fake.wifiFails) return CP_WIFI_FAILED;
    fake.wifiConnected = 1;
    return CP_WIFI_CONNECTED;
}

static int WifiScanFake(void *arg)
{
    (void)arg;
    fake.scansStarted++;
    fake.scanning = 1;
    fake.joining = fake.slotsActive = 0;
    if (!fake.staleAfterScan) fake.wifiConnected = 0;
    fake.wifiPolls = 0;
    return 0;
}

static unsigned WifiNetworks(void *arg, CpWifiNetwork *out, unsigned max)
{
    unsigned i;
    (void)arg;
    assert(fake.scanning);
    for (i = 0; i < fake.networkCount && i < max; ++i) out[i] = fake.networks[i];
    return i;
}

static int WifiJoinFake(void *arg, const char *ssid, const unsigned char *key, size_t length)
{
    unsigned i;
    int found = 0;
    (void)arg;
    assert(strlen(ssid) <= CP_WIFI_SSID_MAX && length <= CP_WIFI_KEY_MAX);
    fake.joinsStarted++;
    snprintf(fake.joinedSsid, sizeof(fake.joinedSsid), "%s", ssid);
    memcpy(fake.joinedKey, key, length);
    fake.joinedKeyLength = length;
    fake.scanning = fake.wifiConnected = fake.slotsActive = 0;
    fake.wifiPolls = 0;
    fake.joining = 1;
    fake.joinPolls = 2;
    for (i = 0; i < fake.networkCount; ++i) found |= !strcmp(fake.networks[i].ssid, ssid);
    fake.joinWorks = found && !fake.noAddress && !strcmp(ssid, fake.goodSsid) && length == fake.goodKeyLength
        && !memcmp(key, fake.goodKey, length);
    fake.joinFailure = !found ? CP_WIFI_FAILURE_NOT_FOUND : fake.noAddress ? CP_WIFI_FAILURE_NO_ADDRESS
        : CP_WIFI_FAILURE_REJECTED;
    return 0;
}

static int WifiFailure(void *arg)
{
    (void)arg;
    return fake.joinFailure;
}

/* psa_crypto_init on the DS: reads the NV seed from the open engine journal
 * and writes the next one back through the same handle. */
static int Prepare(void *arg)
{
    WtJournalEntry entry;
    (void)arg;
    ++fake.prepares;
    if (fake.cryptoReady) return 0;
    assert(fake.journal);
    if (WtJournal_Read(fake.journal, &entry) != 0) return -1;
    entry.generation++;
    entry.seed[0] ^= 0x5A; entry.seed[1]++;
    if (WtJournal_Commit(fake.journal, &entry) != 0) return -1;
    ++fake.seedWrites;
    fake.cryptoReady = 1;
    return 0;
}

static int Post(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    int result = FsPost(arg, request, size, reply, replySize);
    if (fake.corruptReply) reply[0] ^= 1;
    ++fake.posts;
    fake.cryptoReady = 0; /* WtTls_Close frees PSA */
    return result;
}

static int FakeRandom(void *arg, unsigned char *bytes, size_t size)
{
    static unsigned counter;
    unsigned char input[4], digest[32];
    (void)arg;
    if (!fake.cryptoReady) { ++fake.randomFailures; return -1; }
    FxPut32(input, ++counter); FxSha(input, 4, digest);
    memcpy(bytes, digest, size);
    return 0;
}

static int Seed(void *arg, unsigned char out[32])
{
    (void)arg;
    if (fake.seedWait) { memset(out, 0, 32); return WT_ENTROPY_WAIT; }
    memset(out, 0xA5, 32);
    return 0;
}

static int FreeSpace(void *context, const char *path, uint64_t *available)
{
    (void)context; (void)path;
    *available = fake.freeSpace;
    return 0;
}

static void LogLine(void *arg, const char *line)
{
    (void)arg;
    ++fake.logLines;
    if (fake.logUsed + strlen(line) < sizeof(fake.log)) {
        memcpy(fake.log + fake.logUsed, line, strlen(line) + 1);
        fake.logUsed += strlen(line);
    }
    if (getenv("MODEL_TEST_LOG")) fputs(line, stdout);
}

/* ---- Files ------------------------------------------------------------------ */

static FxFile saveBytes;
static char backupRoot[300];

static void Path(char *out, size_t size, const char *relative)
{
    snprintf(out, size, "%s%s", fake.root, relative);
}

static void MakeDirectories(const char *relative)
{
    char path[512], *p;
    Path(path, sizeof(path), relative);
    for (p = path + strlen(fake.root); *p; ++p) {
        if (*p != '/') continue;
        *p = 0; mkdir(path, 0700); *p = '/';
    }
    mkdir(path, 0700);
}

static void WriteFile(const char *relative, const void *bytes, size_t size)
{
    char path[512];
    FILE *file;
    Path(path, sizeof(path), relative);
    file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, size, file) == size && fclose(file) == 0);
}

static void ReadFile(const char *relative, void *bytes, size_t size)
{
    char path[512];
    FILE *file;
    Path(path, sizeof(path), relative);
    file = fopen(path, "rb");
    assert(file && fread(bytes, 1, size, file) == size);
    fclose(file);
}

/* Backup folders of a save: <stem>-<id> (and the older <stem>). */
static int BackupFolder(const char *folder, const char *stem)
{
    size_t length = strlen(stem);
    return !strncmp(folder, stem, length) && (!folder[length] || folder[length] == '-');
}

static unsigned CountBackups(const char *stem)
{
    char path[700];
    DIR *root = opendir(backupRoot), *directory;
    struct dirent *folder, *item;
    unsigned count = 0;
    if (!root) return 0;
    while ((folder = readdir(root))) {
        if (!BackupFolder(folder->d_name, stem)) continue;
        snprintf(path, sizeof(path), "%s/%s", backupRoot, folder->d_name);
        if (!(directory = opendir(path))) continue;
        while ((item = readdir(directory))) if (strstr(item->d_name, ".sav") && !strstr(item->d_name, ".tmp")) ++count;
        closedir(directory);
    }
    closedir(root);
    return count;
}

static unsigned CountBackupFolders(const char *stem)
{
    DIR *root = opendir(backupRoot);
    struct dirent *folder;
    unsigned count = 0;
    if (!root) return 0;
    while ((folder = readdir(root))) count += BackupFolder(folder->d_name, stem);
    closedir(root);
    return count;
}

/* The path of a save's only backup file, relative to the card's root. */
static void OnlyBackup(const char *stem, char *out, size_t size)
{
    char path[700];
    DIR *root = opendir(backupRoot), *directory;
    struct dirent *folder, *item;
    out[0] = 0;
    assert(root);
    while ((folder = readdir(root))) {
        if (!BackupFolder(folder->d_name, stem)) continue;
        snprintf(path, sizeof(path), "%s/%s", backupRoot, folder->d_name);
        directory = opendir(path);
        while (directory && (item = readdir(directory)))
            if (strstr(item->d_name, ".sav")) {
                int written = snprintf(out, size, "_nds/dswt/backups/%s/%s", folder->d_name, item->d_name);
                assert(written >= 0 && (size_t)written < size);
            }
        if (directory) closedir(directory);
    }
    closedir(root);
    assert(out[0]);
}

static void RemoveBackups(const char *stem)
{
    char path[700], file[1000];
    DIR *root = opendir(backupRoot), *directory;
    struct dirent *folder, *item;
    if (!root) return;
    while ((folder = readdir(root))) {
        if (!BackupFolder(folder->d_name, stem)) continue;
        snprintf(path, sizeof(path), "%s/%s", backupRoot, folder->d_name);
        if (!(directory = opendir(path))) continue;
        while ((item = readdir(directory))) {
            if (item->d_name[0] == '.') continue;
            snprintf(file, sizeof(file), "%s/%s", path, item->d_name);
            assert(unlink(file) == 0);
        }
        closedir(directory);
        assert(rmdir(path) == 0);
    }
    closedir(root);
}

static void WriteRom(const char *relative, const char code[4], uint8_t revision)
{
    Bytes rom = SyntheticPlatinum(code, revision);
    WriteFile(relative, rom.data, rom.size);
    free(rom.data);
}

/* LUNE-880: a game the companion does not trade for (Pokémon Ranger's code). */
static void WriteOtherRom(const char *relative)
{
    Bytes rom = SyntheticPlatinum("CPUE", 1);
    memcpy(rom.data + 0x0C, "YP2E", 4);
    WriteFile(relative, rom.data, rom.size);
    free(rom.data);
}

/* LUNE-881: a synthetic Black, and a Black save of `count` party Pokémon. */
static void WriteBlackRom(const char *relative)
{
    Bytes rom = SyntheticGen5("IRBO", 0);
    WriteFile(relative, rom.data, rom.size);
    free(rom.data);
}

static Fx5File gen5Save;

static void WriteBlackSave(const char *relative, unsigned count, int musical)
{
    Fx5Spec specs[6];
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = Fx5Default(i);
    Fx5Build(&gen5Save, specs, count, 40);
    if (musical) {
        /* A downloaded musical the game would load, marked in the save. */
        unsigned char *base = gen5Save.bytes + FX5_MUSICAL, *footer = base + FX5_MUSICAL_USED - 16;
        unsigned char *trainer = Fx5Block(&gen5Save, 0, FX5_TRAINER);
        unsigned crc;
        memset(base, 0x42, FX5_MUSICAL_BLOCK);
        crc = Fx5Crc(base, FX5_MUSICAL_BLOCK);
        Fx5W16(base + FX5_MUSICAL_BLOCK + 2, crc);
        Fx5W16(footer - 4, crc); Fx5W16(footer - 2, 0);
        Fx5W32(footer, 9); Fx5W32(footer + 4, FX5_MUSICAL_USED); Fx5W32(footer + 8, CP_SAVE_MAGIC_GEN5);
        Fx5W16(footer + 14, Fx5Crc(footer - 4, 4));
        Fx5W16(trainer + 0x3C, 0xC21E);
        Fx5W32(trainer + FX5_COUNTS + 24, 9);
        Fx5SealBoth(&gen5Save, 40);
    } else {
        /* Leftovers of a musical the game no longer reads, in the journal banks. */
        for (i = 0; i < 0x2000; ++i) gen5Save.bytes[FX5_JOURNAL + i] = (unsigned char)(i * 5 + 1);
    }
    WriteFile(relative, gen5Save.bytes, sizeof(gen5Save.bytes));
}

/* Pokedex_Init, an empty Pokétch history and journal page, encoded records:
 * what a matched delivery's receive effects read (as platinum_receive_test.c). */
static void FreshGeneral(unsigned char *general)
{
    unsigned char *dex = general + CP_RECEIVE_POKEDEX_OFFSET, *records = general + CP_RECEIVE_RECORDS_OFFSET;
    unsigned sum = 0, i;
    memset(dex, 0, 804);
    FxPut32(dex, 0xBEEFCAFE);
    memset(dex + 268, 0xFF, 28);
    dex[264] = dex[265] = dex[266] = dex[267] = 0xFF;
    FxPut32(dex + 796, 0xFFFFFFFF); dex[800] = dex[801] = 0xFF;
    dex[67] = 0xFF; dex[131] = 0xFF;
    memset(records, 0, 444);
    FxPut16(records + 442, 0x1234);
    for (i = 4; i < 440; ++i) sum += records[i];
    FxPut16(records + 440, sum & 65535);
    FxCrypt(records + 4, 436, (sum & 65535) | (0x1234u << 16));
    memset(general + CP_RECEIVE_POKETCH_OFFSET + 184, 0, 96);
    memset(general + CP_RECEIVE_JOURNAL_OFFSET, 0, 112);
    FxPut16(general + CP_RECEIVE_VARS_OFFSET + 2 * 86, 0);
}

/* A Platinum save with `count` party Pokémon; the journal banks are blank. */
static void WriteSave(const char *relative, unsigned count)
{
    FxSpec specs[6];
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = FxDefault(i);
    FxBuild(&saveBytes, specs, count);
    for (i = 0; i < 2; ++i) {
        FreshGeneral(FxGeneral(&saveBytes, i));
        FxSeal(&saveBytes, i, 0, 10 + i, 20 + i);
    }
    WriteFile(relative, saveBytes.bytes, sizeof(saveBytes.bytes));
}

/* The retail blocks of both halves: what the game reads. */
static void Retail(const char *relative, unsigned char *out)
{
    static unsigned char bytes[CP_SAVE_FILE_SIZE];
    ReadFile(relative, bytes, sizeof(bytes));
    memcpy(out, bytes, 0x20000);
    memcpy(out + 0x20000, bytes + CP_SAVE_HALF_SIZE, 0x20000);
}

static void Setup(void)
{
    char template[] = "/tmp/dswt-model-XXXXXX", *made = mkdtemp(template);
    static const unsigned char small[100] = { 0 };
    static unsigned char blank[CP_SAVE_FILE_SIZE];
    assert(made);
    snprintf(fake.root, sizeof(fake.root), "%s/", made);
    snprintf(backupRoot, sizeof(backupRoot), "%s_nds/dswt/backups", fake.root);
    fake.saveFd = -1;
    fake.freeSpace = UINT64_C(1) << 30;
    MakeDirectories("roms/nds/saves");
    MakeDirectories("roms/nds/pt");
    MakeDirectories("_nds/dswt");
    /* TWiLight's saves folder, a ROM beside its save one folder down, a save
     * without a ROM, a game the companion does not trade for, a save the game
     * never wrote, and files that are not saves. */
    WriteRom("roms/nds/Platinum.nds", "CPUE", 1);
    WriteSave("roms/nds/saves/Platinum.sav", 4);
    WriteRom("roms/nds/pt/Second.nds", "CPUE", 1);
    WriteSave("roms/nds/pt/Second.sav", 3);
    WriteSave("roms/nds/saves/Lonely.sav", 2);
    WriteRom("roms/nds/Diamond.nds", "ADAE", 5);
    WriteSave("roms/nds/saves/Diamond.sav", 2);
    WriteOtherRom("roms/nds/Ranger.nds");
    WriteSave("roms/nds/saves/Ranger.sav", 2);
    WriteBlackRom("roms/nds/Black.nds");
    WriteBlackSave("roms/nds/saves/Black.sav", 4, 0);
    WriteBlackRom("roms/nds/Musical.nds");
    WriteBlackSave("roms/nds/saves/Musical.sav", 2, 1);
    WriteRom("roms/nds/Blank.nds", "CPUE", 1);
    memset(blank, 0xFF, sizeof(blank));
    WriteFile("roms/nds/saves/Blank.sav", blank, sizeof(blank));
    WriteFile("roms/nds/saves/Short.sav", small, sizeof(small));
    WriteFile("roms/nds/saves/notes.txt", small, sizeof(small));

    platform.arg = &fake;
    platform.root = fake.root;
    platform.list = List;
    platform.openSave = OpenSave; platform.closeSave = CloseSave;
    platform.openRom = OpenRom; platform.closeRom = CloseRom;
    platform.start = Start; platform.finished = Finished; platform.cancel = Cancel;
    platform.wifiConnect = WifiConnect; platform.wifiStatus = WifiStatus;
    platform.transport.arg = NULL;
    platform.transport.post = Post;
    platform.transport.cancel = &fake.cancel;
    platform.transport.prepare = Prepare;
    platform.random = FakeRandom;
    platform.seed = Seed;
    platform.backup.root = backupRoot;
    platform.backup.freeSpace = FreeSpace;
    platform.log = LogLine;
    platform.wifiScan = WifiScanFake;
    platform.wifiNetworks = WifiNetworks;
    platform.wifiJoin = WifiJoinFake;
    platform.wifiFailure = WifiFailure;
    platform.wifiWpa2 = 1;
}

/* ---- Driving the model ------------------------------------------------------- */

static CpUiModel *model = &app.model;

static void Pump(void)
{
    unsigned frames;
    for (frames = 0; frames < 16; ++frames) {
        model->ops->update(model->context);
        if (!model->busy && model->net != CP_UI_NET_BUSY) return;
    }
    assert(!"the model never settled");
}

static int FindSave(const char *name)
{
    unsigned i;
    for (i = 0; i < model->saveCount; ++i) if (!strcmp(model->saves[i].name, name)) return (int)i;
    return -1;
}

static void LoadByName(const char *name)
{
    int index = FindSave(name);
    assert(index >= 0);
    model->ops->load(model->context, (unsigned)index);
    Pump();
}

static void Connect(void)
{
    model->ops->connect(model->context);
    Pump();
    assert(model->net == CP_UI_NET_READY);
}

static void Visit(int slot, int collect, int cancel)
{
    if (collect) model->ops->collect(model->context, cancel);
    else model->ops->trade(model->context, slot);
    assert(model->net == CP_UI_NET_BUSY && !model->canLeave);
    Pump();
}

static void CloseSession(void)
{
    model->ops->close(model->context);
    Pump();
    assert(model->net == CP_UI_NET_CLOSED);
    model->ops->unload(model->context);
}

static int NameIs(const uint16_t *text, const uint16_t *expected)
{
    unsigned i;
    for (i = 0;; ++i) {
        if (text[i] != expected[i]) return 0;
        if (expected[i] == CP_CHAR_EOS) return 1;
    }
}

/* ---- Tests ------------------------------------------------------------------- */

static void ScanFindsSavesAndTheirRoms(void)
{
    int platinum, second, lonely, diamond, blank, black, ranger;
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    model->ops->scan(model->context);
    assert(model->busy);
    Pump();
    assert(model->saveCount == 8);
    platinum = FindSave("Platinum.sav"); second = FindSave("Second.sav"); lonely = FindSave("Lonely.sav");
    diamond = FindSave("Diamond.sav"); blank = FindSave("Blank.sav"); black = FindSave("Black.sav");
    ranger = FindSave("Ranger.sav");
    assert(platinum >= 0 && second >= 0 && lonely >= 0 && diamond >= 0 && blank >= 0 && black >= 0 && ranger >= 0);
    assert(model->saves[platinum].status == CP_UI_SAVE_READY && model->saves[platinum].version == CP_VERSION_PLATINUM
        && model->saves[platinum].language == CP_LANGUAGE_ENGLISH);
    assert(model->saves[second].status == CP_UI_SAVE_READY);
    assert(model->saves[lonely].status == CP_UI_SAVE_NO_ROM);
    /* LUNE-880: Diamond/Pearl and HeartGold/SoulSilver trade too. */
    assert(model->saves[diamond].status == CP_UI_SAVE_READY && model->saves[diamond].version == CP_VERSION_DIAMOND);
    assert(model->saves[ranger].status == CP_UI_SAVE_UNSUPPORTED);
    /* LUNE-881: and Black/White and Black 2/White 2. */
    assert(model->saves[black].status == CP_UI_SAVE_READY && model->saves[black].version == CP_VERSION_BLACK);
    assert(!strcmp(app.saves[second].rom + strlen(fake.root), "roms/nds/pt/Second.nds"));
    assert(!fake.romOpen[CP_APP_ROM_SCAN]);
    puts("PASS model: scan pairs saves in saves/, beside ROMs and one folder down; skips non-saves");
}

static void LoadRefusals(void)
{
    LoadByName("Lonely.sav");
    assert(model->loadMessage == Companion_Text_MatchingRomMissing && !model->busy);
    model->ops->unload(model->context);
    LoadByName("Ranger.sav");
    assert(model->loadMessage == Companion_Text_SaveUnsupported);
    model->ops->unload(model->context);
    /* LUNE-881: a downloaded musical the game would load is never erased. */
    {
        static unsigned char before[CP_SAVE_FILE_SIZE], after[CP_SAVE_FILE_SIZE];
        ReadFile("roms/nds/saves/Musical.sav", before, sizeof(before));
        LoadByName("Musical.sav");
        assert(model->loadMessage == Companion_Text_MusicalSaved);
        ReadFile("roms/nds/saves/Musical.sav", after, sizeof(after));
        assert(memcmp(before, after, sizeof(before)) == 0);
    }
    model->ops->unload(model->context);
    /* A Platinum save beside a Diamond ROM is not a save Diamond would load. */
    LoadByName("Diamond.sav");
    assert(model->loadMessage == Companion_Text_SaveInGameFirst);
    model->ops->unload(model->context);
    LoadByName("Blank.sav");
    assert(model->loadMessage == Companion_Text_SaveInGameFirst);
    assert(fake.saveFd < 0 && !fake.romOpen[CP_APP_ROM_GAME]);
    model->ops->unload(model->context);
    puts("PASS model: no ROM, an unsupported game and a never-saved file are refused with their messages");
}

static unsigned char before[0x40000], after[0x40000], original[FX_MON];

static void OfferStatusCancelCollect(void)
{
    unsigned char backup[CP_SAVE_FILE_SIZE], live[CP_SAVE_FILE_SIZE];
    char name[256];
    unsigned party;
    memset(&server, 0, sizeof(server));
    Retail("roms/nds/saves/Platinum.sav", before);
    LoadByName("Platinum.sav");
    assert(model->loadMessage == -1 && !model->recovery && model->font);
    assert(model->gameVersion == CP_VERSION_PLATINUM && model->gameLanguage == CP_LANGUAGE_ENGLISH);
    assert(model->partyCount == 4 && model->tradePhase == WT_JOURNAL_EMPTY);
    assert(model->party[0].species == 25 && NameIs(model->party[0].speciesName, kWesternName));
    assert(model->party[0].level == 5 && !model->party[0].isEgg && model->party[0].ineligible == CP_UI_ELIGIBLE);
    assert(app.needsProvision && CountBackups("Platinum") == 0);
    memcpy(original, FxImageParty(&gCpAppImage, 1), FX_MON);

    /* Connect, then the UI resumes: provision, enroll, back up, reconcile. */
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0 && model->tradePhase == WT_JOURNAL_IDLE);
    assert(model->canLeave && !model->completed);
    assert(server.requests[WT_OP_ENROLL] == 1 && !server.requests[WT_OP_SUBMIT]);
    /* The backup holds the save as it was: enrolled, retail untouched. */
    assert(CountBackups("Platinum") == 1);
    Retail("roms/nds/saves/Platinum.sav", after);
    assert(memcmp(before, after, sizeof(before)) == 0);
    OnlyBackup("Platinum", name, sizeof(name));
    ReadFile(name, backup, sizeof(backup));
    ReadFile("roms/nds/saves/Platinum.sav", live, sizeof(live));
    assert(memcmp(backup, live, sizeof(live)) == 0);
    puts("PASS model: first visit provisions, enrolls and backs up before any edit; retail unchanged");

    /* Offer slot 1: deposited, submitted, queued; the party shows it gone. */
    Visit(1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0);
    assert(model->tradePhase == WT_JOURNAL_QUEUED && model->canLeave && !model->waiting);
    assert(model->partyCount == 3 && server.requests[WT_OP_SUBMIT] == 1);
    assert(CountBackups("Platinum") == 1);
    puts("PASS model: an offer leaves the party and waits queued");

    /* Collect while unmatched: no partner yet. */
    Visit(-1, 1, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->waiting && model->tradePhase == WT_JOURNAL_QUEUED);
    puts("PASS model: collecting an unmatched offer reports waiting");

    /* Cancel: the server returns it, the visit collects and acknowledges it. */
    Visit(-1, 1, 1);
    assert(model->net == CP_UI_NET_RESPONSE && model->completed && model->returned);
    assert(model->tradePhase == WT_JOURNAL_IDLE && model->canLeave);
    party = model->partyCount;
    assert(party == 4 && memcmp(FxImageParty(&gCpAppImage, 3), original, FX_MON) == 0);
    assert(server.requests[WT_OP_ACKNOWLEDGE] == 1);
    /* Several visits and many exchanges in one session: PSA started again
     * before each, the NV seed advanced through the engine's own journal, and
     * the engine never lacked random numbers. */
    printf("  exchanges %u, PSA starts %u, random failures %u\n", fake.posts, fake.seedWrites, fake.randomFailures);
    assert(fake.randomFailures == 0 && fake.seedWrites >= fake.posts && fake.posts >= 8);
    printf("PASS model: cancel collects the return; %u exchanges, %u seed updates, no random failures\n",
        fake.posts, fake.seedWrites);
    CloseSession();
    assert(fake.saveFd < 0 && !fake.romOpen[CP_APP_ROM_GAME]);
}

/* LUNE-881: a Black save end to end: the party as the screens show it, the
 * journal provisioned in the musical distribution area after its leftovers
 * are erased, an offer and its return. */
static void BlackOfferAndReturn(void)
{
    static unsigned char bytes[CP_SAVE_FILE_SIZE];
    unsigned i;
    memset(&server, 0, sizeof(server));
    LoadByName("Black.sav");
    assert(model->loadMessage == -1 && model->font && model->gameVersion == CP_VERSION_BLACK);
    assert(model->partyCount == 4 && model->party[0].species == 495 && model->party[0].level == 5);
    assert(model->party[0].speciesName[0] == 'S' && model->party[0].speciesName[5] == CP_CHAR_EOS);
    assert(model->party[0].ineligible == CP_UI_ELIGIBLE && app.needsProvision);
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0 && model->tradePhase == WT_JOURNAL_IDLE);
    ReadFile("roms/nds/saves/Black.sav", bytes, sizeof(bytes));
    assert(memcmp(bytes + 0x74000, "DSWJ", 4) == 0 && memcmp(bytes + 0x75000, "DSWJ", 4) == 0);
    for (i = 4096 - 4; i < 4096; ++i) assert(bytes[0x74000 + i] == "CMIT"[i - 4092]);
    assert(CountBackups("Black") == 1);
    Visit(1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0 && model->tradePhase == WT_JOURNAL_QUEUED);
    assert(model->partyCount == 3 && server.requests[WT_OP_SUBMIT] == 1);
    Visit(-1, 1, 1);
    assert(model->completed && model->returned && model->tradePhase == WT_JOURNAL_IDLE && model->partyCount == 4);
    /* The game loads the save the companion left: both copies hold it. */
    ReadFile("roms/nds/saves/Black.sav", bytes, sizeof(bytes));
    assert(memcmp(bytes, bytes + 0x24000, 0x23F9C) == 0);
    CloseSession();
    puts("PASS model: a Black save trades: journal in the musical area, offer and return, both copies alike");
}

static void SupportCodesMatchTheGame(void)
{
    unsigned submits = server.requests[WT_OP_SUBMIT];
    LoadByName("Platinum.sav");
    Connect();
    /* The server has counted a trade this older copy never sent. */
    ++server.nextCounter;
    Visit(-1, 0, 0);
    assert(app.visit.result == CP_VISIT_ERROR_ROLLED_BACK && app.visit.rolledBack);
    assert(model->net == CP_UI_NET_ERROR && model->error == -41 && model->canLeave);
    assert(server.requests[WT_OP_SUBMIT] == submits);
    --server.nextCounter;
    CloseSession();

    LoadByName("Platinum.sav");
    Connect();
    fake.corruptReply = 1;
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_ERROR && model->error == -18 && !model->canLeave);
    fake.corruptReply = 0;
    CloseSession();

    LoadByName("Platinum.sav");
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0 && model->canLeave);
    CloseSession();
    puts("PASS model: rollback -41 and malformed reply -18 match the game; a valid retry clears the error");
}

static void ReloadEnrolledSave(void)
{
    /* An enrolled save: the load checks for a backup of this enrollment
     * before the boot check; the first one still counts. */
    LoadByName("Platinum.sav");
    assert(model->loadMessage == -1 && !model->recovery && model->tradePhase == WT_JOURNAL_IDLE);
    assert(model->partyCount == 4 && CountBackups("Platinum") == 1);
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->tradePhase == WT_JOURNAL_IDLE);
    CloseSession();
    puts("PASS model: an enrolled save with a backup of its enrollment loads and resumes");
}

/* A matched delivery lands with the game's receive effects; the RECEIVED card
 * shows it. One this game cannot hold stays pending, the save untouched. */
static void MatchedDeliveries(void)
{
    unsigned char canonical[136];
    FxSpec spec = FxDefault(5);
    unsigned acknowledged;
    LoadByName("Platinum.sav");
    Connect();
    Visit(-1, 0, 0);
    Visit(0, 0, 0);
    assert(model->tradePhase == WT_JOURNAL_QUEUED && model->partyCount == 3);
    spec.species = 25;
    FxCanonical(&spec, canonical);
    FsMatch(canonical);
    acknowledged = server.requests[WT_OP_ACKNOWLEDGE];
    Visit(-1, 1, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->completed && !model->returned && !model->refused);
    assert(model->tradePhase == WT_JOURNAL_IDLE && model->canLeave);
    assert(server.requests[WT_OP_ACKNOWLEDGE] == acknowledged + 1);
    assert(model->partyCount == 4 && model->party[3].species == 25);
    assert(model->hasReceived && model->received.species == 25 && NameIs(model->received.speciesName, kWesternName));
    assert(model->received.originalTrainer[CP_UI_TRAINER_MAX - 1] == CP_CHAR_EOS);
    CloseSession();
    /* Reopened: the save loads and holds the received Pokémon. */
    LoadByName("Platinum.sav");
    assert(model->loadMessage == -1 && !model->recovery && model->partyCount == 4 && model->party[3].species == 25);
    puts("PASS model: a matched delivery lands in the party; the RECEIVED card shows it");

    /* A species this game cannot hold: refused before anything is recorded. */
    Connect();
    Visit(-1, 0, 0);
    Visit(0, 0, 0);
    assert(model->tradePhase == WT_JOURNAL_QUEUED && model->partyCount == 3);
    Retail("roms/nds/saves/Platinum.sav", before);
    spec.species = 600;
    FxCanonical(&spec, canonical);
    FsMatch(canonical);
    acknowledged = server.requests[WT_OP_ACKNOWLEDGE];
    Visit(-1, 1, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->refused && !model->completed && !model->hasReceived);
    assert(model->tradePhase == WT_JOURNAL_QUEUED && model->partyCount == 3 && model->canLeave);
    Retail("roms/nds/saves/Platinum.sav", after);
    assert(memcmp(before, after, sizeof(before)) == 0 && server.requests[WT_OP_ACKNOWLEDGE] == acknowledged);
    CloseSession();
    puts("PASS model: a delivery this game cannot hold stays pending; the save is untouched");
}

static void RestoredRetailNeedsRecovery(void)
{
    static unsigned char queued[CP_SAVE_FILE_SIZE], older[CP_SAVE_FILE_SIZE];
    /* The journal says the offer left, but the retail blocks are an older copy
     * that still holds it: the boot check refuses, recovery is required. */
    ReadFile("roms/nds/saves/Platinum.sav", queued, sizeof(queued));
    ReadFile("roms/nds/pt/Second.sav", older, sizeof(older));
    memcpy(older + 0x30000, queued + 0x30000, 0x10000);
    memcpy(older + 0x70000, queued + 0x70000, 0x10000);
    WriteFile("roms/nds/pt/Second.sav", older, sizeof(older));
    LoadByName("Second.sav");
    assert(model->loadMessage == -1 && model->recovery);
    model->ops->unload(model->context);
    puts("PASS model: a journal that the retail copy cannot prove needs recovery");
}

static void BackupFailureStopsBeforeAnyEdit(void)
{
    unsigned submits;
    RemoveBackups("Second");
    WriteSave("roms/nds/pt/Second.sav", 3);
    memset(&server, 0, sizeof(server));
    fake.freeSpace = 1000;
    Retail("roms/nds/pt/Second.sav", before);
    LoadByName("Second.sav");
    assert(model->loadMessage == -1 && model->partyCount == 3);
    Connect();
    Visit(-1, 0, 0);
    /* Enrolled, but the backup failed: the visit stops with its code. */
    assert(model->net == CP_UI_NET_ERROR && model->error == CP_APP_ERROR_BACKUP && model->canLeave);
    assert(model->tradePhase == WT_JOURNAL_IDLE && CountBackups("Second") == 0);
    submits = server.requests[WT_OP_SUBMIT];
    /* Retry with an offer: still no backup, so no deposit and no submit. */
    Visit(0, 0, 0);
    assert(model->net == CP_UI_NET_ERROR && model->error == CP_APP_ERROR_BACKUP);
    assert(server.requests[WT_OP_SUBMIT] == submits && model->partyCount == 3);
    Retail("roms/nds/pt/Second.sav", after);
    assert(memcmp(before, after, sizeof(before)) == 0);
    CloseSession();
    /* Loading an enrolled save also backs up first, and stops when it cannot. */
    LoadByName("Second.sav");
    assert(model->loadMessage == Companion_Text_BackupFailed);
    model->ops->unload(model->context);
    fake.freeSpace = UINT64_C(1) << 30;
    puts("PASS model: when the backup fails nothing is edited, at load or before an offer");
}

static void NeedsEntropyAndWifiFailure(void)
{
    WriteSave("roms/nds/pt/Second.sav", 3);
    LoadByName("Second.sav");
    assert(app.needsProvision);
    fake.seedWait = 1;
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->needsEntropy && model->canLeave);
    assert(app.needsProvision);
    fake.seedWait = 0;
    CloseSession();

    fake.wifiConnected = 0; fake.wifiFails = 1;
    LoadByName("Second.sav");
    model->ops->connect(model->context);
    Pump();
    assert(model->net == CP_UI_NET_ERROR && model->error == CP_APP_ERROR_WIFI);
    model->ops->close(model->context);
    assert(model->net == CP_UI_NET_CLOSED);
    model->ops->unload(model->context);
    fake.wifiFails = 0;
    puts("PASS model: too little entropy asks for more play; no working Wi-Fi is a clear error");
}

/* The screens on the real model: choose a save, confirm the game, connect
 * and reach the menu, as a player would. */
static void ScreensDriveTheModel(void)
{
    CpUi ui;
    CpUiInput input;
    unsigned frame;
    int target;
    RemoveBackups("Second");
    WriteSave("roms/nds/pt/Second.sav", 3);
    memset(&server, 0, sizeof(server));
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    CpUi_Start(&ui, model);
    memset(&input, 0, sizeof(input));
    for (frame = 0; frame < 4; ++frame) CpUi_Frame(&ui, model, &input);
    assert(ui.state == CP_UI_SAVE_PICKER && model->saveCount == 8);
    target = FindSave("Second.sav");
    ui.listCursor = target;
    input.pressed = CP_UI_KEY_A; CpUi_Frame(&ui, model, &input); input.pressed = 0;
    for (frame = 0; frame < 4; ++frame) CpUi_Frame(&ui, model, &input);
    assert(ui.state == CP_UI_GAME_CONFIRM);
    input.pressed = CP_UI_KEY_A; CpUi_Frame(&ui, model, &input); input.pressed = 0;
    for (frame = 0; frame < 8; ++frame) CpUi_Frame(&ui, model, &input);
    assert(ui.state == CP_UI_MENU && model->tradePhase == WT_JOURNAL_IDLE);
    puts("PASS model: the screens pick a save, confirm it, connect and reach the menu");
}

static void Rescan(void)
{
    model->ops->close(model->context);
    Pump();
    model->ops->unload(model->context);
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    model->ops->scan(model->context);
    Pump();
}

static void LoadByPath(const char *part)
{
    unsigned i;
    for (i = 0; i < model->saveCount; ++i) {
        if (!strstr(app.saves[i].path, part)) continue;
        model->ops->load(model->context, i);
        Pump();
        return;
    }
    assert(!"no such save");
}

/* Retail writes that never reach the card: the save's checks read the card,
 * not the cache, so the removal is not proven and nothing is submitted. The
 * visit ends unable to leave, and reconnecting does not change that. */
static void UnprovenWritesAndTheLeaveFence(void)
{
    unsigned submits;
    RemoveBackups("Second");
    WriteSave("roms/nds/pt/Second.sav", 3);
    memset(&server, 0, sizeof(server));
    fake.wifiConnected = 1; fake.wifiFails = 0;
    Rescan();
    LoadByName("Second.sav");
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->tradePhase == WT_JOURNAL_IDLE);
    submits = server.requests[WT_OP_SUBMIT];
    fake.loseRetail = 1;
    Visit(1, 0, 0);
    assert(model->net == CP_UI_NET_ERROR && model->error != CP_APP_ERROR_BACKUP);
    assert(server.requests[WT_OP_SUBMIT] == submits && !model->canLeave);
    puts("PASS model: a retail write the card does not hold is never proven; nothing is submitted");

    /* Retry: close, reconnect, and Wi-Fi fails. Exit must stay hidden. */
    model->ops->close(model->context);
    assert(model->net == CP_UI_NET_CLOSED);
    fake.wifiConnected = 0; fake.wifiFails = 1;
    model->ops->connect(model->context);
    assert(!model->canLeave);
    Pump();
    assert(model->net == CP_UI_NET_ERROR && model->error == CP_APP_ERROR_WIFI && !model->canLeave);
    fake.loseRetail = 0; fake.wifiFails = 0;
    model->ops->close(model->context);
    model->ops->unload(model->context);
    /* Reopened, the boot check settles the cut deposit from the card. */
    LoadByName("Second.sav");
    assert(model->loadMessage == -1 && model->partyCount == 3);
    model->ops->unload(model->context);
    puts("PASS model: a reconnect keeps the last proven leave status (no Exit past the recovery fence)");
}

/* Two saves named Platinum.sav in different folders keep separate backups,
 * named by their enrollment; a backup in the older <stem>/ folder still counts. */
static void SameNameSavesKeepSeparateBackups(void)
{
    char from[700], to[700];
    unsigned folders;
    MakeDirectories("roms/nds/other");
    WriteRom("roms/nds/other/Platinum.nds", "CPUE", 1);
    WriteSave("roms/nds/other/Platinum.sav", 4);
    memset(&server, 0, sizeof(server));
    fake.wifiConnected = 1;
    Rescan();
    assert(CountBackupFolders("Platinum") == 1);
    LoadByPath("other/Platinum.sav");
    assert(model->loadMessage == -1);
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0);
    assert(CountBackupFolders("Platinum") == 2);
    CloseSession();
    puts("PASS model: same-named saves in two folders back up side by side");

    /* Move this enrollment's folder to the older naming: it still counts. */
    snprintf(from, sizeof(from), "%s/Platinum-%02x515151", backupRoot, fsEnrollments & 0xff);
    snprintf(to, sizeof(to), "%s/Platinum", backupRoot);
    assert(rename(from, to) == 0);
    folders = CountBackupFolders("Platinum");
    Rescan();
    LoadByPath("other/Platinum.sav");
    assert(model->loadMessage == -1 && CountBackupFolders("Platinum") == folders);
    model->ops->unload(model->context);
    puts("PASS model: a backup in the older per-name folder still satisfies the contract");
}

/* Copies of a Pokémon (by canonical bytes) in a save file's party and PC. */
static unsigned Copies(const char *relative, const unsigned char original[FX_MON])
{
    static FxFile file;
    static CpSaveImage image;
    CpFile cpFile = { &file, FxRead, FxWrite, FxSync, FxSize };
    unsigned char want[136], have[136];
    unsigned count = 0, i;
    ReadFile(relative, file.bytes, sizeof(file.bytes));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    assert(WtPokemon_Decode(original, want) == 0);
    for (i = 0; i < FxImageCount(&image); ++i)
        if (WtPokemon_Decode(FxImageParty(&image, i), have) == 0 && !memcmp(have, want, 136)) ++count;
    for (i = 0; i < 18 * 30; ++i)
        if (WtPokemon_Decode(FxImageBox(&image, i / 30, i % 30), have) == 0 && !memcmp(have, want, 136)) ++count;
    return count;
}

/* LUNE-897's guard through the model: an offer whose Pokémon is already back
 * in the save (an older copy restored, then saved in a stock game) is never
 * added again when its return is collected; the visit fails closed. */
static void ReturnAlreadyPresentFailsClosed(void)
{
    unsigned char original[FX_MON];
    unsigned half, acknowledged;
    RemoveBackups("Second");
    WriteSave("roms/nds/pt/Second.sav", 3);
    memset(&server, 0, sizeof(server));
    fake.wifiConnected = 1;
    Rescan();
    LoadByName("Second.sav");
    memcpy(original, FxImageParty(&gCpAppImage, 1), FX_MON);
    Connect();
    Visit(-1, 0, 0);
    Visit(1, 0, 0);
    assert(model->tradePhase == WT_JOURNAL_QUEUED && Copies("roms/nds/pt/Second.sav", original) == 0);
    Rescan();
    /* The player puts it back in the PC and saves in a stock game. */
    ReadFile("roms/nds/pt/Second.sav", saveBytes.bytes, sizeof(saveBytes.bytes));
    for (half = 0; half < 2; ++half) {
        memcpy(FxBoxes(&saveBytes, half) + FX_PC, original, FX_BOX_MON);
        FxSeal(&saveBytes, half, 0, 1000 + half, 2000 + half);
        FxSeal(&saveBytes, half, 1, 1000 + half, 3000 + half);
    }
    WriteFile("roms/nds/pt/Second.sav", saveBytes.bytes, sizeof(saveBytes.bytes));
    assert(Copies("roms/nds/pt/Second.sav", original) == 1);
    LoadByName("Second.sav");
    assert(model->loadMessage == -1 && model->recovery);
    Connect();
    acknowledged = server.requests[WT_OP_ACKNOWLEDGE];
    Visit(-1, 1, 1);
    assert(model->net == CP_UI_NET_ERROR && !model->completed && !model->canLeave);
    assert(server.requests[WT_OP_ACKNOWLEDGE] == acknowledged);
    assert(Copies("roms/nds/pt/Second.sav", original) == 1);
    model->ops->close(model->context);
    Pump();
    model->ops->unload(model->context);
    puts("PASS model: a returned offer already back in the save is never added twice (fails closed)");
}


/* LUNE-902 through the model: an offer cut before its removal reached the
 * card, then trained in a stock game. The load needs recovery; connecting
 * abandons the unsent escrow, the trained Pokémon stays, nothing is sent. */
static void UnsentOfferTrainedInStockGame(void)
{
    unsigned char original[FX_MON], trained[FX_MON];
    unsigned half, submits;
    RemoveBackups("Second");
    WriteSave("roms/nds/pt/Second.sav", 3);
    /* Both retail copies already hold the save, so the offer's checkpoint
     * writes nothing and the first retail write is the removal. */
    memcpy(FxGeneral(&saveBytes, 0), FxGeneral(&saveBytes, 1), FX_GENERAL_SIZE);
    memcpy(FxBoxes(&saveBytes, 0), FxBoxes(&saveBytes, 1), FX_BOXES_SIZE);
    FxSeal(&saveBytes, 0, 0, 10, 20);
    FxSeal(&saveBytes, 0, 1, 10, 30);
    WriteFile("roms/nds/pt/Second.sav", saveBytes.bytes, sizeof(saveBytes.bytes));
    memset(&server, 0, sizeof(server));
    fake.wifiConnected = 1;
    Rescan();
    LoadByName("Second.sav");
    memcpy(original, FxImageParty(&gCpAppImage, 1), FX_MON);
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->tradePhase == WT_JOURNAL_IDLE);
    submits = server.requests[WT_OP_SUBMIT];
    fake.failRetail = 1;
    Visit(1, 0, 0);
    fake.failRetail = 0;
    assert(model->net == CP_UI_NET_ERROR && model->tradePhase == WT_JOURNAL_ESCROW);
    assert(server.requests[WT_OP_SUBMIT] == submits && Copies("roms/nds/pt/Second.sav", original) == 1);
    model->ops->close(model->context);
    Pump();
    model->ops->unload(model->context);
    /* The player trains it in a stock game and saves. */
    ReadFile("roms/nds/pt/Second.sav", saveBytes.bytes, sizeof(saveBytes.bytes));
    for (half = 0; half < 2; ++half) {
        FxTrain(FxGeneral(&saveBytes, half) + FX_PARTY + 8 + FX_MON);
        memcpy(trained, FxGeneral(&saveBytes, half) + FX_PARTY + 8 + FX_MON, FX_MON);
        FxSeal(&saveBytes, half, 0, 1000 + half, 2000 + half);
        FxSeal(&saveBytes, half, 1, 1000 + half, 3000 + half);
    }
    WriteFile("roms/nds/pt/Second.sav", saveBytes.bytes, sizeof(saveBytes.bytes));
    assert(Copies("roms/nds/pt/Second.sav", original) == 0 && Copies("roms/nds/pt/Second.sav", trained) == 1);
    Rescan();
    LoadByName("Second.sav");
    assert(model->loadMessage == -1 && model->recovery && model->tradePhase == WT_JOURNAL_ESCROW);
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->tradePhase == WT_JOURNAL_IDLE && model->canLeave);
    assert(!model->recovery && server.requests[WT_OP_SUBMIT] == submits && server.state == WT_TRADE_NONE);
    assert(Copies("roms/nds/pt/Second.sav", trained) == 1);
    CloseSession();
    puts("PASS model: an unsent offer trained in a stock game is abandoned on connect; it stays, nothing is sent");
}

/* Valid UTF-8 throughout, with no partial character at the end. */
static int ValidUtf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned extra = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : 9;
        if (extra == 9) return 0;
        for (++p; extra; --extra, ++p) if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

static void BackupStems(void)
{
    static const struct { const char *name, *stem; } cases[] = {
        { "Platinum.sav", "Platinum" },
        { "Platinum.SAV", "Platinum" },
        /* 3-byte Japanese: 13 characters fit in 40 bytes (39), the 14th would split. */
        { "ポケットモンスタープラチナ（日本語）.sav", "ポケットモンスタープラチナ" },
        /* Korean, also 3 bytes each. */
        { "포켓몬스터 플래티넘 한국어판 세이브 파일.sav", "포켓몬스터 플래티넘 한국어" },
        { "A very long English save file name that goes on and on.sav", "A very long English save file name that" }, /* no trailing space */
        { "Bad<name>:with*forbidden?chars|\"and\\slash.sav", "Bad_name__with_forbidden_chars__and_slas" },
        { "Trailing dots... .sav", "Trailing dots" },
        { "....sav", "save" },
        { "é.sav", "é" },
    };
    char stem[CP_APP_BACKUP_STEM_MAX + 1];
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        size_t length = CpApp_BackupStem(cases[i].name, stem);
        if (strcmp(stem, cases[i].stem) != 0) printf("stem %u: '%s' -> '%s' (want '%s')\n", i, cases[i].name, stem, cases[i].stem);
        assert(length == strlen(stem) && length <= CP_APP_BACKUP_STEM_MAX && ValidUtf8(stem));
        assert(!strpbrk(stem, "\\/:*?\"<>|") && stem[length - 1] != '.' && stem[length - 1] != ' ');
    }
    for (i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        CpApp_BackupStem(cases[i].name, stem);
        assert(strcmp(stem, cases[i].stem) == 0);
    }
    puts("PASS model: backup folder stems are cut at characters and safe for FAT");
}

/* A long Japanese save name: its backup folder is a valid name and the backup works. */
static void JapaneseSaveBacksUp(void)
{
    memset(&server, 0, sizeof(server));
    fake.wifiConnected = 1;
    MakeDirectories("roms/nds/jp");
    WriteRom("roms/nds/jp/ポケットモンスタープラチナ（日本語）.nds", "CPUE", 1);
    WriteSave("roms/nds/jp/ポケットモンスタープラチナ（日本語）.sav", 3);
    Rescan();
    LoadByPath("jp/");
    assert(model->loadMessage == -1);
    Connect();
    Visit(-1, 0, 0);
    assert(model->net == CP_UI_NET_RESPONSE && model->error == 0);
    assert(CountBackups("ポケットモンスタープラチナ") == 1);
    CloseSession();
    puts("PASS model: a long Japanese save name backs up");
}

/* ---- Wi-Fi settings (LUNE-875) ------------------------------------------------ */

static void AddNetwork(const char *ssid, unsigned security, unsigned bars)
{
    CpWifiNetwork *network = &fake.networks[fake.networkCount++];
    memset(network, 0, sizeof(*network));
    snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
    network->security = (uint8_t)security;
    network->bars = (uint8_t)bars;
}

static void Frames(unsigned count)
{
    while (count--) model->ops->update(model->context);
}

static int CfgExists(const char *suffix)
{
    char path[400];
    FILE *file;
    snprintf(path, sizeof(path), "%s_nds/dswt/wifi.cfg%s", fake.root, suffix);
    file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static void ReadCfg(char *out, size_t size)
{
    char path[400];
    FILE *file;
    size_t used;
    snprintf(path, sizeof(path), "%s_nds/dswt/wifi.cfg", fake.root);
    file = fopen(path, "rb");
    assert(file);
    used = fread(out, 1, size - 1, file);
    out[used] = 0;
    fclose(file);
}

/* Join a listed network as the screens do, and let the attempt finish. */
static void JoinListed(const char *ssid, unsigned security, const char *key)
{
    model->ops->wifiJoin(model->context, ssid, security, key);
    assert(model->wifi.state == CP_UI_NETWORK_JOINING);
    Frames(4);
}

static void WifiScanListsNetworks(void)
{
    unsigned i;
    memset(&server, 0, sizeof(server));
    fake.networkCount = 0;
    fake.wifiConnected = 0;
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    assert(model->ops->wifiScan && model->wifi.wpa2 && !model->wifi.saved);
    AddNetwork("Home", CP_WIFI_SECURITY_WPA2, 2);
    AddNetwork("", CP_WIFI_SECURITY_OPEN, 3);         /* hidden: not listed */
    AddNetwork("Guest", CP_WIFI_SECURITY_OPEN, 1);
    AddNetwork("Home", CP_WIFI_SECURITY_WPA2, 3);     /* a second access point */
    AddNetwork("Old", CP_WIFI_SECURITY_WPA, 2);
    model->ops->wifiScan(model->context);
    assert(fake.scansStarted == 1 && model->wifi.state == CP_UI_NETWORK_SCANNING && model->wifi.count == 0);
    Frames(1);
    assert(model->wifi.count == 3);
    assert(!strcmp(model->wifi.networks[0].ssid, "Home") && model->wifi.networks[0].bars == 3
        && model->wifi.networks[0].security == CP_WIFI_SECURITY_WPA2);
    assert(!strcmp(model->wifi.networks[1].ssid, "Guest") && !strcmp(model->wifi.networks[2].ssid, "Old")
        && model->wifi.networks[2].security == CP_WIFI_SECURITY_WPA);
    /* Later scans keep the order, add newcomers and follow the signal. */
    {
        unsigned revision = model->wifi.revision;
        fake.networks[3].bars = 1;
        fake.networks[0].bars = 1;
        AddNetwork("New", CP_WIFI_SECURITY_WEP, 2);
        Frames(40);
        assert(model->wifi.count == 4 && !strcmp(model->wifi.networks[3].ssid, "New"));
        assert(model->wifi.networks[0].bars == 1 && model->wifi.revision != revision);
        revision = model->wifi.revision;
        Frames(40);
        assert(model->wifi.revision == revision);
    }
    /* No more rows than the list holds. */
    for (i = 0; fake.networkCount < sizeof(fake.networks) / sizeof(*fake.networks); ++i) {
        char name[16];
        snprintf(name, sizeof(name), "Extra %u", i);
        AddNetwork(name, CP_WIFI_SECURITY_WPA2, 1);
    }
    Frames(40);
    assert(model->wifi.count == CP_UI_NETWORKS_MAX);
    /* Leaving without joining goes back to the saved connections. */
    i = fake.slotsStarted;
    model->ops->wifiLeave(model->context);
    assert(fake.slotsStarted == i + 1 && model->wifi.state == CP_UI_NETWORK_IDLE);
    fake.wifiConnected = 1;
    Frames(2);
    puts("PASS model: the Wi-Fi list shows each named network once, in a stable order");
}

static void WifiJoinStoresAndForgets(void)
{
    char cfg[2048];
    const char *pass = "correct horse 9";
    memset(&server, 0, sizeof(server));
    fake.networkCount = 0;
    fake.wifiConnected = 0;
    fake.logUsed = 0;
    fake.log[0] = 0;
    AddNetwork("Home", CP_WIFI_SECURITY_WPA2, 3);
    AddNetwork("Guest", CP_WIFI_SECURITY_OPEN, 2);
    snprintf(fake.goodSsid, sizeof(fake.goodSsid), "Home");
    memcpy(fake.goodKey, pass, strlen(pass));
    fake.goodKeyLength = strlen(pass);
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    model->ops->wifiScan(model->context);
    Frames(1);

    /* A wrong password: a clear failure, nothing stored. */
    JoinListed("Home", CP_WIFI_SECURITY_WPA2, "wrong password");
    assert(model->wifi.state == CP_UI_NETWORK_FAILED && model->wifi.failure == CP_WIFI_FAILURE_REJECTED
        && model->wifi.code == CP_APP_ERROR_WIFI_REJECTED);
    assert(fake.joinedKeyLength == 14 && !memcmp(fake.joinedKey, "wrong password", 14));
    assert(!CfgExists("") && !model->wifi.saved);
    /* A network out of range, and one that gives no address. */
    JoinListed("Gone", CP_WIFI_SECURITY_WPA2, "whatever-pass");
    assert(model->wifi.state == CP_UI_NETWORK_FAILED && model->wifi.code == CP_APP_ERROR_WIFI_NOT_FOUND);
    fake.noAddress = 1;
    JoinListed("Home", CP_WIFI_SECURITY_WPA2, pass);
    assert(model->wifi.state == CP_UI_NETWORK_FAILED && model->wifi.code == CP_APP_ERROR_WIFI_NO_ADDRESS);
    fake.noAddress = 0;
    /* A key that does not suit the security never reaches the driver. */
    {
        unsigned joins = fake.joinsStarted;
        model->ops->wifiJoin(model->context, "Home", CP_WIFI_SECURITY_WPA2, "short");
        assert(model->wifi.state == CP_UI_NETWORK_FAILED && fake.joinsStarted == joins);
    }

    /* The right one: connected and stored, through a .tmp that is gone. */
    JoinListed("Home", CP_WIFI_SECURITY_WPA2, pass);
    assert(model->wifi.state == CP_UI_NETWORK_JOINED && !model->wifi.saveFailed);
    assert(model->wifi.saved && !strcmp(model->wifi.savedSsid, "Home"));
    assert(CfgExists("") && !CfgExists(".tmp"));
    ReadCfg(cfg, sizeof(cfg));
    assert(strstr(cfg, "ssid=Home\n") && strstr(cfg, "key=correct horse 9\n") && strstr(cfg, "security=wpa2\n"));
    /* Connecting now finds Wi-Fi ready. */
    model->ops->wifiLeave(model->context);
    Connect();
    model->ops->close(model->context);

    /* The next launch joins the stored network first, before the console's
     * saved connections. */
    {
        unsigned slots = fake.slotsStarted, joins = fake.joinsStarted;
        fake.wifiConnected = 0;
        memset(fake.joinedKey, 0, sizeof(fake.joinedKey));
        CpApp_Init(&app, &platform, Companion_Language_en_US);
        assert(model->wifi.saved && !strcmp(model->wifi.savedSsid, "Home"));
        CpApp_StartWifi(&app);
        assert(fake.joinsStarted == joins + 1 && fake.slotsStarted == slots);
        assert(!strcmp(fake.joinedSsid, "Home") && fake.joinedKeyLength == strlen(pass)
            && !memcmp(fake.joinedKey, pass, strlen(pass)));
        Frames(4);
        assert(fake.wifiConnected && fake.slotsStarted == slots);
        /* connect finds it joined. */
        Connect();
        model->ops->close(model->context);
    }

    /* Out of range next time: the console's saved connections follow. */
    {
        unsigned slots = fake.slotsStarted;
        fake.wifiConnected = 0;
        fake.networkCount = 1;
        snprintf(fake.networks[0].ssid, sizeof(fake.networks[0].ssid), "Elsewhere");
        CpApp_Init(&app, &platform, Companion_Language_en_US);
        model->ops->connect(model->context);
        assert(model->net == CP_UI_NET_BUSY);
        Pump();
        assert(fake.slotsStarted == slots + 1 && model->net == CP_UI_NET_READY);
        assert(strstr(fake.log, "the stored network failed (code -25)"));
        model->ops->close(model->context);
        /* And when those fail too, the screens are told to offer the settings. */
        fake.wifiConnected = 0;
        fake.wifiFails = 1;
        model->ops->connect(model->context);
        Pump();
        assert(model->net == CP_UI_NET_ERROR && model->error == CP_APP_ERROR_WIFI && model->wifiError);
        model->ops->close(model->context);
        model->ops->unload(model->context);
        fake.wifiFails = 0;
    }

    /* Forget deletes the file; the next launch uses the saved connections only. */
    model->ops->wifiForget(model->context);
    assert(!model->wifi.saved && !model->wifi.savedSsid[0] && !CfgExists("") && !CfgExists(".tmp"));
    {
        unsigned joins = fake.joinsStarted, slots = fake.slotsStarted;
        fake.wifiConnected = 0;
        CpApp_Init(&app, &platform, Companion_Language_en_US);
        assert(!model->wifi.saved);
        CpApp_StartWifi(&app);
        assert(fake.joinsStarted == joins && fake.slotsStarted == slots + 1);
        Frames(4);
    }

    /* An open network stores no key. */
    fake.networkCount = 0;
    AddNetwork("Guest", CP_WIFI_SECURITY_OPEN, 2);
    snprintf(fake.goodSsid, sizeof(fake.goodSsid), "Guest");
    fake.goodKeyLength = 0;
    model->ops->wifiScan(model->context);
    Frames(1);
    JoinListed("Guest", CP_WIFI_SECURITY_OPEN, "");
    assert(model->wifi.state == CP_UI_NETWORK_JOINED && fake.joinedKeyLength == 0);
    ReadCfg(cfg, sizeof(cfg));
    assert(strstr(cfg, "security=open\n") && strstr(cfg, "key=\n"));

    /* A WEP key typed as hex digits reaches the driver as bytes. */
    fake.networkCount = 0;
    AddNetwork("Retro", CP_WIFI_SECURITY_WEP, 2);
    snprintf(fake.goodSsid, sizeof(fake.goodSsid), "Retro");
    memcpy(fake.goodKey, "\x01\x23\x45\x67\x89", 5);
    fake.goodKeyLength = 5;
    model->ops->wifiScan(model->context);
    Frames(1);
    JoinListed("Retro", CP_WIFI_SECURITY_WEP, "0123456789");
    assert(model->wifi.state == CP_UI_NETWORK_JOINED);
    ReadCfg(cfg, sizeof(cfg));
    assert(strstr(cfg, "key=0123456789\n"));

    /* The log names no network and no password. */
    assert(fake.logUsed > 0);
    assert(!strstr(fake.log, pass) && !strstr(fake.log, "wrong password") && !strstr(fake.log, "0123456789"));
    assert(!strstr(fake.log, "Home") && !strstr(fake.log, "Retro"));
    model->ops->wifiForget(model->context);
    fake.wifiConnected = 1;
    puts("PASS model: a joined network is stored, joined first next launch, and forgotten");
}

/* A damaged wifi.cfg is ignored; one left as .tmp by a power cut is used;
 * no card means nothing is stored, but joining still works. */
static void WifiConfigOnTheCard(void)
{
    char path[400];
    FILE *file;
    snprintf(path, sizeof(path), "%s_nds/dswt/wifi.cfg", fake.root);
    file = fopen(path, "wb");
    assert(file);
    fputs("version=1\nsecurity=wpa2\nssid=Home\nkey=x\n", file);
    fclose(file);
    fake.logUsed = 0;
    fake.log[0] = 0;
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    assert(!model->wifi.saved && strstr(fake.log, "ignoring an unreadable wifi.cfg"));
    remove(path);
    snprintf(path, sizeof(path), "%s_nds/dswt/wifi.cfg.tmp", fake.root);
    file = fopen(path, "wb");
    assert(file);
    fputs("version=1\nsecurity=wpa2\nssid=Home\nkey=12345678\n", file);
    fclose(file);
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    assert(model->wifi.saved && !strcmp(model->wifi.savedSsid, "Home"));
    model->ops->wifiForget(model->context);
    assert(!CfgExists(".tmp"));

    {
        CpAppPlatform noCard = platform;
        noCard.root = "";
        fake.networkCount = 0;
        AddNetwork("Guest", CP_WIFI_SECURITY_OPEN, 2);
        snprintf(fake.goodSsid, sizeof(fake.goodSsid), "Guest");
        fake.goodKeyLength = 0;
        CpApp_Init(&app, &noCard, Companion_Language_en_US);
        model->ops->wifiScan(model->context);
        Frames(1);
        JoinListed("Guest", CP_WIFI_SECURITY_OPEN, "");
        assert(model->wifi.state == CP_UI_NETWORK_JOINED && model->wifi.saveFailed && !model->wifi.saved);
    }
    /* Without the platform's settings, the screens are offered none. */
    {
        CpAppPlatform plain = platform;
        plain.wifiScan = NULL;
        CpApp_Init(&app, &plain, Companion_Language_en_US);
        assert(!model->ops->wifiScan);
    }
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    fake.wifiConnected = 1;
    puts("PASS model: wifi.cfg is read defensively, and joining works without a card");
}

/* A card that refuses to delete wifi.cfg: the network stays stored and
 * shown, with a support code, until a forget succeeds. */
static void WifiForgetFailureKeepsTheNetwork(void)
{
    char folder[400];
    CpWifiConfig stored;
    memset(&stored, 0, sizeof(stored));
    stored.security = CP_WIFI_SECURITY_WPA2;
    snprintf(stored.ssid, sizeof(stored.ssid), "Home");
    snprintf(stored.key, sizeof(stored.key), "correct horse 9");
    snprintf(folder, sizeof(folder), "%s_nds/dswt", fake.root);
    {
        char path[420];
        snprintf(path, sizeof(path), "%s/wifi.cfg", folder);
        assert(CpWifiConfig_Save(path, &stored) == 0);
    }
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    assert(model->wifi.saved);
    assert(chmod(folder, 0500) == 0);
    model->ops->wifiForget(model->context);
    assert(model->wifi.forgetFailed && model->wifi.code == CP_APP_ERROR_WIFI_FORGET);
    assert(model->wifi.saved && !strcmp(model->wifi.savedSsid, "Home") && CfgExists(""));
    assert(chmod(folder, 0700) == 0);
    model->ops->wifiForget(model->context);
    assert(!model->wifi.forgetFailed && !model->wifi.saved && !CfgExists(""));
    puts("PASS model: a wifi.cfg the card will not delete stays stored and is reported");
}

/* Listing networks takes the radio off whatever network it was on, so
 * leaving the settings without joining always reconnects, even when the
 * driver still reports the old association; connect never trusts it. */
static void WifiSettingsDropTheConnection(void)
{
    unsigned slots;
    memset(&server, 0, sizeof(server));
    fake.networkCount = 0;
    AddNetwork("Home", CP_WIFI_SECURITY_WPA2, 3);

    /* Opened while connected. */
    fake.wifiConnected = 1;
    fake.staleAfterScan = 1;
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    slots = fake.slotsStarted;
    model->ops->wifiScan(model->context);
    Frames(2);
    model->ops->wifiLeave(model->context);
    assert(fake.slotsStarted == slots + 1);
    Frames(4);

    /* Opened, then a connect before any attempt finished: it waits for a
     * real attempt instead of reporting the stale association as ready. */
    model->ops->wifiScan(model->context);
    Frames(2);
    model->ops->wifiLeave(model->context);
    slots = fake.slotsStarted;
    fake.wifiPolls = 3;
    model->ops->connect(model->context);
    assert(model->net == CP_UI_NET_BUSY);
    Pump();
    assert(model->net == CP_UI_NET_READY && fake.slotsStarted == slots);
    model->ops->close(model->context);

    /* Opened while the boot's attempt was still under way. */
    fake.staleAfterScan = 0;
    fake.wifiConnected = 0;
    CpApp_Init(&app, &platform, Companion_Language_en_US);
    CpApp_StartWifi(&app);
    slots = fake.slotsStarted;
    model->ops->wifiScan(model->context);
    Frames(2);
    model->ops->wifiLeave(model->context);
    assert(fake.slotsStarted == slots + 1);
    Frames(4);
    assert(fake.wifiConnected);
    puts("PASS model: leaving the Wi-Fi settings reconnects, whatever the driver last reported");
}

int main(void)
{
    Setup();
    ScanFindsSavesAndTheirRoms();
    LoadRefusals();
    OfferStatusCancelCollect();
    BlackOfferAndReturn();
    SupportCodesMatchTheGame();
    ReloadEnrolledSave();
    MatchedDeliveries();
    RestoredRetailNeedsRecovery();
    BackupFailureStopsBeforeAnyEdit();
    NeedsEntropyAndWifiFailure();
    ScreensDriveTheModel();
    SameNameSavesKeepSeparateBackups();
    UnprovenWritesAndTheLeaveFence();
    ReturnAlreadyPresentFailsClosed();
    UnsentOfferTrainedInStockGame();
    BackupStems();
    JapaneseSaveBacksUp();
    WifiScanListsNetworks();
    WifiJoinStoresAndForgets();
    WifiConfigOnTheCard();
    WifiSettingsDropTheConnection();
    WifiForgetFailureKeepsTheNetwork();
    assert(fake.logLines > 0);
    puts("PASS session model");
    return 0;
}
