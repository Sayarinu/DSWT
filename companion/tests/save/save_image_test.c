#include "save/save_image.h"
#include "save/posix_file.h"
#include "wonder_trade/pokemon_codec.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const unsigned offsets[2] = { 0, 53036 };
static const unsigned sizes[2] = { 53036, 74212 };

typedef struct {
    unsigned offset;
    size_t size;
} Event;

typedef struct {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    uint64_t size;
    Event events[24];
    unsigned count, failEvent, reads, failRead;
    int failSize, partialWrite;
} MemoryFile;

static MemoryFile memory;
static CpSaveImage image;
static unsigned char before[CP_SAVE_FILE_SIZE];

static unsigned Read32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void Write32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

/* A bit-at-a-time oracle, separate from the save implementation. */
static unsigned Crc(const unsigned char *p, unsigned size)
{
    unsigned crc = 0xffff, i;
    while (size--) {
        unsigned byte = *p++;
        for (i = 0; i < 8; ++i) {
            unsigned carry = ((crc >> 15) ^ (byte >> 7)) & 1;
            crc = ((crc << 1) ^ (carry ? 0x1021 : 0)) & 65535;
            byte <<= 1;
        }
    }
    return crc;
}

static unsigned char *Body(unsigned half, unsigned block)
{
    return memory.bytes + half * CP_SAVE_HALF_SIZE + offsets[block];
}

static unsigned char *Footer(unsigned half, unsigned block)
{
    return Body(half, block) + sizes[block] - 20;
}

static void Seal(unsigned half, unsigned block, uint32_t global, uint32_t counter, uint32_t magic)
{
    unsigned char *footer = Footer(half, block);
    unsigned crc = Crc(Body(half, block), sizes[block] - 20);
    Write32(footer, global);
    Write32(footer + 4, counter);
    Write32(footer + 8, sizes[block]);
    Write32(footer + 12, magic);
    footer[16] = (unsigned char)block;
    footer[17] = 0xa7;
    footer[18] = (unsigned char)crc;
    footer[19] = (unsigned char)(crc >> 8);
}

static int ReadAt(void *context, uint32_t offset, void *data, size_t size)
{
    MemoryFile *file = context;
    if (++file->reads == file->failRead || offset > file->size || size > file->size - offset)
        return -1;
    memcpy(data, file->bytes + offset, size);
    return 0;
}

static int WriteAt(void *context, uint32_t offset, const void *data, size_t size)
{
    MemoryFile *file = context;
    assert(file->count < 24);
    file->events[file->count].offset = offset;
    file->events[file->count++].size = size;
    assert(offset <= file->size && size <= file->size - offset);
    if (file->count == file->failEvent) {
        if (file->partialWrite) memcpy(file->bytes + offset, data, size / 2);
        return -1;
    }
    memcpy(file->bytes + offset, data, size);
    return 0;
}

static int Sync(void *context)
{
    MemoryFile *file = context;
    assert(file->count < 24);
    file->events[file->count].offset = 0;
    file->events[file->count++].size = 0;
    return file->count == file->failEvent ? -1 : 0;
}

static int Size(void *context, uint64_t *size)
{
    MemoryFile *file = context;
    *size = file->size;
    return file->failSize ? -1 : 0;
}

static CpFile file = { &memory, ReadAt, WriteAt, Sync, Size };

static void Fixture(void)
{
    unsigned i, half, block;
    memset(&memory, 0, sizeof(memory));
    memory.size = sizeof(memory.bytes);
    for (i = 0; i < sizeof(memory.bytes); ++i) memory.bytes[i] = (unsigned char)(i * 37 + 11);
    for (half = 0; half < 2; ++half) for (block = 0; block < 2; ++block)
        Seal(half, block, 40 + half, 70 + half + 10 * block, CP_SAVE_MAGIC_INTERNATIONAL);
}

static CpSaveResult Load(void)
{
    return CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_INTERNATIONAL);
}

static int RetailCompare(uint32_t a, uint32_t b)
{
    if (a == UINT32_MAX && b == 0) return -1;
    if (a == 0 && b == UINT32_MAX) return 1;
    return (a > b) - (a < b);
}

/* Independent LoadCheck model: CORRUPT still supplies a usable pair, unless
 * CompareSectors would assert or the selected global counters disagree. */
static CpSaveResult RetailLoad(unsigned selected[2])
{
    unsigned block, half, valid[2] = { 0, 0 };
    uint32_t globals[2][2], counters[2][2];
    int assertion = 0;
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            const unsigned char *footer = Footer(half, block);
            unsigned crc = footer[18] | ((unsigned)footer[19] << 8);
            globals[block][half] = Read32(footer);
            counters[block][half] = Read32(footer + 4);
            if (Read32(footer + 8) == sizes[block]
                && Read32(footer + 12) == CP_SAVE_MAGIC_INTERNATIONAL
                && footer[16] == block && crc == Crc(Body(half, block), sizes[block] - 20))
                valid[block] |= 1u << half;
        }
        if (valid[block] == 3) {
            int global = RetailCompare(globals[block][0], globals[block][1]);
            int counter = RetailCompare(counters[block][0], counters[block][1]);
            if (global > 0) {
                assertion |= counter <= 0;
                selected[block] = 0;
            } else if (global < 0) {
                assertion |= counter >= 0;
                selected[block] = 1;
            } else selected[block] = counter < 0;
        } else selected[block] = valid[block] == 2;
    }
    if (!valid[0] || !valid[1]) return CP_SAVE_INVALID;
    if (assertion) return CP_SAVE_NEEDS_GAME_SAVE;

    /* Four LoadCheck branches, classified by whether each block has a backup. */
    switch ((valid[0] == 3 ? 2 : 0) | (valid[1] == 3 ? 1 : 0)) {
    case 3: /* Both valid: mismatch always selects stale general, current boxes. */
    case 2: /* Valid general, partial boxes: the same fallback. */
        if (globals[0][selected[0]] != globals[1][selected[1]]) selected[0] ^= 1;
        break;
    case 1: /* Partial general, valid boxes: try current boxes, then stale boxes. */
        if (globals[0][selected[0]] != globals[1][selected[1]]
            && globals[0][selected[0]] == globals[1][selected[1] ^ 1]) selected[1] ^= 1;
        break;
    case 0: /* Partial/partial: either same or split halves, with a global assert. */
        break;
    }
    return globals[0][selected[0]] == globals[1][selected[1]]
        ? CP_SAVE_OK : CP_SAVE_NEEDS_GAME_SAVE;
}

static void LoadValidityMatrix(void)
{
    static const uint32_t globals[][2][2] = {
        { { 10, 10 }, { 10, 10 } },
        { { 40, 42 }, { 40, 41 } },
        { { 40, 42 }, { 40, 40 } },
        { { 40, 41 }, { 40, 42 } },
        { { 40, 40 }, { 40, 41 } },
        { { 40, 41 }, { 40, 41 } },
        { { 41, 40 }, { 41, 40 } },
        { { UINT32_MAX, 0 }, { UINT32_MAX, UINT32_MAX } }
    };
    unsigned general, boxes, half, block, pattern;
    for (pattern = 0; pattern < sizeof(globals) / sizeof(globals[0]); ++pattern)
    for (general = 0; general < 4; ++general) for (boxes = 0; boxes < 4; ++boxes) {
        unsigned selected[2];
        CpSaveResult expected;
        Fixture();
        for (half = 0; half < 2; ++half) for (block = 0; block < 2; ++block) {
            unsigned mask = block ? boxes : general;
            int order = RetailCompare(globals[pattern][block][0], globals[pattern][block][1]);
            Seal(half, block, globals[pattern][block][half],
                 20 + (order > 0 ? 1 - half : half), CP_SAVE_MAGIC_INTERNATIONAL);
            if (!(mask & (1u << half))) Footer(half, block)[18] ^= 1;
        }
        expected = RetailLoad(selected);
        assert(Load() == expected);
        assert(image.loaded == (expected == CP_SAVE_OK));
        if (expected == CP_SAVE_OK) {
            for (block = 0; block < 2; ++block) {
                assert(image.currentHalf[block] == selected[block]);
                assert(CpSaveImage_Block(&image, block)
                       == image.bytes + selected[block] * CP_SAVE_HALF_SIZE + offsets[block]);
                assert(image.blockCounters[block] == Read32(Footer(selected[block], block) + 4));
            }
            assert(image.globalCounter == Read32(Footer(selected[0], 0)));
        } else assert(CpSaveImage_Block(&image, 0) == NULL);
    }
    puts("PASS load: 128 validity/global combinations match the independent LoadCheck model");
}

static void CounterSelection(void)
{
    static const struct {
        uint32_t ga, gb, ca, cb;
        unsigned half;
        CpSaveResult result;
    } cases[] = {
        { 4, 5, 8, 9, 1, CP_SAVE_OK },
        { 5, 4, 9, 8, 0, CP_SAVE_OK },
        { 5, 5, 9, 8, 0, CP_SAVE_OK },
        { 5, 5, 8, 9, 1, CP_SAVE_OK },
        { 5, 5, 8, 8, 0, CP_SAVE_OK },
        { UINT32_MAX, 0, UINT32_MAX, 0, 1, CP_SAVE_OK },
        { 0, UINT32_MAX, 0, UINT32_MAX, 0, CP_SAVE_OK },
        { 5, 5, UINT32_MAX, 0, 1, CP_SAVE_OK },
        { 5, 5, 0, UINT32_MAX, 0, CP_SAVE_OK },
        { 0xfffffffe, 0, 0xfffffffe, 0, 0, CP_SAVE_OK },
        { 4, 5, 9, 8, 0, CP_SAVE_NEEDS_GAME_SAVE },
        { 5, 4, 8, 9, 0, CP_SAVE_NEEDS_GAME_SAVE },
        { 4, 5, 8, 8, 0, CP_SAVE_NEEDS_GAME_SAVE }
    };
    unsigned i, block;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Fixture();
        for (block = 0; block < 2; ++block) {
            Seal(0, block, cases[i].ga, cases[i].ca, CP_SAVE_MAGIC_INTERNATIONAL);
            Seal(1, block, cases[i].gb, cases[i].cb, CP_SAVE_MAGIC_INTERNATIONAL);
        }
        assert(Load() == cases[i].result);
        if (image.loaded) for (block = 0; block < 2; ++block)
            assert(image.currentHalf[block] == cases[i].half);
    }
    /* Independent per-block selection may legitimately use different halves. */
    Fixture();
    for (block = 0; block < 2; ++block) {
        Seal(0, block, 41, block ? 80 : 72, CP_SAVE_MAGIC_INTERNATIONAL);
        Seal(1, block, 41, block ? 81 : 71, CP_SAVE_MAGIC_INTERNATIONAL);
    }
    assert(Load() == CP_SAVE_OK);
    assert(image.currentHalf[0] == 0 && image.currentHalf[1] == 1);

    /* Matching stale/stale blocks do not permit a different fallback choice. */
    for (i = 0; i < 4; ++i) {
        Fixture();
        Seal(1, 0, 42, 72, CP_SAVE_MAGIC_INTERNATIONAL);
        if (i == 1) Footer(0, 0)[16] = 7;
        if (i == 2) Footer(0, 1)[16] = 7;
        if (i == 3) { Footer(0, 0)[16] = 7; Footer(0, 1)[16] = 7; }
        assert(Load() == CP_SAVE_NEEDS_GAME_SAVE);
    }
    Fixture();
    Seal(0, 0, 41, 71, CP_SAVE_MAGIC_INTERNATIONAL);
    Seal(1, 0, 42, 72, CP_SAVE_MAGIC_INTERNATIONAL);
    assert(Load() == CP_SAVE_OK);
    assert(image.currentHalf[0] == 0 && image.currentHalf[1] == 1);
    Footer(0, 1)[16] = 7;
    assert(Load() == CP_SAVE_OK);
    assert(image.currentHalf[0] == 0 && image.currentHalf[1] == 1);
    /* A lone general block matching only the stale boxes is retail recovery. */
    Fixture(); Footer(1, 0)[18] ^= 1;
    assert(Load() == CP_SAVE_OK);
    assert(image.currentHalf[0] == 0 && image.currentHalf[1] == 0);
    /* Likewise a lone boxes block matching only the stale general block. */
    Fixture(); Footer(1, 1)[18] ^= 1;
    assert(Load() == CP_SAVE_OK);
    assert(image.currentHalf[0] == 0 && image.currentHalf[1] == 0);
    puts("PASS selection: global/block ordering, ties, split halves, wrap and mismatches");
}

static void InvalidInputs(void)
{
    static const unsigned corrupt[] = { 8, 12, 16, 18 };
    unsigned i, half;
    for (i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        Fixture();
        for (half = 0; half < 2; ++half) Footer(half, 0)[corrupt[i]] ^= 1;
        assert(Load() == CP_SAVE_INVALID);
    }
    Fixture(); Body(0, 0)[100] ^= 1; Body(1, 0)[100] ^= 1;
    assert(Load() == CP_SAVE_INVALID);
    Fixture(); memset(memory.bytes, 0xff, sizeof(memory.bytes));
    assert(Load() == CP_SAVE_INVALID);
    Fixture(); memory.size--; assert(Load() == CP_SAVE_IO_ERROR);
    Fixture(); memory.size++; assert(Load() == CP_SAVE_IO_ERROR);
    Fixture(); memory.failSize = 1; assert(Load() == CP_SAVE_IO_ERROR);
    Fixture(); memory.failRead = 1; assert(Load() == CP_SAVE_IO_ERROR);
    Fixture(); assert(CpSaveImage_Load(&image, &file, 0) == CP_SAVE_INVALID);
    assert(CpSaveImage_Load(NULL, &file, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_INVALID);
    assert(CpSaveImage_Load(&image, NULL, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_IO_ERROR);
    Fixture();
    for (half = 0; half < 2; ++half) {
        Seal(half, 0, 20 + half, 30 + half, CP_SAVE_MAGIC_KOREAN);
        Seal(half, 1, 20 + half, 40 + half, CP_SAVE_MAGIC_KOREAN);
    }
    assert(Load() == CP_SAVE_INVALID);
    assert(CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_KOREAN) == CP_SAVE_OK);
    {
        unsigned char digest[32];
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_OK);
        for (half = 0; half < 2; ++half) {
            assert(Read32(Footer(half, 0) + 12) == CP_SAVE_MAGIC_KOREAN);
            assert(Read32(Footer(half, 1) + 12) == CP_SAVE_MAGIC_KOREAN);
        }
    }
    assert(CpSaveImage_Block(&image, 2) == NULL);
    puts("PASS validation: CRC, footer fields, international/Korean commits, size and I/O failures");
}

static void CheckUntouched(void)
{
    unsigned half;
    for (half = 0; half < 2; ++half) {
        unsigned start = half * CP_SAVE_HALF_SIZE + offsets[1] + sizes[1];
        unsigned size = CP_SAVE_HALF_SIZE - offsets[1] - sizes[1];
        assert(memcmp(memory.bytes + start, before + start, size) == 0);
    }
}

static void CommitLayout(uint32_t global, uint32_t general, uint32_t boxes, int split)
{
    unsigned block, half, pass, event = 0;
    unsigned initialHalf[2];
    unsigned char expected[32], actual[32];
    uint32_t counters[2] = { general, boxes };
    Fixture();
    for (block = 0; block < 2; ++block) {
        Seal(0, block, global, counters[block], CP_SAVE_MAGIC_INTERNATIONAL);
        Seal(1, block, global, counters[block] - 1, CP_SAVE_MAGIC_INTERNATIONAL);
    }
    if (split) Seal(1, 1, global, boxes + 1, CP_SAVE_MAGIC_INTERNATIONAL);
    assert(Load() == CP_SAVE_OK);
    for (block = 0; block < 2; ++block) {
        initialHalf[block] = image.currentHalf[block];
        counters[block] = image.blockCounters[block];
        CpSaveImage_Block(&image, block)[123 + block] ^= 0xd3;
    }
    assert(CpSaveImage_Hash(&image, expected) == CP_SAVE_OK);
    memcpy(before, memory.bytes, sizeof(before));
    assert(CpSaveImage_CommitBoth(&image, &file, expected) == CP_SAVE_OK);
    assert(image.globalCounter == (uint32_t)(global + 2));
    assert(memory.count == 24);
    for (pass = 0; pass < 2; ++pass) for (block = 0; block < 2; ++block) {
        unsigned target = initialHalf[block] ^ (pass == 0);
        unsigned start = target * CP_SAVE_HALF_SIZE + offsets[block];
        unsigned bodySize = sizes[block] - 20;
        assert(memory.events[event].offset == start && memory.events[event].size == bodySize);
        assert(memory.events[event + 1].size == 0);
        assert(memory.events[event + 2].offset == start + bodySize + 8 && memory.events[event + 2].size == 8);
        assert(memory.events[event + 3].size == 0);
        assert(memory.events[event + 4].offset == start + bodySize && memory.events[event + 4].size == 20);
        assert(memory.events[event + 5].size == 0);
        event += 6;
    }
    for (block = 0; block < 2; ++block) {
        assert(image.currentHalf[block] == initialHalf[block]);
        assert(image.blockCounters[block] == (uint32_t)(counters[block] + 2));
        assert(memcmp(Body(0, block), Body(1, block), sizes[block] - 20) == 0);
        for (half = 0; half < 2; ++half) {
            uint32_t advance = half == initialHalf[block] ? 2 : 1;
            unsigned char *footer = Footer(half, block);
            assert(Read32(footer) == (uint32_t)(global + advance));
            assert(Read32(footer + 4) == (uint32_t)(counters[block] + advance));
            assert(Read32(footer + 8) == sizes[block]);
            assert(Read32(footer + 12) == CP_SAVE_MAGIC_INTERNATIONAL);
            assert(footer[16] == block && footer[17] == 0xa7);
            assert((unsigned)(footer[18] | (footer[19] << 8)) == Crc(Body(half, block), sizes[block] - 20));
        }
    }
    CheckUntouched();
    assert(CpSaveImage_Hash(&image, actual) == CP_SAVE_OK);
    assert(memcmp(expected, actual, 32) == 0);
    assert(Load() == CP_SAVE_OK);
    assert(CpSaveImage_Verify(&image, &file, expected) == CP_SAVE_OK);
}

static void VerifyFailures(void)
{
    unsigned char digest[32];
    unsigned half, block, field;
    static const unsigned fields[] = { 0, 4, 8, 12, 16, 18 };
    CommitLayout(40, 70, 80, 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    digest[0] ^= 1;
    memory.count = 0;
    assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
    assert(memory.count == 0);
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
    digest[0] ^= 1;
    for (half = 0; half < 2; ++half) for (block = 0; block < 2; ++block) {
        for (field = 0; field < sizeof(fields) / sizeof(fields[0]); ++field) {
            unsigned char *p = Footer(half, block) + fields[field];
            *p ^= 0x40;
            assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
            *p ^= 0x40;
        }
        Body(half, block)[55] ^= 1;
        assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
        /* A changed body with a repaired CRC still fails the SHA-256 check. */
        Seal(half, block, Read32(Footer(half, block)), Read32(Footer(half, block) + 4), CP_SAVE_MAGIC_INTERNATIONAL);
        assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
        Body(half, block)[55] ^= 1;
        Seal(half, block, Read32(Footer(half, block)), Read32(Footer(half, block) + 4), CP_SAVE_MAGIC_INTERNATIONAL);
    }
    memory.reads = 0; memory.failRead = 1;
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_IO_ERROR);
    /* Footer-read failure, after the general body's 208 pages. */
    memory.reads = 0; memory.failRead = 209;
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_IO_ERROR);
    memory.failRead = 0; memory.size--;
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_IO_ERROR);
    memory.size++;
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_OK);
    puts("PASS verify: both halves, all footer fields, body/CRC/hash changes and read failures");
}

static void InterruptedCommits(void)
{
    unsigned fail, partial, fixture;
    for (fixture = 0; fixture < 20; ++fixture)
    for (partial = 0; partial < 2; ++partial) for (fail = 1; fail <= 24; ++fail) {
        unsigned char digest[32], oldDigest[32], actual[32];
        unsigned initial[2], selected[2], block, changed[2];
        Fixture();
        if (fixture >= 8) {
            unsigned kind = ((fixture - 8) % 6) / 2, half = fixture % 2;
            uint32_t global = fixture < 14 ? 41 : UINT32_MAX;
            for (block = 0; block < 2; ++block) {
                /* Distinguish discarded bodies from the selected old inventory. */
                Body(0, block)[20] = (unsigned char)(0x10 + 4 * block);
                Body(1, block)[20] = (unsigned char)(0x12 + 4 * block);
                Seal(half, block, global, global, CP_SAVE_MAGIC_INTERNATIONAL);
            }
            if (kind < 2) {
                /* Stale general, with either two valid boxes or partial boxes. */
                Seal(1 - half, 0, global + 1, global + 1, CP_SAVE_MAGIC_INTERNATIONAL);
                Seal(1 - half, 1, global - 1, global - 1, CP_SAVE_MAGIC_INTERNATIONAL);
                if (kind == 1) Footer(1 - half, 1)[18] ^= 1;
            } else {
                /* Partial general matching only stale boxes. */
                Footer(1 - half, 0)[18] ^= 1;
                Seal(1 - half, 1, global + 1, global + 1, CP_SAVE_MAGIC_INTERNATIONAL);
            }
        } else if (fixture == 1) {
            Seal(0, 0, 41, 72, CP_SAVE_MAGIC_INTERNATIONAL);
            Seal(1, 0, 41, 71, CP_SAVE_MAGIC_INTERNATIONAL);
        } else if (fixture >= 6) {
            uint32_t global = fixture == 6 ? UINT32_MAX : UINT32_MAX - 1;
            for (block = 0; block < 2; ++block) {
                Seal(0, block, global - 1, global - 1, CP_SAVE_MAGIC_INTERNATIONAL);
                Seal(1, block, global, global, CP_SAVE_MAGIC_INTERNATIONAL);
            }
        } else if (fixture >= 2) {
            unsigned half = fixture % 2;
            /* First full save, then general-only saves without touching PC.
             * Also cover a first full save with BOTH other footers erased. */
            memset(Footer(1 - half, 1), 0xff, 20);
            Seal(half, 1, 1, 1, CP_SAVE_MAGIC_INTERNATIONAL);
            Seal(half, 0, 1, fixture < 4 ? 3 : 1, CP_SAVE_MAGIC_INTERNATIONAL);
            if (fixture < 4) Seal(1 - half, 0, 1, 2, CP_SAVE_MAGIC_INTERNATIONAL);
            else memset(Footer(1 - half, 0), 0xff, 20);
        }
        assert(Load() == CP_SAVE_OK);
        for (block = 0; block < 2; ++block) initial[block] = image.currentHalf[block];
        assert(RetailLoad(selected) == CP_SAVE_OK);
        assert(selected[0] == initial[0] && selected[1] == initial[1]);
        assert(CpSaveImage_Hash(&image, oldDigest) == CP_SAVE_OK);
        CpSaveImage_Block(&image, 0)[20] ^= 1;
        CpSaveImage_Block(&image, 1)[20] ^= 1;
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        memcpy(before, memory.bytes, sizeof(before));
        memory.failEvent = fail;
        memory.partialWrite = (int)partial;
        assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_IO_ERROR);
        assert(memory.count == fail && !image.loaded);
        assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_INVALID);
        CheckUntouched();
        /* Check the actual game-selected bodies, even on recovery paths. */
        assert(RetailLoad(selected) == CP_SAVE_OK);
        for (block = 0; block < 2; ++block) {
            const unsigned char *old = before + initial[block] * CP_SAVE_HALF_SIZE + offsets[block];
            const unsigned char *body = Body(selected[block], block);
            assert(memcmp(body, old, 20) == 0);
            assert(memcmp(body + 21, old + 21, sizes[block] - 20 - 21) == 0);
            if (body[20] != old[20] && body[20] != (unsigned char)(old[20] ^ 1))
                fprintf(stderr, "discarded retail block: fixture=%u partial=%u event=%u block=%u halves=%u/%u old=%u actual=%u\n",
                        fixture, partial, fail, block, selected[0], selected[1], old[20], body[20]);
            assert(body[20] == old[20] || body[20] == (unsigned char)(old[20] ^ 1));
            changed[block] = body[20] != old[20];
        }
        if (changed[0] != changed[1])
            fprintf(stderr, "mixed retail checkpoint: fixture=%u partial=%u event=%u halves=%u/%u changed=%u/%u\n",
                    fixture, partial, fail, selected[0], selected[1], changed[0], changed[1]);
        assert(changed[0] == changed[1]);
        assert(Load() == CP_SAVE_OK);
        for (block = 0; block < 2; ++block) {
            assert(image.currentHalf[block] == selected[block]);
            assert(image.blockCounters[block] == Read32(Footer(selected[block], block) + 4));
        }
        assert(image.globalCounter == Read32(Footer(selected[0], 0)));
        assert(CpSaveImage_Hash(&image, actual) == CP_SAVE_OK);
        assert(memcmp(actual, changed[0] ? digest : oldDigest, sizeof(actual)) == 0);
        /* Settle can finish either journal-recorded inventory from the chosen pair. */
        memory.count = memory.failEvent = 0;
        assert(CpSaveImage_CommitBoth(&image, &file, actual) == CP_SAVE_OK);
        CheckUntouched();
        assert(Load() == CP_SAVE_OK);
        assert(CpSaveImage_Verify(&image, &file, actual) == CP_SAVE_OK);
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        assert(memcmp(actual, digest, sizeof(actual)) == 0);
    }
    /* Failure during the final re-read also invalidates the image. */
    Fixture(); assert(Load() == CP_SAVE_OK);
    {
        unsigned char digest[32];
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        memory.failRead = memory.reads + 1;
        assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_IO_ERROR);
        assert(!image.loaded && memory.count == 24);
    }
    puts("PASS interruptions: 960 stopped prefixes across 24 write/sync boundaries, partial writes, split halves, erased footers and counter wrap; all three fallback starts in both layouts and at wrap; exact retail selection, old/new hashes, resumed commit and verify; final read failure");
}

static void ExportFixture(const char *path)
{
    unsigned half, block, slot, i;
    unsigned char canonical[136] = { 0 }, empty[136], party[136], digest[32];
    CpPosixFile context;
    CpFile disk;
    int fd = open(path, O_RDWR);
    assert(fd >= 0 && CpPosixFile_Init(&disk, &context, fd) == 0);
    Fixture();
    assert(WtPokemon_Encode(canonical, empty) == 0);
    canonical[6] = canonical[8] = 25;
    assert(WtPokemon_Encode(canonical, party) == 0);
    for (half = 0; half < 2; ++half) {
        Write32(Body(half, 0) + CP_SAVE_PARTY_CAPACITY_OFFSET, 6);
        Write32(Body(half, 0) + CP_SAVE_PARTY_COUNT_OFFSET, 1);
        memcpy(Body(half, 0) + CP_SAVE_PARTY_SLOTS_OFFSET, party, sizeof(party));
        for (slot = 0; slot < 540; ++slot)
            memcpy(Body(half, 1) + 4 + slot * 136, empty, sizeof(empty));
        for (block = 0; block < 2; ++block)
            Seal(half, block, 40 + half, 70 + half + block * 10, CP_SAVE_MAGIC_INTERNATIONAL);
    }
    assert(disk.writeAt(disk.context, 0, memory.bytes, sizeof(memory.bytes)) == 0);
    assert(disk.sync(disk.context) == 0);
    assert(CpSaveImage_Load(&image, &disk, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    CpSaveImage_Block(&image, 0)[4000] ^= 0x9b;
    CpSaveImage_Block(&image, 1)[0] = 3;
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_CommitBoth(&image, &disk, digest) == CP_SAVE_OK);
    assert(close(fd) == 0);
    for (i = 0; i < sizeof(digest); ++i) printf("%02x", digest[i]);
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--fixture") == 0) {
        ExportFixture(argv[2]);
        return 0;
    }
    assert(argc == 1);
    LoadValidityMatrix();
    CounterSelection();
    InvalidInputs();
    CommitLayout(40, 70, 80, 0);
    CommitLayout(UINT32_MAX, UINT32_MAX, UINT32_MAX - 1, 0);
    CommitLayout(UINT32_MAX - 1, UINT32_MAX - 1, UINT32_MAX - 1, 0);
    CommitLayout(40, 70, 80, 1);
    puts("PASS commit: ordered write/sync trace, counters, wrap, split halves and untouched regions");
    VerifyFailures();
    InterruptedCommits();
    return 0;
}
