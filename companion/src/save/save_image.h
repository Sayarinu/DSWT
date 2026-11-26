#ifndef CP_SAVE_IMAGE_H
#define CP_SAVE_IMAGE_H

#include "file.h"

/* The CP_SAVE_* layout constants are Platinum's (CpSaveLayout_Platinum). */
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
    CP_SAVE_HASH_SIZE = 32,
    /* The largest general block body of any layout (HeartGold/SoulSilver). */
    CP_SAVE_GENERAL_BODY_MAX = 0xF628 - 16
};

#define CP_SAVE_MAGIC_INTERNATIONAL UINT32_C(0x20060623)
#define CP_SAVE_MAGIC_KOREAN UINT32_C(0x20070903)
/* Black/White and Black 2/White 2, in every language. */
#define CP_SAVE_MAGIC_GEN5 UINT32_C(0x31053527)

/* How a game family keeps its save: the two retail blocks of each half,
 * their footer, and where the party and the PC sit in them. */
typedef enum {
    /* Diamond, Pearl and Platinum: a 20-byte footer {global counter, block
     * counter, size, magic, block u8, pad, CRC u16}. Each block alternates
     * between the halves on its own; a full save advances every counter. */
    CP_SAVE_FOOTER_BLOCK_COUNTERS,
    /* HeartGold and SoulSilver: a 16-byte footer {counter, size, magic,
     * block u16, CRC u16}. One counter per save; a half's two blocks are
     * loaded and written together (src/save.c Save_GetSaveFilesStatus). */
    CP_SAVE_FOOTER_SHARED_COUNTER,
    /* Black/White and Black 2/White 2: two copies of many blocks, each block
     * with a {count u16, CRC u16} footer, then a CRC of every block and a
     * 16-byte footer {save count, size, magic, pad u16, CRC of the CRC table}.
     * The game's backup system loads each block from whichever copy holds it
     * and writes only changed blocks (save/gen5_save.h). */
    CP_SAVE_FOOTER_GEN5
} CpSaveFooterKind;

enum { CP_SAVE_GEN5_BLOCKS_MAX = 73 };

/* What a Generation V layout adds: its blocks, and the parts of the save
 * control the game's load applies to the blocks it read. */
typedef struct CpSaveGen5 {
    /* The backup copy's flash address and the bytes of one copy the game
     * uses: blocks, CRC table and footer. */
    uint32_t backupOffset, dataSize;
    uint16_t blockCount;
    /* Block sizes in ID order; each block is followed by its 4-byte footer
     * and padded to 0x100 bytes (SAVEDATA_BuildLayout). */
    const uint16_t *blockSizes;
    /* The block IDs the adapter and the load use. */
    uint8_t partyBlock, boxTrayBlock, battleBoxBlock, pokedexBlock, trainerBlock, musicalBlock;
    /* In the trainer block: the intro save flag (a load treats such a save
     * as none), the extra saves' markers (u16, 0xC21E when set) and their
     * save counts (u32). In the musical block: whether a distributed musical
     * program loaded. */
    uint16_t introOffset, markerOffset, extraCountOffset, hasDistOffset;
    /* The musical distribution extra save (one copy, one block). */
    uint32_t musicalAddress, musicalDataSize, musicalBlockSize;
} CpSaveGen5;

typedef struct CpSaveLayout {
    CpSaveFooterKind footer;
    uint32_t footerSize;
    uint32_t offsets[2], sizes[2];
    /* The party (capacity, count, six party Pokémon) in the general block. */
    uint32_t partyOffset;
    /* The first PC Pokémon in the box block, and the distance between boxes. */
    uint32_t boxSlotsOffset, boxStride;
    /* Box block bytes the hashes read as zero, and what a commit writes there:
     * HeartGold/SoulSilver's PCStorage.boxModifiedFlag, the game's own note of
     * which boxes to write next, which both halves never share. Size 0: none. */
    uint32_t maskOffset, maskSize, maskCommit;
    /* Generation V only (CP_SAVE_FOOTER_GEN5); the fields above are unused. */
    const CpSaveGen5 *gen5;
} CpSaveLayout;

extern const CpSaveLayout CpSaveLayout_DiamondPearl, CpSaveLayout_Platinum, CpSaveLayout_HeartGoldSoulSilver;
extern const CpSaveLayout CpSaveLayout_BlackWhite, CpSaveLayout_Black2White2;

typedef enum {
    CP_SAVE_OK = 0,
    CP_SAVE_IO_ERROR,
    CP_SAVE_INVALID,
    /* Load and save once in the game first; no writable image is exposed. */
    CP_SAVE_NEEDS_GAME_SAVE,
    CP_SAVE_VERIFY_FAILED
} CpSaveResult;

/* Allocate outside the stack on the DS. Only block bodies returned by Block
 * may be edited; metadata and footers belong to this module. With a shared
 * counter, globalCounter and both blockCounters hold that one counter.
 * Generation V: bytes holds the save as the game holds it in RAM (its first
 * dataSize bytes), globalCounter and both blockCounters the save count, and
 * areas which copy each block, the CRC table and the footer must be written
 * to (0 neither, 1 main, 2 backup), as the game's backup system keeps them. */
typedef struct {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    const CpSaveLayout *layout;
    uint32_t globalCounter;
    uint32_t blockCounters[2];
    uint32_t magic;
    unsigned currentHalf[2];
    unsigned firstBlock;
    int loaded;
    uint32_t blockOffsets[CP_SAVE_GEN5_BLOCKS_MAX];
    unsigned char areas[CP_SAVE_GEN5_BLOCKS_MAX + 2];
    /* LUNE-902: CommitBoth writes the general block's (Generation V: each
     * copy's) footer save counter, synced, before the rest of it (the CRC and
     * block counter that make the new party load): no torn footer can
     * publish a removal with an older counter. */
    int counterFirst;
} CpSaveImage;

/* magic comes from the ROM's region: international or Korean.
 * Block counters: uses retail LoadCheck's chosen pair, including recovery
 * fallbacks, when its selected global counters agree and sector counters do
 * not contradict. Shared counter: the newest half whose blocks agree, else
 * the older one, as that game's LoadCheck chooses. Generation V (magic
 * CP_SAVE_MAGIC_GEN5): each block as BACKUP_SYSTEM_LoadDoubleCopy takes it,
 * then the save control's own changes (save/gen5_save.h); a save the game
 * would call broken, or an intro save it treats as none, needs a game save.
 * CpSaveImage_Load reads the Platinum layout. */
CpSaveResult CpSaveImage_LoadLayout(CpSaveImage *image, const CpFile *file, const CpSaveLayout *layout,
                                  uint32_t magic);
CpSaveResult CpSaveImage_Load(CpSaveImage *image, const CpFile *file, uint32_t magic);
/* Any supported layout with either magic, for checks that only need a retail
 * save the game would load (backups): the first that loads, else the most
 * telling failure. */
CpSaveResult CpSaveImage_LoadAny(CpSaveImage *image, const CpFile *file);
/* Gen 4: block 0 general, 1 PC. Gen 5: the body of block ID `block`. */
unsigned char *CpSaveImage_Block(CpSaveImage *image, unsigned block);
/* The size of a block's body. */
uint32_t CpSaveImage_BlockSize(const CpSaveImage *image, unsigned block);
/* SHA-256 of both block bodies as the game's adapter digests them (the
 * layout's mask read as zero). */
CpSaveResult CpSaveImage_Hash(const CpSaveImage *image, unsigned char digest[CP_SAVE_HASH_SIZE]);
/* Re-reads both complete retail copies, checking bodies, CRCs, magic, IDs and
 * counters equal to the image's counters or one behind, as the client does.
 * With a shared counter both blocks of a half must carry the same one. */
CpSaveResult CpSaveImage_Verify(const CpSaveImage *image, const CpFile *file,
                              const unsigned char expected[CP_SAVE_HASH_SIZE]);
/* Always performs two alternating full saves, incrementing global and each
 * block counter per pass. Bodies become identical; footers have adjacent
 * counters. A fallback block is published first, then general-first saves resume.
 * Each block is written as its game writes it: block counters write the body,
 * then the footer's size and magic, then the whole footer; HeartGold/
 * SoulSilver write the general body and footer, then the PC box by box, the
 * rest of the box block and its footer, with every box marked modified.
 * Generation V saves as SAVE_CONTROL_MainSave does (save/gen5_save.h), and
 * once more with every block when the copies then differ from the image.
 * Only the two retail blocks are written. On any write/sync failure
 * the image is invalidated: reload/recover before any retry. */
CpSaveResult CpSaveImage_CommitBoth(CpSaveImage *image, const CpFile *file,
                                  const unsigned char expected[CP_SAVE_HASH_SIZE]);

#endif
