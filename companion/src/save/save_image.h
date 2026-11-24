#ifndef CP_SAVE_IMAGE_H
#define CP_SAVE_IMAGE_H

#include "file.h"

enum {
    CP_SAVE_FILE_SIZE = 0x80000,
    CP_SAVE_HALF_SIZE = 0x40000,
    CP_SAVE_FOOTER_SIZE = 20,
    CP_SAVE_GENERAL_OFFSET = 0,
    CP_SAVE_GENERAL_SIZE = 53036,
    CP_SAVE_BOX_OFFSET = 53036,
    CP_SAVE_BOX_SIZE = 74212,
    CP_SAVE_PARTY_CAPACITY_OFFSET = 152,
    CP_SAVE_PARTY_COUNT_OFFSET = 156,
    CP_SAVE_PARTY_SLOTS_OFFSET = 160,
    CP_SAVE_PARTY_STRIDE = 236,
    CP_SAVE_PARTY_CAPACITY = 6,
    CP_SAVE_BOX_SLOTS_OFFSET = 53040,
    CP_SAVE_BOX_STRIDE = 136,
    CP_SAVE_BOX_CAPACITY = 540,
    CP_SAVE_HASH_SIZE = 32
};

#define CP_SAVE_MAGIC_INTERNATIONAL UINT32_C(0x20060623)
#define CP_SAVE_MAGIC_KOREAN UINT32_C(0x20070903)

typedef enum {
    CP_SAVE_OK = 0,
    CP_SAVE_IO_ERROR,
    CP_SAVE_INVALID,
    /* Load and save once in the game first; no writable image is exposed. */
    CP_SAVE_NEEDS_GAME_SAVE,
    CP_SAVE_VERIFY_FAILED
} CpSaveResult;

/* Allocate outside the stack on the DS. Only block bodies returned by Block
 * may be edited; metadata and footers belong to this module. */
typedef struct {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    uint32_t globalCounter;
    uint32_t blockCounters[2];
    uint32_t magic;
    unsigned currentHalf[2];
    int loaded;
} CpSaveImage;

/* magic comes from the ROM's region; only Platinum international/Korean work.
 * Clean means retail LOAD_OK, with matching selected global counters. */
CpSaveResult CpSaveImage_Load(CpSaveImage *image, const CpFile *file, uint32_t magic);
unsigned char *CpSaveImage_Block(CpSaveImage *image, unsigned block);
CpSaveResult CpSaveImage_Hash(const CpSaveImage *image, unsigned char digest[CP_SAVE_HASH_SIZE]);
/* Re-reads both complete retail copies, checking bodies, CRCs, magic, IDs and
 * counters equal to the image's counters or one behind, as the client does. */
CpSaveResult CpSaveImage_Verify(const CpSaveImage *image, const CpFile *file,
                              const unsigned char expected[CP_SAVE_HASH_SIZE]);
/* Always performs two alternating full saves, incrementing global and each
 * block counter per pass. Bodies become identical; footers have adjacent
 * counters. Only the two retail blocks are written. On any write/sync failure
 * the image is invalidated: reload/recover before any retry. */
CpSaveResult CpSaveImage_CommitBoth(CpSaveImage *image, const CpFile *file,
                                  const unsigned char expected[CP_SAVE_HASH_SIZE]);

#endif
