#include "platform/nds/card_save_spi.h"
#include <nds.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t REG_ROMCTRL;
uint16_t REG_SCFG_MC;
static uint16_t control, controls[10];
static uint8_t bytes[10];
static uint64_t ticks, times[10];
static size_t accesses;
static bool arm9, twl, wedge, ejectAfterTransfer;

uint16_t *SimControl(void)
{
    if (wedge && accesses) control |= CARD_SPI_BUSY;
    return &control;
}

uint8_t *SimData(void)
{
    assert(accesses < sizeof(bytes));
    controls[accesses] = control;
    times[accesses] = ticks;
    if (ejectAfterTransfer) REG_SCFG_MC |= SCFG_MC_EJECTED;
    return &bytes[accesses++];
}

bool sysGetCardOwner(void) { return arm9; }
bool isDSiMode(void) { return twl; }
uint64_t systemCounterGetTicks(void) { return ticks++; }

int main(void)
{
    const uint8_t read[] = {0x00,0x03,0x06,0x12,0x34};
    uint16_t previous = control = 0xC000;
    assert(CpSaveSpi_Transfer(NULL, 1, NULL, NULL, 0) == CP_CHIP_RANGE);
    assert(CpSaveSpi_Transfer(read, 0, NULL, NULL, 0) == CP_CHIP_RANGE);
    assert(CpSaveSpi_Transfer(read, 1, NULL, NULL, 0) == CP_CHIP_RANGE);
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, SIZE_MAX) == CP_CHIP_RANGE);
    twl = true;
    REG_SCFG_MC = SCFG_MC_PWR_ON;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_IO);
    arm9 = true;
    REG_SCFG_MC = 0;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_IO);
    REG_SCFG_MC = SCFG_MC_PWR_ON | SCFG_MC_EJECTED;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_IO);
    REG_SCFG_MC = SCFG_MC_PWR_ON;
    REG_ROMCTRL = CARD_ACTIVATE;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_IO);
    REG_ROMCTRL = 0;
    control |= CARD_SPI_HOLD;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_IO);
    control = previous | CARD_SPI_BUSY;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_IO);
    control = previous;
    assert(!accesses);
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_OK);
    assert(accesses == 6 && !memcmp(read, bytes, 5) && bytes[5] == 0);
    assert(controls[0] == 0xA042 && times[1] - times[0] >= 65);
    for (size_t i = 1; i < 5; ++i) assert(controls[i] == 0xA040);
    assert(controls[5] == 0xA000 && ticks - times[5] >= 65 && control == previous);

    accesses = 0;
    wedge = true;
    assert(CpSaveSpi_Transfer(read, 5, NULL, NULL, 1) == CP_CHIP_TIMEOUT);
    assert(accesses == 1 && control == previous);
    accesses = 0;
    wedge = false;
    ejectAfterTransfer = true;
    assert(CpSaveSpi_Transfer(read + 1, 4, NULL, NULL, 1) == CP_CHIP_IO);
    assert(accesses == 5 && controls[0] == 0xA040 && control == previous);
    twl = false;
    accesses = 0;
    /* Four header writes, then one dummy write and one register read per byte. */
    bytes[5] = 0xC7;
    bytes[7] = 0xA2;
    uint8_t reply[2];
    assert(CpSaveSpi_Transfer(read + 1, 4, NULL, reply, 2) == CP_CHIP_OK);
    assert(accesses == 8 && !memcmp(reply, (uint8_t[]){0xC7, 0xA2}, 2));
    assert(bytes[4] == 0 && bytes[6] == 0 && controls[4] == 0xA040 && controls[6] == 0xA000);
    uint64_t start = ticks;
    CpSaveSpi_PollDelay();
    assert(ticks - start >= 1000);

#if !CP_CARD_SAVE_WRITES
    /* Exercise every byte as the opcode, with both possible framings. */
    for (unsigned ir = 0; ir <= 1; ++ir) {
        for (unsigned opcode = 1; opcode <= 255; ++opcode) {
            if (opcode == 3 || opcode == 5 || opcode == 0x0B || opcode == 0x9F) continue;
            uint8_t header[] = {0, (uint8_t)opcode};
            assert(CpSaveSpi_Transfer(header + 1 - ir, ir + 1, NULL, NULL, 0) == CP_CHIP_WRITE_DISABLED);
        }
    }
    assert(CpSaveSpi_Transfer(read, 5, bytes, NULL, 1) == CP_CHIP_WRITE_DISABLED);
#else
    const uint8_t program[] = {0, 2, 1, 2, 3};
    const uint8_t payload[] = {0x12, 0xAB};
    accesses = 0;
    assert(CpSaveSpi_Transfer(program, 5, payload, NULL, 2) == CP_CHIP_OK);
    assert(accesses == 7 && !memcmp(bytes, program, 5) && !memcmp(bytes + 5, payload, 2));
    assert(controls[5] == 0xA040 && controls[6] == 0xA000);
#endif
    puts("Verified slot ownership/power, AUXSPI framing, prefix timing, busy timeout and control restoration.");
    return 0;
}
