#ifndef WONDER_TRADE_BW2_MUSICAL_AREA_H
#define WONDER_TRADE_BW2_MUSICAL_AREA_H

/* Black 2 and White 2 keep the journal banks in the musical distribution save
 * (client/core/save-layouts/save-layout-b2w2.json). A save that once stored a downloaded
 * musical can still hold its data there after the game released the area.
 * The journal is created only in two blank banks, so leftover data is erased
 * first: after the release is durable, one bank at a time. A journal always
 * starts each bank it has written with its magic, so banks without one hold
 * no journal; an interrupted erase leaves the same state and repeats. */

#define WT_MUSICAL_AREA_BANK_SIZE 4096

enum {
    WT_MUSICAL_AREA_FREE = 0,     /* both banks blank */
    WT_MUSICAL_AREA_JOURNAL = 1,  /* a bank starts with a journal record */
    WT_MUSICAL_AREA_LEFTOVER = 2  /* other data and no journal */
};

typedef struct WtMusicalAreaIO {
    void *arg;
    int (*read)(void *arg, int bank, unsigned char bytes[WT_MUSICAL_AREA_BANK_SIZE]);
    /* Fills the bank with erased flash and verifies it. */
    int (*erase)(void *arg, int bank);
} WtMusicalAreaIO;

/* The area's state, or -1 when a bank cannot be read. */
int WtMusicalArea_Inspect(WtMusicalAreaIO io);
/* Erases leftover data; zero once both banks are blank or hold the journal.
 * Call only after the release is durable: the game must never read the area. */
int WtMusicalArea_Erase(WtMusicalAreaIO io);

#endif
