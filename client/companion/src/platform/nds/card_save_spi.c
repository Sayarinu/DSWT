#include "card_save_spi.h"

#include <nds.h>

/* GBATEK, DS Cartridge I/O Ports and DS Cart Infrared SPI Commands.
 * The system counter runs at the same rate at both ARM9 clock speeds. */
static void DelayUs(unsigned us)
{
    uint64_t start = systemCounterGetTicks();
    unsigned ticks = systemCounterUsecsToTicks(us) + 1;
    while (systemCounterGetTicks() - start < ticks) {}
}

void CpSaveSpi_PollDelay(void)
{
    DelayUs(1000);
}

static bool SlotReady(void)
{
    if (!sysGetCardOwner()) return false;
    if (!isDSiMode()) return true;
    return (REG_SCFG_MC & (SCFG_MC_EJECTED | SCFG_MC_PWR_MASK)) == SCFG_MC_PWR_ON;
}

CpChipResult CpSaveSpi_Transfer(const uint8_t *header, size_t headerSize,
                                const uint8_t *tx, uint8_t *rx, size_t size)
{
    if (!header || !headerSize || size > SIZE_MAX - headerSize) return CP_CHIP_RANGE;
    bool ir = header[0] == 0;
    if (ir && headerSize < 2) return CP_CHIP_RANGE;
#if !CP_CARD_SAVE_WRITES
    /* Enforce the read-only build at the register boundary as well. */
    uint8_t opcode = header[ir ? 1 : 0];
    if (tx || (opcode != 0x03 && opcode != 0x0B && opcode != 0x05 && opcode != 0x9F))
        return CP_CHIP_WRITE_DISABLED;
#endif
    if (!SlotReady() || (REG_ROMCTRL & CARD_ACTIVATE)) return CP_CHIP_IO;
    uint16_t previous = REG_AUXSPICNT;
    if (previous & (CARD_SPI_BUSY | CARD_SPI_HOLD)) return CP_CHIP_IO;

    size_t count = headerSize + size;
    CpChipResult result = CP_CHIP_OK;
    for (size_t i = 0; i < count; ++i) {
        bool prefix = ir && i == 0;
        uint16_t control = CARD_ENABLE | CARD_SPI_ENABLE;
        if (prefix) control |= CARD_SPI_BAUD_1MHz;
        if (i + 1 < count) control |= CARD_SPI_HOLD;
        REG_AUXSPICNT = control;
        REG_AUXSPIDATA = i < headerSize ? header[i] : (tx ? tx[i - headerSize] : 0);
        /* One byte at the slowest clock used here takes 8 us. Bound even a
         * wedged controller, independently of the save chip's WIP timeout. */
        unsigned remaining = 4096;
        while ((REG_AUXSPICNT & CARD_SPI_BUSY) && --remaining) {}
        if (!remaining) {
            result = CP_CHIP_TIMEOUT;
            break;
        }
        if (rx && i >= headerSize) rx[i - headerSize] = REG_AUXSPIDATA;
        if (prefix) DelayUs(65); /* IR MCU needs 0x800 clocks at 33 MHz. */
    }
    /* On timeout, disable the port to release CS before restoring ROM mode. */
    REG_AUXSPICNT = 0;
    DelayUs(ir ? 65 : 2);
    REG_AUXSPICNT = previous;
    return SlotReady() ? result : CP_CHIP_IO;
}
