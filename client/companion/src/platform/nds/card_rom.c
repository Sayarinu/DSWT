#include "platform/nds/card_rom.h"
#include "platform/nds/card_key.h"

#include "save/sha256.h"

#include <stdlib.h>
#include <string.h>

/* GBATEK, DS Cartridge Protocol and DS Cartridge I/O Ports. Only clock,
 * gap and encryption bits come from the cartridge, never the write bit. */
#define COMMAND(op) ((uint64_t)(op) << 56)
#define CLOCK_SLOW (1u << 27)
#define GAP_CLOCKS (1u << 28)
#define KEY2_DATA (3u << 13)
#define KEY2_COMMAND (1u << 22)
#define GAP_MASK 0x003f1fffu

static int Transfer(CpCardRom *card, uint64_t command, uint32_t flags, void *out, unsigned size)
{
    return card->bus->transfer(card->bus->context, command, flags, out, size);
}

static int ResetHeader(CpCardRom *card, int compare)
{
    uint32_t id;
    if (card->bus->reset(card->bus->context) != 0 ||
        Transfer(card, COMMAND(0x9f), CLOCK_SLOW, NULL, 0x2000) != 0 ||
        Transfer(card, 0, CLOCK_SLOW, card->page, CP_CARD_PAGE) != 0 ||
        Transfer(card, COMMAND(0x90), CLOCK_SLOW, &id, 4) != 0 ||
        id == 0 || id == UINT32_MAX || (compare && id != card->id)) return -1;

    /* Read the extended header even on NTR titles with a DSi signature.
     * 1T-ROM needs eight addressed snippets. MROM can stream it all at once. */
    unsigned chunk = (id >> 31) ? CP_CARD_PAGE : CP_CARD_HEADER;
    uint8_t *header = compare ? malloc(CP_CARD_HEADER) : card->header;
    if (!header) return -1;
    int ok = 1;
    for (unsigned at = 0; ok && at < CP_CARD_HEADER; at += chunk)
        ok = Transfer(card, (uint64_t)at << 24, CLOCK_SLOW, header + at, chunk) == 0;
    ok = ok && memcmp(header, card->page, CP_CARD_PAGE) == 0;
    if (compare) {
        ok = ok && memcmp(card->header, header, CP_CARD_HEADER) == 0;
        free(header);
    }
    card->id = id;
    return ok ? 0 : -1;
}

/* Newer chips latch the command with an empty transfer, then repeat it after
 * the delay. An entire secure block takes eight identical 512-byte reads. */
static int SecureCommand(CpCardRom *card, const uint32_t *key, uint64_t plain,
                         uint32_t flags, uint8_t *out, unsigned bytes)
{
    uint64_t command = CpCardKey_Crypt(key, plain, 0);
    unsigned chunk = bytes;
    /* The 4-KiB MROM burst is contiguous. 1T-ROM chips split it into
     * separate commands. Clock the entire command gap before each burst. */
    unsigned gap = (flags & 0x1fff) + ((flags >> 16) & 63);
    if (gap > 0x1fff) return -1;
    flags = (flags & ~GAP_MASK) | gap;
    if (card->id >> 31) {
        if (Transfer(card, command, flags, NULL, 0) != 0) return -1;
        if (chunk > CP_CARD_PAGE) chunk = CP_CARD_PAGE;
    } else {
        flags |= GAP_CLOCKS;
    }
    unsigned delay = (CpLe16(card->header + 0x6e) & 0x3fff) + 2;
    card->bus->delay(card->bus->context, delay * 8);
    for (unsigned at = 0;; at += chunk) {
        if (Transfer(card, command, flags, out ? out + at : NULL, chunk) != 0) return -1;
        if (at + chunk == bytes) return 0;
    }
}

static void DecryptBlock(uint32_t *key, uint8_t *bytes)
{
    uint64_t value = (uint64_t)CpLe32(bytes + 4) << 32 | CpLe32(bytes);
    value = CpCardKey_Crypt(key, value, 1);
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static int EnterDataMode(CpCardRom *card, uint32_t *key, int twl)
{
    static const uint8_t seeds[] = { 0xe8, 0x4d, 0x5a, 0xb1, 0x17, 0x8f, 0x99, 0xd5 };
    uint32_t game = CpLe32(card->header + 12);
    uint32_t flags = CpLe32(card->header + 0x64) & (CLOCK_SLOW | GAP_MASK);
    uint8_t *secure = twl ? card->secureTwl : card->secure;
    uint32_t id;
    /* Fixed session parameters iii=jjj=llll=mmm=nnn=0. Only the low
     * 20-bit command counter changes, once per distinct KEY1 command. */
    if (card->bus->loadKey(card->bus->context, twl, key) != 0)
        return twl ? CP_CARD_TWL_KEY_MISSING : CP_CARD_NTR_KEY_MISSING;
    CpCardKey_Init(key, game, twl ? 1 : 2);
    if (Transfer(card, COMMAND(twl ? 0x3d : 0x3c), CLOCK_SLOW, NULL, 0) != 0 ||
        SecureCommand(card, key, COMMAND(0x40), flags, NULL, 0) != 0) return -1;

    card->bus->seed(card->bus->context, 0x6000u + seeds[card->header[0x13] & 7]);
    flags |= KEY2_DATA;
    if (SecureCommand(card, key, COMMAND(0x10) | 1, flags, (uint8_t *)&id, 4) != 0 || id != card->id)
        return -1;
    for (unsigned block = 0; block < 4; ++block) {
        uint64_t command = COMMAND(0x20) | ((uint64_t)(4 + block) << 44) | (2 + block);
        if (SecureCommand(card, key, command, flags, secure + block * 4096, 4096) != 0) return -1;
    }
    if (SecureCommand(card, key, COMMAND(0xa0) | 6, flags, NULL, 0) != 0 ||
        Transfer(card, COMMAND(0xb8), card->dataFlags, &id, 4) != 0 || id != card->id) return -1;
    if (!twl) {
        DecryptBlock(key, secure);
        if (card->bus->loadKey(card->bus->context, 0, key) != 0) return CP_CARD_NTR_KEY_MISSING;
        CpCardKey_Init(key, game, 3);
        for (unsigned at = 0; at < 0x800; at += 8) DecryptBlock(key, secure + at);
        if (memcmp(secure, "encryObj", 8) != 0) return -1;
        memcpy(secure, "\xff\xde\xff\xe7\xff\xde\xff\xe7", 8);
    }
    return 0;
}

int CpCardRom_Check(CpCardRom *card)
{
    uint32_t id;
    /* The header is immutable while powered. Ejection powers down the slot
     * and loses KEY2 sync, even if an identical chip ID is reinserted. Never
     * silently reset a live session. Check even when serving cached bytes. */
    if (!card->active || card->generation != card->bus->generation ||
        Transfer(card, COMMAND(0xb8), card->dataFlags, &id, 4) != 0 || id != card->id) {
        card->active = 0;
        return -1;
    }
    return 0;
}

static uint32_t Size(void *ctx)
{
    CpCardRom *card = ctx;
    return card->active && card->generation == card->bus->generation ? card->capacity : 0;
}

static int FillPage(CpCardRom *card, uint32_t at)
{
    if (at < CP_CARD_HEADER) memcpy(card->page, card->header + at, CP_CARD_PAGE);
    else if (at < 0x4000) memset(card->page, 0, CP_CARD_PAGE);
    else if (at < 0x8000) memcpy(card->page, card->secure + (at - 0x4000), CP_CARD_PAGE);
    else if (card->twlStart && at >= card->twlStart && at - card->twlStart < CP_CARD_SECURE)
        memcpy(card->page, card->secureTwl + (at - card->twlStart), CP_CARD_PAGE);
    else if (Transfer(card, COMMAND(0xb7) | ((uint64_t)at << 24), card->dataFlags, card->page, CP_CARD_PAGE) != 0)
        return -1;
    card->pageOffset = at;
    return 0;
}

static int ReadAt(void *ctx, uint32_t offset, void *out, uint32_t size)
{
    CpCardRom *card = ctx;
    uint8_t *dest = out;
    if (offset > card->capacity || size > card->capacity - offset || (size && !dest)) return -1;
    if (CpCardRom_Check(card) != 0) return -1;
    /* Inherited main mode cannot read a retail header or secure area. Only
     * the two known header ranges used by the game reader are available. */
    if (card->inherited && size && offset < 0x8000 &&
        !(offset < 0x20 && size <= 0x20 - offset) &&
        !(offset >= 0x40 && offset < 0x50 && size <= 0x50 - offset)) return -1;
    while (size) {
        unsigned within = offset % CP_CARD_PAGE, count = CP_CARD_PAGE - within;
        uint32_t at = offset - within;
        if (card->pageOffset != at && FillPage(card, at) != 0) {
            card->active = 0;
            return -1;
        }
        if (count > size) count = size;
        memcpy(dest, card->page + within, count);
        dest += count;
        offset += count;
        size -= count;
    }
    return CpCardRom_Check(card);
}

/* TWiLight can leave Slot-1 in main data mode. Identify English White rev 0
 * from its code, game-data, embedded title ID and allocation-table pages.
 * Other builds continue through the normal physical header probe. */
int CpCardRom_OpenHandoff(CpCardRom *card, CpCardBus *bus, uint32_t control, CpReader *reader)
{
    static const uint32_t offsets[] = { 0x8000, 0x03471c00, 0x207800, 0x2f1400 };
    static const uint8_t expected[32] = {
        0xf1,0x7f,0x6b,0xc8,0xea,0x6a,0xf2,0x5b,0xed,0x26,0xdc,0xc3,0x75,0x6a,0xc0,0xec,
        0x3d,0x65,0x21,0xba,0x4a,0xc2,0xd0,0x02,0xd5,0x74,0x01,0xde,0x8a,0x7f,0x23,0x52
    };
    static const uint8_t prefix[0x20] = "POKEMON W\0\0\0IRAO01\x02\x00\x0b"
        "\0\0\0\0\0\0\0\x03\x40";
    /* File name and allocation table offsets and sizes, little endian. */
    static const uint8_t fs[16] = { 0,0x0e,0x2f,0, 0x10,0x04,0,0, 0,0x14,0x2f,0, 0x20,0x0f,0,0 };
    memset(card, 0, sizeof(*card));
    card->bus = bus;
    card->generation = ++bus->generation;
    card->dataFlags = control & KEY2_COMMAND
        ? control & (CLOCK_SLOW | GAP_MASK | KEY2_DATA | KEY2_COMMAND) : 0x00416657;
    if (Transfer(card, COMMAND(0xb8), card->dataFlags, &card->id, 4) || card->id != 0xc001ffc2)
        return -1;
    CpSaveSha256 sha;
    uint8_t hash[32];
    CpSaveSha256_Init(&sha);
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        if (Transfer(card, COMMAND(0xb7) | ((uint64_t)offsets[i] << 24),
                     card->dataFlags, card->page, CP_CARD_PAGE)) return -1;
        CpSaveSha256_Update(&sha, card->page, CP_CARD_PAGE);
    }
    CpSaveSha256_Finish(&sha, hash);
    if (memcmp(hash, expected, sizeof(hash))) return -1;
    memcpy(card->header, prefix, sizeof(prefix));
    memcpy(card->header + 0x40, fs, sizeof(fs));
    card->capacity = 0x0c3b9400; /* End of the used NTR data, before TWL secure data. */
    card->pageOffset = UINT32_MAX;
    card->inherited = card->active = 1;
    if (CpCardRom_Check(card)) return -1;
    *reader = (CpReader){ card, ReadAt, Size };
    return 0;
}

int CpCardRom_Open(CpCardRom *card, CpCardBus *bus, CpReader *reader)
{
    memset(card, 0, sizeof(*card));
    card->bus = bus;
    card->generation = ++bus->generation;
    if (ResetHeader(card, 0) != 0 || card->header[0x14] > 14) return -1;
    card->capacity = 0x20000u << card->header[0x14];
    card->dataFlags = (CpLe32(card->header + 0x60) & (CLOCK_SLOW | GAP_MASK)) | KEY2_DATA | KEY2_COMMAND;
    if (card->header[0x12] & 2) {
        card->twlStart = CpLe32(card->header + 0x1c0);
        if (card->twlStart < 0xb000 || card->twlStart % CP_CARD_PAGE ||
            card->twlStart > card->capacity - CP_CARD_SECURE) return -1;
    }
    uint32_t *key = malloc(CP_CARD_KEY_WORDS * sizeof(*key));
    if (!key) return -1;
    int result = EnterDataMode(card, key, 0);
    if (result == 0 && card->twlStart) {
        result = ResetHeader(card, 1);
        if (result == 0) result = EnterDataMode(card, key, 1);
    }
    free(key);
    if (result != 0) return result;
    card->pageOffset = UINT32_MAX;
    card->active = 1;
    *reader = (CpReader){ card, ReadAt, Size };
    return 0;
}

void CpCardRom_Close(CpCardRom *card)
{
    card->active = 0;
}
