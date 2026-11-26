#ifndef CP_MUSICAL_AREA_H
#define CP_MUSICAL_AREA_H

#include "file.h"
#include "platform/journal_io.h"

/* Black/White and Black 2/White 2 keep the journal banks in the musical
 * distribution save, as the patched game does (client/games/src/wonder_trade/bw2/
 * musical_area.c). A save that once stored a downloaded musical can still
 * hold its data there after the game stopped reading it: such leftovers are
 * erased before the journal is created, and only once the save the game
 * loads no longer marks a musical as saved (adapter/gen5.h
 * CpGen5_ReleaseMusical). A journal starts each bank it wrote with its magic;
 * banks without one hold no journal. */
enum {
    CP_MUSICAL_FREE = 0,     /* both banks blank (0x00 or 0xFF) */
    CP_MUSICAL_JOURNAL = 1,  /* a bank starts with a journal record */
    CP_MUSICAL_LEFTOVER = 2  /* other data and no journal */
};

/* The banks' state, or -1 when they cannot be read. */
int CpMusicalArea_Inspect(const CpFile *, const CpJournalArea *);
/* Fills each bank that is not blank with erased flash (0xFF), synced and
 * read back; zero once both banks are blank or hold the journal. Only after
 * the release is durable: the game must never read the area again. */
int CpMusicalArea_Erase(const CpFile *, const CpJournalArea *);

#endif
