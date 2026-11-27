#include "store_fixture.h"
#include <stdio.h>
static TestCardFile copy, record;
static uint8_t original[CP_SAVE_SIZE], target[CP_SAVE_SIZE];
enum { AT = 0x74020, LENGTH = 521 };

static CpCardStore *Start(CpPlatformFile *file)
{
    SimInsert(SIM_MX25L4005, true);
    memcpy(sim.memory, original, CP_SAVE_SIZE);
    memset(&record, 0, sizeof(record));
    CpCardStore *store = OpenTestStore(&copy, &record, file);
    assert(store && !memcmp(copy.bytes, original, CP_SAVE_SIZE) && !sim.modifies);
    return store;
}

int main(void)
{
    for (unsigned i = 0; i < CP_SAVE_SIZE; ++i) original[i] = i * 31 + (i >> 8);
    memcpy(target, original, sizeof(target));
    memset(target + AT, 0x38, LENGTH);
    CpPlatformFile f;
    CpCardStore *store = Start(&f);
    assert(!f.writeAt(f.arg, AT, target + AT, LENGTH));
    unsigned commands = sim.modifies;
    CpCardStore_Close(store);
    for (unsigned m = 1; m <= commands; ++m) {
        for (unsigned outcome = SIM_OLD; outcome <= SIM_MIXED; ++outcome) {
            store = Start(&f);
            sim.cut = outcome;
            sim.cutModify = m;
            sim.cutTick = 1;
            assert(f.writeAt(f.arg, AT, target + AT, LENGTH) != 0 && f.sync(f.arg) != 0);
            CpCardStore_Close(store);
            SimPowerOn();
            /* An unrelated save with the same header and chip ID must survive. */
            sim.memory[0x300] ^= 1;
            unsigned prior = sim.modifies;
            assert(!OpenTestStore(&copy, &record, &f) && sim.modifies == prior);
            sim.memory[0x300] ^= 1;
            store = OpenTestStore(&copy, &record, &f);
            assert(store && !memcmp(sim.memory, target, sizeof(target)) && !memcmp(copy.bytes, target, sizeof(target)));
            CpCardStore_Close(store);
        }
    }
    printf("PASS all %u flash commands x old/erased/mixed cuts, wrong-save refusal and full byte-for-byte recovery\n", commands);
    /* Damaged or unwritable intent storage can never authorize an erase. */
    store = Start(&f);
    record.fail = record.operations + 1;
    assert(f.writeAt(f.arg, AT, target + AT, LENGTH) && !sim.modifies);
    CpCardStore_Close(store);
    record.bytes[1] = 1;
    assert(!OpenTestStore(&copy, &record, &f) && !sim.modifies);
    puts("PASS SD record write failure and corrupt record block chip writes");
    return 0;
}
