#ifndef COMPANION_GAME_ROM_H
#define COMPANION_GAME_ROM_H

#include "game/reader.h"

/* Trade families, as server/src/game_variant.h and protocol/wire.py number them. */
typedef enum CpFamily {
    CP_FAMILY_NONE = 0,
    CP_FAMILY_DP = 1,
    CP_FAMILY_PLATINUM = 2,
    CP_FAMILY_HGSS = 3,
    CP_FAMILY_BW = 4,
    CP_FAMILY_B2W2 = 5
} CpFamily;

/* The games' own version and language IDs. Language 6 is unused. */
enum {
    CP_VERSION_HEARTGOLD = 7, CP_VERSION_SOULSILVER = 8,
    CP_VERSION_DIAMOND = 10, CP_VERSION_PEARL = 11, CP_VERSION_PLATINUM = 12,
    CP_VERSION_WHITE = 20, CP_VERSION_BLACK = 21, CP_VERSION_WHITE2 = 22, CP_VERSION_BLACK2 = 23
};
enum {
    CP_LANGUAGE_JAPANESE = 1, CP_LANGUAGE_ENGLISH = 2, CP_LANGUAGE_FRENCH = 3,
    CP_LANGUAGE_ITALIAN = 4, CP_LANGUAGE_GERMAN = 5, CP_LANGUAGE_SPANISH = 7,
    CP_LANGUAGE_KOREAN = 8
};

/* Code 1 is the legacy alias for US English Platinum that released clients
 * enrolled with. The companion always sends a structured code; the server
 * treats an enrolled alias and English Platinum's code as the same variant. */
#define CP_BUILD_CODE_LEGACY_PLATINUM 1u

/* Save block footer magic: every Korean Generation IV game uses its own. */
#ifndef CP_SAVE_MAGIC_INTERNATIONAL
#define CP_SAVE_MAGIC_INTERNATIONAL 0x20060623u
#endif
#ifndef CP_SAVE_MAGIC_KOREAN
#define CP_SAVE_MAGIC_KOREAN 0x20070903u
#endif

/* What a cartridge header says about the game. */
typedef struct CpRomInfo {
    char gameCode[5]; /* header 0x0C, NUL-terminated */
    uint8_t revision; /* header 0x1E */
    CpFamily family;
    uint8_t version, language;
    uint32_t buildCode; /* the enroll build code */
} CpRomInfo;

/* family << 24 | version << 16 | language << 8 | revision */
uint32_t CpBuildCode(CpFamily, unsigned version, unsigned language, unsigned revision);

/* The trade family of an enroll build code, CP_FAMILY_NONE for one the server
 * would refuse. Code 1 is Platinum. */
CpFamily CpBuildCode_Family(uint32_t code);

/* Identify a Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White,
 * Black 2 or White 2 game code and header revision. CP_ERR_UNSUPPORTED for any
 * other game. */
int CpRom_FromCode(const char gameCode[4], uint8_t revision, CpRomInfo *out);

/* Read and identify the header of a ROM. */
int CpRom_Identify(const CpReader *, CpRomInfo *out);

/* What the save parser found in a .sav: the size field of the general
 * block's footer and that footer's magic. */
typedef struct CpSaveFingerprint {
    uint32_t generalSize;
    uint32_t footerMagic;
} CpSaveFingerprint;

/* The general block size every language of a Generation IV family uses: DP
 * 0xC100, Platinum 0xCF2C, HGSS 0xF628; 0 for none (and for Generation V). */
uint32_t CpRom_GeneralBlockSize(CpFamily);

/* 1 if the save can belong to this ROM: the same family's block size and the
 * Korean magic exactly when the ROM is Korean. */
int CpRom_MatchesSave(const CpRomInfo *, const CpSaveFingerprint *);

#endif
