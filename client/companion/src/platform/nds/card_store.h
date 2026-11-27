#ifndef COMPANION_NDS_CARD_STORE_H
#define COMPANION_NDS_CARD_STORE_H

#include "platform/file.h"
#include "platform/nds/card_save.h"

#if CP_CARD_SAVE_WRITES
typedef struct CpCardStore CpCardStore;

/* Both SD files have CP_SAVE_SIZE bytes. The intent file starts zeroed and
 * survives reopening. check() validates the powered, authenticated ROM session. */
CpCardStore *CpCardStore_Open(const CpCardSave *chip, uint32_t romID, uint32_t build,
    const CpPlatformFile *copy, const CpPlatformFile *intent, int (*check)(void *),
    void *card, CpPlatformFile *out);
void CpCardStore_Close(CpCardStore *store);
#endif
#endif
