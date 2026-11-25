#ifndef COMPANION_TEST_SAVE_FIXTURE_H
#define COMPANION_TEST_SAVE_FIXTURE_H

/* Synthetic Platinum saves for the adapter tests: no ROM, save or player
 * data. Pokémon are built here and encrypted with WtPokemon_Encode; the party
 * extension and the empty slots are encrypted by this file's own LCG. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "adapter/platinum.h"
#include "save/save_image.h"
#include "save/sha256.h"
#include "wonder_trade/pokemon_codec.h"

/* LUNE-880: FX_GAME picks the game whose saves these are: 0 Platinum (the
 * default), 1 Diamond/Pearl, 2 HeartGold/SoulSilver. FX_FOOTER is the block
 * footer's size, FX_BOX_STRIDE the distance between PC boxes and FX_SHARED
 * marks HeartGold/SoulSilver's one counter per half. */
#ifndef FX_GAME
#define FX_GAME 0
#endif
#if FX_GAME == 0
enum {
    FX_GENERAL = 0, FX_GENERAL_SIZE = 53036, FX_BOXES = 53036, FX_BOXES_SIZE = 74212,
    FX_PARTY = 152, FX_MON = 236, FX_BOX_MON = 136, FX_PC = 4
};
enum { FX_FOOTER = 20, FX_BOX_STRIDE = 30 * 136 };
#define FX_SHARED 0
#define FX_LAYOUT (&CpSaveLayout_Platinum)
#define FX_GAME_NAME "Platinum"
enum { FX_DAYCARE = 0x1654, FX_GTS = 0x8080, FX_PAL_PARK = 0xC7F0 };
#elif FX_GAME == 1
enum {
    FX_GENERAL = 0, FX_GENERAL_SIZE = 0xC100, FX_BOXES = 0xC100, FX_BOXES_SIZE = 0x121E0,
    FX_PARTY = 0x90, FX_MON = 236, FX_BOX_MON = 136, FX_PC = 4
};
enum { FX_FOOTER = 20, FX_BOX_STRIDE = 30 * 136 };
#define FX_SHARED 0
#define FX_LAYOUT (&CpSaveLayout_DiamondPearl)
#define FX_GAME_NAME "Diamond/Pearl"
enum { FX_DAYCARE = 0x141C, FX_GTS = 0x73E8, FX_PAL_PARK = 0xBA28 };
#elif FX_GAME == 2
enum {
    FX_GENERAL = 0, FX_GENERAL_SIZE = 0xF628, FX_BOXES = 0xF700, FX_BOXES_SIZE = 0x12310,
    FX_PARTY = 0x90, FX_MON = 236, FX_BOX_MON = 136, FX_PC = 0
};
enum { FX_FOOTER = 16, FX_BOX_STRIDE = 0x1000 };
#define FX_SHARED 1
#define FX_LAYOUT (&CpSaveLayout_HeartGoldSoulSilver)
#define FX_GAME_NAME "HeartGold/SoulSilver"
enum { FX_DAYCARE = 0x15FC, FX_GTS = 0x6910, FX_PAL_PARK = 0xB3C0, FX_WALKER_MON = 0xE5E0 };
#endif
/* LUNE-913: the Day Care's second DaycareMon and the GTS's pokemonListed
 * follow the first Pokémon 0xEC on; Pal Park's six Pokémon are 0xEC apart. */
enum { FX_DAYCARE_SLOT = 0xEC, FX_GTS_FLAG = FX_GTS + 0xEC };
/* The Aprijuice modifiers after the party (HeartGold/SoulSilver), and the
 * PC as the bytes from its first slot to past its last. */
enum { FX_APRIJUICE = FX_PARTY + 8 + 6 * FX_MON, FX_PC_SPAN = 17 * FX_BOX_STRIDE + 30 * FX_BOX_MON };

typedef struct FxFile {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    unsigned writes, failWrite;
} FxFile;

static inline uint32_t Fx32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void FxPut16(unsigned char *p, unsigned value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }
static inline void FxPut32(unsigned char *p, uint32_t value) { FxPut16(p, value & 65535); FxPut16(p + 2, value >> 16); }

static inline int FxRead(void *context, uint32_t offset, void *data, size_t size)
{
    FxFile *file = context;
    if (offset > sizeof(file->bytes) || size > sizeof(file->bytes) - offset) return -1;
    memcpy(data, file->bytes + offset, size);
    return 0;
}

static inline int FxWrite(void *context, uint32_t offset, const void *data, size_t size)
{
    FxFile *file = context;
    if (++file->writes == file->failWrite) return -1;
    if (offset > sizeof(file->bytes) || size > sizeof(file->bytes) - offset) return -1;
    memcpy(file->bytes + offset, data, size);
    return 0;
}

static inline int FxSync(void *context) { (void)context; return 0; }
static inline int FxSize(void *context, uint64_t *size) { (void)context; *size = CP_SAVE_FILE_SIZE; return 0; }

/* A separate formulation of the game's EncodeData, halfword by halfword. */
static inline void FxCrypt(unsigned char *bytes, unsigned size, uint32_t seed)
{
    unsigned i;
    for (i = 0; i < size / 2; ++i) {
        unsigned word;
        seed = seed * 1103515245u + 24691u;
        word = (bytes[2 * i] | (bytes[2 * i + 1] << 8)) ^ (seed >> 16);
        bytes[2 * i] = (unsigned char)word; bytes[2 * i + 1] = (unsigned char)(word >> 8);
    }
}

static inline unsigned FxCrc(const unsigned char *p, unsigned size)
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

static inline void FxSeal(FxFile *file, unsigned half, unsigned block, uint32_t global, uint32_t counter)
{
    static const unsigned offsets[2] = { FX_GENERAL, FX_BOXES }, sizes[2] = { FX_GENERAL_SIZE, FX_BOXES_SIZE };
    unsigned char *body = file->bytes + half * CP_SAVE_HALF_SIZE + offsets[block];
    unsigned char *footer = body + sizes[block] - FX_FOOTER;
    unsigned crc = FxCrc(body, sizes[block] - FX_FOOTER);
#if FX_SHARED
    /* {counter, size, magic, block u16, CRC u16}: the one counter is global. */
    (void)counter;
    FxPut32(footer, global); FxPut32(footer + 4, sizes[block]); FxPut32(footer + 8, CP_SAVE_MAGIC_INTERNATIONAL);
    FxPut16(footer + 12, block); FxPut16(footer + 14, crc);
#else
    FxPut32(footer, global); FxPut32(footer + 4, counter); FxPut32(footer + 8, sizes[block]);
    FxPut32(footer + 12, CP_SAVE_MAGIC_INTERNATIONAL); footer[16] = (unsigned char)block; footer[17] = 0;
    footer[18] = (unsigned char)crc; footer[19] = (unsigned char)(crc >> 8);
#endif
}

/* The PC slot at index box * 30 + slot from the start of the box block. */
static inline unsigned char *FxPcSlot(unsigned char *boxes, unsigned index)
{
    return boxes + FX_PC + index / 30 * FX_BOX_STRIDE + index % 30 * FX_BOX_MON;
}

static inline const unsigned char *FxPcSlotConst(const unsigned char *boxes, unsigned index)
{
    return boxes + FX_PC + index / 30 * FX_BOX_STRIDE + index % 30 * FX_BOX_MON;
}

/* The save layout these fixtures build, with the international magic. */
static inline CpSaveResult FxLoad(CpSaveImage *image, const CpFile *file)
{
    return CpSaveImage_LoadLayout(image, file, FX_LAYOUT, CP_SAVE_MAGIC_INTERNATIONAL);
}

/* Pokemon_Init's empty party Pokémon, or BoxPokemon_Init's for 136 bytes. */
static inline void FxEmpty(unsigned char *bytes, unsigned size)
{
    memset(bytes, 0, size);
    FxCrypt(bytes + 8, 128, 0);
    if (size == FX_MON) FxCrypt(bytes + FX_BOX_MON, 100, 0);
}

typedef struct FxSpec {
    uint32_t pid;
    unsigned species, item, form, ball, egg, hp, capsule;
} FxSpec;

static inline void FxCanonical(const FxSpec *spec, unsigned char canonical[136])
{
    unsigned sum = 0, i;
    memset(canonical, 0, 136);
    FxPut32(canonical, spec->pid);
    FxPut16(canonical + 0x08, spec->species); FxPut16(canonical + 0x0A, spec->item);
    FxPut32(canonical + 0x0C, 12345); /* OT ID */
    FxPut32(canonical + 0x10, spec->pid % 9973); /* experience: distinct per Pokémon */
    FxPut16(canonical + 0x28, 33); /* a move */
    FxPut32(canonical + 0x38, (spec->pid & 0x3fffffff) | ((uint32_t)(spec->egg & 1) << 30));
    canonical[0x40] = (unsigned char)(spec->form << 3);
    canonical[0x83] = (unsigned char)spec->ball;
    for (i = 8; i < 136; i += 2) sum += canonical[i] | (canonical[i + 1] << 8);
    FxPut16(canonical + 6, sum & 65535);
}

/* A party image: the encrypted box Pokémon, then its encrypted extension. */
static inline void FxPartyMon(const FxSpec *spec, unsigned char image[FX_MON])
{
    unsigned char canonical[136], *extension = image + FX_BOX_MON;
    FxCanonical(spec, canonical);
    assert(WtPokemon_Encode(canonical, image) == 0);
    memset(extension, 0, 100);
    extension[4] = 5; extension[5] = (unsigned char)spec->capsule;
    FxPut16(extension + 6, spec->hp); FxPut16(extension + 8, 40);
    FxCrypt(extension, 100, spec->pid);
}

static inline FxSpec FxDefault(unsigned index)
{
    FxSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.pid = 0x1000193u * (index + 1) + 77; spec.species = 25 + index; spec.ball = 4; spec.hp = 20;
    return spec;
}

/* Play in the unmodified game: the Pokémon gains experience. Its identity
 * (personality and original trainer ID) never changes. */
static inline void FxTrain(unsigned char *stored)
{
    unsigned char canonical[136];
    unsigned sum = 0, i;
    assert(WtPokemon_Decode(stored, canonical) == 0);
    FxPut32(canonical + 0x10, Fx32(canonical + 0x10) + 100); /* experience */
    for (i = 8; i < 136; i += 2) sum += canonical[i] | (canonical[i + 1] << 8);
    FxPut16(canonical + 6, sum & 65535);
    assert(WtPokemon_Encode(canonical, stored) == 0);
}

static inline unsigned char *FxGeneral(FxFile *file, unsigned half) { return file->bytes + half * CP_SAVE_HALF_SIZE + FX_GENERAL; }
static inline unsigned char *FxBoxes(FxFile *file, unsigned half) { return file->bytes + half * CP_SAVE_HALF_SIZE + FX_BOXES; }

/* A save whose newer half (1) holds count party Pokémon from specs; the older
 * half differs, as a retail save's alternate copy does. */
static inline void FxBuild(FxFile *file, const FxSpec *specs, unsigned count)
{
    unsigned half, i;
    memset(file, 0, sizeof(*file));
    for (half = 0; half < 2; ++half) {
        unsigned char *general = FxGeneral(file, half), *boxes = FxBoxes(file, half);
        for (i = 0; i < FX_GENERAL_SIZE - FX_FOOTER; ++i) general[i] = (unsigned char)(i * 7 + half);
        for (i = 0; i < FX_BOXES_SIZE - FX_FOOTER; ++i) boxes[i] = (unsigned char)(i * 11 + 3);
        FxPut32(general + FX_PARTY, 6); FxPut32(general + FX_PARTY + 4, count);
        for (i = 0; i < 6; ++i) {
            unsigned char *slot = general + FX_PARTY + 8 + i * FX_MON;
            if (i < count) FxPartyMon(&specs[i], slot); else FxEmpty(slot, FX_MON);
        }
#if FX_GAME == 2
        memset(general + FX_APRIJUICE, 0, 30); /* no Aprijuice modifiers */
        memset(general + FX_WALKER_MON, 0, FX_BOX_MON); /* no Pokémon on a walk */
#endif
        /* LUNE-913: nothing at the Day Care, the GTS or Pal Park. */
        FxEmpty(general + FX_DAYCARE, FX_BOX_MON); FxEmpty(general + FX_DAYCARE + FX_DAYCARE_SLOT, FX_BOX_MON);
        memset(general + FX_GTS, 0, FX_GTS_FLAG + 2 - FX_GTS);
        for (i = 0; i < 6; ++i) FxEmpty(general + FX_PAL_PARK + i * FX_MON, FX_MON);
        for (i = 0; i < 18 * 30; ++i) FxEmpty(FxPcSlot(boxes, i), FX_BOX_MON);
        FxSeal(file, half, 0, 10 + half, 20 + half);
        FxSeal(file, half, 1, 10 + half, 30 + half);
    }
}

static inline unsigned char *FxImageParty(CpSaveImage *image, unsigned slot)
{
    return CpSaveImage_Block(image, 0) + FX_PARTY + 8 + slot * FX_MON;
}

static inline unsigned FxImageCount(CpSaveImage *image)
{
    return Fx32(CpSaveImage_Block(image, 0) + FX_PARTY + 4);
}

static inline unsigned char *FxImageBox(CpSaveImage *image, unsigned box, unsigned slot)
{
    return FxPcSlot(CpSaveImage_Block(image, 1), box * 30 + slot);
}

static inline int FxSha(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, bytes, size);
    CpSaveSha256_Finish(&hash, digest);
    return 0;
}

#endif
