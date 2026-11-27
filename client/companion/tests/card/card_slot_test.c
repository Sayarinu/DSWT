#include <nds.h>
#include <assert.h>

#include "platform/nds/card_rom.h"

volatile uint16_t REG_SCFG_MC, REG_AUXSPICNT, REG_CARD_1B8, REG_CARD_1BA;
volatile uint32_t REG_ROMCTRL, REG_CARD_DATA_RD, REG_CARD_1B0, REG_CARD_1B4;
volatile uint8_t REG_CARD_COMMAND[8];

static enum { STUCK_POWER, STUCK_TRANSFER, SHORT_TRANSFER, EJECT_DURING_TRANSFER, GOOD_HEADER } fault;
static uint64_t ticks;
static unsigned requests, transferred, commands;
static uint64_t offAt, wakeAt;
static bool dsi;

bool isDSiMode(void) { return dsi; }
void sysSetCardOwner(bool arm9) { assert(arm9); }

bool fifoSendValue32(unsigned channel, unsigned state)
{
    assert(channel == FIFO_USER_01);
    ++requests;
    if (state == SCFG_MC_PWR_REQUEST_OFF) offAt = ticks;
    if (fault == GOOD_HEADER && state == SCFG_MC_PWR_RESET && commands >= 2)
        assert(ticks - offAt >= 420000);
    if (fault != STUCK_POWER) REG_SCFG_MC = state == SCFG_MC_PWR_REQUEST_OFF ? SCFG_MC_PWR_OFF : state;
    return true;
}

uint64_t systemCounterGetTicks(void)
{
    ticks += 100;
    if (REG_ROMCTRL & CARD_BUSY) {
        assert(!(REG_ROMCTRL & CARD_WR));
        if (fault == GOOD_HEADER) {
            /* A card which drives FF until it gets a cold, full reset. */
            static const uint8_t op[] = {0x9f, 0, 0x9f, 0x9f, 0x90, 0};
            assert(commands < sizeof(op) && REG_CARD_COMMAND[0] == op[commands]);
            unsigned bytes = CP_CARD_PAGE;
            if (commands == 3) {
                assert(ticks - wakeAt >= 420000 && REG_AUXSPICNT == (CARD_ENABLE | CARD_IRQ));
                assert((REG_ROMCTRL & 0x0f3f1fff) == 0x0d180000);
                bytes = 8192;
            } else if (commands == 4) {
                assert((REG_ROMCTRL & 0x0f3f1fff) == 0x0f000000);
                bytes = 4;
            } else assert((REG_ROMCTRL & 0x0f3f1fff) == 0x093f1fff);
            if (transferred == bytes) {
                if (commands == 2) wakeAt = ticks;
                REG_ROMCTRL = 0;
                transferred = 0;
                ++commands;
            } else {
                REG_ROMCTRL |= CARD_DATA_READY;
                REG_CARD_DATA_RD = commands == 1 ? UINT32_MAX : 0x12345678;
                transferred += 4;
            }
            return ticks;
        }
        assert(REG_CARD_COMMAND[0] == 0x9f);
        if (fault == SHORT_TRANSFER) REG_ROMCTRL = 0;
        if (fault == EJECT_DURING_TRANSFER) REG_SCFG_MC = SCFG_MC_EJECTED;
    }
    return ticks;
}

int main(void)
{
    CpCardRom card;
    CpReader reader;
    assert(!CpNdsSlot_Present());
    dsi = true;
    REG_SCFG_MC = SCFG_MC_EJECTED;
    assert(!CpNdsSlot_Present() && CpNdsSlot_Open(&card, &reader) < 0 && requests == 0);
    for (int i = STUCK_POWER; i <= EJECT_DURING_TRANSFER; ++i) {
        fault = i;
        ticks = requests = 0;
        REG_SCFG_MC = SCFG_MC_PWR_ON;
        REG_ROMCTRL = 0;
        assert(CpNdsSlot_Present() && CpNdsSlot_Open(&card, &reader) < 0 && !card.active);
        if (fault == STUCK_POWER) {
            assert(requests == 1 && ticks >= 1200000 && ticks < 1210000);
        } else {
            assert(requests == 3 && REG_AUXSPICNT == CARD_ENABLE);
            if (fault == STUCK_TRANSFER) assert(ticks >= 550000);
            else assert(ticks < 320000);
            assert(ticks < 570000);
        }
    }
    uint8_t header[CP_CARD_PAGE];
    fault = GOOD_HEADER;
    REG_SCFG_MC = SCFG_MC_PWR_ON;
    REG_ROMCTRL = 0;
    assert(CpNdsSlot_Header(header, NULL) == 0 && commands == 2);
    for (unsigned i = 0; i < sizeof(header); ++i) assert(header[i] == 0xff);
    uint32_t id = 0;
    assert(CpNdsSlot_Header(header, &id) == 0 && commands == 6 && id == 0x12345678);
    for (unsigned i = 0; i < sizeof(header); i += 4) assert(CpLe32(header + i) == 0x12345678);
    return 0;
}
