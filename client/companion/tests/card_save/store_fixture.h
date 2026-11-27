#ifndef COMPANION_CARD_STORE_FIXTURE_H
#define COMPANION_CARD_STORE_FIXTURE_H

#include "sim.h"
#include "platform/nds/card_store.h"
#include <string.h>
#include <assert.h>

typedef struct {
    uint8_t bytes[CP_SAVE_SIZE];
    unsigned operations, fail;
} TestCardFile;

static int TestRead(void *arg, uint32_t offset, void *bytes, size_t size)
{
    TestCardFile *f = arg;
    assert(offset <= CP_SAVE_SIZE && size <= CP_SAVE_SIZE - offset);
    if (++f->operations == f->fail) return -1;
    memcpy(bytes, f->bytes + offset, size);
    return 0;
}

static int TestWrite(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    TestCardFile *f = arg;
    assert(offset <= CP_SAVE_SIZE && size <= CP_SAVE_SIZE - offset);
    if (++f->operations == f->fail) return -1;
    memcpy(f->bytes + offset, bytes, size);
    return 0;
}

static int TestSync(void *arg)
{
    TestCardFile *f = arg;
    return ++f->operations == f->fail ? -1 : 0;
}

static int TestCardCheck(void *context) { (void)context; return sim.powered ? 0 : -1; }

static CpCardStore *OpenTestStore(TestCardFile *copy, TestCardFile *record, CpPlatformFile *out)
{
    CpPlatformFile sd = {copy, TestRead, TestWrite, TestSync, NULL, TestRead};
    CpPlatformFile log = {record, TestRead, TestWrite, TestSync, NULL, TestRead};
    CpCardSave chip;
    if (CpCardSave_Detect("IRAO", &chip) != CP_CHIP_OK) return NULL;
    return CpCardStore_Open(&chip, 1234, 0x04070200, &sd, &log, TestCardCheck, NULL, out);
}
#endif
