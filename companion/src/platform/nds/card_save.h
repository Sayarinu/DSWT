#ifndef COMPANION_NDS_CARD_SAVE_H
#define COMPANION_NDS_CARD_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Retail save memory (ADVANsCEne Nintendo DS Collection):
 *   Diamond, Pearl, Platinum                 512 KiB SPI FLASH
 *   HeartGold, SoulSilver                    512 KiB SPI FLASH behind IR
 *   Black, White, Black 2, White 2           512 KiB SPI FLASH behind IR
 * https://www.advanscene.com/html/dats.php (entries 1057/58, 5115, 4903/04,
 * 5803/04 and 6391/92 each specify 4-Mbit flash). GBATEK records
 * a 62:11:00 Sanyo in Diamond and the IR board in Black. Board lots vary:
 * identify the memory itself before choosing commands or erase geometry.
 * https://problemkaputt.de/gbatek.htm (DS Cartridge Backup, DS Cart Infrared
 * SPI Commands and Component Lists, DS Firmware Serial Flash Memory).
 * Chip references: Sanyo LE25FW403A (A1229), ST M45PE40, M25PE40 and M25P40,
 * ST M95040-W and Cypress FM25V02A (001-90865). The latter two describe the
 * EEPROM and F-RAM protocols; neither is a claimed retail Pokémon variant.
 */
#ifndef CP_CARD_SAVE_WRITES
#define CP_CARD_SAVE_WRITES 0
#endif

typedef enum {
    CP_CHIP_UNKNOWN, CP_CHIP_EEPROM, CP_CHIP_FLASH, CP_CHIP_FRAM
} CpSaveChipType;

typedef struct {
    CpSaveChipType type;
    uint32_t size;
    uint32_t pageSize;   /* Program boundary, zero for streaming F-RAM. */
    uint32_t eraseSize;  /* Smallest erase unit, zero for EEPROM/F-RAM. */
    uint32_t sectorSize; /* Largest supported single erase. */
    uint8_t jedec[9];
    uint8_t jedecLength; /* Zero for the ID-less 512-byte DS EEPROM. */
    bool infrared;
} CpCardSave;

typedef enum {
    CP_CHIP_OK, CP_CHIP_RANGE, CP_CHIP_UNSUPPORTED, CP_CHIP_IO,
    CP_CHIP_TIMEOUT, CP_CHIP_WRITE_DISABLED, CP_CHIP_MISMATCH
} CpChipResult;

/* Caller owns the powered Slot-1 bus on ARM9 and serializes access with ROM
 * reads. Supply the current card header's game code. Discard the descriptor
 * on removal. A failed detection clears it. Unknown/ambiguous IDs fail closed. */
CpChipResult CpCardSave_Detect(const char gameCode[4], CpCardSave *chip);
CpChipResult CpCardSave_Read(const CpCardSave *chip, uint32_t offset, void *data, size_t size);
CpChipResult CpCardSave_Verify(const CpCardSave *chip, uint32_t offset, const void *data, size_t size);

#if CP_CARD_SAVE_WRITES
/* Program one page (any range for F-RAM). Erase FLASH before programming.
 * Erase accepts one aligned eraseSize or sectorSize. Both functions poll
 * completion and verify all affected bytes. On error the unit can be torn. */
CpChipResult CpCardSave_Program(const CpCardSave *chip, uint32_t offset, const void *data, size_t size);
CpChipResult CpCardSave_Erase(const CpCardSave *chip, uint32_t offset, uint32_t size);
#endif

#endif
