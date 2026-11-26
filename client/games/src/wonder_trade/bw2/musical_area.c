#include "wonder_trade/bw2/musical_area.h"

#include <string.h>

/* One bank at a time: the boot check's worker and the screen never overlap. */
static unsigned char sBank[WT_MUSICAL_AREA_BANK_SIZE];

static int Blank(const unsigned char *bytes)
{
    unsigned i;
    if (bytes[0] != 0 && bytes[0] != 0xff) return 0;
    for (i = 1; i < WT_MUSICAL_AREA_BANK_SIZE; ++i) if (bytes[i] != bytes[0]) return 0;
    return 1;
}

/* Every journal record begins with its magic (wonder_trade/journal.c), and a
 * bank write starts at the record's first page. */
static int Journal(const unsigned char *bytes) { return memcmp(bytes, "DSWJ", 4) == 0; }

int WtMusicalArea_Inspect(WtMusicalAreaIO io)
{
    int bank, journal = 0, blank = 1;
    for (bank = 0; bank < 2; ++bank) {
        if (io.read(io.arg, bank, sBank) != 0) {
            memset(sBank, 0, sizeof(sBank));
            return -1;
        }
        journal |= Journal(sBank);
        blank &= Blank(sBank);
    }
    memset(sBank, 0, sizeof(sBank));
    if (journal) return WT_MUSICAL_AREA_JOURNAL;
    return blank ? WT_MUSICAL_AREA_FREE : WT_MUSICAL_AREA_LEFTOVER;
}

int WtMusicalArea_Erase(WtMusicalAreaIO io)
{
    int state = WtMusicalArea_Inspect(io), bank, result = 0;
    if (state != WT_MUSICAL_AREA_LEFTOVER) return state < 0 ? -1 : 0;
    for (bank = 0; bank < 2 && result == 0; ++bank) {
        if (io.read(io.arg, bank, sBank) != 0) result = -1;
        else if (!Blank(sBank) && io.erase(io.arg, bank) != 0) result = -1;
    }
    memset(sBank, 0, sizeof(sBank));
    if (result == 0 && WtMusicalArea_Inspect(io) != WT_MUSICAL_AREA_FREE) result = -1;
    return result;
}
