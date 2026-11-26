#ifndef COMPANION_TEST_GEN5_FIXTURE_H
#define COMPANION_TEST_GEN5_FIXTURE_H

/* LUNE-881: synthetic Black/White (FX5_GAME 0) and Black 2/White 2 (1) saves
 * for the adapter tests: no ROM, save or player data. Both copies hold the
 * same save, sealed by this file's own CRC; Pokémon are built here and
 * encrypted with WtPokemon_Encode, their battle stats by this file's LCG. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "adapter/gen5.h"
#include "save/save_image.h"
#include "save/sha256.h"
#include "wonder_trade/pokemon_codec.h"

#ifndef FX5_GAME
#define FX5_GAME 0
#endif
#if FX5_GAME == 0
#define FX5_LAYOUT (&CpSaveLayout_BlackWhite)
#define FX5_NAME "Black/White"
enum { FX5_BACKUP = 0x24000, FX5_USED = 0x23F9C, FX5_BLOCKS = 69, FX5_POKEDEX = 0x37, FX5_POKEDEX_SIZE = 0x4D4,
       FX5_JOURNAL = 0x74000, FX5_COUNTS = 0x44, FX5_MUSICAL = 0x56000, FX5_MUSICAL_USED = 0x1FD14,
       FX5_MUSICAL_BLOCK = 0x1FC00 };
#else
#define FX5_LAYOUT (&CpSaveLayout_Black2White2)
#define FX5_NAME "Black 2/White 2"
enum { FX5_BACKUP = 0x26000, FX5_USED = 0x25FA4, FX5_BLOCKS = 73, FX5_POKEDEX = 0x36, FX5_POKEDEX_SIZE = 0x4DC,
       FX5_JOURNAL = 0x6B000, FX5_COUNTS = 0x5C, FX5_MUSICAL = 0x55800, FX5_MUSICAL_USED = 0x17D14,
       FX5_MUSICAL_BLOCK = 0x17C00 };
#endif
enum { FX5_PARTY = 0x1A, FX5_TRAINER = 0x1B, FX5_MUSICAL_SAVE = 0x2A, FX5_BATTLE_BOX = 0x31, FX5_MON = 220,
       FX5_BOX_MON = 136 };
/* LUNE-913: the other places the save holds a Pokémon. The Day Care: two
 * {BOOL used; party Pokémon; u32 steps} slots 0xE4 apart; the Dream World's
 * tucked-in Pokémon at 8, asleep at 0x1A3; the GTS deposit at 0, its flag
 * (u16) at 0xEC; Black 2/White 2's DNA Splicers partner at 4. */
enum { FX5_DAYCARE = 0x32, FX5_DAYCARE_SLOT = 0xE4, FX5_DREAM_WORLD = 0x23, FX5_GTS = 0x2E, FX5_FUSION = 0x2B };

typedef struct Fx5File {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    unsigned writes, failWrite;
} Fx5File;

static inline unsigned Fx5R16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static inline uint32_t Fx5R32(const unsigned char *p) { return Fx5R16(p) | ((uint32_t)Fx5R16(p + 2) << 16); }
static inline void Fx5W16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static inline void Fx5W32(unsigned char *p, uint32_t v) { Fx5W16(p, v & 0xFFFF); Fx5W16(p + 2, v >> 16); }

static inline int Fx5Read(void *context, uint32_t offset, void *data, size_t size)
{
    Fx5File *file = context;
    if (offset > sizeof(file->bytes) || size > sizeof(file->bytes) - offset) return -1;
    memcpy(data, file->bytes + offset, size);
    return 0;
}

static inline int Fx5Write(void *context, uint32_t offset, const void *data, size_t size)
{
    Fx5File *file = context;
    if (++file->writes == file->failWrite) return -1;
    if (offset > sizeof(file->bytes) || size > sizeof(file->bytes) - offset) return -1;
    memcpy(file->bytes + offset, data, size);
    return 0;
}

static inline int Fx5Sync(void *context) { (void)context; return 0; }
static inline int Fx5Size(void *context, uint64_t *size) { (void)context; *size = CP_SAVE_FILE_SIZE; return 0; }

/* The game's LCG, halfword by halfword. */
static inline void Fx5Crypt(unsigned char *bytes, unsigned size, uint32_t seed)
{
    unsigned i;
    for (i = 0; i < size / 2; ++i) {
        unsigned word;
        seed = seed * 0x41C64E6Du + 0x6073u;
        word = Fx5R16(bytes + 2 * i) ^ (seed >> 16);
        Fx5W16(bytes + 2 * i, word);
    }
}

static inline unsigned Fx5Crc(const unsigned char *p, unsigned size)
{
    unsigned crc = 0xFFFF, i;
    while (size--) {
        unsigned byte = *p++;
        for (i = 0; i < 8; ++i) {
            unsigned carry = ((crc >> 15) ^ (byte >> 7)) & 1;
            crc = ((crc << 1) ^ (carry ? 0x1021 : 0)) & 0xFFFF;
            byte <<= 1;
        }
    }
    return crc;
}

static inline unsigned Fx5Size_(unsigned id) { return FX5_LAYOUT->gen5->blockSizes[id]; }

static inline unsigned Fx5Offset(unsigned id)
{
    unsigned i, at = 0;
    for (i = 0; i < id; ++i) at = (at + Fx5Size_(i) + 4 + 0xFF) & ~0xFFu;
    return at;
}

static inline unsigned char *Fx5Block(Fx5File *file, unsigned copy, unsigned id)
{
    return file->bytes + (copy ? FX5_BACKUP : 0) + Fx5Offset(id);
}

static inline unsigned Fx5TableSize(void) { return FX5_BLOCKS * 2 + (FX5_BLOCKS & 1) * 2; }

/* Seal a copy as the game's save leaves it: block footers, CRC table, footer. */
static inline void Fx5Seal(Fx5File *file, unsigned copy, uint32_t count)
{
    unsigned char *base = file->bytes + (copy ? FX5_BACKUP : 0), *table = base + FX5_USED - 16 - Fx5TableSize();
    unsigned id;
    for (id = 0; id < FX5_BLOCKS; ++id) {
        unsigned char *block = base + Fx5Offset(id);
        unsigned crc = Fx5Crc(block, Fx5Size_(id));
        Fx5W16(block + Fx5Size_(id) + 2, crc);
        Fx5W16(table + 2 * id, crc);
    }
    Fx5W32(base + FX5_USED - 16, count);
    Fx5W32(base + FX5_USED - 12, FX5_USED);
    Fx5W32(base + FX5_USED - 8, CP_SAVE_MAGIC_GEN5);
    Fx5W16(base + FX5_USED - 2, Fx5Crc(table, Fx5TableSize()));
}

/* Both copies sealed from the main copy's blocks. */
static inline void Fx5SealBoth(Fx5File *file, uint32_t count)
{
    Fx5Seal(file, 0, count);
    memcpy(file->bytes + FX5_BACKUP, file->bytes, FX5_USED);
}

/* POKE_TOOL_ClearPartyParam's empty Pokémon, or its first 136 bytes. */
static inline void Fx5Empty(unsigned char *bytes, unsigned size)
{
    unsigned char empty[FX5_MON];
    memset(empty, 0, sizeof(empty));
    Fx5Crypt(empty + 8, 128, 0);
    Fx5Crypt(empty + FX5_BOX_MON, 84, 0);
    memcpy(bytes, empty, size);
}

typedef struct Fx5Spec {
    uint32_t pid;
    unsigned species, item, form, ball, egg, hp, language;
} Fx5Spec;

static inline void Fx5Canonical(const Fx5Spec *spec, unsigned char canonical[136])
{
    unsigned sum = 0, i;
    memset(canonical, 0, 136);
    Fx5W32(canonical, spec->pid);
    Fx5W16(canonical + 0x08, spec->species); Fx5W16(canonical + 0x0A, spec->item);
    Fx5W32(canonical + 0x0C, 54321); /* OT ID */
    Fx5W32(canonical + 0x10, spec->pid % 9973); /* experience: distinct per Pokémon */
    canonical[0x17] = (unsigned char)(spec->language ? spec->language : 2);
    Fx5W16(canonical + 0x28, 33); /* a move */
    Fx5W32(canonical + 0x38, (spec->pid & 0x3fffffff) | ((uint32_t)(spec->egg & 1) << 30));
    canonical[0x40] = (unsigned char)(spec->form << 3);
    canonical[0x5F] = 21; /* Black */
    canonical[0x83] = (unsigned char)spec->ball;
    for (i = 8; i < 136; i += 2) sum += Fx5R16(canonical + i);
    Fx5W16(canonical + 6, sum & 0xFFFF);
}

static inline void Fx5PartyMon(const Fx5Spec *spec, unsigned char image[FX5_MON])
{
    unsigned char canonical[136], *extension = image + FX5_BOX_MON;
    Fx5Canonical(spec, canonical);
    assert(WtPokemon_Encode(canonical, image) == 0);
    memset(extension, 0, 84);
    extension[4] = 5;
    Fx5W16(extension + 6, spec->hp); Fx5W16(extension + 8, 40);
    Fx5Crypt(extension, 84, spec->pid);
}

static inline Fx5Spec Fx5Default(unsigned index)
{
    Fx5Spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.pid = 0x1000193u * (index + 1) + 77; spec.species = 495 + index; spec.ball = 4; spec.hp = 20;
    return spec;
}

/* A played save: count party Pokémon from specs, an empty PC and Battle Box,
 * an empty Pokédex, no marker set, blank journal banks; both copies alike. */
static inline void Fx5Build(Fx5File *file, const Fx5Spec *specs, unsigned count, uint32_t saveCount)
{
    unsigned id, i;
    memset(file, 0, sizeof(*file));
    for (id = 0; id < FX5_BLOCKS; ++id) {
        unsigned char *block = Fx5Block(file, 0, id);
        for (i = 0; i < Fx5Size_(id); ++i) block[i] = (unsigned char)(i * 7 + id);
        Fx5W16(block + Fx5Size_(id), 3 + id);
    }
    for (id = 1; id <= 24; ++id) for (i = 0; i < 30; ++i) Fx5Empty(Fx5Block(file, 0, id) + i * FX5_BOX_MON, FX5_BOX_MON);
    for (i = 0; i < 6; ++i) Fx5Empty(Fx5Block(file, 0, FX5_BATTLE_BOX) + i * FX5_BOX_MON, FX5_BOX_MON);
    {
        unsigned char *party = Fx5Block(file, 0, FX5_PARTY);
        Fx5W32(party, 6); Fx5W32(party + 4, count);
        for (i = 0; i < 6; ++i) {
            if (i < count) Fx5PartyMon(&specs[i], party + 8 + i * FX5_MON);
            else Fx5Empty(party + 8 + i * FX5_MON, FX5_MON);
        }
    }
    /* LUNE-913: nothing in the Day Care, the Dream World, the GTS or (Black
     * 2/White 2) fused into Kyurem: blocks zeroed as their inits leave them. */
    memset(Fx5Block(file, 0, FX5_DAYCARE), 0, Fx5Size_(FX5_DAYCARE));
    memset(Fx5Block(file, 0, FX5_DREAM_WORLD), 0, Fx5Size_(FX5_DREAM_WORLD));
    memset(Fx5Block(file, 0, FX5_GTS), 0, Fx5Size_(FX5_GTS));
#if FX5_GAME == 1
    memset(Fx5Block(file, 0, FX5_FUSION), 0, Fx5Size_(FX5_FUSION));
#endif
    memset(Fx5Block(file, 0, FX5_POKEDEX), 0, FX5_POKEDEX_SIZE);
    Fx5W32(Fx5Block(file, 0, FX5_POKEDEX), 0xBEEFCAFE);
    memset(Fx5Block(file, 0, FX5_TRAINER) + 0x2C, 0, Fx5Size_(FX5_TRAINER) - 0x2C);
    Fx5Block(file, 0, FX5_MUSICAL_SAVE)[0x29E] = 0;
    memset(file->bytes + FX5_JOURNAL, 0xFF, 0x2000);
    Fx5SealBoth(file, saveCount);
}

static inline CpSaveResult Fx5Load(CpSaveImage *image, const CpFile *file)
{
    return CpSaveImage_LoadLayout(image, file, FX5_LAYOUT, CP_SAVE_MAGIC_GEN5);
}

static inline unsigned char *Fx5ImageParty(CpSaveImage *image, unsigned slot)
{
    return CpSaveImage_Block(image, FX5_PARTY) + 8 + slot * FX5_MON;
}

static inline unsigned Fx5ImageCount(CpSaveImage *image) { return Fx5R32(CpSaveImage_Block(image, FX5_PARTY) + 4); }

static inline unsigned char *Fx5ImageBox(CpSaveImage *image, unsigned box, unsigned slot)
{
    return CpSaveImage_Block(image, 1 + box) + slot * FX5_BOX_MON;
}

static inline unsigned char *Fx5ImageBattleBox(CpSaveImage *image, unsigned slot)
{
    return CpSaveImage_Block(image, FX5_BATTLE_BOX) + slot * FX5_BOX_MON;
}

static inline int Fx5Sha(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, bytes, size);
    CpSaveSha256_Finish(&hash, digest);
    return 0;
}

#endif
