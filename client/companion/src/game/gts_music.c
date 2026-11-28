#include "game/gts_music.h"
#include "game/nitrofs.h"
#include "save/sha256.h"
#include <stdlib.h>
#include <string.h>

/* File IDs and lengths inside the game's SDAT FAT, in sequence, bank and
 * wave archive order. Only pass the known retail sequences to the ARM7. */
typedef struct Track {
    const char *archive;
    uint16_t file[4];
    uint32_t length[4];
    const char *sha256;
} Track;

static const Track tracks[] = {
    [CP_FAMILY_DP] = {"data/sound/sound_data.sdat", {97,1308,1822,1825}, {7600,3948,458564,34312},
        "56ff1fe48fbb42df50582762153af0a0c986d13a9c9182bfc2c89f3107c76492"},
    [CP_FAMILY_PLATINUM] = {"data/sound/pl_sound_data.sdat", {97,1464,1982,1985}, {7600,3960,458564,34312},
        "54cb7b9ec879c8e83d52a827d901aba6aa64baa6e2508033fe933495508bad46"},
    [CP_FAMILY_HGSS] = {"data/sound/gs_sound_data.sdat", {128,1727,2286,2288}, {7948,4612,431288,29316},
        "2b222ac5f20ad892ae910ad31258f1102ccd603b919db58d4c78fe246778b1cb"},
    [CP_FAMILY_BW] = {"wb_sound_data.sdat", {155,1694,3188}, {6788,708,127924},
        "c02cf1d1ad4ce902a63cfcfea3a930c09b87fb6d9939605a76610a1f5a548977"},
    [CP_FAMILY_B2W2] = {"swan_sound_data.sdat", {154,2132,4078}, {6788,708,127924},
        "c02cf1d1ad4ce902a63cfcfea3a930c09b87fb6d9939605a76610a1f5a548977"}
};

void CpGtsMusic_Free(CpGtsMusic *song)
{
    free((void *)song->sequence);
    memset(song, 0, sizeof(*song));
}

int CpGtsMusic_Load(const CpReader *rom, CpFamily family, CpGtsMusic *out)
{
    if ((unsigned)family >= sizeof(tracks)/sizeof(*tracks) || !tracks[family].archive) return CP_ERR_UNSUPPORTED;
    const Track *track = &tracks[family];
    CpRomFile archive;
    uint8_t header[36], offsets[8], digest[32];
    if (CpNitroFs_Open(rom, track->archive, &archive) || CpRomFile_Read(&archive, 0, header, sizeof(header)) ||
        memcmp(header, "SDAT", 4)) return CP_ERR_FORMAT;
    uint32_t fat = CpLe32(header + 32);
    if (fat > archive.size || archive.size - fat < 12 ||
        CpRomFile_Read(&archive, fat, offsets, 4) || memcmp(offsets, "FAT ", 4)) return CP_ERR_FORMAT;
    uint32_t total = 0;
    for (unsigned i = 0; i < 4; ++i) total += track->length[i];
    uint8_t *buffer = malloc(total);
    if (!buffer) return CP_ERR_RANGE;
    uint8_t *cursor = buffer;
    for (unsigned i = 0; i < 4 && track->length[i]; ++i) {
        uint32_t entry = 12 + 16 * track->file[i];
        if (entry > archive.size - fat || CpRomFile_Read(&archive, fat + entry, offsets, 8) ||
            CpLe32(offsets + 4) != track->length[i] ||
            CpRomFile_Read(&archive, CpLe32(offsets), cursor, track->length[i])) goto fail;
        cursor += track->length[i];
    }
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, buffer, total);
    CpSaveSha256_Finish(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i)
        if (hex[digest[i] >> 4] != track->sha256[2*i] || hex[digest[i] & 15] != track->sha256[2*i+1]) goto fail;
    const uint8_t *wave = buffer + track->length[0] + track->length[1];
    *out = (CpGtsMusic){ buffer, buffer + track->length[0], {wave, track->length[3] ? wave + track->length[2] : NULL}, total, NULL };
    return CP_OK;
fail:
    free(buffer);
    return CP_ERR_FORMAT;
}
