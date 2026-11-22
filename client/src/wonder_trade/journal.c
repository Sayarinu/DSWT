#include "wonder_trade/journal.h"
#include "wonder_trade/game_family.h"
#include "wonder_trade/wire.h"
#include "wonder_trade/wire_layout.h"

#include <string.h>

/* Byte offsets of the 4 KiB journal record (protocol/save-layout.json,
 * record_sections). Sections: header 0, identity 32, progress 96,
 * transaction 160, pokemon_images 416, transport_private_state 1440,
 * pending_request 1952, reserved_zero 2216, sha256 4060, commit_tag 4092.
 * RESERVED_* ranges are zero in every supported record. */
enum {
    WT_REC_FORMAT = 0,                    /* "DSWJ" and format version 1 */
    WT_REC_PHASE = 6,
    WT_REC_SIZE = 8,
    WT_REC_RESERVED_SIZE_TAIL = 12,
    WT_REC_GENERATION = 16,
    WT_REC_RESERVED_HEADER_TAIL = 24,
    WT_REC_SAVE_ID = 32,                  /* identity */
    WT_REC_CREDENTIAL = 48,
    WT_REC_ENROLLMENT_ID = 80,
    WT_REC_NEXT_COUNTER = 96,             /* progress */
    WT_REC_EVENT_CURSOR = 104,
    WT_REC_LAST_RECEIPT = 112,
    WT_REC_LAST_OPERATION = 128,
    WT_REC_LAST_OPERATION_COUNTER = 144,
    WT_REC_RESERVED_PROGRESS_TAIL = 152,
    WT_REC_OPERATION = 160,               /* transaction */
    WT_REC_OPERATION_COUNTER = 176,
    WT_REC_TRADE = 184,
    WT_REC_RECEIPT = 200,
    WT_REC_SOURCE = 216,
    WT_REC_DESTINATION = 220,
    WT_REC_DELIVERY_KIND = 224,
    WT_REC_GLOBAL_COUNTER = 225,
    WT_REC_GENERAL_COUNTER = 229,
    WT_REC_BOX_COUNTER = 233,
    WT_REC_INVENTORY_BEFORE = 237,
    WT_REC_INVENTORY_AFTER = 269,
    WT_REC_ORIGINAL_DIGEST = 301,
    WT_REC_INCOMING_DIGEST = 333,
    WT_REC_APPLIED_DIGEST = 365,
    WT_REC_RESERVED_TRANSACTION_TAIL = 397,
    WT_REC_ORIGINAL = 416,                /* pokemon_images */
    WT_REC_DESTINATION_BEFORE = 652,
    WT_REC_DESTINATION_AFTER = 888,
    WT_REC_OUTGOING = 1124,
    WT_REC_INCOMING = 1260,
    WT_REC_RESERVED_IMAGES_TAIL = 1396,
    WT_REC_ENTROPY_FORMAT = 1440,         /* transport_private_state */
    WT_REC_SEED = 1444,
    WT_REC_RESERVED_ENTROPY_TAIL = 1476,
    WT_REC_PENDING_REQUEST = 1952,        /* pending_request: length, then the frame */
    WT_REC_RESERVED_REQUEST_HEADER = 1954,
    WT_REC_REQUEST = 1960,
    WT_REC_RESERVED = 2216,               /* reserved_zero */
    WT_REC_DIGEST = 4060,                 /* sha256 of the bytes before it */
    WT_REC_COMMIT = 4092                  /* "CMIT" */
};

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
    if (location.kind == 1) return location.box == 0 && location.slot < 6 && location.length == WT_GAME_PARTY_SIZE;
    return location.kind == 2 && location.box < WT_GAME_BOX_COUNT && location.slot < WT_GAME_BOX_SLOTS
        && location.length == WT_POKEMON_SIZE;
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
        && Get(record + WT_REC_PHASE, 2) <= WT_JOURNAL_RECOVERY_REQUIRED
        && (Get(record + WT_REC_PHASE, 2) != WT_JOURNAL_EMPTY || (Zero(record + WT_REC_SAVE_ID, WT_REC_ENTROPY_FORMAT - WT_REC_SAVE_ID) && Zero(record + WT_REC_PENDING_REQUEST, WT_REC_RESERVED - WT_REC_PENDING_REQUEST)))
        && Get(record + WT_REC_SIZE, 4) == WT_JOURNAL_RECORD_SIZE && Zero(record + WT_REC_RESERVED_SIZE_TAIL, WT_REC_GENERATION - WT_REC_RESERVED_SIZE_TAIL) && Zero(record + WT_REC_RESERVED_HEADER_TAIL, WT_REC_SAVE_ID - WT_REC_RESERVED_HEADER_TAIL)
        && Zero(record + WT_REC_RESERVED_PROGRESS_TAIL, WT_REC_OPERATION - WT_REC_RESERVED_PROGRESS_TAIL) && Zero(record + WT_REC_RESERVED_TRANSACTION_TAIL, WT_REC_ORIGINAL - WT_REC_RESERVED_TRANSACTION_TAIL) && Zero(record + WT_REC_RESERVED_IMAGES_TAIL, WT_REC_ENTROPY_FORMAT - WT_REC_RESERVED_IMAGES_TAIL)
        && memcmp(record + WT_REC_ENTROPY_FORMAT, "\1\0\40\0", 4) == 0 && Zero(record + WT_REC_RESERVED_ENTROPY_TAIL, WT_REC_PENDING_REQUEST - WT_REC_RESERVED_ENTROPY_TAIL)
        && Zero(record + WT_REC_RESERVED_REQUEST_HEADER, WT_REC_REQUEST - WT_REC_RESERVED_REQUEST_HEADER) && Zero(record + WT_REC_RESERVED, WT_REC_DIGEST - WT_REC_RESERVED);
}

static int RequestValid(const WtJournalEntry *entry, WtJournalHash hash)
{
    const unsigned char *frame = entry->request;
    const unsigned char *body = frame + WT_WIRE_REQUEST_HEADER_SIZE;
    uint16_t length = entry->requestLength;
    unsigned char digest[WT_WIRE_DIGEST_SIZE];
    unsigned opcode;
    if (length == 0) return Zero(frame, WT_JOURNAL_FRAME_MAX);
    if (length < WT_WIRE_REQUEST_HEADER_SIZE || length > WT_JOURNAL_FRAME_MAX
        || !Zero(frame + length, WT_JOURNAL_FRAME_MAX - length)
        || !WT_WIRE_KNOWN_VERSION(frame)
        || Get(frame + WT_WIRE_REQUEST_BODY_LENGTH, 2) != (unsigned)length - WT_WIRE_REQUEST_HEADER_SIZE
        || memcmp(frame + WT_WIRE_REQUEST_CREDENTIAL, entry->credential, WT_WIRE_CREDENTIAL_SIZE) != 0) return 0;
    opcode = frame[WT_WIRE_REQUEST_OPERATION];
    if (opcode == WT_OP_ENROLL) {
        return entry->phase == WT_JOURNAL_ENROLLING
            && length >= WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS
            && length <= WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS + WT_WIRE_ENROLL_HINTS_MAX
            && Get(body + WT_WIRE_ENROLL_HINT_LENGTH, 1) == (unsigned)length - (WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS)
            && Zero(frame + WT_WIRE_REQUEST_SAVE_ID, WT_WIRE_ID_SIZE)
            && Get(frame + WT_WIRE_REQUEST_COUNTER, 8) == 0
            && memcmp(frame + WT_WIRE_REQUEST_ID, entry->enrollmentID, WT_WIRE_ID_SIZE) == 0;
    }
    if (memcmp(frame + WT_WIRE_REQUEST_SAVE_ID, entry->saveID, WT_WIRE_ID_SIZE) != 0
        || Zero(frame + WT_WIRE_REQUEST_ID, WT_WIRE_ID_SIZE)) return 0;
    /* Only mutations require crash-stable packet storage. Reads can be rebuilt. */
    if (opcode != WT_OP_SUBMIT && opcode != WT_OP_CANCEL && opcode != WT_OP_ACKNOWLEDGE) return 0;
    if (memcmp(frame + WT_WIRE_REQUEST_ID, entry->operation, WT_WIRE_ID_SIZE) != 0
        || entry->operationCounter == 0 || entry->operationCounter == UINT64_MAX
        || Get(frame + WT_WIRE_REQUEST_COUNTER, 8) != entry->operationCounter) return 0;
    if (opcode == WT_OP_SUBMIT) {
        return length == WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_SUBMIT_SIZE
            && Get(body + WT_WIRE_SUBMIT_GENERATION, 8) != 0
            && Get(body + WT_WIRE_SUBMIT_GENERATION, 8) <= entry->generation
            && memcmp(body + WT_WIRE_SUBMIT_POKEMON, entry->outgoing, WT_WIRE_PK4_SIZE) == 0
            && hash(entry->outgoing, WT_WIRE_PK4_SIZE, digest) == 0
            && memcmp(body + WT_WIRE_SUBMIT_DIGEST, digest, WT_WIRE_DIGEST_SIZE) == 0;
    }
    if (opcode == WT_OP_CANCEL) return length == WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_TRADE_REQUEST_SIZE
        && !Zero(entry->trade, WT_WIRE_ID_SIZE) && memcmp(body, entry->trade, WT_WIRE_ID_SIZE) == 0;
    return length == WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ACK_SIZE
        && !Zero(entry->trade, WT_WIRE_ID_SIZE) && !Zero(entry->receipt, WT_WIRE_ID_SIZE)
        && memcmp(body + WT_WIRE_ACK_TRADE, entry->trade, WT_WIRE_ID_SIZE) == 0
        && memcmp(body + WT_WIRE_ACK_RECEIPT, entry->receipt, WT_WIRE_ID_SIZE) == 0
        && memcmp(body + WT_WIRE_ACK_DELIVERED_DIGEST, entry->incomingDigest, WT_WIRE_DIGEST_SIZE) == 0
        && memcmp(body + WT_WIRE_ACK_APPLIED_DIGEST, entry->appliedDigest, WT_WIRE_DIGEST_SIZE) == 0
        && Get(body + WT_WIRE_ACK_GENERATION, 8) != 0
        && Get(body + WT_WIRE_ACK_GENERATION, 8) <= entry->generation;
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
    /* A located image shorter than its 236-byte field leaves the rest zero. */
    if ((entry->source.kind != 0
            && !Zero(entry->original + entry->source.length, sizeof(entry->original) - entry->source.length))
        || (entry->destination.kind != 0
            && (!Zero(entry->destinationBefore + entry->destination.length,
                    sizeof(entry->destinationBefore) - entry->destination.length)
                || !Zero(entry->destinationAfter + entry->destination.length,
                    sizeof(entry->destinationAfter) - entry->destination.length)))) return 0;
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
    memset(record, 0, WT_JOURNAL_RECORD_SIZE);
    memcpy(record, "DSWJ\1\0", 6); Put(record + WT_REC_PHASE, entry->phase, 2);
    Put(record + WT_REC_SIZE, WT_JOURNAL_RECORD_SIZE, 4); Put(record + WT_REC_GENERATION, entry->generation, 8);
    memcpy(record + WT_REC_SAVE_ID, entry->saveID, 16); memcpy(record + WT_REC_CREDENTIAL, entry->credential, 32);
    memcpy(record + WT_REC_ENROLLMENT_ID, entry->enrollmentID, 16);
    Put(record + WT_REC_NEXT_COUNTER, entry->nextCounter, 8); Put(record + WT_REC_EVENT_CURSOR, entry->eventCursor, 8);
    memcpy(record + WT_REC_LAST_RECEIPT, entry->lastReceipt, 16); memcpy(record + WT_REC_LAST_OPERATION, entry->lastOperation, 16);
    Put(record + WT_REC_LAST_OPERATION_COUNTER, entry->lastOperationCounter, 8);
    memcpy(record + WT_REC_OPERATION, entry->operation, 16); Put(record + WT_REC_OPERATION_COUNTER, entry->operationCounter, 8);
    memcpy(record + WT_REC_TRADE, entry->trade, 16); memcpy(record + WT_REC_RECEIPT, entry->receipt, 16);
    PutLocator(record + WT_REC_SOURCE, entry->source); PutLocator(record + WT_REC_DESTINATION, entry->destination);
    record[WT_REC_DELIVERY_KIND] = entry->deliveryKind;
    Put(record + WT_REC_GLOBAL_COUNTER, entry->globalCounter, 4); Put(record + WT_REC_GENERAL_COUNTER, entry->generalCounter, 4);
    Put(record + WT_REC_BOX_COUNTER, entry->boxCounter, 4);
    memcpy(record + WT_REC_INVENTORY_BEFORE, entry->inventoryBefore, 32); memcpy(record + WT_REC_INVENTORY_AFTER, entry->inventoryAfter, 32);
    memcpy(record + WT_REC_ORIGINAL_DIGEST, entry->originalDigest, 32); memcpy(record + WT_REC_INCOMING_DIGEST, entry->incomingDigest, 32);
    memcpy(record + WT_REC_APPLIED_DIGEST, entry->appliedDigest, 32);
    memcpy(record + WT_REC_ORIGINAL, entry->original, 236); memcpy(record + WT_REC_DESTINATION_BEFORE, entry->destinationBefore, 236);
    memcpy(record + WT_REC_DESTINATION_AFTER, entry->destinationAfter, 236); memcpy(record + WT_REC_OUTGOING, entry->outgoing, 136);
    memcpy(record + WT_REC_INCOMING, entry->incoming, 136);
    memcpy(record + WT_REC_ENTROPY_FORMAT, "\1\0\40\0", 4); memcpy(record + WT_REC_SEED, entry->seed, 32);
    Put(record + WT_REC_PENDING_REQUEST, entry->requestLength, 2); memcpy(record + WT_REC_REQUEST, entry->request, 256);
    if (entry->phase == WT_JOURNAL_EMPTY && (!Zero(record + WT_REC_SAVE_ID, WT_REC_ENTROPY_FORMAT - WT_REC_SAVE_ID) || !Zero(record + WT_REC_PENDING_REQUEST, WT_REC_RESERVED - WT_REC_PENDING_REQUEST))) return -1;
    if (entry->phase <= WT_JOURNAL_IDLE && !Zero(record + WT_REC_OPERATION, WT_REC_ENTROPY_FORMAT - WT_REC_OPERATION)) return -1;
    if (hash(record, WT_REC_DIGEST, record + WT_REC_DIGEST) != 0) return -1;
    memcpy(record + WT_REC_COMMIT, "CMIT", 4);
    return 0;
}

int WtJournal_Decode(const unsigned char *record, WtJournalHash hash, WtJournalEntry *entry)
{
    unsigned char digest[32];
    if (!record || !entry || !hash) return -1;
    memset(entry, 0, sizeof(*entry));
    if (!LayoutKnown(record) || memcmp(record + WT_REC_COMMIT, "CMIT", 4) != 0
        || hash(record, WT_REC_DIGEST, digest) != 0 || memcmp(digest, record + WT_REC_DIGEST, 32) != 0) return -1;
    entry->phase = (uint16_t)Get(record + WT_REC_PHASE, 2); entry->generation = Get(record + WT_REC_GENERATION, 8);
    memcpy(entry->saveID, record + WT_REC_SAVE_ID, 16); memcpy(entry->credential, record + WT_REC_CREDENTIAL, 32);
    memcpy(entry->enrollmentID, record + WT_REC_ENROLLMENT_ID, 16);
    entry->nextCounter = Get(record + WT_REC_NEXT_COUNTER, 8); entry->eventCursor = Get(record + WT_REC_EVENT_CURSOR, 8);
    memcpy(entry->lastReceipt, record + WT_REC_LAST_RECEIPT, 16); memcpy(entry->lastOperation, record + WT_REC_LAST_OPERATION, 16);
    entry->lastOperationCounter = Get(record + WT_REC_LAST_OPERATION_COUNTER, 8);
    memcpy(entry->operation, record + WT_REC_OPERATION, 16); entry->operationCounter = Get(record + WT_REC_OPERATION_COUNTER, 8);
    memcpy(entry->trade, record + WT_REC_TRADE, 16); memcpy(entry->receipt, record + WT_REC_RECEIPT, 16);
    entry->source = GetLocator(record + WT_REC_SOURCE); entry->destination = GetLocator(record + WT_REC_DESTINATION);
    entry->deliveryKind = record[WT_REC_DELIVERY_KIND];
    entry->globalCounter = (uint32_t)Get(record + WT_REC_GLOBAL_COUNTER, 4); entry->generalCounter = (uint32_t)Get(record + WT_REC_GENERAL_COUNTER, 4);
    entry->boxCounter = (uint32_t)Get(record + WT_REC_BOX_COUNTER, 4);
    memcpy(entry->inventoryBefore, record + WT_REC_INVENTORY_BEFORE, 32); memcpy(entry->inventoryAfter, record + WT_REC_INVENTORY_AFTER, 32);
    memcpy(entry->originalDigest, record + WT_REC_ORIGINAL_DIGEST, 32); memcpy(entry->incomingDigest, record + WT_REC_INCOMING_DIGEST, 32);
    memcpy(entry->appliedDigest, record + WT_REC_APPLIED_DIGEST, 32);
    memcpy(entry->original, record + WT_REC_ORIGINAL, 236); memcpy(entry->destinationBefore, record + WT_REC_DESTINATION_BEFORE, 236);
    memcpy(entry->destinationAfter, record + WT_REC_DESTINATION_AFTER, 236); memcpy(entry->outgoing, record + WT_REC_OUTGOING, 136);
    memcpy(entry->incoming, record + WT_REC_INCOMING, 136); memcpy(entry->seed, record + WT_REC_SEED, 32);
    entry->requestLength = (uint16_t)Get(record + WT_REC_PENDING_REQUEST, 2); memcpy(entry->request, record + WT_REC_REQUEST, 256);
    if (!EntryValid(entry, hash) || (entry->phase == WT_JOURNAL_EMPTY
        && (!Zero(record + WT_REC_SAVE_ID, WT_REC_ENTROPY_FORMAT - WT_REC_SAVE_ID) || !Zero(record + WT_REC_PENDING_REQUEST, WT_REC_RESERVED - WT_REC_PENDING_REQUEST)))
        || (entry->phase <= WT_JOURNAL_IDLE && !Zero(record + WT_REC_OPERATION, WT_REC_ENTROPY_FORMAT - WT_REC_OPERATION))) {
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    return 0;
}

static int Blank(const unsigned char *record)
{
    size_t i;
    if (record[0] != 0 && record[0] != 255) return 0;
    for (i = 1; i < WT_JOURNAL_RECORD_SIZE; i++) if (record[i] != record[0]) return 0;
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
    return memcmp(record + WT_REC_COMMIT, "CMIT", 4) == 0 && hash(record, WT_REC_DIGEST, digest) == 0
        && memcmp(record + WT_REC_DIGEST, digest, 32) == 0;
}

/* What a cut first write can leave: every byte that a phase-EMPTY record
 * fixes holds its value or a blank one (0 or 255), and no seal. The seed, the
 * generation, the digest and the tag may hold anything. Such a bank names no
 * enrollment and no Pokémon: nothing in it was ever sent or escrowed. */
static int Unwritten(const unsigned char *record, WtJournalHash hash)
{
    static const unsigned char header[WT_REC_GENERATION] = "DSWJ\1\0\0\0\0\20\0\0\0\0\0";
    static const unsigned char entropy[4] = { 1, 0, 32, 0 };
    size_t i;
    for (i = 0; i < WT_REC_DIGEST; i++) {
        unsigned char expected = 0;
        if (i < WT_REC_GENERATION) expected = header[i];
        else if (i < WT_REC_RESERVED_HEADER_TAIL) continue;
        else if (i >= WT_REC_ENTROPY_FORMAT && i < WT_REC_SEED) expected = entropy[i - WT_REC_ENTROPY_FORMAT];
        else if (i >= WT_REC_SEED && i < WT_REC_RESERVED_ENTROPY_TAIL) continue;
        if (record[i] != expected && record[i] != 0 && record[i] != 255) return 0;
    }
    return !Sealed(record, hash);
}

int WtJournal_Unused(const unsigned char *first, const unsigned char *second, WtJournalHash hash)
{
    return first && second && hash && Unwritten(first, hash) && Unwritten(second, hash);
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
    unsigned length = (unsigned)Get(record + WT_REC_PENDING_REQUEST, 2), candidate;
    unsigned candidates[6];
    if ((length != 0 && (length < 85 || length > 149)) || record[WT_REC_PENDING_REQUEST + 92] > 64) return 0;
    candidates[0] = length ? length : 85u + record[WT_REC_PENDING_REQUEST + 92]; candidates[1] = length ? length : 149u;
    candidates[2] = candidates[4] = candidates[0]; candidates[3] = candidates[5] = candidates[1];
    for (candidate = 0; candidate < 6; candidate++) {
        unsigned size = candidates[candidate];
        memset(expected, 0, sizeof(expected)); Put(expected, size, 2);
        /* Earlier clients enrolled with versions 1 and 2; this one uses version 3. */
        memcpy(expected + 8, "WTDS\1\1", 6); Put(expected + 14, size - 80, 2);
        expected[12] = (unsigned char)(1 + candidate / 2);
        memcpy(expected + 16, record + WT_REC_ENROLLMENT_ID, 16); memcpy(expected + 48, record + WT_REC_CREDENTIAL, 32);
        memcpy(expected + 88, record + WT_REC_PENDING_REQUEST + 88, 4); expected[92] = (unsigned char)(size - 85);
        memcpy(expected + 93, record + WT_REC_PENDING_REQUEST + 93, size - 85);
        if (PrefixAndZeros(record + WT_REC_PENDING_REQUEST, expected, sizeof(expected))
            || (allowClear && ZerosAndSuffix(record + WT_REC_PENDING_REQUEST, expected, sizeof(expected)))) return 1;
    }
    return 0;
}

static int InitialEnrollment(const unsigned char *record)
{
    return Get(record + WT_REC_PHASE, 2) == WT_JOURNAL_ENROLLING && Zero(record + WT_REC_SAVE_ID, 16)
        && Get(record + WT_REC_NEXT_COUNTER, 8) <= 1 && Zero(record + WT_REC_EVENT_CURSOR, WT_REC_OPERATION - WT_REC_EVENT_CURSOR) && Zero(record + WT_REC_OPERATION, WT_REC_ENTROPY_FORMAT - WT_REC_OPERATION)
        && InitialPacketKnown(record, 0);
}

static int TornIdentityKnown(const unsigned char *partial, const unsigned char *complete)
{
    uint16_t partialPhase = (uint16_t)Get(partial + WT_REC_PHASE, 2), completePhase = (uint16_t)Get(complete + WT_REC_PHASE, 2);
    if (Blank(partial)) return 1;
    if (partialPhase == WT_JOURNAL_EMPTY) return completePhase == WT_JOURNAL_EMPTY || InitialEnrollment(complete);
    if (completePhase == WT_JOURNAL_EMPTY) return InitialEnrollment(partial);
    /* Identity bytes never change after enrollment. A cut during the first
     * provisioning mirror can contain only a known prefix followed by zeros. */
    if (memcmp(partial + WT_REC_CREDENTIAL, complete + WT_REC_CREDENTIAL, WT_REC_NEXT_COUNTER - WT_REC_CREDENTIAL) != 0
        && !(partialPhase == WT_JOURNAL_ENROLLING && completePhase == WT_JOURNAL_ENROLLING
            && PrefixAndZeros(partial + WT_REC_CREDENTIAL, complete + WT_REC_CREDENTIAL, WT_REC_NEXT_COUNTER - WT_REC_CREDENTIAL))) return 0;
    if (partialPhase <= WT_JOURNAL_IDLE && Get(complete + WT_REC_NEXT_COUNTER, 8) == 1 && Zero(complete + WT_REC_EVENT_CURSOR, WT_REC_OPERATION - WT_REC_EVENT_CURSOR)
        && (completePhase == WT_JOURNAL_ENROLLING || completePhase == WT_JOURNAL_IDLE)
        && (Get(partial + WT_REC_NEXT_COUNTER, 8) > 1 || !Zero(partial + WT_REC_EVENT_CURSOR, WT_REC_OPERATION - WT_REC_EVENT_CURSOR) || !Zero(partial + WT_REC_OPERATION, WT_REC_ENTROPY_FORMAT - WT_REC_OPERATION)
            || !InitialPacketKnown(partial, 1))) return 0;
    if (Zero(complete + WT_REC_SAVE_ID, 16)) return partialPhase <= WT_JOURNAL_IDLE
        && Get(partial + WT_REC_NEXT_COUNTER, 8) <= 1 && Zero(partial + WT_REC_EVENT_CURSOR, WT_REC_OPERATION - WT_REC_EVENT_CURSOR) && Zero(partial + WT_REC_OPERATION, WT_REC_ENTROPY_FORMAT - WT_REC_OPERATION)
        && (PrefixAndZeros(partial + WT_REC_PENDING_REQUEST, complete + WT_REC_PENDING_REQUEST, WT_REC_RESERVED - WT_REC_PENDING_REQUEST)
            || ZerosAndSuffix(partial + WT_REC_PENDING_REQUEST, complete + WT_REC_PENDING_REQUEST, WT_REC_RESERVED - WT_REC_PENDING_REQUEST));
    if (memcmp(partial + WT_REC_SAVE_ID, complete + WT_REC_SAVE_ID, 16) == 0) return 1;
    return partialPhase <= WT_JOURNAL_IDLE && completePhase == WT_JOURNAL_IDLE
        && Get(complete + WT_REC_NEXT_COUNTER, 8) == 1 && Get(complete + WT_REC_EVENT_CURSOR, 8) == 0
        && PrefixAndZeros(partial + WT_REC_SAVE_ID, complete + WT_REC_SAVE_ID, 16);
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
    unsigned char other[WT_JOURNAL_RECORD_SIZE], digest[32];
    WtJournalEntry entry;
    uint64_t firstGeneration = 0, secondGeneration = 0;
    int firstValid, secondValid, firstEmpty, secondEmpty, firstBlank, secondBlank;
    int result = WT_JOURNAL_ERROR_UNKNOWN;
    if (!store) return -1;
    WtJournal_Close(store);
    if (!io.read || !io.write || !io.hash || io.hash((const unsigned char *)"abc", 3, digest) != 0
        || memcmp(digest, kHashABC, 32) != 0) return WT_JOURNAL_ERROR_SELF_TEST;
    if (io.read(io.arg, 0, store->record) != 0 || io.read(io.arg, 1, other) != 0) {
        result = WT_JOURNAL_ERROR_READ;
        goto failed;
    }
    firstValid = WtJournal_Decode(store->record, io.hash, &entry) == 0;
    firstEmpty = firstValid && entry.phase == WT_JOURNAL_EMPTY;
    if (firstValid) firstGeneration = entry.generation;
    secondValid = WtJournal_Decode(other, io.hash, &entry) == 0;
    secondEmpty = secondValid && entry.phase == WT_JOURNAL_EMPTY;
    if (secondValid) secondGeneration = entry.generation;
    /* Before enrollment, a bank cut while it was written counts as blank:
     * the other holds no record or an EMPTY one, and neither names anything. */
    firstBlank = Blank(store->record)
        || (Unwritten(store->record, io.hash) && (secondEmpty || Unwritten(other, io.hash)));
    secondBlank = Blank(other) || (Unwritten(other, io.hash) && (firstEmpty || Unwritten(store->record, io.hash)));
    if ((!firstBlank && (!LayoutKnown(store->record) || (emptyOnly && Get(store->record + WT_REC_PHASE, 2) != 0)))
        || (!secondBlank && (!LayoutKnown(other) || (emptyOnly && Get(other + 6, 2) != 0)))) goto failed;
    if ((!firstValid && Sealed(store->record, io.hash)) || (!secondValid && Sealed(other, io.hash))) goto failed;
    if (!firstValid && !secondValid) goto failed;
    if (firstValid && secondValid && firstGeneration == secondGeneration && memcmp(store->record, other, WT_JOURNAL_RECORD_SIZE) != 0) {
        result = WT_JOURNAL_ERROR_CONFLICT;
        goto failed;
    }
    if (firstValid && secondValid && (firstGeneration < secondGeneration
        ? !Follows(store->record, other) : !Follows(other, store->record))) goto failed;
    if ((!firstValid && !firstBlank && !TornIdentityKnown(store->record, other))
        || (!secondValid && !secondBlank && !TornIdentityKnown(other, store->record))) goto failed;
    if (io.hash(store->record, WT_JOURNAL_RECORD_SIZE, store->observed[0]) != 0 || io.hash(other, WT_JOURNAL_RECORD_SIZE, store->observed[1]) != 0) goto failed;
    if (!firstValid || (secondValid && secondGeneration > firstGeneration)) {
        memcpy(store->record, other, WT_JOURNAL_RECORD_SIZE); store->selected = 1;
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
    unsigned char other[WT_JOURNAL_RECORD_SIZE], digest[32];
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
    if (!WtJournal_Unused(store->record, other, io.hash)) goto done;
    if (!seed) { result = WT_JOURNAL_NEEDS_SEED; goto done; }
    /* A first write cut short left only bytes it can redo: erase them, each
     * erasure verified. A cut erasure leaves the bank unwritten still. */
    memset(store->record, 0, WT_JOURNAL_RECORD_SIZE);
    for (bank = 0; bank < 2; bank++) {
        if (io.read(io.arg, bank, other) != 0) { result = WT_JOURNAL_ERROR_READ; goto done; }
        if (Blank(other)) continue;
        if (!Unwritten(other, io.hash) || io.write(io.arg, bank, store->record) != 0) goto done;
        if (io.read(io.arg, bank, other) != 0) { result = WT_JOURNAL_ERROR_READ; goto done; }
        if (!Blank(other)) goto done;
    }
    entry.generation = 1;
    memcpy(entry.seed, seed, 32);
    if (WtJournal_Encode(&entry, io.hash, store->record) != 0) goto done;
    for (bank = 0; bank < 2; bank++) {
        /* Refuse a bank that changed before its first write. The backend owns
         * tag-last persistence; verify the complete committed image as well. */
        if (io.read(io.arg, bank, other) != 0) { result = WT_JOURNAL_ERROR_READ; goto done; }
        if (!Blank(other) || io.write(io.arg, bank, store->record) != 0) goto done;
        if (io.read(io.arg, bank, other) != 0) { result = WT_JOURNAL_ERROR_READ; goto done; }
        if (memcmp(other, store->record, WT_JOURNAL_RECORD_SIZE) != 0) goto done;
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
    unsigned char record[WT_JOURNAL_RECORD_SIZE], digest[32];
    int result = -1;
    int bank;
    if (store->io.hash(store->record, WT_JOURNAL_RECORD_SIZE, digest) != 0
        || memcmp(digest, store->observed[store->selected], 32) != 0) goto done;
    for (bank = 0; bank < 2; bank++) {
        if (store->io.read(store->io.arg, bank, record) != 0
            || store->io.hash(record, WT_JOURNAL_RECORD_SIZE, digest) != 0 || memcmp(digest, store->observed[bank], 32) != 0) goto done;
    }
    result = 0;
done:
    Clear(record, sizeof(record));
    return result;
}

static int WriteBank(WtJournal *store, int bank, const unsigned char *record)
{
    unsigned char readback[WT_JOURNAL_RECORD_SIZE];
    int result = -1;
    if (store->io.write(store->io.arg, bank, record) == 0
        && store->io.read(store->io.arg, bank, readback) == 0 && memcmp(readback, record, WT_JOURNAL_RECORD_SIZE) == 0) result = 0;
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
    unsigned char next[WT_JOURNAL_RECORD_SIZE], fingerprint[32];
    uint64_t generation;
    int result = -1;
    if (!store || !store->usable || !entry) return -1;
    generation = Get(store->record + WT_REC_GENERATION, 8);
    if (generation == UINT64_MAX || entry->generation != generation + 1) goto done;
    if (WtJournal_Encode(entry, store->io.hash, next) != 0 || !Follows(store->record, next)
        || store->io.hash(next, WT_JOURNAL_RECORD_SIZE, fingerprint) != 0) goto done;
    store->usable = 0;
    if (Current(store) != 0 || WriteBank(store, store->selected ^ 1, next) != 0
        || WriteBank(store, store->selected, next) != 0) goto done;
    memcpy(store->record, next, WT_JOURNAL_RECORD_SIZE); store->usable = 1;
    memcpy(store->observed[0], fingerprint, 32); memcpy(store->observed[1], fingerprint, 32);
    result = 0;
done:
    Clear(next, sizeof(next));
    return result;
}
