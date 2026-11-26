#include "musical_area.h"

#include <string.h>

enum { BANK = 4096 };

/* One bank at a time, outside the worker's stack. */
static unsigned char sBank[BANK];

static int Blank(const unsigned char *bytes)
{
    size_t i;
    if (bytes[0] != 0 && bytes[0] != 0xFF) return 0;
    for (i = 1; i < BANK; ++i) if (bytes[i] != bytes[0]) return 0;
    return 1;
}

static int Read(const CpFile *file, const CpJournalArea *area, int bank)
{
    return file && file->readAt && area ? file->readAt(file->context, area->bank[bank], sBank, BANK) : -1;
}

int CpMusicalArea_Inspect(const CpFile *file, const CpJournalArea *area)
{
    int bank, journal = 0, blank = 1;
    for (bank = 0; bank < 2; ++bank) {
        if (Read(file, area, bank) != 0) {
            memset(sBank, 0, sizeof(sBank));
            return -1;
        }
        journal |= memcmp(sBank, "DSWJ", 4) == 0;
        blank &= Blank(sBank);
    }
    memset(sBank, 0, sizeof(sBank));
    if (journal) return CP_MUSICAL_JOURNAL;
    return blank ? CP_MUSICAL_FREE : CP_MUSICAL_LEFTOVER;
}

int CpMusicalArea_Erase(const CpFile *file, const CpJournalArea *area)
{
    int state = CpMusicalArea_Inspect(file, area), bank, result = 0;
    if (state != CP_MUSICAL_LEFTOVER) return state < 0 ? -1 : 0;
    if (!file->writeAt || !file->sync) return -1;
    for (bank = 0; bank < 2 && result == 0; ++bank) {
        if (Read(file, area, bank) != 0) { result = -1; break; }
        if (Blank(sBank)) continue;
        memset(sBank, 0xFF, sizeof(sBank));
        if (file->writeAt(file->context, area->bank[bank], sBank, BANK) != 0 || file->sync(file->context) != 0
            || Read(file, area, bank) != 0 || !Blank(sBank) || sBank[0] != 0xFF) result = -1;
    }
    memset(sBank, 0, sizeof(sBank));
    if (result == 0 && CpMusicalArea_Inspect(file, area) != CP_MUSICAL_FREE) result = -1;
    return result;
}
