/* Slot-1 MMIO and time for the ARM9 timeout tests. */
#ifndef CP_TEST_NDS_H
#define CP_TEST_NDS_H

#include <stdint.h>
#include <stdbool.h>

extern volatile uint16_t REG_SCFG_MC, REG_AUXSPICNT, REG_CARD_1B8, REG_CARD_1BA;
extern volatile uint32_t REG_ROMCTRL, REG_CARD_DATA_RD, REG_CARD_1B0, REG_CARD_1B4;
extern volatile uint8_t REG_CARD_COMMAND[8];
enum {
    SCFG_MC_EJECTED = 1, SCFG_MC_PWR_MASK = 12, SCFG_MC_PWR_OFF = 0,
    SCFG_MC_PWR_RESET = 4, SCFG_MC_PWR_ON = 8, SCFG_MC_PWR_REQUEST_OFF = 12,
    FIFO_USER_01 = 8, CARD_ENABLE = 1 << 15, CARD_IRQ = 1 << 14, CARD_nRESET = 1 << 29, CARD_WR = 1 << 30,
    CARD_DATA_READY = 1 << 23, CARD_SEC_SEED = 1 << 15, CARD_SEC_EN = 1 << 14, CARD_SEC_DAT = 1 << 13
};
#define CARD_ACTIVATE (1u << 31)
#define CARD_BUSY CARD_ACTIVATE
#define CARD_BLK_SIZE(n) ((n) << 24)

bool isDSiMode(void);
void sysSetCardOwner(bool arm9);
bool fifoSendValue32(unsigned channel, unsigned state);
uint64_t systemCounterGetTicks(void);
static inline uint32_t systemCounterUsecsToTicks(uint32_t us) { return us; }
static inline uint32_t systemCounterMsecsToTicks(uint32_t ms) { return ms * 1000; }
#endif
