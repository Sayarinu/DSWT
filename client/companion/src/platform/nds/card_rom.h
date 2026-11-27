#ifndef COMPANION_NDS_CARD_ROM_H
#define COMPANION_NDS_CARD_ROM_H

#include "game/reader.h"

enum { CP_CARD_PAGE = 512, CP_CARD_HEADER = 4096, CP_CARD_SECURE = 16384, CP_CARD_KEY_WORDS = 1042 };

/* Read-only Slot-1 bus. Command bytes go out most significant first. A reset
 * power cycles the slot. Once ejected, power stays off until an explicit reset.
 * The transport returns an error on a timeout, removal or short transfer. */
typedef struct CpCardBus {
    void *context;
    int (*reset)(void *);
    int (*transfer)(void *, uint64_t command, uint32_t flags, void *out, unsigned bytes);
    void (*seed)(void *, uint64_t seed0);
    void (*delay)(void *, unsigned microseconds);
    int (*loadKey)(void *, int twl, uint32_t table[CP_CARD_KEY_WORDS]);
    uint32_t generation; /* advanced by Open, including failed attempts */
} CpCardBus;

enum { CP_CARD_NTR_KEY_MISSING = -10, CP_CARD_TWL_KEY_MISSING = -11 };

typedef struct CpCardRom {
    const CpCardBus *bus;
    uint8_t header[CP_CARD_HEADER], secure[CP_CARD_SECURE], secureTwl[CP_CARD_SECURE];
    uint32_t id, capacity, twlStart, dataFlags;
    uint32_t pageOffset, generation;
    uint8_t page[CP_CARD_PAGE];
    int active, inherited;
} CpCardRom;

/* Allocate the header and secure-area caches off the DS stack. Keep this
 * object and the bus alive until Close. Reopening the bus or a failed card
 * check invalidates the previous session. */
int CpCardRom_Open(CpCardRom *, CpCardBus *, CpReader *);
int CpCardRom_OpenHandoff(CpCardRom *, CpCardBus *, uint32_t control, CpReader *);
int CpCardRom_Check(CpCardRom *);
void CpCardRom_Close(CpCardRom *);

/* The physical slot on DSi and 3DS in DSi mode. */
int CpNdsSlot_Present(void);
/* On retry, use a full cold start and return the raw ROM chip ID. */
int CpNdsSlot_Header(uint8_t header[CP_CARD_PAGE], uint32_t *retryChipId);
int CpNdsSlot_Handoff(CpCardRom *, CpReader *);
int CpNdsSlot_Open(CpCardRom *, CpReader *);

#endif
