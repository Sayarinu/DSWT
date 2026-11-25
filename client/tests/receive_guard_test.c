#include "wonder_trade/receive_guard.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    CHECK(size == 136);
    memcpy(digest, bytes, 32);
    return 0;
}

int main(void)
{
    WtJournalEntry entry;
    WtReceiveScan scan;
    unsigned char empty[236] = { 0 }, stored[236] = { 0 }, canonical[136] = { 0 };
    unsigned i, length;
    memset(&entry, 0, sizeof(entry));
    entry.deliveryKind = WT_DELIVERY_RETURN;
    entry.globalCounter = entry.generalCounter = entry.boxCounter = 10;
    canonical[0] = 123; canonical[8] = canonical[6] = 25;
    memcpy(entry.outgoing, canonical, 136); memcpy(entry.incoming, canonical, 136);
    CHECK(WtPokemon_Encode(canonical, stored) == 0);
    memset(canonical, 0, sizeof(canonical));
    CHECK(WtPokemon_Encode(canonical, empty) == 0);
    for (length = 136; length <= 236; length += length == 136 ? 84 : 16) {
        memset(&scan, 0, sizeof(scan));
        CHECK(WtReceive_Inspect(&scan, entry.outgoing, empty, 2, 0, 0, 136) == 0);
        CHECK(scan.matches == 0 && WtReceive_Check(&entry, &scan, 10, 10, 10) == 0);
        CHECK(WtReceive_Inspect(&scan, entry.outgoing, stored, length == 136 ? 2 : 1, 0, 1, length) == 0);
        CHECK(WtReceive_Check(&entry, &scan, 11, 10, 10) == 1);
        CHECK(WtReceive_Check(&entry, &scan, 10, 11, 10) == 1);
        CHECK(WtReceive_Check(&entry, &scan, 10, 10, 11) == 1);
        CHECK(WtReceive_Check(&entry, &scan, 10, 10, 10) == -1);
        CHECK(WtReceive_Check(&entry, &scan, 9, 11, 11) == -1);
        CHECK(WtReceive_Check(&entry, &scan, 11, 9, 11) == -1);
        CHECK(WtReceive_Check(&entry, &scan, 11, 11, 9) == -1);
        entry.deliveryKind = WT_DELIVERY_MATCH;
        CHECK(WtReceive_Check(&entry, &scan, 11, 11, 11) == -1);
        entry.deliveryKind = WT_DELIVERY_RETURN;
        memset(entry.destinationBefore, 0xAA, 236); memset(entry.destinationAfter, 0xAA, 236);
        CHECK(WtReceive_Record(&entry, &scan, empty, Hash) == 0);
        CHECK(entry.destination.length == length && entry.destination.slot == 1);
        CHECK(memcmp(entry.destinationBefore, empty, length) == 0);
        CHECK(memcmp(entry.destinationAfter, stored, length) == 0);
        for (i = length; i < 236; ++i) CHECK(entry.destinationBefore[i] == 0 && entry.destinationAfter[i] == 0);
        CHECK(memcmp(entry.appliedDigest, entry.outgoing, 32) == 0);
        CHECK(WtReceive_Inspect(&scan, entry.outgoing, stored, 2, 0, 2, 136) == -1);
    }
    /* Counter rollover is forward; half-range ambiguity is refused. */
    scan.matches = 1;
    entry.globalCounter = entry.generalCounter = entry.boxCounter = UINT32_MAX;
    CHECK(WtReceive_Check(&entry, &scan, 0, 0, 0) == 1);
    CHECK(WtReceive_Check(&entry, &scan, UINT32_C(0x7FFFFFFF), 0, 0) == -1);
    memset(&scan, 0, sizeof(scan));
    memcpy(canonical, entry.outgoing, 136); ++canonical[10]; ++canonical[6];
    CHECK(WtPokemon_Encode(canonical, stored) == 0);
    CHECK(WtReceive_Inspect(&scan, entry.outgoing, stored, 1, 0, 0, 236) == -1);
    CHECK(WtReceive_Inspect(&scan, entry.outgoing, NULL, 1, 0, 0, 236) == -1);
    stored[7] ^= 1;
    CHECK(WtReceive_Inspect(&scan, entry.outgoing, stored, 1, 0, 0, 236) == -1);
    puts("Receive guard: exact identity, counters, wraparound and padded PK4/PK5 records passed");
    return 0;
}
