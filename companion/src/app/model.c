#include "app/model.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "adapter/holdable.h"
#include "platform/wifi_status.h"
#include "save/journal_save.h"
#include "wonder_trade/boot.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"

enum { JOB_NONE, JOB_SCAN, JOB_LOAD, JOB_VISIT };

/* Offsets in a canonical (decrypted, unshuffled) Generation IV Pokémon. */
enum {
    PK_PID = 0x00, PK_SPECIES = 0x08, PK_ITEM = 0x0A, PK_OT_ID = 0x0C, PK_IVS = 0x38,
    PK_FLAGS = 0x40, PK_NICKNAME = 0x48, PK_EXTENSION_SIZE = 100,
    EXT_LEVEL = 4, EXT_CAPSULE = 5, EXT_HP = 6
};

/* The item name bank of the message archive (TEXT_BANK_ITEM_NAMES); the
 * Japanese and Korean archives leave out two banks before it. */
#define ITEM_NAME_BANK 392
#define ITEM_NAME_BANK_ASIAN 390

CpSaveImage gCpAppImage;

/* The engine's random source has no argument of its own. */
static CpApp *sApp;

static void Log(CpApp *app, const char *format, ...)
{
    char line[160];
    va_list arguments;
    if (!app->platform->log) return;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    app->platform->log(app->platform->arg, line);
}

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }

static const char *Basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int SameFolded(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int EndsWithSav(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && name[length - 4] == '.' && tolower((unsigned char)name[length - 3]) == 's'
        && tolower((unsigned char)name[length - 2]) == 'a' && tolower((unsigned char)name[length - 1]) == 'v';
}

/* ---- The save file as save/file.h's CpFile ---------------------------------- */

/* Every read of the save goes to what the card holds (readDurable: raw
 * sectors on the DS), never a cache: CpSaveImage_Verify after a retail write,
 * the adapter's checks and the backup's source must see the card itself.
 * Raw reads are slow per call, so whole windows are read at once and later
 * reads served from the window until the next write or sync, which cannot
 * change the card in between. */
enum { WINDOW_SIZE = 0x8000 };
static struct {
    unsigned char bytes[WINDOW_SIZE];
    uint32_t start;
    int valid;
} sWindow;

static void Invalidate(void) { sWindow.valid = 0; }

static int FileRead(void *context, uint32_t offset, void *data, size_t size)
{
    const CpPlatformFile *file = &((CpApp *)context)->platformFile;
    unsigned char *out = data;
    if (!file->readDurable) return file->readAt(file->arg, offset, data, size) == 0 ? 0 : -1;
    if (offset > CP_SAVE_SIZE || size > CP_SAVE_SIZE - offset) return -1;
    while (size) {
        uint32_t start = offset - offset % WINDOW_SIZE, within = offset - start;
        size_t count = WINDOW_SIZE - within < size ? WINDOW_SIZE - within : size;
        if (!sWindow.valid || sWindow.start != start) {
            Invalidate();
            if (file->readDurable(file->arg, start, sWindow.bytes, WINDOW_SIZE) != 0) return -1;
            sWindow.start = start;
            sWindow.valid = 1;
        }
        memcpy(out, sWindow.bytes + within, count);
        out += count; offset += (uint32_t)count; size -= count;
    }
    return 0;
}

static int FileWrite(void *context, uint32_t offset, const void *data, size_t size)
{
    const CpPlatformFile *file = &((CpApp *)context)->platformFile;
    Invalidate();
    return file->writeAt && file->writeAt(file->arg, offset, data, size) == 0 ? 0 : -1;
}

static int FileSync(void *context)
{
    const CpPlatformFile *file = &((CpApp *)context)->platformFile;
    Invalidate();
    return file->sync && file->sync(file->arg) == 0 ? 0 : -1;
}

static int FileSize(void *context, uint64_t *size)
{
    const CpPlatformFile *file = &((CpApp *)context)->platformFile;
    uint32_t length = 0;
    if (file->size(file->arg, &length) != 0) return -1;
    *size = length;
    return 0;
}

/* The journal writes the same file: its writes and syncs drop the window too. */
static int JournalWrite(void *arg, uint32_t offset, const void *data, size_t size)
{
    (void)arg;
    Invalidate();
    return sApp->platformFile.writeAt(sApp->platformFile.arg, offset, data, size);
}

static int JournalSync(void *arg)
{
    (void)arg;
    Invalidate();
    return sApp->platformFile.sync(sApp->platformFile.arg);
}

static WtJournalIO JournalIO(CpApp *app)
{
    app->journalFile = app->platformFile;
    if (app->platformFile.writeAt) app->journalFile.writeAt = JournalWrite;
    if (app->platformFile.sync) app->journalFile.sync = JournalSync;
    app->store.file = &app->journalFile;
    app->store.hold = app->platform->hold;
    app->store.error = 0;
    return CpJournal_IO(&app->store, CpSaveJournal_Hash);
}

/* ---- Scan ------------------------------------------------------------------- */

typedef struct ScanState {
    CpApp *app;
    const char *directory;
    char subdirectories[CP_UI_SAVES_MAX][CP_PATH_MAX];
    unsigned subdirectoryCount;
} ScanState;

static ScanState sScan;

/* out = a + b + c; zero when it does not fit. */
static int Join(char out[CP_PATH_MAX], const char *a, const char *b, const char *c)
{
    size_t lengthA = strlen(a), lengthB = strlen(b), lengthC = strlen(c);
    if (lengthA + lengthB + lengthC >= CP_PATH_MAX) return 0;
    memcpy(out, a, lengthA);
    memcpy(out + lengthA, b, lengthB);
    memcpy(out + lengthA + lengthB, c, lengthC + 1);
    return 1;
}

static void AddSave(CpApp *app, const char *path)
{
    char candidates[CP_PAIR_CANDIDATES][CP_PATH_MAX];
    unsigned index = app->result.saveCount;
    CpAppSave *save = &app->saves[index];
    CpUiSave *shown = &app->result.saves[index];
    CpReader reader;
    size_t count, i;
    int opened = 0;
    if (index >= CP_UI_SAVES_MAX || strlen(path) >= sizeof(save->path)) return;
    memset(save, 0, sizeof(*save));
    memset(shown, 0, sizeof(*shown));
    strcpy(save->path, path);
    {
        const char *base = Basename(path);
        size_t length = strlen(base);
        if (length >= sizeof(shown->name)) length = sizeof(shown->name) - 1;
        memcpy(shown->name, base, length);
        shown->name[length] = 0;
    }
    shown->status = CP_UI_SAVE_NO_ROM;
    count = CpPair_RomCandidates(path, candidates);
    for (i = 0; i < count && !opened; ++i) {
        if (app->platform->openRom(app->platform->arg, CP_APP_ROM_SCAN, candidates[i], &reader) != 0) continue;
        opened = 1;
        strcpy(save->rom, candidates[i]);
        if (CpRom_Identify(&reader, &save->info) == CP_OK) {
            shown->version = save->info.version;
            shown->language = save->info.language;
            shown->status = save->info.family == CP_FAMILY_PLATINUM ? CP_UI_SAVE_READY : CP_UI_SAVE_UNSUPPORTED;
        } else {
            shown->status = CP_UI_SAVE_UNSUPPORTED;
        }
        app->platform->closeRom(app->platform->arg, CP_APP_ROM_SCAN);
    }
    Log(app, "scan: %s rom=%s status=%u\n", path, opened ? save->rom : "-", shown->status);
    app->result.saveCount = index + 1;
}

static void ScanEntry(void *context, const char *name, int directory, uint32_t size)
{
    ScanState *scan = context;
    char path[CP_PATH_MAX];
    if (name[0] == '.' || !Join(path, scan->directory, name, directory ? "/" : "")) return;
    if (directory) {
        if (scan->subdirectoryCount < CP_UI_SAVES_MAX && !SameFolded(name, "saves"))
            strcpy(scan->subdirectories[scan->subdirectoryCount++], path);
    } else if (EndsWithSav(name) && size == CP_SAVE_SIZE) {
        AddSave(scan->app, path);
    }
}

static void ScanDirectory(ScanState *scan, const char *directory)
{
    scan->directory = directory;
    (void)scan->app->platform->list(scan->app->platform->arg, directory, ScanEntry, scan);
}

/* TWiLight Menu++ keeps X.sav in roms/nds/saves/ beside roms/nds/X.nds, or
 * next to the ROM; the companion also looks in its own _nds/dswt/. ROMs one
 * folder down (roms/nds/<folder>/) are searched the same way. */
static int ScanJob(void *arg)
{
    ScanState *scan = &sScan;
    CpApp *app = arg;
    char roms[CP_PATH_MAX], path[CP_PATH_MAX], own[CP_PATH_MAX];
    unsigned i, subdirectories;
    memset(scan, 0, sizeof(*scan));
    scan->app = app;
    app->result.saveCount = 0;
    if (!Join(roms, app->platform->root, "roms/nds/", "")) return -1;
    if (Join(path, roms, "saves/", "")) ScanDirectory(scan, path);
    scan->subdirectoryCount = 0;
    ScanDirectory(scan, roms);
    subdirectories = scan->subdirectoryCount;
    for (i = 0; i < subdirectories; ++i) {
        char folder[CP_PATH_MAX];
        strcpy(folder, scan->subdirectories[i]);
        if (Join(path, folder, "saves/", "")) ScanDirectory(scan, path);
        ScanDirectory(scan, folder);
        scan->subdirectoryCount = subdirectories;
    }
    if (Join(own, app->platform->root, "_nds/dswt/", "")) ScanDirectory(scan, own);
    Log(app, "scan: %u saves\n", app->result.saveCount);
    return 0;
}

/* ---- The party, as the screens show it ----------------------------------- */

static void Crypt(unsigned char *bytes, size_t size, uint32_t seed)
{
    size_t i;
    for (i = 0; i + 1 < size; i += 2) {
        seed = seed * UINT32_C(1103515245) + 24691;
        bytes[i] ^= (unsigned char)(seed >> 16);
        bytes[i + 1] ^= (unsigned char)(seed >> 24);
    }
}

static void Empty(uint16_t *text) { text[0] = CP_CHAR_EOS; }

static void ItemName(const CpApp *app, unsigned item, uint16_t out[CP_UI_ITEM_NAME_MAX])
{
    int asian = app->game.rom.language == CP_LANGUAGE_JAPANESE || app->game.rom.language == CP_LANGUAGE_KOREAN;
    CpRomFile bank;
    Empty(out);
    if (!item) return;
    /* A name too long for the card leaves the held-item mark alone. */
    if (CpNarc_Member(&app->game.messages, asian ? ITEM_NAME_BANK_ASIAN : ITEM_NAME_BANK, &bank) != CP_OK
        || CpMessage_Read(&bank, (uint16_t)item, out, CP_UI_ITEM_NAME_MAX, NULL) != CP_OK) Empty(out);
}

/* Why the adapter refuses a slot, in the order the game's checks run. */
static unsigned Reason(const unsigned char *party, unsigned count, unsigned slot, const unsigned char canonical[WT_POKEMON_SIZE],
    const unsigned char extension[PK_EXTENSION_SIZE])
{
    unsigned item = Read16(canonical + PK_ITEM), usable = 0, i;
    if ((Read32(canonical + PK_IVS) >> 30) & 1) return CP_UI_INELIGIBLE_EGG;
    if (item >= CP_ADAPTER_FIRST_MAIL && item <= CP_ADAPTER_LAST_MAIL) return CP_UI_INELIGIBLE_MAIL;
    if (extension[EXT_CAPSULE]) return CP_UI_INELIGIBLE_CAPSULE;
    for (i = 0; i < count; ++i) {
        const unsigned char *image = party + 8 + i * CP_SAVE_PARTY_STRIDE;
        unsigned char other[WT_POKEMON_SIZE], ext[PK_EXTENSION_SIZE];
        if (i == slot || WtPokemon_Decode(image, other) != 0) continue;
        memcpy(ext, image + WT_POKEMON_SIZE, sizeof(ext));
        Crypt(ext, sizeof(ext), Read32(image));
        if (!((Read32(other + PK_IVS) >> 30) & 1) && Read16(ext + EXT_HP)) ++usable;
    }
    return usable ? CP_UI_INELIGIBLE_OTHER : CP_UI_INELIGIBLE_LAST_USABLE;
}

static void CopyName(uint16_t out[CP_UI_NAME_MAX], const unsigned char *name)
{
    unsigned i;
    for (i = 0; i + 1 < CP_UI_NAME_MAX; ++i) {
        out[i] = (uint16_t)Read16(name + 2 * i);
        if (out[i] == CP_CHAR_EOS) return;
    }
    out[i] = CP_CHAR_EOS;
}

static unsigned FillParty(CpApp *app, CpUiPokemon party[CP_UI_PARTY_MAX])
{
    unsigned char *general = gCpAppImage.loaded ? CpSaveImage_Block(&gCpAppImage, 0) : NULL;
    const unsigned char *list;
    unsigned count, slot;
    memset(party, 0, CP_UI_PARTY_MAX * sizeof(*party));
    if (!general) return 0;
    list = general + CP_SAVE_PARTY_CAPACITY_OFFSET;
    count = Read32(list + 4);
    if (Read32(list) != CP_SAVE_PARTY_CAPACITY || count > CP_SAVE_PARTY_CAPACITY) return 0;
    for (slot = 0; slot < count; ++slot) {
        const unsigned char *image = list + 8 + slot * CP_SAVE_PARTY_STRIDE;
        unsigned char canonical[WT_POKEMON_SIZE], extension[PK_EXTENSION_SIZE];
        CpUiPokemon *mon = &party[slot];
        uint32_t pid, ot;
        Empty(mon->speciesName); Empty(mon->nickname); Empty(mon->itemName);
        if (WtPokemon_Decode(image, canonical) != 0) { mon->ineligible = CP_UI_INELIGIBLE_OTHER; continue; }
        memcpy(extension, image + WT_POKEMON_SIZE, sizeof(extension));
        Crypt(extension, sizeof(extension), Read32(image));
        pid = Read32(canonical + PK_PID);
        ot = Read32(canonical + PK_OT_ID);
        mon->species = (uint16_t)Read16(canonical + PK_SPECIES);
        mon->heldItem = (uint16_t)Read16(canonical + PK_ITEM);
        mon->level = extension[EXT_LEVEL];
        mon->isEgg = (Read32(canonical + PK_IVS) >> 30) & 1;
        mon->gender = (canonical[PK_FLAGS] & 4) ? CP_UI_GENDERLESS : (canonical[PK_FLAGS] & 2) ? CP_UI_FEMALE : CP_UI_MALE;
        mon->shiny = ((ot & 0xFFFF) ^ (ot >> 16) ^ (pid & 0xFFFF) ^ (pid >> 16)) < 8;
        CopyName(mon->nickname, canonical + PK_NICKNAME);
        if (mon->isEgg || CpGame_SpeciesName(&app->game, mon->species, mon->speciesName, CP_UI_NAME_MAX, NULL) != CP_OK)
            memcpy(mon->speciesName, mon->nickname, sizeof(mon->speciesName));
        ItemName(app, mon->heldItem, mon->itemName);
        if (!CpAdapter_Eligible(&gCpAppImage, slot)) mon->ineligible = (uint8_t)Reason(list, count, slot, canonical, extension);
        else mon->ineligible = CpAdapter_Unique(&gCpAppImage, slot) ? CP_UI_ELIGIBLE : CP_UI_INELIGIBLE_DUPLICATE;
        memset(canonical, 0, sizeof(canonical));
    }
    return count;
}

/* ---- Load ----------------------------------------------------------------- */

static int Enrolled(unsigned phase)
{
    return phase != WT_JOURNAL_EMPTY && phase != WT_JOURNAL_ENROLLING;
}

static int ReadPhase(CpApp *app, unsigned *phase, unsigned char saveID[16])
{
    WtJournal journal;
    WtJournalEntry entry;
    int result = WtJournal_Open(&journal, JournalIO(app));
    if (result == 0) result = WtJournal_Read(&journal, &entry);
    if (result == 0) { *phase = entry.phase; memcpy(saveID, entry.saveID, 16); }
    WtJournal_Close(&journal);
    memset(&entry, 0, sizeof(entry));
    return result;
}

/* Backups live in <root>/<stem>-<id>/, where <id> is the first four bytes of
 * the enrolled journal's saveID in hex. The saveID travels with the save: a
 * moved or renamed file keeps its backups, two saves with the same file name
 * in different folders never share a folder, and a restored older copy that
 * enrolls again gets a folder of its own rather than a conflicting one.
 * Backups made before this naming, in <root>/<stem>/, still count when they
 * hold this enrollment. */
size_t CpApp_BackupStem(const char *name, char out[CP_APP_BACKUP_STEM_MAX + 1])
{
    size_t length = strlen(name), i;
    if (EndsWithSav(name)) length -= 4;
    if (length > CP_APP_BACKUP_STEM_MAX) {
        /* Cut before a character, never inside one: back over continuation bytes. */
        length = CP_APP_BACKUP_STEM_MAX;
        while (length > 0 && ((unsigned char)name[length] & 0xC0) == 0x80) --length;
    }
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        out[i] = c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", c) ? '_' : (char)c;
    }
    /* FAT drops trailing dots and spaces from a name. */
    while (length > 0 && (out[length - 1] == '.' || out[length - 1] == ' ')) --length;
    if (!length) { memcpy(out, "save", 4); length = 4; }
    out[length] = 0;
    return length;
}

static void BackupName(const CpApp *app, const unsigned char saveID[16], char out[64])
{
    static const char hex[] = "0123456789abcdef";
    size_t length = CpApp_BackupStem(Basename(app->saves[app->index].path), out), i;
    out[length++] = '-';
    for (i = 0; i < 4; ++i) { out[length++] = hex[saveID[i] >> 4]; out[length++] = hex[saveID[i] & 15]; }
    memcpy(out + length, ".sav", 5);
}

/* The backup contract: a backup taken after enrollment exists before the
 * first edit. One that holds this enrollment (its saveID) already counts;
 * otherwise one is made now. Any failure stops. */
static int Backup(CpApp *app, const unsigned char saveID[16])
{
    const char *legacy = Basename(app->saves[app->index].path);
    char name[64];
    CpBackupResult result;
    int existing;
    if (app->backedUp) return 0;
    BackupName(app, saveID, name);
    existing = CpBackup_HasIdentity(&app->platform->backup, name, saveID);
    if (existing == 0) existing = CpBackup_HasIdentity(&app->platform->backup, legacy, saveID);
    if (existing == 1) {
        Log(app, "backup: one of this enrollment exists\n");
        app->backedUp = 1;
        return 0;
    }
    if (existing < 0) { Log(app, "backup: cannot read the backups\n"); return -1; }
    result = CpBackup_Create(&app->platform->backup, &app->file, name, gCpAppImage.globalCounter);
    Log(app, "backup: counter %lu result %d\n", (unsigned long)gCpAppImage.globalCounter, (int)result);
    if (result != CP_BACKUP_OK) return -1;
    app->backedUp = 1;
    return 0;
}

static int Reload(CpApp *app)
{
    uint32_t magic = app->game.rom.language == CP_LANGUAGE_KOREAN ? CP_SAVE_MAGIC_KOREAN : CP_SAVE_MAGIC_INTERNATIONAL;
    return CpSaveImage_Load(&gCpAppImage, &app->file, magic);
}

static int LoadJob(void *arg)
{
    CpApp *app = arg;
    const CpAppPlatform *platform = app->platform;
    CpAppSave *save = &app->saves[app->index];
    CpSaveResult loaded;
    CpSaveJournalDetection detection;
    unsigned phase = WT_JOURNAL_EMPTY;
    unsigned char saveID[16];
    int boot;
    app->result.loadMessage = Companion_Text_SaveError;
    app->result.recovery = 0;
    if (platform->openRom(platform->arg, CP_APP_ROM_GAME, save->rom, &app->romReader) != 0) {
        Log(app, "load: cannot open %s\n", save->rom);
        app->result.loadMessage = Companion_Text_MatchingRomMissing;
        return -1;
    }
    app->romOpen = 1;
    if (CpGame_Open(&app->romReader, &app->game) != CP_OK) {
        Log(app, "load: %s is not a game the companion reads\n", save->rom);
        app->result.loadMessage = Companion_Text_SaveUnsupported;
        return -1;
    }
    app->hasFont = CpGame_Font(&app->game, CP_FONT_SYSTEM, &app->font) == CP_OK;
    if (platform->openSave(platform->arg, save->path, &app->platformFile) != 0) {
        Log(app, "load: cannot open %s\n", save->path);
        return -1;
    }
    app->saveOpen = 1;
    loaded = Reload(app);
    Log(app, "load: %s image %d\n", save->path, (int)loaded);
    /* A file the game never saved to, or one it would not load either. */
    if (loaded == CP_SAVE_NEEDS_GAME_SAVE || loaded == CP_SAVE_INVALID) app->result.loadMessage = Companion_Text_SaveInGameFirst;
    if (loaded != CP_SAVE_OK) return -1;
    app->adapter.image = &gCpAppImage;
    app->adapter.file = &app->file;
    app->adapter.holdable = CpAdapter_RomHoldable;
    app->adapter.holdableArg = &app->game;
    /* Matched deliveries take the game's receive effects from the paired ROM. */
    CpAdapter_RomData(&app->romData, &app->game);
    app->adapter.data = &app->romData;
    detection = CpSaveJournal_Detect(&app->file);
    Log(app, "load: journal %s\n", detection == CP_SAVE_JOURNAL_HAS_JOURNAL ? "present"
        : detection == CP_SAVE_JOURNAL_UNUSED ? "unused" : "refused");
    if (detection == CP_SAVE_JOURNAL_REFUSE) return -1;
    app->needsProvision = detection == CP_SAVE_JOURNAL_UNUSED;
    if (detection == CP_SAVE_JOURNAL_HAS_JOURNAL) {
        if (ReadPhase(app, &phase, saveID) != 0) { Log(app, "load: journal read failed\n"); return -1; }
        /* The boot check may finish a trade's local step in the save: back it up first. */
        if (Enrolled(phase) && Backup(app, saveID) != 0) { app->result.loadMessage = Companion_Text_BackupFailed; return -1; }
        boot = WtBoot_Check(&app->adapter, JournalIO(app));
        app->result.recovery = boot != 0;
        if (!gCpAppImage.loaded && Reload(app) != CP_SAVE_OK) return -1;
        if (ReadPhase(app, &phase, saveID) != 0) app->result.recovery = 1;
        Log(app, "load: boot check %d phase %u\n", boot, phase);
    }
    app->result.phase = phase;
    app->result.partyCount = FillParty(app, app->result.party);
    app->result.loadMessage = -1;
    return 0;
}

/* ---- Visits ---------------------------------------------------------------- */

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    (void)arg;
    return sApp && sApp->platform->random(sApp->platform->arg, bytes, size) == 0 ? 0 : -1;
}

/* The received Pokémon's card, from what the visit read before its ACK. */
static void FillReceived(CpApp *app)
{
    const WtReceivedPokemon *from = &app->visit.received;
    CpUiReceived *to = &app->result.received;
    unsigned i;
    memset(to, 0, sizeof(*to));
    app->result.hasReceived = app->visit.hasReceived;
    if (!app->visit.hasReceived) return;
    to->species = from->species;
    to->gender = from->gender > CP_UI_GENDERLESS ? CP_UI_GENDERLESS : from->gender;
    to->shiny = from->shiny;
    to->hasNickname = from->hasNickname != 0;
    to->originalTrainerGender = from->originalTrainerGender;
    for (i = 0; i + 1 < CP_UI_NAME_MAX && i < WT_GAME_NAME_LENGTH + 1; ++i) to->nickname[i] = from->nickname[i];
    to->nickname[CP_UI_NAME_MAX - 1] = CP_CHAR_EOS;
    for (i = 0; i + 1 < CP_UI_TRAINER_MAX && i < WT_GAME_TRAINER_NAME_LENGTH + 1; ++i)
        to->originalTrainer[i] = from->originalTrainer[i];
    to->originalTrainer[CP_UI_TRAINER_MAX - 1] = CP_CHAR_EOS;
    if (CpGame_SpeciesName(&app->game, to->species, to->speciesName, CP_UI_NAME_MAX, NULL) != CP_OK)
        memcpy(to->speciesName, to->nickname, sizeof(to->speciesName));
}

static int Provision(CpApp *app)
{
    unsigned char seed[32];
    WtJournal journal;
    CpSaveJournalDetection detection = CpSaveJournal_Detect(&app->file);
    int result;
    if (detection == CP_SAVE_JOURNAL_HAS_JOURNAL) { app->needsProvision = 0; return 0; }
    if (detection != CP_SAVE_JOURNAL_UNUSED) return CP_VISIT_ERROR_JOURNAL;
    result = app->platform->seed(app->platform->arg, seed);
    if (result == WT_ENTROPY_WAIT) { app->needsEntropy = 1; memset(seed, 0, sizeof(seed)); return 1; }
    if (result != 0) { memset(seed, 0, sizeof(seed)); return CP_APP_ERROR_ENTROPY; }
    memset(&journal, 0, sizeof(journal));
    result = CpSaveJournal_Provision(&app->file, &journal, JournalIO(app), seed);
    WtJournal_Close(&journal);
    memset(seed, 0, sizeof(seed));
    Log(app, "journal: provisioned %d\n", result);
    if (result != 0) return CP_VISIT_ERROR_JOURNAL;
    app->needsProvision = 0;
    return 0;
}

static void LogVisit(CpApp *app, const char *what, const CpVisit *visit)
{
    Log(app, "visit %s slot=%d action=%u: result=%d phase=%u done=%d ret=%d rej=%d wait=%d sup=%d ref=%d leave=%d\n",
        what, app->slot, app->action, visit->result, visit->phase, visit->completed, visit->returned, visit->rejected,
        visit->waiting, visit->superseded, visit->refused, visit->canLeave);
}

static int VisitJob(void *arg)
{
    CpApp *app = arg;
    WtEngine *engine = &app->engine;
    WtEngineIO io = { &app->adapter, Random, WtGame_Inventory, WtGame_CanHold };
    WtJournalIO journal = JournalIO(app);
    uint32_t build = app->saves[app->index].info.buildCode;
    int result, resumed = 0;
    memset(&app->visit, 0, sizeof(app->visit));
    app->result.hasReceived = 0;
    app->visitOpened = 0;
    app->visitCanLeave = 0;
    if (app->needsProvision) {
        result = Provision(app);
        if (result != 0) { app->visitCanLeave = 1; return result > 0 ? 0 : result; }
    }
    if (WtEngine_Open(engine, journal, io) != 0) {
        /* Only verified pre-enrollment data may leave, as in the game. */
        app->visitCanLeave = WtRecovery_Check(journal) == 0;
        WtEngine_Close(engine);
        Log(app, "visit: engine blocked\n");
        return CP_VISIT_ERROR_JOURNAL;
    }
    app->visitOpened = 1;
    if (!app->backedUp) {
        if (!Enrolled(engine->entry.phase)) {
            /* Enroll and reconcile first: a fresh enrollment has nothing to edit. */
            result = CpVisit_Run(engine, &app->adapter, build, -1, 0, &app->platform->transport, &app->visit);
            LogVisit(app, "enroll", &app->visit);
            resumed = 1;
            if (result != 0) goto done;
        }
        if (WtJournal_Read(&engine->journal, &engine->entry) != 0 || !Enrolled(engine->entry.phase)) {
            result = CP_VISIT_ERROR_JOURNAL; goto done;
        }
        if (Backup(app, engine->entry.saveID) != 0) {
            result = CP_APP_ERROR_BACKUP;
            app->visit.phase = engine->entry.phase;
            app->visit.canLeave = WtGame_CanLeave(&app->adapter, &engine->entry);
            goto done;
        }
    }
    if (!resumed || app->slot >= 0 || app->action) {
        result = CpVisit_Run(engine, &app->adapter, build, app->slot, app->action, &app->platform->transport, &app->visit);
        LogVisit(app, "run", &app->visit);
    }
    result = app->visit.result;
done:
    app->visitCanLeave = app->visit.canLeave;
    WtEngine_Close(engine);
    if (!gCpAppImage.loaded) {
        /* A failed write unloads the image: read what the card holds again. */
        if (Reload(app) != CP_SAVE_OK) { app->visitCanLeave = 0; if (result == 0) result = CP_APP_ERROR_SAVE; }
    }
    app->result.partyCount = FillParty(app, app->result.party);
    FillReceived(app);
    return result;
}

/* ---- Ops ------------------------------------------------------------------- */

static int Start(CpApp *app, int job, CpAppJob function, WtSeed *journal)
{
    if (app->job || app->platform->start(app->platform->arg, function, app, journal) != 0) return -1;
    app->job = job;
    return 0;
}

static void ClearOutcome(CpUiModel *model)
{
    model->completed = model->returned = model->rejected = model->waiting = 0;
    model->superseded = model->needsEntropy = model->refused = model->hasReceived = 0;
    model->refusal = 0;
    model->error = 0;
}

static void Scan(void *context)
{
    CpApp *app = context;
    app->model.busy = 1;
    if (Start(app, JOB_SCAN, ScanJob, NULL) != 0) {
        app->model.busy = 0;
        app->model.saveCount = 0;
    }
}

static void CloseFiles(CpApp *app)
{
    if (app->saveOpen) app->platform->closeSave(app->platform->arg);
    if (app->romOpen) app->platform->closeRom(app->platform->arg, CP_APP_ROM_GAME);
    app->saveOpen = app->romOpen = app->hasFont = 0;
    gCpAppImage.loaded = 0;
    Invalidate();
}

static void Unload(void *context)
{
    CpApp *app = context;
    CpUiModel *model = &app->model;
    if (app->job) return;
    CloseFiles(app);
    app->loaded = app->needsProvision = app->backedUp = app->leaveProven = 0;
    model->partyCount = 0;
    memset(model->party, 0, sizeof(model->party));
    model->loadMessage = -1;
    model->font = NULL;
    model->recovery = 0;
    model->gameVersion = model->gameLanguage = 0;
    model->tradePhase = WT_JOURNAL_IDLE;
    model->canLeave = 1;
    model->net = CP_UI_NET_CLOSED;
    ClearOutcome(model);
}

static void Load(void *context, unsigned index)
{
    CpApp *app = context;
    CpUiModel *model = &app->model;
    Unload(app);
    model->loadMessage = Companion_Text_SaveError;
    if (index >= model->saveCount) return;
    app->index = index;
    if (model->saves[index].status == CP_UI_SAVE_NO_ROM) { model->loadMessage = Companion_Text_MatchingRomMissing; return; }
    if (model->saves[index].status != CP_UI_SAVE_READY) { model->loadMessage = Companion_Text_SaveUnsupported; return; }
    model->busy = 1;
    if (Start(app, JOB_LOAD, LoadJob, NULL) != 0) model->busy = 0;
}

static void Connect(void *context)
{
    CpApp *app = context;
    CpUiModel *model = &app->model;
    const CpAppPlatform *platform = app->platform;
    ClearOutcome(model);
    app->closing = 0;
    /* Only what the last check proved: a reconnect never grants leave. */
    model->canLeave = app->leaveProven && !model->recovery;
    if (platform->wifiStatus(platform->arg) == CP_WIFI_CONNECTED) { model->net = CP_UI_NET_READY; return; }
    model->net = CP_UI_NET_BUSY;
    if (platform->wifiConnect(platform->arg) != 0) {
        Log(app, "wifi: cannot start\n");
        model->net = CP_UI_NET_ERROR;
        model->error = CP_APP_ERROR_WIFI_OFF;
        return;
    }
    Log(app, "wifi: connecting with the console's saved connections\n");
    app->connecting = 1;
}

static void Visit(CpApp *app, int slot, unsigned action)
{
    CpUiModel *model = &app->model;
    ClearOutcome(model);
    app->needsEntropy = 0;
    app->slot = slot;
    app->action = action;
    /* As the game's network: nothing may leave until the visit proves it. */
    model->canLeave = 0;
    model->net = CP_UI_NET_BUSY;
    if (Start(app, JOB_VISIT, VisitJob, &app->engine.journal) != 0) {
        model->net = CP_UI_NET_ERROR;
        model->error = CP_APP_ERROR_WORKER;
        model->canLeave = app->leaveProven;
    }
}

static void Trade(void *context, int slot)
{
    Visit(context, slot, 0);
}

static void Collect(void *context, int cancel)
{
    Visit(context, -1, cancel ? WT_OP_CANCEL : WT_OP_COLLECT);
}

static void Close(void *context)
{
    CpApp *app = context;
    app->connecting = 0;
    if (app->job == JOB_VISIT) {
        app->platform->cancel(app->platform->arg);
        app->closing = 1;
        app->model.net = CP_UI_NET_BUSY;
        return;
    }
    app->model.net = CP_UI_NET_CLOSED;
}

/* ---- Publishing results --------------------------------------------------- */

static void Finish(CpApp *app, int job, int result)
{
    CpUiModel *model = &app->model;
    switch (job) {
    case JOB_SCAN:
        model->saveCount = app->result.saveCount;
        memcpy(model->saves, app->result.saves, sizeof(model->saves));
        model->busy = 0;
        break;
    case JOB_LOAD:
        model->busy = 0;
        model->loadMessage = result == 0 ? -1 : app->result.loadMessage;
        if (result != 0) { CloseFiles(app); break; }
        app->loaded = 1;
        model->gameVersion = app->saves[app->index].info.version;
        model->gameLanguage = app->saves[app->index].info.language;
        model->font = app->hasFont ? &app->font : NULL;
        model->recovery = app->result.recovery;
        /* The boot check proved gameplay may resume, or recovery fences it. */
        app->leaveProven = !app->result.recovery;
        model->tradePhase = app->result.phase;
        model->partyCount = app->result.partyCount;
        memcpy(model->party, app->result.party, sizeof(model->party));
        model->canLeave = 1;
        model->net = CP_UI_NET_CLOSED;
        break;
    case JOB_VISIT:
        model->partyCount = app->result.partyCount;
        memcpy(model->party, app->result.party, sizeof(model->party));
        if (app->visitOpened) model->tradePhase = app->visit.phase;
        model->completed = app->visit.completed;
        model->returned = app->visit.returned;
        model->rejected = app->visit.rejected;
        model->waiting = app->visit.waiting;
        model->superseded = app->visit.superseded;
        model->refused = app->visit.refused;
        model->refusal = app->visit.refusal;
        model->needsEntropy = app->needsEntropy;
        model->hasReceived = app->result.hasReceived;
        model->received = app->result.received;
        model->canLeave = app->leaveProven = app->visitCanLeave;
        model->error = result;
        if (result == 0) model->recovery = 0;
        if (app->closing) {
            app->closing = 0;
            model->net = CP_UI_NET_CLOSED;
        } else {
            model->net = result == 0 ? CP_UI_NET_RESPONSE : CP_UI_NET_ERROR;
        }
        break;
    default:
        break;
    }
}

static void Update(void *context)
{
    CpApp *app = context;
    const CpAppPlatform *platform = app->platform;
    int result = 0, job = app->job;
    if (platform->frame) platform->frame(platform->arg);
    if (job && platform->finished(platform->arg, &result)) {
        app->job = JOB_NONE;
        Finish(app, job, result);
    }
    if (app->connecting) {
        int status = platform->wifiStatus(platform->arg);
        if (status == CP_WIFI_CONNECTED) {
            app->connecting = 0;
            app->model.net = CP_UI_NET_READY;
            Log(app, "wifi: connected\n");
        } else if (status != CP_WIFI_CONNECTING) {
            app->connecting = 0;
            app->model.net = CP_UI_NET_ERROR;
            app->model.error = status == CP_WIFI_OFF ? CP_APP_ERROR_WIFI_OFF : CP_APP_ERROR_WIFI;
            Log(app, "wifi: no saved connection worked (%d)\n", status);
        }
    }
}

static const CpUiOps kOps = { Scan, Load, Unload, Connect, Trade, Collect, Close, Update };

void CpApp_Init(CpApp *app, const CpAppPlatform *platform, CompanionLanguage systemLanguage)
{
    memset(app, 0, sizeof(*app));
    sApp = app;
    app->platform = platform;
    app->file.context = app;
    app->file.readAt = FileRead;
    app->file.writeAt = FileWrite;
    app->file.sync = FileSync;
    app->file.size = FileSize;
    app->model.ops = &kOps;
    app->model.context = app;
    app->model.systemLanguage = systemLanguage;
    app->model.loadMessage = -1;
    app->model.canLeave = 1;
    app->model.tradePhase = WT_JOURNAL_IDLE;
    app->model.net = CP_UI_NET_CLOSED;
}
