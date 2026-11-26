#include "sim.h"
#include "platform/nds/card_save_spi.h"

#include <assert.h>
#include <string.h>

CardSaveSim sim;
static struct {
    uint32_t address;
    size_t length;
    bool erase;
    uint8_t data[256]; /* Buffered writes on EEPROM and FLASH. */
} pending;

void SimPowerOn(void)
{
    sim.powered = true;
    sim.wel = false;
    sim.busy = 0;
    sim.cut = SIM_NO_CUT;
}

void SimInsert(enum SimPart part, bool ir)
{
    static const uint8_t ids[][9] = {
        {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
        {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0xC2,0x22,0x08},
        {0x62,0x11,0x00}, {0x20,0x40,0x13}, {0x20,0x80,0x13}, {0x20,0x20,0x13}
    };
    memset(&sim, 0, sizeof(sim));
    sim.part = part;
    sim.ir = ir;
    sim.capacity = sizeof(sim.memory);
    sim.sector = 65536;
    sim.page = 256;
    if (part == SIM_EEPROM) {
        sim.capacity = 512;
        sim.page = sim.sector = 16;
    } else if (part == SIM_FRAM) {
        sim.capacity = sim.sector = 32768;
        sim.page = 0;
    }
    memset(sim.memory, 0xFF, sim.capacity);
    memcpy(sim.id, ids[part], sizeof(sim.id));
    sim.latency = 3;
    SimPowerOn();
}

static void Cut(uint32_t address, size_t unit)
{
    /* Adversarial outcome for the whole recovery unit, including bytes outside
     * the program command. Neighbouring units retain their original contents. */
    size_t base = address - address % unit;
    for (size_t i = 0; i < unit; ++i) {
        if (sim.cut == SIM_ERASED || (sim.cut == SIM_MIXED && i % 2 == 0))
            sim.memory[base + i] = 0xFF;
    }
    sim.busy = 0;
    sim.wel = sim.powered = false;
}

static void Complete(void)
{
    if (pending.erase) {
        memset(sim.memory + pending.address, 0xFF, pending.length);
    } else {
        size_t base = pending.address - pending.address % sim.page;
        for (size_t i = 0; i < pending.length; ++i) {
            size_t address = base + (pending.address + i) % sim.page;
            if (sim.part == SIM_EEPROM) sim.memory[address] = pending.data[i];
            else sim.memory[address] &= pending.data[i];
        }
    }
    sim.wel = false;
}

void CpSaveSpi_PollDelay(void)
{
    ++sim.elapsed;
    if (!sim.powered || !sim.busy) return;
    if (sim.cut && sim.latency - sim.busy + 1 == sim.cutTick) {
        Cut(pending.address, sim.sector);
    } else if (!sim.stuck && --sim.busy == 0) {
        Complete();
    }
}

CpChipResult CpSaveSpi_Transfer(const uint8_t *h, size_t n,
                                const uint8_t *tx, uint8_t *rx, size_t size)
{
    ++sim.transactions;
    if (!sim.powered || sim.transactions == sim.failTransaction) return CP_CHIP_IO;
    if (rx) memset(rx, 0xFF, size);
    if (sim.ir) {
        if (!n || *h++ != 0) return CP_CHIP_OK;
        --n;
    }
    assert(n);
    uint8_t opcode = *h++;
    --n;
    if (opcode == 0x05) {
        assert(!n && rx && size == 1);
        *rx = (sim.part == SIM_EEPROM ? 0xF0 : 0) | (sim.wel ? 2 : 0) | (sim.busy ? 1 : 0);
        return CP_CHIP_OK;
    }
    if (sim.busy) return CP_CHIP_OK;
    if (opcode == 0x9F) {
        assert(!n && rx && size <= sizeof(sim.id));
        memcpy(rx, sim.id, size);
        return CP_CHIP_OK;
    }
    if (opcode == 0x06) {
        assert(!n && !size);
        sim.wel = !sim.refuseWren;
        return CP_CHIP_OK;
    }
    if (opcode == 0) return CP_CHIP_OK; /* IR prefix on a plain chip. */
    uint32_t address = 0;
    if (sim.part == SIM_EEPROM) {
        assert(n == 1);
        if (opcode & 8) address = 1;
        opcode &= ~8u;
    } else {
        assert(n == (sim.part == SIM_FRAM ? 2u : 3u));
    }
    while (n--) address = (address << 8) | *h++;
    address %= sim.capacity;
    if (opcode == 0x03) {
        assert(rx && !tx);
        for (size_t i = 0; i < size; ++i) rx[i] = sim.memory[(address + i) % sim.capacity];
        return CP_CHIP_OK;
    }
    ++sim.modifies;
    assert(opcode == 0x02 || opcode == 0xDB || opcode == 0xD8);
    if (!sim.wel || sim.protected) return CP_CHIP_OK;

    if (sim.part == SIM_FRAM) {
        assert(opcode == 0x02 && tx);
        /* F-RAM has no erase sector or WIP interval. A byte-boundary cut
         * normally leaves a written prefix. Also allow the recovery tests
         * to inject an old or all-ones outcome over the addressed range. */
        bool cut = sim.cut && sim.cutTick < size;
        for (size_t i = 0; i < size; ++i) {
            size_t pos = (address + i) % sim.capacity;
            if (!cut || (sim.cut == SIM_MIXED && i < sim.cutTick)) sim.memory[pos] = tx[i];
            else if (sim.cut == SIM_ERASED) sim.memory[pos] = 0xFF;
        }
        if (cut) {
            sim.powered = sim.wel = false;
            return CP_CHIP_IO;
        }
        sim.wel = false;
        return CP_CHIP_OK;
    }
    pending.erase = opcode != 0x02;
    if (pending.erase) {
        assert(sim.part != SIM_EEPROM && !size && !tx);
        assert(opcode != 0xDB || sim.part != SIM_M25P40);
        pending.length = opcode == 0xDB ? sim.page : sim.sector;
        pending.address = address - address % pending.length;
    } else {
        assert(size && size <= sim.page && tx);
        pending.address = address;
        pending.length = size;
        memcpy(pending.data, tx, size);
    }
    sim.busy = sim.latency;
    if (sim.cut && sim.cutTick == 0) Cut(address, sim.sector);
    return CP_CHIP_OK;
}
