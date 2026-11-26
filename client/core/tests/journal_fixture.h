#ifndef WT_TEST_JOURNAL_FIXTURE_H
#define WT_TEST_JOURNAL_FIXTURE_H

/* Synthetic format fixtures, with no ROM, save or user Pokemon data. */
#include <string.h>
#include "wonder_trade/journal.h"
#include "wonder_trade/wire.h"

static void WtTest_Pokemon(unsigned char bytes[136], unsigned char species)
{
    memset(bytes, 0, 136);
    bytes[0] = 123; bytes[6] = species; bytes[8] = species;
}

static int WtTest_JournalEntry(unsigned phase, WtJournalHash hash, WtJournalEntry *entry)
{
    WtWireRequest request;
    size_t size;
    if (phase > WT_JOURNAL_RECOVERY_REQUIRED) return -1;
    memset(entry, 0, sizeof(*entry)); memset(&request, 0, sizeof(request));
    entry->phase = (uint16_t)phase; entry->generation = 20; memset(entry->seed, 4, 32);
    if (phase == WT_JOURNAL_EMPTY) return 0;
    memset(entry->saveID, 1, 16); memset(entry->credential, 2, 32); memset(entry->enrollmentID, 3, 16);
    entry->nextCounter = 1; entry->eventCursor = 1;
    memcpy(request.saveID, entry->saveID, 16); memcpy(request.credential, entry->credential, 32);
    if (phase == WT_JOURNAL_ENROLLING) {
        memset(entry->saveID, 0, 16); memset(request.saveID, 0, 16); entry->eventCursor = 0;
        request.operation = WT_OP_ENROLL; memcpy(request.requestID, entry->enrollmentID, 16);
        request.body.enroll.build = 1; request.body.enroll.hintLength = 64; memset(request.body.enroll.hints, 0x42, 64);
    } else if (phase >= WT_JOURNAL_ESCROW) {
        entry->source.kind = 1; entry->source.slot = 2; entry->source.length = 236;
        memset(entry->operation, 5, 16); entry->operationCounter = 1;
        memset(entry->original, 6, 236); WtTest_Pokemon(entry->outgoing, 25);
        memset(entry->inventoryBefore, 8, 32); memset(entry->inventoryAfter, 9, 32);
        if (hash(entry->original, 236, entry->originalDigest) != 0) return -1;
        if (phase <= WT_JOURNAL_QUEUED || phase == WT_JOURNAL_RECOVERY_REQUIRED) {
            request.operation = WT_OP_SUBMIT; request.counter = 1;
            memcpy(request.requestID, entry->operation, 16); request.body.submit.generation = 10;
            memcpy(request.body.submit.pokemon, entry->outgoing, 136);
        }
        if (phase >= WT_JOURNAL_QUEUED && phase <= WT_JOURNAL_RESTORING) {
            entry->nextCounter = 2; entry->lastOperationCounter = 1; memcpy(entry->lastOperation, entry->operation, 16);
            entry->eventCursor = 2;
            if (phase <= WT_JOURNAL_APPLIED) memset(entry->trade, 11, 16);
        }
        if (phase >= WT_JOURNAL_RECEIVED && phase <= WT_JOURNAL_APPLIED) {
            memset(entry->receipt, 12, 16); entry->deliveryKind = WT_DELIVERY_MATCH;
            WtTest_Pokemon(entry->incoming, 2);
            if (hash(entry->incoming, 136, entry->incomingDigest) != 0) return -1;
            entry->eventCursor = 3;
        }
        if (phase == WT_JOURNAL_APPLYING || phase == WT_JOURNAL_APPLIED) {
            entry->destination.kind = 2; entry->destination.box = 17; entry->destination.slot = 29; entry->destination.length = 136;
            memset(entry->destinationBefore, 13, 136); memset(entry->destinationAfter, 14, 136);
            memset(entry->appliedDigest, 17, 32);
        }
        if (phase == WT_JOURNAL_APPLIED) {
            memcpy(entry->lastReceipt, entry->receipt, 16);
            memset(entry->operation, 18, 16); entry->operationCounter = 2;
            request.operation = WT_OP_ACKNOWLEDGE; request.counter = 2;
            memcpy(request.requestID, entry->operation, 16);
            memcpy(request.body.acknowledge.trade, entry->trade, 16);
            memcpy(request.body.acknowledge.receipt, entry->receipt, 16);
            memcpy(request.body.acknowledge.deliveredDigest, entry->incomingDigest, 32);
            memcpy(request.body.acknowledge.appliedDigest, entry->appliedDigest, 32);
            request.body.acknowledge.generation = 19;
        }
    }
    if (request.operation) {
        if (WtWire_Encode(&request, hash, entry->request, 256, &size) != 0) return -1;
        entry->requestLength = (uint16_t)size;
    }
    return 0;
}
#endif
