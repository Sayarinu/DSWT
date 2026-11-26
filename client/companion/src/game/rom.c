#include "game/rom.h"

#include <string.h>

uint32_t CpBuildCode(CpFamily family, unsigned version, unsigned language, unsigned revision)
{
    return (uint32_t)family << 24 | (uint32_t)(version & 0xFF) << 16 | (uint32_t)(language & 0xFF) << 8 |
        (uint32_t)(revision & 0xFF);
}

CpFamily CpBuildCode_Family(uint32_t code)
{
    unsigned family = code >> 24, version = code >> 16 & 0xFF, language = code >> 8 & 0xFF;
    int game;

    if (code == CP_BUILD_CODE_LEGACY_PLATINUM) return CP_FAMILY_PLATINUM;
    game = (family == CP_FAMILY_DP && (version == CP_VERSION_DIAMOND || version == CP_VERSION_PEARL)) ||
        (family == CP_FAMILY_PLATINUM && version == CP_VERSION_PLATINUM) ||
        (family == CP_FAMILY_HGSS && (version == CP_VERSION_HEARTGOLD || version == CP_VERSION_SOULSILVER)) ||
        (family == CP_FAMILY_BW && (version == CP_VERSION_WHITE || version == CP_VERSION_BLACK)) ||
        (family == CP_FAMILY_B2W2 && (version == CP_VERSION_WHITE2 || version == CP_VERSION_BLACK2));
    return game && language >= 1 && language <= 8 && language != 6 ? (CpFamily)family : CP_FAMILY_NONE;
}

static const struct {
    char prefix[4];
    CpFamily family;
    uint8_t version;
} kGames[] = {
    { "ADA", CP_FAMILY_DP, CP_VERSION_DIAMOND },
    { "APA", CP_FAMILY_DP, CP_VERSION_PEARL },
    { "CPU", CP_FAMILY_PLATINUM, CP_VERSION_PLATINUM },
    { "IPK", CP_FAMILY_HGSS, CP_VERSION_HEARTGOLD },
    { "IPG", CP_FAMILY_HGSS, CP_VERSION_SOULSILVER },
    { "IRB", CP_FAMILY_BW, CP_VERSION_BLACK },
    { "IRA", CP_FAMILY_BW, CP_VERSION_WHITE },
    { "IRE", CP_FAMILY_B2W2, CP_VERSION_BLACK2 },
    { "IRD", CP_FAMILY_B2W2, CP_VERSION_WHITE2 },
};

/* The game code's last letter is the region. European English cartridges use
 * E, like the US ones; their header revision tells them apart. Generation V's
 * English cartridges, US and European alike, use O instead. */
static const struct {
    char region;
    uint8_t language;
} kRegions[] = {
    { 'J', CP_LANGUAGE_JAPANESE }, { 'E', CP_LANGUAGE_ENGLISH }, { 'F', CP_LANGUAGE_FRENCH },
    { 'I', CP_LANGUAGE_ITALIAN }, { 'D', CP_LANGUAGE_GERMAN }, { 'S', CP_LANGUAGE_SPANISH },
    { 'K', CP_LANGUAGE_KOREAN },
};

int CpRom_FromCode(const char gameCode[4], uint8_t revision, CpRomInfo *out)
{
    size_t i, j;

    for (i = 0; i < sizeof kGames / sizeof kGames[0]; ++i) {
        if (memcmp(gameCode, kGames[i].prefix, 3) != 0) continue;
        int gen5 = kGames[i].family == CP_FAMILY_BW || kGames[i].family == CP_FAMILY_B2W2;
        for (j = 0; j < sizeof kRegions / sizeof kRegions[0]; ++j) {
            char region = kRegions[j].region == 'E' && gen5 ? 'O' : kRegions[j].region;
            if (gameCode[3] != region) continue;
            memcpy(out->gameCode, gameCode, 4);
            out->gameCode[4] = '\0';
            out->revision = revision;
            out->family = kGames[i].family;
            out->version = kGames[i].version;
            out->language = kRegions[j].language;
            out->buildCode = CpBuildCode(out->family, out->version, out->language, revision);
            return CP_OK;
        }
    }
    return CP_ERR_UNSUPPORTED;
}

int CpRom_Identify(const CpReader *reader, CpRomInfo *out)
{
    uint8_t header[0x20];
    CpRomFile rom;

    CpRomFile_Whole(reader, &rom);
    if (CpRomFile_Read(&rom, 0, header, sizeof header) != CP_OK) return CP_ERR_IO;
    return CpRom_FromCode((const char *)header + 0x0C, header[0x1E], out);
}

uint32_t CpRom_GeneralBlockSize(CpFamily family)
{
    switch (family) {
    case CP_FAMILY_DP: return 0xC100;
    case CP_FAMILY_PLATINUM: return 0xCF2C;
    case CP_FAMILY_HGSS: return 0xF628;
    default: return 0;
    }
}

int CpRom_MatchesSave(const CpRomInfo *rom, const CpSaveFingerprint *save)
{
    uint32_t magic = rom->language == CP_LANGUAGE_KOREAN ? CP_SAVE_MAGIC_KOREAN : CP_SAVE_MAGIC_INTERNATIONAL;
    uint32_t size = CpRom_GeneralBlockSize(rom->family);

    return size != 0 && save->generalSize == size && save->footerMagic == magic;
}
