#include "sim_card.h"

#include <assert.h>
#include <string.h>

/* Synthetic tables for the protocol simulation. */
static int LoadKey(void *ctx, int twl, uint32_t *table)
{
    (void)ctx;
    for (unsigned i = 0; i < CP_CARD_KEY_WORDS; ++i)
        table[i] = 0xabcdef01u * (i + (twl ? 2 : 1));
    return 0;
}

static int ImageRead(SimCard *sim, uint32_t at, void *out, unsigned bytes)
{
    return sim->image->readAt(sim->image->context, at, out, bytes);
}

static void EncryptWord(uint32_t *key, uint8_t *p)
{
    uint64_t value = (uint64_t)CpLe32(p + 4) << 32 | CpLe32(p);
    value = CpCardKey_Crypt(key, value, 0);
    for (unsigned i = 0; i < 8; ++i) p[i] = value >> (8 * i);
}

static int Reset(void *ctx)
{
    SimCard *sim = ctx;
    sim->mode = 0;
    sim->counter = sim->repeats = 0;
    sim->seeded = sim->delayed = 0;
    ++sim->resets;
    return sim->present ? 0 : -1;
}

static int Secure(SimCard *sim, uint64_t command, uint32_t flags, void *out, unsigned bytes)
{
    uint64_t plain = CpCardKey_Crypt(sim->key, command, 1);
    unsigned op = plain >> 60, block = (plain >> 44) & 0xffff;
    assert((plain & 0xfffff) == sim->counter);
    assert(!!(flags & (1u << 28)) == !(sim->id >> 31));
    assert((flags & (1u << 22)) == 0);
    if ((sim->id >> 31) && sim->repeats++ == 0) {
        assert(bytes == 0);
        return 0;
    }
    assert(sim->delayed);
    assert(sim->seeded == (op != 4));
    assert(!!(flags & (1u << 13)) == sim->seeded);
    switch (op) {
    case 4:
        assert(sim->counter == 0 && bytes == 0);
        break;
    case 1:
        assert(sim->counter == 1 && bytes == 4);
        memcpy(out, &sim->id, 4);
        break;
    case 2: {
        assert(block >= 4 && block < 8 && sim->counter == block - 2);
        unsigned index = (block - 4) * 4096;
        if (sim->id >> 31) {
            assert(bytes == CP_CARD_PAGE && sim->repeats >= 2 && sim->repeats <= 9);
            index += (sim->repeats - 2) * CP_CARD_PAGE;
        } else {
            assert(bytes == 4096);
        }
        memcpy(out, sim->secure + index, bytes);
        if ((sim->id >> 31) && sim->repeats < 9) return 0;
        break;
    }
    case 10:
        assert(sim->counter == 6 && bytes == 0);
        sim->mode = 2;
        break;
    default:
        return -1;
    }
    ++sim->counter;
    sim->repeats = 0;
    sim->delayed = 0;
    return 0;
}

static int Transfer(void *ctx, uint64_t command, uint32_t flags, void *out, unsigned bytes)
{
    SimCard *sim = ctx;
    uint32_t offset = (uint32_t)(command >> 24);
    unsigned op = command >> 56;
    assert((flags & (1u << 30)) == 0);
    if (!sim->present) return -1;
    if (sim->mode == 1) return Secure(sim, command, flags, out, bytes);
    if (sim->mode == 2) {
        assert((flags & (1u << 22)) && (flags & (1u << 13)));
        if (op == 0xb8 && bytes == 4) {
            memcpy(out, &sim->id, 4);
            return 0;
        }
        assert(op == 0xb7 && offset >= 0x8000 && offset % CP_CARD_PAGE == 0 && bytes == CP_CARD_PAGE);
        if (!sim->twl && sim->twlStart && offset >= sim->twlStart - 0x3000) return -1;
        if (sim->failPage || ImageRead(sim, offset, out, bytes) != 0) return -1;
        if (offset == sim->corruptPage) ((uint8_t *)out)[0] ^= 1;
        return 0;
    }
    if (op == 0x9f) return bytes == 8192 && !out ? 0 : -1;
    if (op == 0x90 && bytes == 4) {
        memcpy(out, &sim->id, 4);
        return 0;
    }
    if (op == 0) {
        assert(bytes == CP_CARD_PAGE || (!(sim->id >> 31) && bytes == CP_CARD_HEADER));
        int result = ImageRead(sim, offset, out, bytes);
        if (sim->changeHeader && sim->resets == 2 && offset == 0) ((uint8_t *)out)[0x1e] ^= 1;
        return result;
    }
    if ((op != 0x3c && op != 0x3d) || bytes != 0) return -1;
    sim->twl = op == 0x3d;
    assert(!sim->twl || sim->twlStart);
    if (ImageRead(sim, sim->twl ? sim->twlStart : 0x4000, sim->secure, CP_CARD_SECURE) != 0) return -1;
    if (!sim->twl) {
        /* Image dumps contain the decrypted NTR area. The physical chip
         * supplies its first 2 KiB encrypted for the BIOS. */
        LoadKey(sim, 0, sim->key);
        CpCardKey_Init(sim->key, sim->gameCode, 3);
        memcpy(sim->secure, "encryObj", 8);
        for (unsigned i = 0; i < 0x800; i += 8) EncryptWord(sim->key, sim->secure + i);
    }
    LoadKey(sim, sim->twl, sim->key);
    CpCardKey_Init(sim->key, sim->gameCode, sim->twl ? 1 : 2);
    if (!sim->twl) EncryptWord(sim->key, sim->secure);
    sim->mode = 1;
    return 0;
}

static void Seed(void *ctx, uint64_t seed0)
{
    SimCard *sim = ctx;
    assert(sim->counter == 1 && (seed0 >> 8) == 0x60);
    sim->seeded = 1;
}

static void Delay(void *ctx, unsigned us)
{
    SimCard *sim = ctx;
    assert(us && us <= 132000);
    sim->delayed = 1;
}

void SimCard_Init(SimCard *sim, const CpReader *image, uint32_t id)
{
    uint8_t header[CP_CARD_PAGE];
    memset(sim, 0, sizeof(*sim));
    sim->bus = (CpCardBus){ sim, Reset, Transfer, Seed, Delay, LoadKey, 0 };
    sim->image = image;
    sim->id = id;
    sim->present = 1;
    assert(ImageRead(sim, 0, header, sizeof header) == 0);
    sim->gameCode = CpLe32(header + 12);
    if (header[0x12] & 2) sim->twlStart = CpLe32(header + 0x1c0);
}
