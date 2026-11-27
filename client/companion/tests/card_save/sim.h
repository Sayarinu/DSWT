#ifndef COMPANION_TEST_CARD_SAVE_SIM_H
#define COMPANION_TEST_CARD_SAVE_SIM_H

#include "platform/nds/card_save.h"

enum SimPart { SIM_EEPROM, SIM_FRAM, SIM_LE25FW403A, SIM_M45PE40, SIM_M25PE40, SIM_M25P40, SIM_MX25L4005 };
enum SimCut { SIM_NO_CUT, SIM_OLD, SIM_ERASED, SIM_MIXED };

/* A single slot. RAM survives power cuts. Configure faults before an operation. */
typedef struct {
    uint8_t memory[512 * 1024], id[9];
    size_t capacity, page, sector;
    enum SimPart part;
    bool ir, powered, wel, refuseWren, protected, stuck;
    unsigned elapsed, transactions, modifies, failTransaction;
    unsigned latency, busy, cutModify;
    enum SimCut cut;
    unsigned cutTick; /* Busy milliseconds from CS release, or bytes shifted for F-RAM. */
} CardSaveSim;

extern CardSaveSim sim;
void SimInsert(enum SimPart part, bool ir);
void SimPowerOn(void);

#endif
