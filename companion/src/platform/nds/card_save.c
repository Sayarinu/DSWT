#include "card_save_spi.h"

#include <string.h>

enum { READ = 0x03, RDSR = 0x05, RDID = 0x9F, WIP = 1, WEL = 2 };
/* Allow a pending 64 KiB sector erase (M45PE40 tSE max 5 s) on re-entry. */
enum { READY_POLLS = 6000 };

static CpChipResult Command(const CpCardSave *chip, uint8_t opcode, unsigned addressBytes,
                            uint32_t offset, const void *tx, void *rx, size_t size)
{
    uint8_t header[5] = {0};
    size_t n = chip->infrared ? 1 : 0;
    header[n++] = opcode;
    while (addressBytes) {
        --addressBytes;
        header[n++] = (uint8_t)(offset >> (8 * addressBytes));
    }
    return CpSaveSpi_Transfer(header, n, tx, rx, size);
}

static CpChipResult Ready(const CpCardSave *chip, unsigned polls, uint8_t *status)
{
    for (;;) {
        CpChipResult result = Command(chip, RDSR, 0, 0, NULL, status, 1);
        if (result != CP_CHIP_OK) return result;
        if (*status == 0xFF) return CP_CHIP_IO; /* Open bus. */
        if (!(*status & WIP)) return CP_CHIP_OK;
        if (polls-- == 0) return CP_CHIP_TIMEOUT;
        CpSaveSpi_PollDelay();
    }
}

CpChipResult CpCardSave_Detect(const char gameCode[4], CpCardSave *out)
{
    if (!out) return CP_CHIP_RANGE;
    *out = (CpCardSave){0};
    if (!gameCode) return CP_CHIP_RANGE;
    CpCardSave chip = {.infrared = gameCode[0] == 'I'};
    uint8_t status;
    CpChipResult result = Ready(&chip, READY_POLLS, &status);
    if (result != CP_CHIP_OK) return result;
    result = Command(&chip, RDID, 0, 0, NULL, chip.jedec, sizeof(chip.jedec));
    if (result != CP_CHIP_OK) return result;

    static const uint8_t fram[] = {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0xC2,0x22,0x08};
    uint32_t id = (uint32_t)chip.jedec[0] << 16 | (uint32_t)chip.jedec[1] << 8 | chip.jedec[2];
    if (id == 0x621100 || id == 0x204013 || id == 0x208013 || id == 0x202013) {
        chip.type = CP_CHIP_FLASH;
        chip.size = 512 * 1024;
        chip.pageSize = 256;
        chip.sectorSize = 64 * 1024;
        chip.eraseSize = id == 0x202013 ? chip.sectorSize : chip.pageSize;
        chip.jedecLength = 3;
    } else if ((status & 0xF0) == 0xF0 && id == 0xFFFFFF) {
        chip.type = CP_CHIP_EEPROM;
        chip.size = 512;
        chip.pageSize = 16;
    } else if (memcmp(chip.jedec, fram, sizeof(fram)) == 0) {
        chip.type = CP_CHIP_FRAM;
        chip.size = 32 * 1024;
        chip.jedecLength = sizeof(fram);
    } else {
        return CP_CHIP_UNSUPPORTED;
    }
    *out = chip;
    return CP_CHIP_OK;
}

static bool InRange(const CpCardSave *chip, uint32_t offset, size_t size)
{
    return chip && chip->type != CP_CHIP_UNKNOWN && offset <= chip->size && size <= chip->size - offset;
}

static CpChipResult Addressed(const CpCardSave *chip, uint8_t opcode, uint32_t offset,
                              const void *tx, void *rx, size_t size)
{
    unsigned width = 3;
    if (chip->type == CP_CHIP_EEPROM) {
        width = 1;
        if (offset & 0x100) opcode |= 0x08;
    } else if (chip->type == CP_CHIP_FRAM) {
        width = 2;
    }
    return Command(chip, opcode, width, offset, tx, rx, size);
}

CpChipResult CpCardSave_Read(const CpCardSave *chip, uint32_t offset, void *data, size_t size)
{
    if (!InRange(chip, offset, size) || (size && !data)) return CP_CHIP_RANGE;
    if (!size) return CP_CHIP_OK;
    uint8_t status;
    CpChipResult result = Ready(chip, READY_POLLS, &status);
    if (result != CP_CHIP_OK) return result;
    return Addressed(chip, READ, offset, NULL, data, size);
}

static CpChipResult Compare(const CpCardSave *chip, uint32_t offset, const uint8_t *expected, size_t size)
{
    uint8_t bytes[256];
    while (size) {
        size_t n = size < sizeof(bytes) ? size : sizeof(bytes);
        CpChipResult result = Addressed(chip, READ, offset, NULL, bytes, n);
        if (result != CP_CHIP_OK) return result;
        for (size_t i = 0; i < n; ++i) {
            if (bytes[i] != (expected ? expected[i] : 0xFF)) return CP_CHIP_MISMATCH;
        }
        offset += n;
        size -= n;
        if (expected) expected += n;
    }
    return CP_CHIP_OK;
}

CpChipResult CpCardSave_Verify(const CpCardSave *chip, uint32_t offset, const void *data, size_t size)
{
    if (!InRange(chip, offset, size) || (size && !data)) return CP_CHIP_RANGE;
    if (!size) return CP_CHIP_OK;
    uint8_t status;
    CpChipResult result = Ready(chip, READY_POLLS, &status);
    if (result != CP_CHIP_OK) return result;
    return Compare(chip, offset, data, size);
}

#if CP_CARD_SAVE_WRITES
enum { WREN = 0x06, PROGRAM = 0x02, PAGE_ERASE = 0xDB, SECTOR_ERASE = 0xD8 };

static CpChipResult Modify(const CpCardSave *chip, uint8_t opcode, uint32_t offset,
                           const void *data, size_t size)
{
    uint8_t status;
    CpChipResult result = Ready(chip, READY_POLLS, &status);
    if (result != CP_CHIP_OK) return result;
    result = Command(chip, WREN, 0, 0, NULL, NULL, 0);
    if (result != CP_CHIP_OK) return result;
    result = Ready(chip, 0, &status);
    if (result != CP_CHIP_OK) return result;
    if (!(status & WEL)) return CP_CHIP_WRITE_DISABLED;
    result = Addressed(chip, opcode, offset, data, NULL, data ? size : 0);
    if (result != CP_CHIP_OK) return result;
    result = Ready(chip, opcode == SECTOR_ERASE ? READY_POLLS : 150, &status);
    if (result != CP_CHIP_OK) return result;
    return Compare(chip, offset, data, size);
}

CpChipResult CpCardSave_Program(const CpCardSave *chip, uint32_t offset, const void *data, size_t size)
{
    if (!InRange(chip, offset, size) || !size || !data) return CP_CHIP_RANGE;
    if (chip->pageSize && size > chip->pageSize - offset % chip->pageSize) return CP_CHIP_RANGE;
    return Modify(chip, PROGRAM, offset, data, size);
}

CpChipResult CpCardSave_Erase(const CpCardSave *chip, uint32_t offset, uint32_t size)
{
    if (!InRange(chip, offset, size) || !size) return CP_CHIP_RANGE;
    if (chip->type != CP_CHIP_FLASH) return CP_CHIP_UNSUPPORTED;
    if ((size != chip->eraseSize && size != chip->sectorSize) || offset % size) return CP_CHIP_RANGE;
    return Modify(chip, size == chip->sectorSize ? SECTOR_ERASE : PAGE_ERASE, offset, NULL, size);
}
#endif
