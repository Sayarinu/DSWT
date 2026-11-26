#include <nds.h>
#include <assert.h>

#include "platform/nds/card_rom.h"

volatile uint16_t REG_SCFG_MC, REG_AUXSPICNT, REG_CARD_1B8, REG_CARD_1BA;
volatile uint32_t REG_ROMCTRL, REG_CARD_DATA_RD, REG_CARD_1B0, REG_CARD_1B4;
volatile uint8_t REG_CARD_COMMAND[8];

static enum { STUCK_POWER, STUCK_TRANSFER, SHORT_TRANSFER, EJECT_DURING_TRANSFER } fault;
static uint64_t ticks;
static unsigned requests;
static bool dsi;

bool isDSiMode(void) { return dsi; }
void sysSetCardOwner(bool arm9) { assert(arm9); }

bool fifoSendValue32(unsigned channel, unsigned state)
{
    assert(channel == FIFO_USER_01);
    ++requests;
    if (fault != STUCK_POWER) REG_SCFG_MC = state == SCFG_MC_PWR_REQUEST_OFF ? SCFG_MC_PWR_OFF : state;
    return true;
}

uint64_t systemCounterGetTicks(void)
{
    ticks += 100;
    if (REG_ROMCTRL & CARD_BUSY) {
        assert(REG_CARD_COMMAND[0] == 0x9f && !(REG_ROMCTRL & CARD_WR));
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
    return 0;
}
