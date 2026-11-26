#include "platform/journal_io.h"
#include "wonder_trade/wire.h"
#include "wonder_trade/wire_layout.h"

#include <string.h>

const CpJournalArea CpJournalArea_Gen4 = { { CP_JOURNAL_BANK_0, CP_JOURNAL_BANK_1 }, CP_JOURNAL_RESERVED_SIZE };
const CpJournalArea CpJournalArea_BlackWhite = { { 0x74000u, 0x75000u }, 0x1000u };
const CpJournalArea CpJournalArea_Black2White2 = { { 0x6B000u, 0x6C000u }, 0x1000u };

static const CpJournalArea *Area(const CpJournalStore *store)
{
    return store->area ? store->area : &CpJournalArea_Gen4;
}

/* Record offsets (record_sections in protocol/save-layout.json). */
enum {
    REC_PHASE = 6, REC_GENERATION = 16, REC_CREDENTIAL = 48, REC_ENROLLMENT_ID = 80,
    REC_NEXT_COUNTER = 96, REC_PENDING = 1952, REC_RESERVED = 2216, REC_DIGEST = 4060,
    REC_TAG = 4092
};
/* The pending request: a 2-byte length, 6 zero bytes, then the frame. */
enum {
    PKT_FRAME = 8, PKT_SIZE = REC_RESERVED - REC_PENDING,
    PKT_OPERATION = PKT_FRAME + WT_WIRE_REQUEST_OPERATION,
    PKT_BODY_LENGTH = PKT_FRAME + WT_WIRE_REQUEST_BODY_LENGTH,
    PKT_ID = PKT_FRAME + WT_WIRE_REQUEST_ID,
    PKT_CREDENTIAL = PKT_FRAME + WT_WIRE_REQUEST_CREDENTIAL,
    PKT_BODY = PKT_FRAME + WT_WIRE_REQUEST_HEADER_SIZE,
    PKT_HINT_LENGTH = PKT_BODY + WT_WIRE_ENROLL_HINT_LENGTH,
    PKT_HINTS = PKT_BODY + WT_WIRE_ENROLL_HINTS,
    PKT_MIN = WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS,
    PKT_MAX = PKT_MIN + WT_WIRE_ENROLL_HINTS_MAX
};

static int Within(size_t i, size_t start, size_t size) { return i >= start && i < start + size; }

/* Each byte of the pending request is zero or the byte of one initial Enroll
 * frame, as the engine builds it: version 1 to 3, a zero save ID and counter,
 * and the frame size its length, body length and hint length name. The
 * request ID, credential, build and hints are the engine's random or build
 * values; where they and the record's identity both hold a byte, it matches. */
static int EnrollPacketMix(const unsigned char *record)
{
    const unsigned char *packet = record + REC_PENDING;
    unsigned clues[3], size = 0, i;
    clues[0] = packet[0] | (unsigned)packet[1] << 8;
    clues[1] = packet[PKT_BODY_LENGTH] | (unsigned)packet[PKT_BODY_LENGTH + 1] << 8;
    clues[1] = clues[1] ? clues[1] + WT_WIRE_REQUEST_HEADER_SIZE : 0;
    clues[2] = packet[PKT_HINT_LENGTH] ? packet[PKT_HINT_LENGTH] + (unsigned)PKT_MIN : 0;
    for (i = 0; i < 3; i++) {
        if (!clues[i]) continue;
        if (size && clues[i] != size) return 0;
        size = clues[i];
    }
    if (!size) size = PKT_MAX;
    if (size < PKT_MIN || size > PKT_MAX) return 0;
    for (i = 0; i < PKT_SIZE; i++) {
        unsigned char byte = packet[i], expected = 0;
        if (!byte) continue;
        if (i < 2) expected = (unsigned char)(size >> (8 * i));
        else if (Within(i, PKT_FRAME, 4)) expected = (unsigned char)WT_WIRE_MAGIC[i - PKT_FRAME];
        else if (i == PKT_FRAME + 4) { if (byte <= WT_WIRE_VERSION_REFUSAL_REASON) continue; return 0; }
        else if (i == PKT_OPERATION) expected = WT_OP_ENROLL;
        else if (Within(i, PKT_BODY_LENGTH, 2)) expected = (unsigned char)((size - WT_WIRE_REQUEST_HEADER_SIZE) >> (8 * (i - PKT_BODY_LENGTH)));
        else if (Within(i, PKT_ID, WT_WIRE_ID_SIZE)) {
            unsigned char held = record[REC_ENROLLMENT_ID + i - PKT_ID];
            if (!held || held == byte) continue;
            return 0;
        } else if (Within(i, PKT_CREDENTIAL, WT_WIRE_CREDENTIAL_SIZE)) {
            unsigned char held = record[REC_CREDENTIAL + i - PKT_CREDENTIAL];
            if (!held || held == byte) continue;
            return 0;
        } else if (Within(i, PKT_BODY, WT_WIRE_ENROLL_HINT_LENGTH)) continue;
        else if (i == PKT_HINT_LENGTH) expected = (unsigned char)(size - PKT_MIN);
        else if (i >= PKT_HINTS && i < PKT_FRAME + size) continue;
        if (byte != expected) return 0;
    }
    return 1;
}

static int Sealed(const unsigned char *record, WtJournalHash hash)
{
    unsigned char digest[32];
    int sealed = memcmp(record + REC_TAG, "CMIT", 4) == 0 && hash(record, REC_DIGEST, digest) == 0
        && memcmp(record + REC_DIGEST, digest, 32) == 0;
    memset(digest, 0, sizeof(digest));
    return sealed;
}

/* Static, like sOther: a bank read nests inside WtJournal_Open, and journal
 * work runs to completion without yielding. */
static WtJournalEntry sEntry;

/* The engine's first enrollment commit writes the initial ENROLLING record
 * over two EMPTY banks, one after the other, and sends nothing until both
 * hold it. A cut write leaves each byte of the bank old or new; the digest
 * and tag may hold anything but a seal. Both records hold the same bytes
 * except the phase, the generation (g, g + 1), the credential, enrollment ID,
 * next counter (0, 1) and pending request (zero in EMPTY). Every other byte
 * must equal the valid bank's, so foreign data is still refused. */
int CpJournal_CutFirstEnrollment(const unsigned char torn[WT_JOURNAL_RECORD_SIZE],
                                 const unsigned char valid[WT_JOURNAL_RECORD_SIZE], WtJournalHash hash)
{
    uint64_t generation[2];
    unsigned char before[8], after[8];
    size_t i;
    int enrolled, result = 0;
    if (!torn || !valid || !hash || Sealed(torn, hash) || WtJournal_Decode(valid, hash, &sEntry) != 0) goto done;
    if (sEntry.phase == WT_JOURNAL_EMPTY) {
        if (sEntry.generation == UINT64_MAX) goto done;
        generation[0] = sEntry.generation;
        enrolled = 0;
    } else if (sEntry.phase == WT_JOURNAL_ENROLLING) {
        /* EntryValid holds it to the initial one: no save ID, counter 1. */
        if (sEntry.generation == 0) goto done;
        generation[0] = sEntry.generation - 1;
        enrolled = 1;
    } else goto done;
    generation[1] = generation[0] + 1;
    for (i = 0; i < 8; i++) {
        before[i] = (unsigned char)(generation[0] >> (8 * i));
        after[i] = (unsigned char)(generation[1] >> (8 * i));
    }
    for (i = 0; i < REC_DIGEST; i++) {
        unsigned char byte = torn[i];
        if (i == REC_PHASE || i == REC_NEXT_COUNTER) { if (byte <= 1) continue; goto done; }
        if (Within(i, REC_GENERATION, 8)) {
            if (byte == before[i - REC_GENERATION] || byte == after[i - REC_GENERATION]) continue;
            goto done;
        }
        if (Within(i, REC_CREDENTIAL, REC_NEXT_COUNTER - REC_CREDENTIAL) || Within(i, REC_PENDING, PKT_SIZE)) {
            /* The ENROLLING record is known when valid is it; else EnrollPacketMix. */
            if (!byte || !enrolled || byte == valid[i]) continue;
            goto done;
        }
        if (byte != valid[i]) goto done;
    }
    result = enrolled || EnrollPacketMix(torn);
done:
    memset(&sEntry, 0, sizeof(sEntry));
    return result;
}

static int Fail(CpJournalStore *store, int error)
{
    if (!store->error) store->error = error;
    return -1;
}

static int SizeMatches(const CpPlatformFile *file)
{
    uint32_t size = 0;
    return file->size(file->arg, &size) == 0 && size == CP_SAVE_SIZE;
}

static unsigned char sOther[WT_JOURNAL_RECORD_SIZE];

/* Show a cut first enrollment write as a blank bank: the journal then opens
 * on the other bank, which is the record before the write or the one it
 * wrote, and the next commit overwrites this bank first. The other bank
 * must be valid, so this never opens a journal the core would refuse for
 * any other reason. */
static int Settle(const CpJournalStore *store, int bank, unsigned char *record)
{
    const CpPlatformFile *file = store->file;
    if (file->readAt(file->arg, Area(store)->bank[bank ^ 1], sOther, sizeof(sOther)) != 0) return -1;
    if (CpJournal_CutFirstEnrollment(record, sOther, store->hash)) memset(record, 0, WT_JOURNAL_RECORD_SIZE);
    memset(sOther, 0, sizeof(sOther));
    return 0;
}

int CpJournal_ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    CpJournalStore *store = arg;
    const CpPlatformFile *file;
    unsigned char page[512];
    uint32_t offset;
    int blank = -1;
    size_t i;
    if (!store || !store->file || !record || bank < 0 || bank > 1) return -1;
    file = store->file;
    if (!SizeMatches(file)) return Fail(store, CP_JOURNAL_ERROR_SIZE);
    if (file->readAt(file->arg, Area(store)->bank[bank], record, WT_JOURNAL_RECORD_SIZE) != 0)
        return Fail(store, CP_JOURNAL_ERROR_READ);
    /* Like the game's flash access: the rest of the reserved area must be
     * one blank value, or this is not a save the journal may use. */
    for (offset = WT_JOURNAL_RECORD_SIZE; offset < Area(store)->reserved; offset += sizeof(page)) {
        if (file->readAt(file->arg, Area(store)->bank[bank] + offset, page, sizeof(page)) != 0)
            return Fail(store, CP_JOURNAL_ERROR_READ);
        for (i = 0; i < sizeof(page); i++) {
            if (blank == -1) blank = page[i];
            if ((blank != 0 && blank != 255) || page[i] != blank) return Fail(store, CP_JOURNAL_ERROR_FOREIGN);
        }
    }
    if (store->hash && Settle(store, bank, record) != 0) return Fail(store, CP_JOURNAL_ERROR_READ);
    return 0;
}

static int Stage(CpJournalStore *store, uint32_t offset, const unsigned char *bytes, size_t size)
{
    const CpPlatformFile *file = store->file;
    if (file->writeAt(file->arg, offset, bytes, size) != 0) return Fail(store, CP_JOURNAL_ERROR_WRITE);
    if (file->sync(file->arg) != 0) return Fail(store, CP_JOURNAL_ERROR_SYNC);
    return 0;
}

int CpJournal_WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    static const unsigned char uncommitted[4] = { 0, 0, 0, 0 };
    enum { TAG = WT_JOURNAL_RECORD_SIZE - 4 };
    CpJournalStore *store = arg;
    const CpPlatformFile *file;
    unsigned char check[WT_JOURNAL_RECORD_SIZE];
    uint32_t address;
    int result = -1;
    if (!store || !store->file || !record || bank < 0 || bank > 1) return -1;
    file = store->file;
    if (!file->writeAt || !file->readDurable) return Fail(store, CP_JOURNAL_ERROR_READ_ONLY);
    if (!SizeMatches(file)) return Fail(store, CP_JOURNAL_ERROR_SIZE);
    address = Area(store)->bank[bank];
    if (store->hold) store->hold(1);
    if (Stage(store, address + TAG, uncommitted, sizeof(uncommitted)) != 0
        || Stage(store, address, record, TAG) != 0
        || Stage(store, address + TAG, record + TAG, 4) != 0) goto done;
    if (file->readDurable(file->arg, address, check, sizeof(check)) != 0) { Fail(store, CP_JOURNAL_ERROR_READ); goto done; }
    if (memcmp(check, record, sizeof(check)) != 0) { Fail(store, CP_JOURNAL_ERROR_VERIFY); goto done; }
    if (!SizeMatches(file)) { Fail(store, CP_JOURNAL_ERROR_SIZE); goto done; }
    result = 0;
done:
    if (store->hold) store->hold(0);
    memset(check, 0, sizeof(check));
    return result;
}

WtJournalIO CpJournal_IO(CpJournalStore *store, WtJournalHash hash)
{
    WtJournalIO io = { store, CpJournal_ReadBank, CpJournal_WriteBank, hash };
    if (store) store->hash = hash;
    return io;
}
