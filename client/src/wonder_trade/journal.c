#include "wonder_trade/journal.h"

#include <string.h>

static const unsigned char kHashABC[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
};

static int Zero(const unsigned char *bytes, size_t length)
{
    size_t i;
    unsigned char value = 0;
    for (i = 0; i < length; i++) value |= bytes[i];
    return value == 0;
}

static void Clear(void *pointer, size_t length)
{
    volatile unsigned char *bytes = pointer;
    size_t i;
    for (i = 0; i < length; i++) bytes[i] = 0;
}

static uint64_t Get(const unsigned char *bytes, int length)
{
    uint64_t value = 0;
    int i;
    for (i = length - 1; i >= 0; i--) value = (value << 8) | bytes[i];
    return value;
}

static void Put(unsigned char *bytes, uint64_t value, int length)
{
    int i;
    for (i = 0; i < length; i++) { bytes[i] = (unsigned char)value; value >>= 8; }
}

static int LocatorValid(WtJournalLocator location)
{
    if (location.kind == 0) return location.box == 0 && location.slot == 0 && location.length == 0;
    if (location.kind == 1) return location.box == 0 && location.slot < 6 && location.length == 236;
    return location.kind == 2 && location.box < 18 && location.slot < 30 && location.length == 136;
}

static void PutLocator(unsigned char *bytes, WtJournalLocator location)
{
    bytes[0] = location.kind; bytes[1] = location.box;
    bytes[2] = location.slot; bytes[3] = location.length;
}

static WtJournalLocator GetLocator(const unsigned char *bytes)
{
    WtJournalLocator result;
    result.kind = bytes[0]; result.box = bytes[1]; result.slot = bytes[2]; result.length = bytes[3];
    return result;
}

static int LayoutKnown(const unsigned char *record)
{
    /* Check future formats even when their digest/tag is torn. All these
     * reserved bytes are zero in every supported phase. */
    return memcmp(record, "DSWJ\1\0", 6) == 0
        && Get(record + 6, 2) <= WT_JOURNAL_RECOVERY_REQUIRED
        && (Get(record + 6, 2) != WT_JOURNAL_EMPTY || (Zero(record + 32, 1408) && Zero(record + 1952, 264)))
        && Get(record + 8, 4) == 4096 && Zero(record + 12, 4) && Zero(record + 24, 8)
        && Zero(record + 152, 8) && Zero(record + 397, 19) && Zero(record + 1396, 44)
        && memcmp(record + 1440, "\1\0\40\0", 4) == 0 && Zero(record + 1476, 476)
        && Zero(record + 1954, 6) && Zero(record + 2216, 1844);
}

static int RequestValid(const WtJournalEntry *entry, WtJournalHash hash)
{
    const unsigned char *frame = entry->request;
    uint16_t length = entry->requestLength;
    unsigned char digest[32];
    unsigned opcode;
    if (length == 0) return Zero(frame, 256);
    if (length < 80 || length > 256 || !Zero(frame + length, 256 - length)
        || memcmp(frame, "WTDS\1", 5) != 0 || Get(frame + 6, 2) != (unsigned)length - 80
        || memcmp(frame + 40, entry->credential, 32) != 0) return 0;
    opcode = frame[5];
    if (opcode == 1) {
        return entry->phase == WT_JOURNAL_ENROLLING && length >= 85 && length <= 149
            && Get(frame + 84, 1) == (unsigned)length - 85 && Zero(frame + 24, 16)
            && Get(frame + 72, 8) == 0 && memcmp(frame + 8, entry->enrollmentID, 16) == 0;
    }
    if (memcmp(frame + 24, entry->saveID, 16) != 0 || Zero(frame + 8, 16)) return 0;
    /* Only mutations require crash-stable packet storage. Reads can be rebuilt. */
    if (opcode != 2 && opcode != 4 && opcode != 6) return 0;
    if (memcmp(frame + 8, entry->operation, 16) != 0 || entry->operationCounter == 0 || entry->operationCounter == UINT64_MAX
        || Get(frame + 72, 8) != entry->operationCounter) return 0;
    if (opcode == 2) {
        return length == 256 && Get(frame + 80, 8) != 0 && Get(frame + 80, 8) <= entry->generation
            && memcmp(frame + 88, entry->outgoing, 136) == 0
            && hash(entry->outgoing, 136, digest) == 0 && memcmp(frame + 224, digest, 32) == 0;
    }
    if (opcode == 4) return length == 96 && !Zero(entry->trade, 16) && memcmp(frame + 80, entry->trade, 16) == 0;
    return length == 184 && !Zero(entry->trade, 16) && !Zero(entry->receipt, 16)
        && memcmp(frame + 80, entry->trade, 16) == 0 && memcmp(frame + 96, entry->receipt, 16) == 0
        && memcmp(frame + 112, entry->incomingDigest, 32) == 0 && memcmp(frame + 144, entry->appliedDigest, 32) == 0
        && Get(frame + 176, 8) != 0 && Get(frame + 176, 8) <= entry->generation;
}

static int DigestMatches(const unsigned char *bytes, size_t length, const unsigned char *expected, WtJournalHash hash)
{
    unsigned char digest[32];
    return hash(bytes, length, digest) == 0 && memcmp(digest, expected, 32) == 0;
}

static int EntryValid(const WtJournalEntry *entry, WtJournalHash hash)
{
    if (entry->phase > WT_JOURNAL_RECOVERY_REQUIRED || Zero(entry->seed, 32)
        || !LocatorValid(entry->source) || !LocatorValid(entry->destination)
        || entry->deliveryKind > 2 || !RequestValid(entry, hash)) return 0;
    if (entry->phase != WT_JOURNAL_EMPTY) {
        if (Zero(entry->credential, 32) || Zero(entry->enrollmentID, 16) || entry->nextCounter == 0) return 0;
        if ((entry->phase == WT_JOURNAL_ENROLLING) != Zero(entry->saveID, 16)) return 0;
        if (entry->phase == WT_JOURNAL_ENROLLING && (entry->nextCounter != 1 || entry->eventCursor != 0
            || !Zero(entry->lastReceipt, 16) || !Zero(entry->lastOperation, 16) || entry->lastOperationCounter != 0)) return 0;
        if ((entry->lastOperationCounter == 0) != Zero(entry->lastOperation, 16)
            || entry->lastOperationCounter >= entry->nextCounter) return 0;
    }
    if (entry->phase >= WT_JOURNAL_ESCROW && entry->phase != WT_JOURNAL_RECOVERY_REQUIRED) {
        if (entry->source.kind == 0 || Zero(entry->original, 236) || Zero(entry->outgoing, 136)
            || Zero(entry->inventoryBefore, 32) || Zero(entry->inventoryAfter, 32) || Zero(entry->originalDigest, 32)) return 0;
        if (entry->phase == WT_JOURNAL_ESCROW || entry->phase == WT_JOURNAL_SUBMIT_READY) {
            if (Zero(entry->operation, 16) || entry->operationCounter == 0) return 0;
        }
        if (entry->phase >= WT_JOURNAL_QUEUED && entry->phase <= WT_JOURNAL_APPLIED && Zero(entry->trade, 16)) return 0;
        if (entry->phase >= WT_JOURNAL_RECEIVED && entry->phase <= WT_JOURNAL_APPLIED
            && (Zero(entry->receipt, 16) || entry->deliveryKind == 0 || Zero(entry->incoming, 136) || Zero(entry->incomingDigest, 32))) return 0;
        if ((entry->phase == WT_JOURNAL_APPLYING || entry->phase == WT_JOURNAL_APPLIED)
            && (entry->destination.kind == 0 || Zero(entry->appliedDigest, 32))) return 0;
        if (entry->phase == WT_JOURNAL_APPLIED && memcmp(entry->lastReceipt, entry->receipt, 16) != 0) return 0;
    }
    if ((entry->source.kind == 2 && !Zero(entry->original + 136, 100))
        || (entry->destination.kind == 2 && (!Zero(entry->destinationBefore + 136, 100)
            || !Zero(entry->destinationAfter + 136, 100)))) return 0;
    if ((!Zero(entry->originalDigest, 32) && (entry->source.kind == 0
            || !DigestMatches(entry->original, entry->source.length, entry->originalDigest, hash)))
        || (!Zero(entry->incomingDigest, 32)
            && !(entry->phase == WT_JOURNAL_QUEUED && !Zero(entry->receipt, 16) && Zero(entry->incoming, 136))
            && !DigestMatches(entry->incoming, 136, entry->incomingDigest, hash))) return 0;
    return 1;
}

int WtJournal_Encode(const WtJournalEntry *entry, WtJournalHash hash, unsigned char *record)
{
    if (!entry || !hash || !record || !EntryValid(entry, hash)) return -1;
    memset(record, 0, 4096);
    memcpy(record, "DSWJ\1\0", 6); Put(record + 6, entry->phase, 2);
    Put(record + 8, 4096, 4); Put(record + 16, entry->generation, 8);
    memcpy(record + 32, entry->saveID, 16); memcpy(record + 48, entry->credential, 32);
    memcpy(record + 80, entry->enrollmentID, 16);
    Put(record + 96, entry->nextCounter, 8); Put(record + 104, entry->eventCursor, 8);
    memcpy(record + 112, entry->lastReceipt, 16); memcpy(record + 128, entry->lastOperation, 16);
    Put(record + 144, entry->lastOperationCounter, 8);
    memcpy(record + 160, entry->operation, 16); Put(record + 176, entry->operationCounter, 8);
    memcpy(record + 184, entry->trade, 16); memcpy(record + 200, entry->receipt, 16);
    PutLocator(record + 216, entry->source); PutLocator(record + 220, entry->destination);
    record[224] = entry->deliveryKind;
    Put(record + 225, entry->globalCounter, 4); Put(record + 229, entry->generalCounter, 4);
    Put(record + 233, entry->boxCounter, 4);
    memcpy(record + 237, entry->inventoryBefore, 32); memcpy(record + 269, entry->inventoryAfter, 32);
    memcpy(record + 301, entry->originalDigest, 32); memcpy(record + 333, entry->incomingDigest, 32);
    memcpy(record + 365, entry->appliedDigest, 32);
    memcpy(record + 416, entry->original, 236); memcpy(record + 652, entry->destinationBefore, 236);
    memcpy(record + 888, entry->destinationAfter, 236); memcpy(record + 1124, entry->outgoing, 136);
    memcpy(record + 1260, entry->incoming, 136);
    memcpy(record + 1440, "\1\0\40\0", 4); memcpy(record + 1444, entry->seed, 32);
    Put(record + 1952, entry->requestLength, 2); memcpy(record + 1960, entry->request, 256);
    if (entry->phase == WT_JOURNAL_EMPTY && (!Zero(record + 32, 1408) || !Zero(record + 1952, 264))) return -1;
    if (entry->phase <= WT_JOURNAL_IDLE && !Zero(record + 160, 1280)) return -1;
    if (hash(record, 4060, record + 4060) != 0) return -1;
    memcpy(record + 4092, "CMIT", 4);
    return 0;
}

int WtJournal_Decode(const unsigned char *record, WtJournalHash hash, WtJournalEntry *entry)
{
    unsigned char digest[32];
    if (!record || !entry || !hash) return -1;
    memset(entry, 0, sizeof(*entry));
    if (!LayoutKnown(record) || memcmp(record + 4092, "CMIT", 4) != 0
        || hash(record, 4060, digest) != 0 || memcmp(digest, record + 4060, 32) != 0) return -1;
    entry->phase = (uint16_t)Get(record + 6, 2); entry->generation = Get(record + 16, 8);
    memcpy(entry->saveID, record + 32, 16); memcpy(entry->credential, record + 48, 32);
    memcpy(entry->enrollmentID, record + 80, 16);
    entry->nextCounter = Get(record + 96, 8); entry->eventCursor = Get(record + 104, 8);
    memcpy(entry->lastReceipt, record + 112, 16); memcpy(entry->lastOperation, record + 128, 16);
    entry->lastOperationCounter = Get(record + 144, 8);
    memcpy(entry->operation, record + 160, 16); entry->operationCounter = Get(record + 176, 8);
    memcpy(entry->trade, record + 184, 16); memcpy(entry->receipt, record + 200, 16);
    entry->source = GetLocator(record + 216); entry->destination = GetLocator(record + 220);
    entry->deliveryKind = record[224];
    entry->globalCounter = (uint32_t)Get(record + 225, 4); entry->generalCounter = (uint32_t)Get(record + 229, 4);
    entry->boxCounter = (uint32_t)Get(record + 233, 4);
    memcpy(entry->inventoryBefore, record + 237, 32); memcpy(entry->inventoryAfter, record + 269, 32);
    memcpy(entry->originalDigest, record + 301, 32); memcpy(entry->incomingDigest, record + 333, 32);
    memcpy(entry->appliedDigest, record + 365, 32);
    memcpy(entry->original, record + 416, 236); memcpy(entry->destinationBefore, record + 652, 236);
    memcpy(entry->destinationAfter, record + 888, 236); memcpy(entry->outgoing, record + 1124, 136);
    memcpy(entry->incoming, record + 1260, 136); memcpy(entry->seed, record + 1444, 32);
    entry->requestLength = (uint16_t)Get(record + 1952, 2); memcpy(entry->request, record + 1960, 256);
    if (!EntryValid(entry, hash) || (entry->phase == WT_JOURNAL_EMPTY
        && (!Zero(record + 32, 1408) || !Zero(record + 1952, 264)))
        || (entry->phase <= WT_JOURNAL_IDLE && !Zero(record + 160, 1280))) {
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    return 0;
}

static int Blank(const unsigned char *record)
{
    size_t i;
    if (record[0] != 0 && record[0] != 255) return 0;
    for (i = 1; i < 4096; i++) if (record[i] != record[0]) return 0;
    return 1;
}

void WtJournal_Close(WtJournal *store)
{
    if (!store) return;
    Clear(store, sizeof(*store));
}

static int Sealed(const unsigned char *record, WtJournalHash hash)
{
    unsigned char digest[32];
    return memcmp(record + 4092, "CMIT", 4) == 0 && hash(record, 4060, digest) == 0
        && memcmp(record + 4060, digest, 32) == 0;
}

static int PrefixAndZeros(const unsigned char *bytes, const unsigned char *complete, size_t length)
{
    size_t prefix;
    for (prefix = 0; prefix <= length; prefix++) {
        if (memcmp(bytes, complete, prefix) == 0 && Zero(bytes + prefix, length - prefix)) return 1;
    }
    return 0;
}

static int ZerosAndSuffix(const unsigned char *bytes, const unsigned char *complete, size_t length)
{
    size_t prefix;
    for (prefix = 0; prefix <= length; prefix++) {
        if (Zero(bytes, prefix) && memcmp(bytes + prefix, complete + prefix, length - prefix) == 0) return 1;
    }
    return 0;
}

static int InitialPacketKnown(const unsigned char *record, int allowClear)
{
    unsigned char expected[264];
    unsigned length = (unsigned)Get(record + 1952, 2), candidate;
    unsigned candidates[2];
    if ((length != 0 && (length < 85 || length > 149)) || record[2044] > 64) return 0;
    candidates[0] = length ? length : 85u + record[2044]; candidates[1] = length ? length : 149u;
    for (candidate = 0; candidate < 2; candidate++) {
        unsigned size = candidates[candidate];
        memset(expected, 0, sizeof(expected)); Put(expected, size, 2);
        memcpy(expected + 8, "WTDS\1\1", 6); Put(expected + 14, size - 80, 2);
        memcpy(expected + 16, record + 80, 16); memcpy(expected + 48, record + 48, 32);
        memcpy(expected + 88, record + 2040, 4); expected[92] = (unsigned char)(size - 85);
        memcpy(expected + 93, record + 2045, size - 85);
        if (PrefixAndZeros(record + 1952, expected, sizeof(expected))
            || (allowClear && ZerosAndSuffix(record + 1952, expected, sizeof(expected)))) return 1;
    }
    return 0;
}

static int InitialEnrollment(const unsigned char *record)
{
    return Get(record + 6, 2) == WT_JOURNAL_ENROLLING && Zero(record + 32, 16)
        && Get(record + 96, 8) <= 1 && Zero(record + 104, 56) && Zero(record + 160, 1280)
        && InitialPacketKnown(record, 0);
}

static int TornIdentityKnown(const unsigned char *partial, const unsigned char *complete)
{
    uint16_t partialPhase = (uint16_t)Get(partial + 6, 2), completePhase = (uint16_t)Get(complete + 6, 2);
    if (Blank(partial)) return 1;
    if (partialPhase == WT_JOURNAL_EMPTY) return completePhase == WT_JOURNAL_EMPTY || InitialEnrollment(complete);
    if (completePhase == WT_JOURNAL_EMPTY) return InitialEnrollment(partial);
    /* Identity bytes never change after enrollment. A cut during the first
     * provisioning mirror can contain only a known prefix followed by zeros. */
    if (memcmp(partial + 48, complete + 48, 48) != 0
        && !(partialPhase == WT_JOURNAL_ENROLLING && completePhase == WT_JOURNAL_ENROLLING
            && PrefixAndZeros(partial + 48, complete + 48, 48))) return 0;
    if (partialPhase <= WT_JOURNAL_IDLE && Get(complete + 96, 8) == 1 && Zero(complete + 104, 56)
        && (completePhase == WT_JOURNAL_ENROLLING || completePhase == WT_JOURNAL_IDLE)
        && (Get(partial + 96, 8) > 1 || !Zero(partial + 104, 56) || !Zero(partial + 160, 1280)
            || !InitialPacketKnown(partial, 1))) return 0;
    if (Zero(complete + 32, 16)) return partialPhase <= WT_JOURNAL_IDLE
        && Get(partial + 96, 8) <= 1 && Zero(partial + 104, 56) && Zero(partial + 160, 1280)
        && (PrefixAndZeros(partial + 1952, complete + 1952, 264)
            || ZerosAndSuffix(partial + 1952, complete + 1952, 264));
    if (memcmp(partial + 32, complete + 32, 16) == 0) return 1;
    return partialPhase <= WT_JOURNAL_IDLE && completePhase == WT_JOURNAL_IDLE
        && Get(complete + 96, 8) == 1 && Get(complete + 104, 8) == 0
        && PrefixAndZeros(partial + 32, complete + 32, 16);
}

static int Follows(const unsigned char *before, const unsigned char *after)
{
    if ((Get(before + 6, 2) == WT_JOURNAL_EMPTY && Get(after + 6, 2) > WT_JOURNAL_ENROLLING)
        || (Get(before + 6, 2) == WT_JOURNAL_ENROLLING && Get(after + 6, 2) != WT_JOURNAL_ENROLLING
            && Get(after + 6, 2) != WT_JOURNAL_IDLE)) return 0;
    if (Get(after + 96, 8) < Get(before + 96, 8) || Get(after + 104, 8) < Get(before + 104, 8)
        || Get(after + 144, 8) < Get(before + 144, 8)) return 0;
    if (Get(after + 144, 8) == Get(before + 144, 8) && memcmp(after + 128, before + 128, 16) != 0) return 0;
    if (memcmp(before + 112, after + 112, 16) != 0
        && (Get(before + 6, 2) != WT_JOURNAL_APPLYING || Get(after + 6, 2) != WT_JOURNAL_APPLIED
            || memcmp(before + 200, after + 200, 16) != 0 || memcmp(after + 112, after + 200, 16) != 0)) return 0;
    if (Get(before + 1952, 2) != 0 && memcmp(before + 1952, after + 1952, 264) != 0) {
        if (Get(after + 1952, 2) != 0 && memcmp(before + 1968, after + 1968, 16) == 0) return 0;
        if (before[1965] == 1) {
            if (Get(after + 6, 2) != WT_JOURNAL_IDLE || Zero(after + 32, 16) || Get(after + 1952, 2) != 0) return 0;
        } else if (Get(after + 144, 8) != Get(before + 2032, 8)
            || Get(after + 96, 8) <= Get(before + 2032, 8)
            || memcmp(after + 128, before + 1968, 16) != 0) return 0;
    }
    if (Get(before + 6, 2) != WT_JOURNAL_EMPTY && (Get(after + 6, 2) == WT_JOURNAL_EMPTY
        || memcmp(before + 48, after + 48, 48) != 0
        || (!Zero(before + 32, 16) && memcmp(before + 32, after + 32, 16) != 0))) return 0;
    return 1;
}

static int Open(WtJournal *store, WtJournalIO io, int emptyOnly)
{
    unsigned char other[4096], digest[32];
    WtJournalEntry entry;
    uint64_t firstGeneration = 0, secondGeneration = 0;
    int firstValid, secondValid;
    int result = WT_JOURNAL_ERROR_UNKNOWN;
    if (!store) return -1;
    WtJournal_Close(store);
    if (!io.read || !io.write || !io.hash || io.hash((const unsigned char *)"abc", 3, digest) != 0
        || memcmp(digest, kHashABC, 32) != 0) return WT_JOURNAL_ERROR_SELF_TEST;
    if (io.read(io.arg, 0, store->record) != 0 || io.read(io.arg, 1, other) != 0) {
        result = WT_JOURNAL_ERROR_READ;
        goto failed;
    }
    if ((!Blank(store->record) && (!LayoutKnown(store->record) || (emptyOnly && Get(store->record + 6, 2) != 0)))
        || (!Blank(other) && (!LayoutKnown(other) || (emptyOnly && Get(other + 6, 2) != 0)))) goto failed;
    firstValid = WtJournal_Decode(store->record, io.hash, &entry) == 0;
    if (firstValid) firstGeneration = entry.generation;
    secondValid = WtJournal_Decode(other, io.hash, &entry) == 0;
    if (secondValid) secondGeneration = entry.generation;
    if ((!firstValid && Sealed(store->record, io.hash)) || (!secondValid && Sealed(other, io.hash))) goto failed;
    if (!firstValid && !secondValid) goto failed;
    if (firstValid && secondValid && firstGeneration == secondGeneration && memcmp(store->record, other, 4096) != 0) {
        result = WT_JOURNAL_ERROR_CONFLICT;
        goto failed;
    }
    if (firstValid && secondValid && (firstGeneration < secondGeneration
        ? !Follows(store->record, other) : !Follows(other, store->record))) goto failed;
    if ((!firstValid && !TornIdentityKnown(store->record, other))
        || (!secondValid && !TornIdentityKnown(other, store->record))) goto failed;
    if (io.hash(store->record, 4096, store->observed[0]) != 0 || io.hash(other, 4096, store->observed[1]) != 0) goto failed;
    if (!firstValid || (secondValid && secondGeneration > firstGeneration)) {
        memcpy(store->record, other, 4096); store->selected = 1;
    }
    store->io = io; store->usable = 1;
    Clear(&entry, sizeof(entry)); Clear(other, sizeof(other));
    return 0;
failed:
    WtJournal_Close(store);
    Clear(&entry, sizeof(entry)); Clear(other, sizeof(other));
    return result;
}

int WtJournal_Open(WtJournal *store, WtJournalIO io) { return Open(store, io, 0); }
int WtJournal_OpenEmpty(WtJournal *store, WtJournalIO io) { return Open(store, io, 1); }

int WtJournal_Initialize(WtJournal *store, WtJournalIO io, const unsigned char *seed)
{
    unsigned char other[4096], digest[32];
    WtJournalEntry entry;
    int bank, result = WT_JOURNAL_ERROR_UNKNOWN;
    if (!store) return -1;
    WtJournal_Close(store);
    memset(&entry, 0, sizeof(entry));
    if (!io.read || !io.write || !io.hash || io.hash((const unsigned char *)"abc", 3, digest) != 0
        || memcmp(digest, kHashABC, 32) != 0) { result = WT_JOURNAL_ERROR_SELF_TEST; goto done; }
    if (io.read(io.arg, 0, store->record) != 0 || io.read(io.arg, 1, other) != 0) {
        result = WT_JOURNAL_ERROR_READ; goto done;
    }
    if (!Blank(store->record) || !Blank(other)) goto done;
    if (!seed) { result = WT_JOURNAL_NEEDS_SEED; goto done; }
    entry.generation = 1;
    memcpy(entry.seed, seed, 32);
    if (WtJournal_Encode(&entry, io.hash, store->record) != 0) goto done;
    for (bank = 0; bank < 2; bank++) {
        /* Refuse a bank that changed before its first write. The backend owns
         * tag-last persistence; verify the complete committed image as well. */
        if (io.read(io.arg, bank, other) != 0) { result = WT_JOURNAL_ERROR_READ; goto done; }
        if (!Blank(other) || io.write(io.arg, bank, store->record) != 0) goto done;
        if (io.read(io.arg, bank, other) != 0) { result = WT_JOURNAL_ERROR_READ; goto done; }
        if (memcmp(other, store->record, 4096) != 0) goto done;
    }
    result = 0;
done:
    WtJournal_Close(store);
    Clear(&entry, sizeof(entry)); Clear(other, sizeof(other)); Clear(digest, sizeof(digest));
    return result == 0 ? WtJournal_OpenEmpty(store, io) : result;
}

int WtJournal_Read(const WtJournal *store, WtJournalEntry *entry)
{
    if (!entry) return -1;
    if (!store || !store->usable) { memset(entry, 0, sizeof(*entry)); return -1; }
    return WtJournal_Decode(store->record, store->io.hash, entry);
}

static int Current(const WtJournal *store)
{
    unsigned char record[4096], digest[32];
    int result = -1;
    int bank;
    if (store->io.hash(store->record, 4096, digest) != 0
        || memcmp(digest, store->observed[store->selected], 32) != 0) goto done;
    for (bank = 0; bank < 2; bank++) {
        if (store->io.read(store->io.arg, bank, record) != 0
            || store->io.hash(record, 4096, digest) != 0 || memcmp(digest, store->observed[bank], 32) != 0) goto done;
    }
    result = 0;
done:
    Clear(record, sizeof(record));
    return result;
}

static int WriteBank(WtJournal *store, int bank, const unsigned char *record)
{
    unsigned char readback[4096];
    int result = -1;
    if (store->io.write(store->io.arg, bank, record) == 0
        && store->io.read(store->io.arg, bank, readback) == 0 && memcmp(readback, record, 4096) == 0) result = 0;
    Clear(readback, sizeof(readback));
    return result;
}

int WtJournal_Repair(WtJournal *store)
{
    if (!store || !store->usable) return -1;
    store->usable = 0;
    if (Current(store) != 0 || WriteBank(store, store->selected ^ 1, store->record) != 0
        || WriteBank(store, store->selected, store->record) != 0) return -1;
    memcpy(store->observed[store->selected ^ 1], store->observed[store->selected], 32);
    store->usable = 1;
    return 0;
}

int WtJournal_Commit(WtJournal *store, const WtJournalEntry *entry)
{
    unsigned char next[4096], fingerprint[32];
    uint64_t generation;
    int result = -1;
    if (!store || !store->usable || !entry) return -1;
    generation = Get(store->record + 16, 8);
    if (generation == UINT64_MAX || entry->generation != generation + 1) goto done;
    if (WtJournal_Encode(entry, store->io.hash, next) != 0 || !Follows(store->record, next)
        || store->io.hash(next, 4096, fingerprint) != 0) goto done;
    store->usable = 0;
    if (Current(store) != 0 || WriteBank(store, store->selected ^ 1, next) != 0
        || WriteBank(store, store->selected, next) != 0) goto done;
    memcpy(store->record, next, 4096); store->usable = 1;
    memcpy(store->observed[0], fingerprint, 32); memcpy(store->observed[1], fingerprint, 32);
    result = 0;
done:
    Clear(next, sizeof(next));
    return result;
}
