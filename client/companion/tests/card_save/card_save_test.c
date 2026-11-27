#include "sim.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static CpCardSave Detect(enum SimPart part, const char *game)
{
    SimInsert(part, game[0] == 'I');
    CpCardSave chip;
    assert(CpCardSave_Detect(game, &chip) == CP_CHIP_OK);
    assert(chip.infrared == sim.ir && chip.size == sim.capacity && chip.pageSize == sim.page);
    assert(chip.jedecLength == (part == SIM_FRAM ? 9 : part == SIM_EEPROM ? 0 : 3));
    assert(!memcmp(chip.jedec, sim.id, chip.jedecLength));
    if (part == SIM_FRAM || part == SIM_EEPROM) {
        assert(chip.type == (part == SIM_FRAM ? CP_CHIP_FRAM : CP_CHIP_EEPROM));
        assert(chip.eraseSize == 0 && chip.sectorSize == 0);
    } else {
        assert(chip.type == CP_CHIP_FLASH && chip.sectorSize == 65536);
        assert(chip.eraseSize == (part == SIM_M25P40 ? 65536u : part == SIM_MX25L4005 ? 4096u : 256u));
    }
    return chip;
}

static void TestRead(void)
{
    static const char *const games[] = {"ADAE","APAE","CPUE","IPKE","IPGE","IRBO","IRAO","IREO","IRDO"};
    for (size_t g = 0; g < sizeof(games) / sizeof(*games); ++g) {
        for (enum SimPart p = SIM_EEPROM; p <= SIM_MX25L4005; ++p) {
            CpCardSave chip = Detect(p, games[g]);
            for (size_t i = 0; i < sim.capacity; ++i)
                sim.memory[i] = (uint8_t)(i * 13 + (i >> 8) + (i >> 16));
            uint8_t bytes[273];
            assert(CpCardSave_Read(&chip, 239, bytes, sizeof(bytes)) == CP_CHIP_OK);
            assert(!memcmp(bytes, sim.memory + 239, sizeof(bytes)));
            uint32_t offset = chip.size - sizeof(bytes);
            assert(CpCardSave_Read(&chip, offset, bytes, sizeof(bytes)) == CP_CHIP_OK);
            assert(!memcmp(bytes, sim.memory + offset, sizeof(bytes)));
            assert(CpCardSave_Verify(&chip, offset, bytes, sizeof(bytes)) == CP_CHIP_OK);
            bytes[272] ^= 1;
            assert(CpCardSave_Verify(&chip, offset, bytes, sizeof(bytes)) == CP_CHIP_MISMATCH);
            assert(sim.modifies == 0 && !sim.wel);
        }
    }
}

static void TestDetectionFailures(void)
{
    CpCardSave chip;
    chip = Detect(SIM_M45PE40, "CPUE");
    assert(CpCardSave_Detect("CPUE", NULL) == CP_CHIP_RANGE);
    assert(CpCardSave_Detect(NULL, &chip) == CP_CHIP_RANGE && chip.size == 0);
    for (unsigned n = 1; n <= 2; ++n) {
        SimInsert(SIM_M45PE40, false);
        sim.failTransaction = n;
        assert(CpCardSave_Detect("CPUE", &chip) == CP_CHIP_IO && chip.size == 0);
    }
    SimInsert(SIM_M45PE40, true);
    assert(CpCardSave_Detect("CPUE", &chip) == CP_CHIP_IO);
    SimInsert(SIM_M45PE40, false);
    assert(CpCardSave_Detect("IPGE", &chip) == CP_CHIP_IO);
    sim.id[2] = 0x14; /* Same manufacturer and family, different density. */
    assert(CpCardSave_Detect("CPUE", &chip) == CP_CHIP_UNSUPPORTED);
    memset(sim.id, 0xFF, sizeof(sim.id));
    assert(CpCardSave_Detect("CPUE", &chip) == CP_CHIP_UNSUPPORTED); /* ambiguous 16-bit EEPROM/F-RAM */
    sim.busy = 1;
    sim.stuck = true;
    assert(CpCardSave_Detect("CPUE", &chip) == CP_CHIP_TIMEOUT && sim.elapsed == 6000);
    assert(!sim.modifies && !sim.wel && !chip.size);
}

static void TestReadErrors(void)
{
    CpCardSave chip = Detect(SIM_M45PE40, "IPGE");
    uint8_t data[257] = {0};
    assert(CpCardSave_Read(NULL, 0, data, 1) == CP_CHIP_RANGE);
    assert(CpCardSave_Read(&(CpCardSave){0}, 0, data, 1) == CP_CHIP_RANGE);
    assert(CpCardSave_Read(&chip, UINT32_MAX, data, 1) == CP_CHIP_RANGE);
    assert(CpCardSave_Verify(&chip, 1, data, SIZE_MAX) == CP_CHIP_RANGE);
    assert(CpCardSave_Read(&chip, chip.size, NULL, 0) == CP_CHIP_OK);
    assert(CpCardSave_Verify(&chip, chip.size, NULL, 0) == CP_CHIP_OK);
    assert(CpCardSave_Read(&chip, chip.size, data, 1) == CP_CHIP_RANGE);
    assert(CpCardSave_Read(&chip, 0, NULL, 1) == CP_CHIP_RANGE);
    assert(CpCardSave_Verify(&chip, 0, NULL, 1) == CP_CHIP_RANGE);
    for (unsigned n = 1; n <= 2; ++n) {
        sim.failTransaction = sim.transactions + n;
        assert(CpCardSave_Read(&chip, 0, data, 1) == CP_CHIP_IO);
    }
    memset(data, 0xFF, sizeof(data));
    for (unsigned n = 1; n <= 3; ++n) {
        sim.failTransaction = sim.transactions + n;
        assert(CpCardSave_Verify(&chip, 0, data, sizeof(data)) == CP_CHIP_IO);
    }
    sim.stuck = true;
    sim.busy = 1;
    assert(CpCardSave_Read(&chip, 0, data, 1) == CP_CHIP_TIMEOUT);
    assert(CpCardSave_Verify(&chip, 0, data, 1) == CP_CHIP_TIMEOUT);
}

#if CP_CARD_SAVE_WRITES
static void TestWrites(void)
{
    uint8_t page[256];
    memset(page, 0x54, sizeof(page));
    for (enum SimPart p = SIM_EEPROM; p <= SIM_MX25L4005; ++p) {
        CpCardSave chip = Detect(p, "IPGE");
        uint32_t n = chip.pageSize ? chip.pageSize : sizeof(page);
        uint32_t offset = chip.size - n;
        assert(CpCardSave_Program(&chip, offset, page, n) == CP_CHIP_OK);
        assert(!memcmp(sim.memory + offset, page, n) && sim.memory[offset - 1] == 0xFF);
        assert(sim.elapsed == (p == SIM_FRAM ? 0 : sim.latency));
        uint8_t opposite = 0xAB;
        if (chip.type == CP_CHIP_FLASH) {
            /* Programming would AND 0xAB into 0x54: refused before WREN. */
            unsigned modifies = sim.modifies;
            assert(CpCardSave_Program(&chip, offset, &opposite, 1) == CP_CHIP_MISMATCH);
            assert(sim.memory[offset] == 0x54 && sim.modifies == modifies && !sim.wel);
            uint8_t cleared = 0x50; /* Clearing further bits is a valid program. */
            assert(CpCardSave_Program(&chip, offset, &cleared, 1) == CP_CHIP_OK);
            assert(sim.memory[offset] == 0x50);
            uint32_t units[] = {chip.eraseSize, chip.sectorSize};
            for (unsigned i = 0; i < 2; ++i) {
                uint32_t unit = units[i], base = chip.size - 2 * unit;
                memset(sim.memory + base - 1, 0x22, unit + 2);
                assert(CpCardSave_Erase(&chip, base, unit) == CP_CHIP_OK);
                assert(sim.memory[base - 1] == 0x22 && sim.memory[base + unit] == 0x22);
                for (uint32_t j = 0; j < unit; ++j) assert(sim.memory[base + j] == 0xFF);
            }
        } else {
            assert(CpCardSave_Program(&chip, offset, &opposite, 1) == CP_CHIP_OK);
            assert(sim.memory[offset] == opposite);
            assert(CpCardSave_Erase(&chip, 0, 16) == CP_CHIP_UNSUPPORTED);
        }
    }
}

static void TestWriteErrors(void)
{
    CpCardSave chip = Detect(SIM_M25P40, "CPUE");
    uint8_t bytes[2] = {0, 0};
    unsigned before = sim.transactions;
    assert(CpCardSave_Erase(&chip, 0, 256) == CP_CHIP_RANGE);
    assert(CpCardSave_Erase(&chip, chip.size, chip.sectorSize) == CP_CHIP_RANGE);
    assert(CpCardSave_Erase(&chip, 1, chip.sectorSize) == CP_CHIP_RANGE);
    assert(CpCardSave_Erase(&chip, 0, 0) == CP_CHIP_RANGE);
    assert(CpCardSave_Program(&chip, 255, bytes, 2) == CP_CHIP_RANGE);
    assert(CpCardSave_Program(&chip, 0, NULL, 1) == CP_CHIP_RANGE);
    assert(CpCardSave_Program(&chip, 0, bytes, 0) == CP_CHIP_RANGE);
    assert(CpCardSave_Program(&chip, chip.size - 1, bytes, 2) == CP_CHIP_RANGE);
    assert(sim.transactions == before);
    sim.refuseWren = true;
    assert(CpCardSave_Erase(&chip, 0, chip.sectorSize) == CP_CHIP_WRITE_DISABLED && !sim.modifies);

    /* Drop each transfer in turn, from initial RDSR to the final read-back. */
    chip = Detect(SIM_M45PE40, "IPGE");
    before = sim.transactions;
    assert(CpCardSave_Program(&chip, 1, bytes, 2) == CP_CHIP_OK);
    unsigned count = sim.transactions - before;
    for (unsigned n = 1; n <= count; ++n) {
        chip = Detect(SIM_M45PE40, "IPGE");
        sim.failTransaction = sim.transactions + n;
        assert(CpCardSave_Program(&chip, 1, bytes, 2) == CP_CHIP_IO);
    }
    chip = Detect(SIM_M45PE40, "CPUE");
    sim.protected = true;
    sim.memory[chip.sectorSize - 1] = 0;
    assert(CpCardSave_Erase(&chip, 0, chip.sectorSize) == CP_CHIP_MISMATCH);
    assert(CpCardSave_Program(&chip, 0, bytes, 1) == CP_CHIP_MISMATCH);

    chip = Detect(SIM_M45PE40, "CPUE");
    sim.stuck = true;
    assert(CpCardSave_Program(&chip, 0, bytes, 1) == CP_CHIP_TIMEOUT);
    assert(sim.elapsed == 400 && sim.busy && sim.memory[0] == 0xFF);
    assert(CpCardSave_Erase(&chip, 0, 256) == CP_CHIP_TIMEOUT && sim.modifies == 1);
    chip = Detect(SIM_M25P40, "CPUE");
    sim.stuck = true;
    assert(CpCardSave_Erase(&chip, 0, chip.sectorSize) == CP_CHIP_TIMEOUT);
    assert(sim.elapsed == 6000 && sim.busy);
    sim.stuck = false;
    assert(CpCardSave_Read(&chip, 0, bytes, 2) == CP_CHIP_OK && bytes[0] == 0xFF && !sim.busy);

    /* A worn LE25FW403A page erase or page program may take up to 300 ms
     * (A1229, tPE 10^5 cycles). That is not a failure. */
    chip = Detect(SIM_LE25FW403A, "ADAE");
    sim.latency = 300;
    memset(sim.memory, 0, 256);
    assert(CpCardSave_Erase(&chip, 0, 256) == CP_CHIP_OK && sim.elapsed == 300);
    assert(CpCardSave_Program(&chip, 0, bytes, 2) == CP_CHIP_OK && sim.memory[0] == 0xFF);
}

static void TestPowerCuts(void)
{
    for (unsigned op = 0; op < 4; ++op) {
        for (unsigned tick = 0; tick <= 3; ++tick) {
            for (enum SimCut cut = SIM_OLD; cut <= SIM_MIXED; ++cut) {
                CpCardSave chip = Detect(op == 3 ? SIM_EEPROM : SIM_M45PE40, "IPGE");
                size_t unit = sim.sector;
                uint32_t base = unit;
                memset(sim.memory + base - 1, 0x55, unit + 2);
                sim.cut = cut;
                sim.cutTick = tick;
                uint8_t byte = 0x11;
                CpChipResult result;
                if (op == 0 || op == 3) result = CpCardSave_Program(&chip, base + 7, &byte, 1);
                else result = CpCardSave_Erase(&chip, base, op == 1 ? chip.eraseSize : chip.sectorSize);
                assert(result == CP_CHIP_IO && !sim.powered);
                SimPowerOn();
                assert(CpCardSave_Detect("IPGE", &chip) == CP_CHIP_OK);
                assert(sim.memory[base - 1] == 0x55 && sim.memory[base + unit] == 0x55);
                for (size_t i = 0; i < unit; ++i) {
                    bool erased = cut == SIM_ERASED || (cut == SIM_MIXED && i % 2 == 0);
                    assert(sim.memory[base + i] == (erased ? 0xFF : 0x55));
                }
            }
        }
    }
    uint8_t bytes[6] = {1,2,3,4,5,6};
    for (enum SimCut cut = SIM_OLD; cut <= SIM_MIXED; ++cut) {
        for (unsigned tick = 0; tick < sizeof(bytes); ++tick) {
            CpCardSave chip = Detect(SIM_FRAM, "CPUE");
            memset(sim.memory + 254, 0xAA, 8);
            sim.cut = cut;
            sim.cutTick = tick;
            assert(CpCardSave_Program(&chip, 255, bytes, sizeof(bytes)) == CP_CHIP_IO);
            SimPowerOn();
            for (size_t i = 0; i < sizeof(bytes); ++i) {
                uint8_t expected = cut == SIM_ERASED ? 0xFF : 0xAA;
                if (cut == SIM_MIXED && i < tick) expected = bytes[i];
                assert(sim.memory[255 + i] == expected);
            }
            assert(sim.memory[254] == 0xAA && sim.memory[261] == 0xAA);
        }
    }
}
#endif

int main(void)
{
    TestRead();
    TestReadErrors();
    TestDetectionFailures();
#if CP_CARD_SAVE_WRITES
    TestWrites();
    TestWriteErrors();
    TestPowerCuts();
    puts("Verified page programs, page/sector erases, read-back, dropped transfers, WIP timeouts and power cuts.");
#else
    puts("Verified detection, read and comparison in the default build with no mutating commands.");
#endif
    return 0;
}
