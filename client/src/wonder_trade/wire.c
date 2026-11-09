#include "wonder_trade/wire.h"
#include "wonder_trade/wire_layout.h"

#include <string.h>

static int IsZero(const unsigned char *bytes, size_t size)
{
    unsigned char value = 0;
    size_t i;
    for (i = 0; i < size; i++) value |= bytes[i];
    return value == 0;
}

static void Clear(void *pointer, size_t size)
{
    volatile unsigned char *bytes = pointer;
    size_t i;
    for (i = 0; i < size; i++) bytes[i] = 0;
}

static uint64_t ReadNumber(const unsigned char *bytes, int size)
{
    uint64_t value = 0;
    int i;
    for (i = size - 1; i >= 0; i--) value = (value << 8) | bytes[i];
    return value;
}

static void WriteNumber(unsigned char *bytes, uint64_t value, int size)
{
    int i;
    for (i = 0; i < size; i++) { bytes[i] = (unsigned char)value; value >>= 8; }
}

static int CanonicalPokemon(const unsigned char *bytes)
{
    unsigned sum = 0;
    int i;
    if (!IsZero(bytes + WT_WIRE_PK4_SANITY, 2) || !IsZero(bytes + WT_WIRE_PK4_UNUSED, WT_WIRE_PK4_UNUSED_SIZE)) return 0;
    for (i = WT_WIRE_PK4_BLOCKS; i < WT_WIRE_PK4_SIZE; i += 2) sum += (unsigned)ReadNumber(bytes + i, 2);
    return (sum & 65535) == ReadNumber(bytes + WT_WIRE_PK4_CHECKSUM, 2);
}

static int DigestMatches(const unsigned char *bytes, size_t size, const unsigned char *expected, WtWireHash hash)
{
    unsigned char digest[WT_WIRE_DIGEST_SIZE];
    int matches = hash(bytes, size, digest) == 0 && memcmp(digest, expected, WT_WIRE_DIGEST_SIZE) == 0;
    Clear(digest, sizeof(digest));
    return matches;
}

int WtWire_CheckRequest(const unsigned char *frame, size_t size, WtWireHash hash)
{
    static const unsigned sizes[] = {
        0, 0, WT_WIRE_SUBMIT_SIZE, WT_WIRE_TRADE_REQUEST_SIZE, WT_WIRE_TRADE_REQUEST_SIZE,
        WT_WIRE_TRADE_REQUEST_SIZE, WT_WIRE_ACK_SIZE, WT_WIRE_RECONCILE_SIZE
    };
    const unsigned char *body;
    unsigned operation;
    uint64_t counter;
    int mutation;
    if (!frame || !hash || size < WT_WIRE_REQUEST_HEADER_SIZE || size > WT_WIRE_REQUEST_MAX
        || memcmp(frame, WT_WIRE_MAGIC, WT_WIRE_MAGIC_SIZE) != 0
        || ReadNumber(frame + WT_WIRE_REQUEST_BODY_LENGTH, 2) != size - WT_WIRE_REQUEST_HEADER_SIZE) return -1;
    operation = frame[WT_WIRE_REQUEST_OPERATION]; body = frame + WT_WIRE_REQUEST_HEADER_SIZE;
    if (operation < WT_OP_ENROLL || operation > WT_OP_RECONCILE
        || IsZero(frame + WT_WIRE_REQUEST_ID, WT_WIRE_ID_SIZE)
        || IsZero(frame + WT_WIRE_REQUEST_CREDENTIAL, WT_WIRE_CREDENTIAL_SIZE)
        || ((operation == WT_OP_ENROLL) != IsZero(frame + WT_WIRE_REQUEST_SAVE_ID, WT_WIRE_ID_SIZE))) return -1;
    counter = ReadNumber(frame + WT_WIRE_REQUEST_COUNTER, 8);
    mutation = operation == WT_OP_SUBMIT || operation == WT_OP_CANCEL || operation == WT_OP_ACKNOWLEDGE;
    if (mutation != (counter != 0) || counter == UINT64_MAX) return -1;
    if (operation == WT_OP_ENROLL) {
        return size >= WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS
            && size <= WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS + WT_WIRE_ENROLL_HINTS_MAX
            && body[WT_WIRE_ENROLL_HINT_LENGTH] == size - (WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ENROLL_HINTS) ? 0 : -1;
    }
    if (size - WT_WIRE_REQUEST_HEADER_SIZE != sizes[operation]) return -1;
    if (operation == WT_OP_SUBMIT) {
        return ReadNumber(body + WT_WIRE_SUBMIT_GENERATION, 8) != 0 && CanonicalPokemon(body + WT_WIRE_SUBMIT_POKEMON)
            && DigestMatches(body + WT_WIRE_SUBMIT_POKEMON, WT_WIRE_PK4_SIZE, body + WT_WIRE_SUBMIT_DIGEST, hash) ? 0 : -1;
    }
    /* Cancel, Collect and Acknowledge bodies all start with the trade ID. */
    if ((operation == WT_OP_CANCEL || operation == WT_OP_COLLECT || operation == WT_OP_ACKNOWLEDGE)
        && IsZero(body + WT_WIRE_ACK_TRADE, WT_WIRE_ID_SIZE)) return -1;
    if (operation == WT_OP_ACKNOWLEDGE && (IsZero(body + WT_WIRE_ACK_RECEIPT, WT_WIRE_ID_SIZE)
        || ReadNumber(body + WT_WIRE_ACK_GENERATION, 8) == 0)) return -1;
    if (operation == WT_OP_RECONCILE && (body[WT_WIRE_RECONCILE_LIMIT] < 1
        || body[WT_WIRE_RECONCILE_LIMIT] > WT_WIRE_RECONCILE_LIMIT_MAX)) return -1;
    return 0;
}

int WtWire_Encode(const WtWireRequest *request, WtWireHash hash, unsigned char *output, size_t capacity, size_t *size)
{
    unsigned char frame[WT_WIRE_REQUEST_MAX] = { 0 };
    unsigned char *body = frame + WT_WIRE_REQUEST_HEADER_SIZE;
    size_t length = 0;
    int result = -1;
    if (!size) return -1;
    *size = 0;
    if (!request || !hash || !output) return -1;
    memcpy(frame, WT_WIRE_MAGIC, WT_WIRE_MAGIC_SIZE); frame[WT_WIRE_REQUEST_OPERATION] = request->operation;
    memcpy(frame + WT_WIRE_REQUEST_ID, request->requestID, WT_WIRE_ID_SIZE);
    memcpy(frame + WT_WIRE_REQUEST_SAVE_ID, request->saveID, WT_WIRE_ID_SIZE);
    memcpy(frame + WT_WIRE_REQUEST_CREDENTIAL, request->credential, WT_WIRE_CREDENTIAL_SIZE);
    WriteNumber(frame + WT_WIRE_REQUEST_COUNTER, request->counter, 8);
    switch (request->operation) {
    case WT_OP_ENROLL:
        if (request->body.enroll.hintLength > WT_WIRE_ENROLL_HINTS_MAX) goto done;
        length = WT_WIRE_ENROLL_HINTS + request->body.enroll.hintLength;
        WriteNumber(body + WT_WIRE_ENROLL_BUILD, request->body.enroll.build, 4);
        body[WT_WIRE_ENROLL_HINT_LENGTH] = request->body.enroll.hintLength;
        memcpy(body + WT_WIRE_ENROLL_HINTS, request->body.enroll.hints, request->body.enroll.hintLength);
        break;
    case WT_OP_SUBMIT:
        length = WT_WIRE_SUBMIT_SIZE; WriteNumber(body + WT_WIRE_SUBMIT_GENERATION, request->body.submit.generation, 8);
        memcpy(body + WT_WIRE_SUBMIT_POKEMON, request->body.submit.pokemon, WT_WIRE_PK4_SIZE);
        if (hash(body + WT_WIRE_SUBMIT_POKEMON, WT_WIRE_PK4_SIZE, body + WT_WIRE_SUBMIT_DIGEST) != 0) goto done;
        break;
    case WT_OP_STATUS: case WT_OP_CANCEL: case WT_OP_COLLECT:
        length = WT_WIRE_TRADE_REQUEST_SIZE; memcpy(body, request->body.trade, WT_WIRE_ID_SIZE);
        break;
    case WT_OP_ACKNOWLEDGE:
        length = WT_WIRE_ACK_SIZE;
        memcpy(body + WT_WIRE_ACK_TRADE, request->body.acknowledge.trade, WT_WIRE_ID_SIZE);
        memcpy(body + WT_WIRE_ACK_RECEIPT, request->body.acknowledge.receipt, WT_WIRE_ID_SIZE);
        memcpy(body + WT_WIRE_ACK_DELIVERED_DIGEST, request->body.acknowledge.deliveredDigest, WT_WIRE_DIGEST_SIZE);
        memcpy(body + WT_WIRE_ACK_APPLIED_DIGEST, request->body.acknowledge.appliedDigest, WT_WIRE_DIGEST_SIZE);
        WriteNumber(body + WT_WIRE_ACK_GENERATION, request->body.acknowledge.generation, 8);
        break;
    case WT_OP_RECONCILE:
        length = WT_WIRE_RECONCILE_SIZE;
        WriteNumber(body + WT_WIRE_RECONCILE_AFTER, request->body.reconcile.after, 8);
        body[WT_WIRE_RECONCILE_LIMIT] = request->body.reconcile.limit;
        break;
    default: goto done;
    }
    WriteNumber(frame + WT_WIRE_REQUEST_BODY_LENGTH, length, 2);
    length += WT_WIRE_REQUEST_HEADER_SIZE;
    if (capacity < length || WtWire_CheckRequest(frame, length, hash) != 0) goto done;
    memcpy(output, frame, length); *size = length; result = 0;
done:
    Clear(frame, sizeof(frame));
    return result;
}

static int SnapshotKnown(const unsigned char *bytes, uint64_t highwater)
{
    unsigned state = bytes[WT_WIRE_SNAPSHOT_STATE], kind = bytes[WT_WIRE_SNAPSHOT_DELIVERY_KIND];
    uint64_t event = ReadNumber(bytes + WT_WIRE_SNAPSHOT_LAST_EVENT, 8);
    if (state == WT_TRADE_NONE) return IsZero(bytes, WT_WIRE_SNAPSHOT_SIZE);
    if (state > WT_TRADE_ACKNOWLEDGED || kind > WT_DELIVERY_RETURN
        || IsZero(bytes + WT_WIRE_SNAPSHOT_TRADE, WT_WIRE_ID_SIZE)
        || IsZero(bytes + WT_WIRE_SNAPSHOT_OFFERED_ASSET, WT_WIRE_ID_SIZE) || event == 0 || event > highwater) return 0;
    /* Received asset, received digest and receipt are contiguous and absent until delivery. */
    if (state == WT_TRADE_QUEUED) return kind == WT_DELIVERY_NONE && IsZero(bytes + WT_WIRE_SNAPSHOT_RECEIVED_ASSET,
        WT_WIRE_SNAPSHOT_LAST_EVENT - WT_WIRE_SNAPSHOT_RECEIVED_ASSET);
    if (kind == WT_DELIVERY_NONE || IsZero(bytes + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, WT_WIRE_ID_SIZE)
        || IsZero(bytes + WT_WIRE_SNAPSHOT_RECEIPT, WT_WIRE_ID_SIZE)
        || (state == WT_TRADE_MATCHED && kind != WT_DELIVERY_MATCH)
        || (state == WT_TRADE_RETURN_READY && kind != WT_DELIVERY_RETURN)) return 0;
    if (kind == WT_DELIVERY_RETURN) {
        return memcmp(bytes + WT_WIRE_SNAPSHOT_OFFERED_ASSET, bytes + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, WT_WIRE_ID_SIZE) == 0
            && memcmp(bytes + WT_WIRE_SNAPSHOT_OFFERED_DIGEST, bytes + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, WT_WIRE_DIGEST_SIZE) == 0;
    }
    return memcmp(bytes + WT_WIRE_SNAPSHOT_OFFERED_ASSET, bytes + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, WT_WIRE_ID_SIZE) != 0;
}

static void ReadSnapshot(const unsigned char *bytes, WtWireSnapshot *snapshot)
{
    memcpy(snapshot->trade, bytes + WT_WIRE_SNAPSHOT_TRADE, WT_WIRE_ID_SIZE);
    snapshot->state = bytes[WT_WIRE_SNAPSHOT_STATE]; snapshot->deliveryKind = bytes[WT_WIRE_SNAPSHOT_DELIVERY_KIND];
    memcpy(snapshot->offeredAsset, bytes + WT_WIRE_SNAPSHOT_OFFERED_ASSET, WT_WIRE_ID_SIZE);
    memcpy(snapshot->offeredDigest, bytes + WT_WIRE_SNAPSHOT_OFFERED_DIGEST, WT_WIRE_DIGEST_SIZE);
    memcpy(snapshot->receivedAsset, bytes + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, WT_WIRE_ID_SIZE);
    memcpy(snapshot->receivedDigest, bytes + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, WT_WIRE_DIGEST_SIZE);
    memcpy(snapshot->receipt, bytes + WT_WIRE_SNAPSHOT_RECEIPT, WT_WIRE_ID_SIZE);
    snapshot->lastEvent = ReadNumber(bytes + WT_WIRE_SNAPSHOT_LAST_EVENT, 8);
}

static int EventKnown(const unsigned char *bytes)
{
    unsigned kind = bytes[WT_WIRE_EVENT_KIND], state = bytes[WT_WIRE_EVENT_STATE];
    if (kind < WT_EVENT_ENROLLED || kind > WT_EVENT_REJECTED || state > WT_TRADE_ACKNOWLEDGED) return 0;
    /* Every field between the kind and the state is zero for enrollment. */
    if (kind == WT_EVENT_ENROLLED) return ReadNumber(bytes + WT_WIRE_EVENT_SEQUENCE, 8) == 1 && state == WT_TRADE_NONE
        && IsZero(bytes + WT_WIRE_EVENT_TRADE, WT_WIRE_EVENT_STATE - WT_WIRE_EVENT_TRADE);
    if (ReadNumber(bytes + WT_WIRE_EVENT_SEQUENCE, 8) <= 1) return 0;
    if (kind == WT_EVENT_REJECTED) {
        return !IsZero(bytes + WT_WIRE_EVENT_OPERATION, WT_WIRE_ID_SIZE) && IsZero(bytes + WT_WIRE_EVENT_ASSET, WT_WIRE_ID_SIZE)
            && IsZero(bytes + WT_WIRE_EVENT_RECEIPT, WT_WIRE_ID_SIZE)
            && (IsZero(bytes + WT_WIRE_EVENT_TRADE, WT_WIRE_ID_SIZE) ? state == WT_TRADE_NONE : state != WT_TRADE_NONE);
    }
    return !IsZero(bytes + WT_WIRE_EVENT_TRADE, WT_WIRE_ID_SIZE) && !IsZero(bytes + WT_WIRE_EVENT_ASSET, WT_WIRE_ID_SIZE)
        && (kind == WT_EVENT_MATCHED ? IsZero(bytes + WT_WIRE_EVENT_OPERATION, WT_WIRE_ID_SIZE)
            : !IsZero(bytes + WT_WIRE_EVENT_OPERATION, WT_WIRE_ID_SIZE))
        && (kind == WT_EVENT_SUBMITTED ? IsZero(bytes + WT_WIRE_EVENT_RECEIPT, WT_WIRE_ID_SIZE)
            : !IsZero(bytes + WT_WIRE_EVENT_RECEIPT, WT_WIRE_ID_SIZE))
        && state == kind - 1;
}

static void ReadEvent(const unsigned char *bytes, WtWireEvent *event)
{
    event->sequence = ReadNumber(bytes + WT_WIRE_EVENT_SEQUENCE, 8); event->kind = bytes[WT_WIRE_EVENT_KIND];
    memcpy(event->trade, bytes + WT_WIRE_EVENT_TRADE, WT_WIRE_ID_SIZE);
    memcpy(event->operation, bytes + WT_WIRE_EVENT_OPERATION, WT_WIRE_ID_SIZE);
    memcpy(event->asset, bytes + WT_WIRE_EVENT_ASSET, WT_WIRE_ID_SIZE);
    memcpy(event->digest, bytes + WT_WIRE_EVENT_DIGEST, WT_WIRE_DIGEST_SIZE);
    memcpy(event->receipt, bytes + WT_WIRE_EVENT_RECEIPT, WT_WIRE_ID_SIZE); event->state = bytes[WT_WIRE_EVENT_STATE];
}

static int DecodeSuccess(const unsigned char *request, const unsigned char *body, size_t size,
    WtWireHash hash, WtWireReply *reply)
{
    const unsigned char *sent = request + WT_WIRE_REQUEST_HEADER_SIZE;
    const unsigned char *snapshot;
    uint64_t event, cursor, end;
    unsigned expectedState, count, more, i;
    switch (reply->operation) {
    case WT_OP_ENROLL:
        if (size != WT_WIRE_ID_SIZE || IsZero(body, WT_WIRE_ID_SIZE)) return -1;
        memcpy(reply->body.saveID, body, WT_WIRE_ID_SIZE);
        return 0;
    case WT_OP_SUBMIT: case WT_OP_CANCEL: case WT_OP_ACKNOWLEDGE:
        if (size != WT_WIRE_OUTCOME_SIZE || reply->nextCounter <= ReadNumber(request + WT_WIRE_REQUEST_COUNTER, 8)) return -1;
        event = ReadNumber(body + WT_WIRE_OUTCOME_EVENT, 8); snapshot = body + WT_WIRE_OUTCOME_SNAPSHOT;
        expectedState = reply->operation == WT_OP_SUBMIT ? WT_TRADE_QUEUED
            : reply->operation == WT_OP_CANCEL ? WT_TRADE_RETURN_READY : WT_TRADE_ACKNOWLEDGED;
        if (!SnapshotKnown(snapshot, reply->highwater) || snapshot[WT_WIRE_SNAPSHOT_STATE] != expectedState
            || event == 0 || event != ReadNumber(snapshot + WT_WIRE_SNAPSHOT_LAST_EVENT, 8)) return -1;
        if (reply->operation == WT_OP_SUBMIT) {
            if (memcmp(snapshot + WT_WIRE_SNAPSHOT_OFFERED_DIGEST, sent + WT_WIRE_SUBMIT_DIGEST, WT_WIRE_DIGEST_SIZE) != 0) return -1;
        } else if (memcmp(snapshot + WT_WIRE_SNAPSHOT_TRADE, sent + WT_WIRE_ACK_TRADE, WT_WIRE_ID_SIZE) != 0) return -1;
        if (reply->operation == WT_OP_ACKNOWLEDGE
            && (memcmp(snapshot + WT_WIRE_SNAPSHOT_RECEIPT, sent + WT_WIRE_ACK_RECEIPT, WT_WIRE_ID_SIZE) != 0
                || memcmp(snapshot + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, sent + WT_WIRE_ACK_DELIVERED_DIGEST,
                    WT_WIRE_DIGEST_SIZE) != 0)) return -1;
        reply->body.mutation.event = event;
        ReadSnapshot(snapshot, &reply->body.mutation.snapshot);
        return 0;
    case WT_OP_STATUS:
        if (size != WT_WIRE_SNAPSHOT_SIZE || !SnapshotKnown(body, reply->highwater)
            || (!IsZero(sent, WT_WIRE_ID_SIZE) && memcmp(body + WT_WIRE_SNAPSHOT_TRADE, sent, WT_WIRE_ID_SIZE) != 0)) return -1;
        ReadSnapshot(body, &reply->body.status);
        return 0;
    case WT_OP_COLLECT:
        if (size != WT_WIRE_COLLECTION_SIZE || !SnapshotKnown(body + WT_WIRE_COLLECTION_SNAPSHOT, reply->highwater)
            || body[WT_WIRE_COLLECTION_SNAPSHOT + WT_WIRE_SNAPSHOT_STATE] < WT_TRADE_MATCHED
            || memcmp(body + WT_WIRE_COLLECTION_SNAPSHOT + WT_WIRE_SNAPSHOT_TRADE, sent, WT_WIRE_ID_SIZE) != 0
            || !CanonicalPokemon(body + WT_WIRE_COLLECTION_POKEMON)
            || !DigestMatches(body + WT_WIRE_COLLECTION_POKEMON, WT_WIRE_PK4_SIZE,
                body + WT_WIRE_COLLECTION_SNAPSHOT + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, hash)) return -1;
        ReadSnapshot(body + WT_WIRE_COLLECTION_SNAPSHOT, &reply->body.collection.snapshot);
        memcpy(reply->body.collection.pokemon, body + WT_WIRE_COLLECTION_POKEMON, WT_WIRE_PK4_SIZE);
        return 0;
    case WT_OP_RECONCILE:
        if (size < WT_WIRE_PAGE_EVENTS) return -1;
        count = body[WT_WIRE_PAGE_COUNT]; more = body[WT_WIRE_PAGE_HAS_MORE];
        cursor = ReadNumber(sent + WT_WIRE_RECONCILE_AFTER, 8);
        if (count > sent[WT_WIRE_RECONCILE_LIMIT] || more > 1 || size != WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE
            || cursor > reply->highwater || count > reply->highwater - cursor) return -1;
        end = cursor + count;
        if (ReadNumber(body + WT_WIRE_PAGE_END, 8) != end || more != (end < reply->highwater)
            || (more && count != sent[WT_WIRE_RECONCILE_LIMIT])) return -1;
        for (i = 0; i < count; i++) {
            const unsigned char *bytes = body + WT_WIRE_PAGE_EVENTS + i * WT_WIRE_EVENT_SIZE;
            if (ReadNumber(bytes + WT_WIRE_EVENT_SEQUENCE, 8) != cursor + i + 1 || !EventKnown(bytes)) return -1;
            ReadEvent(bytes, &reply->body.page.events[i]);
        }
        reply->body.page.count = (unsigned char)count; reply->body.page.hasMore = (unsigned char)more; reply->body.page.end = end;
        return 0;
    default: return -1;
    }
}

int WtWire_Decode(const unsigned char *request, size_t requestSize,
    const unsigned char *packet, size_t packetSize, WtWireHash hash, WtWireReply *reply)
{
    if (!reply) return -1;
    Clear(reply, sizeof(*reply));
    if (WtWire_CheckRequest(request, requestSize, hash) != 0 || !packet
        || packetSize < WT_WIRE_REPLY_HEADER_SIZE || packetSize > WT_WIRE_REPLY_MAX
        || memcmp(packet, WT_WIRE_MAGIC, WT_WIRE_MAGIC_SIZE) != 0
        || packet[WT_WIRE_REPLY_OPERATION] != request[WT_WIRE_REQUEST_OPERATION]
        || ReadNumber(packet + WT_WIRE_REPLY_BODY_LENGTH, 2) != packetSize - WT_WIRE_REPLY_HEADER_SIZE
        || memcmp(packet + WT_WIRE_REPLY_REQUEST_ID, request + WT_WIRE_REQUEST_ID, WT_WIRE_ID_SIZE) != 0
        || !IsZero(packet + WT_WIRE_REPLY_RESERVED, 2)) return -1;
    reply->operation = packet[WT_WIRE_REPLY_OPERATION];
    reply->result = (uint16_t)ReadNumber(packet + WT_WIRE_REPLY_RESULT, 2);
    reply->nextCounter = ReadNumber(packet + WT_WIRE_REPLY_NEXT_COUNTER, 8);
    reply->highwater = ReadNumber(packet + WT_WIRE_REPLY_HIGHWATER, 8);
    if (reply->result < WT_RESULT_OK || reply->result > WT_RESULT_UNAVAILABLE
        || ((reply->nextCounter == 0) != (reply->highwater == 0))) goto invalid;
    if (reply->result != WT_RESULT_OK) {
        if (packetSize != WT_WIRE_REPLY_HEADER_SIZE || ((reply->result == WT_RESULT_UNAUTHENTICATED
            || reply->result == WT_RESULT_PROCESSING) && reply->nextCounter != 0)) goto invalid;
        return 0;
    }
    if (reply->nextCounter == 0 || DecodeSuccess(request, packet + WT_WIRE_REPLY_HEADER_SIZE,
        packetSize - WT_WIRE_REPLY_HEADER_SIZE, hash, reply) != 0) goto invalid;
    return 0;
invalid:
    Clear(reply, sizeof(*reply));
    return -1;
}
