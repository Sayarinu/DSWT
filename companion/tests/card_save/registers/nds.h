#ifndef COMPANION_TEST_CARD_SAVE_NDS_H
#define COMPANION_TEST_CARD_SAVE_NDS_H

#include <stdbool.h>
#include <stdint.h>

/* AUXSPI register trace for the ARM9 transport. Tick = 1 us in this test. */
extern uint32_t REG_ROMCTRL;
extern uint16_t REG_SCFG_MC;
uint16_t *SimControl(void);
uint8_t *SimData(void);
bool sysGetCardOwner(void);
bool isDSiMode(void);
uint64_t systemCounterGetTicks(void);
static inline unsigned systemCounterUsecsToTicks(unsigned us) { return us; }

#define REG_AUXSPICNT (*SimControl())
#define REG_AUXSPIDATA (*SimData())
#define SCFG_MC_EJECTED 1
#define SCFG_MC_PWR_MASK 12
#define SCFG_MC_PWR_ON 8
#define CARD_ACTIVATE (1u << 31)
#define CARD_ENABLE 0x8000
#define CARD_SPI_ENABLE 0x2000
#define CARD_SPI_BAUD_1MHz 2
#define CARD_SPI_BUSY 0x80
#define CARD_SPI_HOLD 0x40

#endif
