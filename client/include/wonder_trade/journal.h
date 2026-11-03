#ifndef WONDER_TRADE_JOURNAL_H
#define WONDER_TRADE_JOURNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WT_JOURNAL_RECORD_SIZE 4096
#define WT_JOURNAL_FRAME_MAX 256

enum WtJournalPhase {
    WT_JOURNAL_EMPTY,
    WT_JOURNAL_ENROLLING,
    WT_JOURNAL_IDLE,
    WT_JOURNAL_ESCROW,
    WT_JOURNAL_SUBMIT_READY,
    WT_JOURNAL_QUEUED,
    WT_JOURNAL_RECEIVED,
    WT_JOURNAL_APPLYING,
    WT_JOURNAL_APPLIED,
    WT_JOURNAL_RESTORING,
    WT_JOURNAL_RECOVERY_REQUIRED
};

enum WtJournalError {
    WT_JOURNAL_NEEDS_SEED = 1,
    WT_JOURNAL_ERROR_UNKNOWN = -37,
    WT_JOURNAL_ERROR_CONFLICT = -38,
    WT_JOURNAL_ERROR_READ = -39,
    WT_JOURNAL_ERROR_SELF_TEST = -40
};

/* These are values, never an on-flash C structure. The codec owns every offset. */
typedef struct WtJournalLocator {
    unsigned char kind, box, slot, length;
} WtJournalLocator;

typedef struct WtJournalEntry {
    uint16_t phase;
    uint64_t generation;
    unsigned char saveID[16], credential[32], enrollmentID[16];
    uint64_t nextCounter, eventCursor;
    unsigned char lastReceipt[16], lastOperation[16];
    uint64_t lastOperationCounter;
    unsigned char operation[16];
    uint64_t operationCounter;
    unsigned char trade[16], receipt[16];
    WtJournalLocator source, destination;
    unsigned char deliveryKind;
    uint32_t globalCounter, generalCounter, boxCounter;
    unsigned char inventoryBefore[32], inventoryAfter[32];
    /* Original hashes stored source.length bytes; incoming/applied hash
     * canonical PK4. The game proves applied canonicalization against the
     * encrypted destinationAfter image, as it proves original/outgoing binding. */
    /* QUEUED may retain a receipt/digest learned from authenticated history
     * before Collect supplies the payload. No insertion is authorized by it. */
    unsigned char originalDigest[32], incomingDigest[32], appliedDigest[32];
    unsigned char original[236], destinationBefore[236], destinationAfter[236];
    unsigned char outgoing[136], incoming[136], seed[32];
    uint16_t requestLength;
    unsigned char request[WT_JOURNAL_FRAME_MAX];
} WtJournalEntry;

typedef int (*WtJournalReadBank)(void *, int, unsigned char[4096]);
/* Success includes tag-last writes and complete readback at a durable boundary. */
typedef int (*WtJournalWriteBank)(void *, int, const unsigned char[4096]);
typedef int (*WtJournalHash)(const unsigned char *, size_t, unsigned char[32]);

typedef struct WtJournalIO {
    void *arg;
    WtJournalReadBank read;
    WtJournalWriteBank write;
    WtJournalHash hash;
} WtJournalIO;

typedef struct WtJournal {
    unsigned char record[WT_JOURNAL_RECORD_SIZE];
    unsigned char observed[2][32];
    WtJournalIO io;
    int selected, usable;
} WtJournal;

/* Format and persistence primitives. Callers must prove inventory/server
 * transitions before committing them; this module does not authorize a trade. */
int WtJournal_Encode(const WtJournalEntry *, WtJournalHash, unsigned char *);
int WtJournal_Decode(const unsigned char *, WtJournalHash, WtJournalEntry *);
/* Serialize whole operations for a save, including the read/compare/write
 * sequence. Per-bank locking alone is insufficient. */
int WtJournal_Open(WtJournal *, WtJournalIO);
/* Legacy connectivity must refuse active journals, including torn ones. */
int WtJournal_OpenEmpty(WtJournal *, WtJournalIO);
/* Initialize only two freshly verified blank banks, with the existing format.
 * NULL seed is read-only classification and returns NEEDS_SEED for blank data.
 * Existing/torn/unknown records are never replaced. Serialize the whole call. */
int WtJournal_Initialize(WtJournal *, WtJournalIO, const unsigned char *seed);
int WtJournal_Read(const WtJournal *, WtJournalEntry *);
int WtJournal_Repair(WtJournal *);
/* Compare-and-commit against the exact opened generation. A failed write
 * invalidates this handle. Reopen and reconcile before retrying anything. */
int WtJournal_Commit(WtJournal *, const WtJournalEntry *);
void WtJournal_Close(WtJournal *);

#ifdef __cplusplus
}
#endif

#endif
