#ifndef POKEPLATINUM_WONDER_TRADE_PERSISTENCE_H
#define POKEPLATINUM_WONDER_TRADE_PERSISTENCE_H

/* Virtual, read-only capability page supplied by the supported emulator.
 * This unused retail address stays untouched in the actual save file. */
#define WT_PERSISTENCE_ADDRESS 0x2FF00
#define WT_PERSISTENCE_SIZE 32
#define WT_PERSISTENCE_DESCRIPTOR "DSWF\1\0\40\0checked-host-storage-v1\0"

#endif
