#ifndef POKEPLATINUM_WONDER_TRADE_PERSISTENCE_H
#define POKEPLATINUM_WONDER_TRADE_PERSISTENCE_H

/* Virtual, read-only capability page supplied by compatible hosts. Each
 * game family has its own address that the game never reads or writes
 * (persistence_address in protocol/save-layout*.json), so the page stays
 * untouched in the actual save file. */
#define WT_PERSISTENCE_ADDRESS_GEN4 0x2FF00
#define WT_PERSISTENCE_ADDRESS_BW 0x47FC0
#define WT_PERSISTENCE_ADDRESS_B2W2 0x4BFC0
/* wonder_trade/game_family.h selects the build's family; the default serves Generation IV. */
#ifndef WT_PERSISTENCE_ADDRESS
#define WT_PERSISTENCE_ADDRESS WT_PERSISTENCE_ADDRESS_GEN4
#endif
#define WT_PERSISTENCE_SIZE 32
#define WT_PERSISTENCE_DESCRIPTOR "DSWF\1\0\40\0checked-host-storage-v1\0"

#endif
