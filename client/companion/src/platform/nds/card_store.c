#include "platform/nds/card_store.h"
#if CP_CARD_SAVE_WRITES
#include "save/sha256.h"
#include <stdlib.h>
#include <string.h>

enum { SECTOR = 65536, COUNT = CP_SAVE_SIZE / SECTOR, DIGEST = 32, CHUNK = 4096 };

/* One physical sector may be torn. Everything else on the inserted save must
 * still match. The SD record is committed before the first chip command. */
typedef struct {
    uint32_t version, build, romID, sector, geometry;
    uint8_t chipID[3], reserved;
    uint8_t hashes[COUNT][DIGEST], before[SECTOR], after[SECTOR];
} Intent;

struct CpCardStore {
    CpCardSave chip;
    uint32_t build, romID;
    int (*check)(void *);
    void *card;
    CpPlatformFile copy, log;
    uint8_t image[CP_SAVE_SIZE], hashes[COUNT][DIGEST], scratch[32768];
    Intent intent;
    int failed;
};

static void Digest(const void *data, size_t size, uint8_t out[DIGEST])
{
    CpSaveSha256 sha;
    CpSaveSha256_Init(&sha);
    CpSaveSha256_Update(&sha, data, size);
    CpSaveSha256_Finish(&sha, out);
}

static int Store(CpCardStore *s, const CpPlatformFile *file, uint32_t offset,
                 const void *data, size_t size)
{
    const uint8_t *p = data;
    if (file->writeAt(file->arg, offset, data, size) || file->sync(file->arg)) return -1;
    while (size) {
        size_t n = size > sizeof(s->scratch) ? sizeof(s->scratch) : size;
        if (file->readDurable(file->arg, offset, s->scratch, n) || memcmp(s->scratch, p, n)) return -1;
        p += n;
        offset += n;
        size -= n;
    }
    return 0;
}

static int ReadChip(CpCardStore *s, uint32_t at, void *data, size_t size)
{
    return s->check(s->card) || CpCardSave_Read(&s->chip, at, data, size) != CP_CHIP_OK ? -1 : 0;
}

/* Skip unchanged pages and erase only if some zero bit must become one. */
static int Program(CpCardStore *s, uint32_t at, uint8_t *live, const uint8_t *target)
{
    unsigned unit = s->chip.eraseSize, page = s->chip.pageSize;
    for (unsigned i = 0; i < SECTOR; i += unit) {
        int erase = 0;
        for (unsigned j = i; j < i + unit; ++j)
            if ((live[j] & target[j]) != target[j]) { erase = 1; break; }
        if (erase) {
            if (s->check(s->card) || CpCardSave_Erase(&s->chip, at + i, unit) != CP_CHIP_OK) return -1;
            memset(live + i, 255, unit);
        }
        for (unsigned j = i; j < i + unit; j += page) {
            if (!memcmp(live + j, target + j, page)) continue;
            if (s->check(s->card) || CpCardSave_Program(&s->chip, at + j, target + j, page) != CP_CHIP_OK)
                return -1;
            memcpy(live + j, target + j, page);
        }
    }
    if (ReadChip(s, at, live, SECTOR) || memcmp(live, target, SECTOR)) return -1;
    return 0;
}

static int Apply(CpCardStore *s, int committed)
{
    Intent *r = &s->intent;
    static const uint8_t clear[DIGEST] = {0};
    unsigned sector = r->sector;
    if (r->version != 1 || r->romID != s->romID || r->build != s->build ||
        r->geometry != s->chip.eraseSize || memcmp(r->chipID, s->chip.jedec, 3) || sector >= COUNT)
        return -1;
    for (unsigned i = 0; i < COUNT; ++i)
        if (i != sector && memcmp(s->hashes[i], r->hashes[i], DIGEST)) return -1;
    uint32_t at = sector * SECTOR;
    uint8_t *live = s->image + at;
    if (!committed) {
        /* A cut while writing/clearing the tag has no authority to modify
         * flash. An exact old or completed image needs only an SD refresh. */
        if (memcmp(live, r->before, SECTOR) && memcmp(live, r->after, SECTOR)) return -1;
        return Store(s, &s->log, 0, clear, DIGEST);
    }
    for (unsigned page = 0; page < SECTOR; page += s->chip.eraseSize) {
        int changing = memcmp(r->before + page, r->after + page, s->chip.eraseSize);
        for (unsigned i = page; i < page + s->chip.eraseSize; ++i) {
            if (live[i] == r->before[i] || live[i] == r->after[i] || live[i] == 255) continue;
            /* Only the erase unit being changed can contain partly raised
             * or programmed bits. Never repair a different stock-game edit. */
            if (!changing || ((live[i] & r->before[i]) != r->before[i] &&
                (live[i] & r->after[i]) != r->after[i])) return -1;
        }
    }
    if (Store(s, &s->copy, at, r->after, SECTOR) || Program(s, at, live, r->after) ||
        Store(s, &s->log, 0, clear, DIGEST)) return -1;
    Digest(live, SECTOR, s->hashes[sector]);
    return 0;
}

static int Recover(CpCardStore *s)
{
    uint8_t tag[DIGEST], digest[DIGEST];
    static const uint8_t clear[DIGEST] = {0};
    if (s->log.readDurable(s->log.arg, 0, tag, sizeof(tag))) return -1;
    if (!memcmp(tag, clear, sizeof(tag))) return 0;
    if (s->log.readDurable(s->log.arg, DIGEST, &s->intent, sizeof(Intent))) return -1;
    Digest(&s->intent, sizeof(Intent), digest);
    if (!memcmp(tag, digest, sizeof(tag))) return Apply(s, 1);
    /* A partial sector write of the tag can leave a mix of digest and zero
     * bytes. Other corrupt records stay blocked even on an exact live save. */
    for (unsigned i = 0; i < DIGEST; ++i)
        if (tag[i] && tag[i] != digest[i]) return -1;
    return Apply(s, 0);
}

static int Write(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    CpCardStore *s = arg;
    const uint8_t *p = bytes;
    if (s->failed || offset > CP_SAVE_SIZE || size > CP_SAVE_SIZE - offset) return -1;
    while (size) {
        unsigned sector = offset / SECTOR, within = offset % SECTOR;
        size_t n = size < SECTOR - within ? size : SECTOR - within;
        uint8_t *live = s->image + sector * SECTOR;
        if (memcmp(live + within, p, n)) {
            Intent *r = &s->intent;
            uint8_t tag[DIGEST];
            s->failed = 1; /* After any I/O failure, require a new copy-in. */
            if (ReadChip(s, sector * SECTOR, r->before, SECTOR) || memcmp(r->before, live, SECTOR)) return -1;
            r->version = 1;
            r->build = s->build;
            r->romID = s->romID;
            r->geometry = s->chip.eraseSize;
            r->sector = sector;
            memcpy(r->chipID, s->chip.jedec, 3);
            memcpy(r->hashes, s->hashes, sizeof(r->hashes));
            memcpy(r->after, live, SECTOR);
            memcpy(r->after + within, p, n);
            Digest(r, sizeof(*r), tag);
            if (Store(s, &s->log, DIGEST, r, sizeof(*r)) || Store(s, &s->log, 0, tag, DIGEST) || Apply(s, 1)) return -1;
            s->failed = 0;
        }
        p += n;
        size -= n;
        offset += n;
    }
    return s->check(s->card);
}

static int Read(void *arg, uint32_t offset, void *bytes, size_t size)
{
    CpCardStore *s = arg;
    if (s->failed || offset > CP_SAVE_SIZE || size > CP_SAVE_SIZE - offset || s->check(s->card)) return -1;
    if (size) memcpy(bytes, s->image + offset, size);
    return 0;
}

static int Durable(void *arg, uint32_t offset, void *bytes, size_t size)
{
    CpCardStore *s = arg;
    return s->failed ? -1 : ReadChip(s, offset, bytes, size);
}

static int Sync(void *arg)
{
    CpCardStore *s = arg;
    return s->failed ? -1 : s->check(s->card);
}

static int Size(void *arg, uint32_t *size)
{
    *size = CP_SAVE_SIZE;
    return Sync(arg);
}

CpCardStore *CpCardStore_Open(const CpCardSave *chip, uint32_t romID, uint32_t build,
    const CpPlatformFile *copy, const CpPlatformFile *intent, int (*check)(void *),
    void *card, CpPlatformFile *out)
{
    /* Larger physical erases could wipe both Gen V journal banks at once. */
    if (chip->type != CP_CHIP_FLASH || chip->size != CP_SAVE_SIZE || chip->pageSize != 256 ||
        (chip->eraseSize != 256 && chip->eraseSize != CHUNK)) return NULL;
    CpCardStore *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->chip = *chip;
    s->romID = romID;
    s->build = build;
    s->copy = *copy;
    s->log = *intent;
    s->check = check;
    s->card = card;
    if (ReadChip(s, 0, s->image, CP_SAVE_SIZE)) goto fail;
    for (unsigned at = 0; at < CP_SAVE_SIZE; at += CHUNK)
        if (ReadChip(s, at, s->scratch, CHUNK) || memcmp(s->scratch, s->image + at, CHUNK)) goto fail;
    for (unsigned i = 0; i < COUNT; ++i) Digest(s->image + i * SECTOR, SECTOR, s->hashes[i]);
    if (Recover(s) || Store(s, &s->copy, 0, s->image, CP_SAVE_SIZE)) goto fail;
    *out = (CpPlatformFile){ s, Read, Write, Sync, Size, Durable };
    return s;
fail:
    CpCardStore_Close(s);
    return NULL;
}

void CpCardStore_Close(CpCardStore *s)
{
    if (s) memset(s, 0, sizeof(*s));
    free(s);
}
#endif
