#include "wonder_trade/wire.h"

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
    if (!IsZero(bytes + 4, 2) || !IsZero(bytes + 0x64, 4)) return 0;
    for (i = 8; i < 136; i += 2) sum += (unsigned)ReadNumber(bytes + i, 2);
    return (sum & 65535) == ReadNumber(bytes + 6, 2);
}

static int DigestMatches(const unsigned char *bytes, size_t size, const unsigned char *expected, WtWireHash hash)
{
    unsigned char digest[32];
    int matches = hash(bytes, size, digest) == 0 && memcmp(digest, expected, 32) == 0;
    Clear(digest, sizeof(digest));
    return matches;
}

int WtWire_CheckRequest(const unsigned char *frame, size_t size, WtWireHash hash)
{
    static const unsigned sizes[] = { 0, 0, 176, 16, 16, 16, 104, 9 };
    const unsigned char *body;
    unsigned operation;
    uint64_t counter;
    int mutation;
    if (!frame || !hash || size < 80 || size > WT_WIRE_REQUEST_MAX
        || memcmp(frame, "WTDS\1", 5) != 0 || ReadNumber(frame + 6, 2) != size - 80) return -1;
    operation = frame[5]; body = frame + 80;
    if (operation < WT_OP_ENROLL || operation > WT_OP_RECONCILE
        || IsZero(frame + 8, 16) || IsZero(frame + 40, 32)
        || ((operation == WT_OP_ENROLL) != IsZero(frame + 24, 16))) return -1;
    counter = ReadNumber(frame + 72, 8);
    mutation = operation == WT_OP_SUBMIT || operation == WT_OP_CANCEL || operation == WT_OP_ACKNOWLEDGE;
    if (mutation != (counter != 0) || counter == UINT64_MAX) return -1;
    if (operation == WT_OP_ENROLL) {
        return size >= 85 && size <= 149 && body[4] == size - 85 ? 0 : -1;
    }
    if (size - 80 != sizes[operation]) return -1;
    if (operation == WT_OP_SUBMIT) {
        return ReadNumber(body, 8) != 0 && CanonicalPokemon(body + 8)
            && DigestMatches(body + 8, 136, body + 144, hash) ? 0 : -1;
    }
    if ((operation == WT_OP_CANCEL || operation == WT_OP_COLLECT || operation == WT_OP_ACKNOWLEDGE)
        && IsZero(body, 16)) return -1;
    if (operation == WT_OP_ACKNOWLEDGE && (IsZero(body + 16, 16) || ReadNumber(body + 96, 8) == 0)) return -1;
    if (operation == WT_OP_RECONCILE && (body[8] < 1 || body[8] > 8)) return -1;
    return 0;
}

int WtWire_Encode(const WtWireRequest *request, WtWireHash hash, unsigned char *output, size_t capacity, size_t *size)
{
    unsigned char frame[WT_WIRE_REQUEST_MAX] = { 0 };
    unsigned char *body = frame + 80;
    size_t length = 0;
    int result = -1;
    if (!size) return -1;
    *size = 0;
    if (!request || !hash || !output) return -1;
    memcpy(frame, "WTDS\1", 5); frame[5] = request->operation;
    memcpy(frame + 8, request->requestID, 16); memcpy(frame + 24, request->saveID, 16);
    memcpy(frame + 40, request->credential, 32); WriteNumber(frame + 72, request->counter, 8);
    switch (request->operation) {
    case WT_OP_ENROLL:
        if (request->body.enroll.hintLength > 64) goto done;
        length = 5 + request->body.enroll.hintLength;
        WriteNumber(body, request->body.enroll.build, 4); body[4] = request->body.enroll.hintLength;
        memcpy(body + 5, request->body.enroll.hints, request->body.enroll.hintLength);
        break;
    case WT_OP_SUBMIT:
        length = 176; WriteNumber(body, request->body.submit.generation, 8);
        memcpy(body + 8, request->body.submit.pokemon, 136);
        if (hash(body + 8, 136, body + 144) != 0) goto done;
        break;
    case WT_OP_STATUS: case WT_OP_CANCEL: case WT_OP_COLLECT:
        length = 16; memcpy(body, request->body.trade, 16);
        break;
    case WT_OP_ACKNOWLEDGE:
        length = 104;
        memcpy(body, request->body.acknowledge.trade, 16); memcpy(body + 16, request->body.acknowledge.receipt, 16);
        memcpy(body + 32, request->body.acknowledge.deliveredDigest, 32);
        memcpy(body + 64, request->body.acknowledge.appliedDigest, 32);
        WriteNumber(body + 96, request->body.acknowledge.generation, 8);
        break;
    case WT_OP_RECONCILE:
        length = 9; WriteNumber(body, request->body.reconcile.after, 8); body[8] = request->body.reconcile.limit;
        break;
    default: goto done;
    }
    WriteNumber(frame + 6, length, 2);
    length += 80;
    if (capacity < length || WtWire_CheckRequest(frame, length, hash) != 0) goto done;
    memcpy(output, frame, length); *size = length; result = 0;
done:
    Clear(frame, sizeof(frame));
    return result;
}

static int SnapshotKnown(const unsigned char *bytes, uint64_t highwater)
{
    unsigned state = bytes[16], kind = bytes[17];
    uint64_t event = ReadNumber(bytes + 130, 8);
    if (state == WT_TRADE_NONE) return IsZero(bytes, 138);
    if (state > WT_TRADE_ACKNOWLEDGED || kind > WT_DELIVERY_RETURN
        || IsZero(bytes, 16) || IsZero(bytes + 18, 16) || event == 0 || event > highwater) return 0;
    if (state == WT_TRADE_QUEUED) return kind == WT_DELIVERY_NONE && IsZero(bytes + 66, 64);
    if (kind == WT_DELIVERY_NONE || IsZero(bytes + 66, 16) || IsZero(bytes + 114, 16)
        || (state == WT_TRADE_MATCHED && kind != WT_DELIVERY_MATCH)
        || (state == WT_TRADE_RETURN_READY && kind != WT_DELIVERY_RETURN)) return 0;
    if (kind == WT_DELIVERY_RETURN) return memcmp(bytes + 18, bytes + 66, 16) == 0 && memcmp(bytes + 34, bytes + 82, 32) == 0;
    return memcmp(bytes + 18, bytes + 66, 16) != 0;
}

static void ReadSnapshot(const unsigned char *bytes, WtWireSnapshot *snapshot)
{
    memcpy(snapshot->trade, bytes, 16); snapshot->state = bytes[16]; snapshot->deliveryKind = bytes[17];
    memcpy(snapshot->offeredAsset, bytes + 18, 16); memcpy(snapshot->offeredDigest, bytes + 34, 32);
    memcpy(snapshot->receivedAsset, bytes + 66, 16); memcpy(snapshot->receivedDigest, bytes + 82, 32);
    memcpy(snapshot->receipt, bytes + 114, 16); snapshot->lastEvent = ReadNumber(bytes + 130, 8);
}

static int EventKnown(const unsigned char *bytes)
{
    unsigned kind = bytes[8], state = bytes[105];
    if (kind < WT_EVENT_ENROLLED || kind > WT_EVENT_REJECTED || state > WT_TRADE_ACKNOWLEDGED) return 0;
    if (kind == WT_EVENT_ENROLLED) return ReadNumber(bytes, 8) == 1 && state == WT_TRADE_NONE && IsZero(bytes + 9, 96);
    if (ReadNumber(bytes, 8) <= 1) return 0;
    if (kind == WT_EVENT_REJECTED) {
        return !IsZero(bytes + 25, 16) && IsZero(bytes + 41, 16) && IsZero(bytes + 89, 16)
            && (IsZero(bytes + 9, 16) ? state == WT_TRADE_NONE : state != WT_TRADE_NONE);
    }
    return !IsZero(bytes + 9, 16) && !IsZero(bytes + 41, 16)
        && (kind == WT_EVENT_MATCHED ? IsZero(bytes + 25, 16) : !IsZero(bytes + 25, 16))
        && (kind == WT_EVENT_SUBMITTED ? IsZero(bytes + 89, 16) : !IsZero(bytes + 89, 16))
        && state == kind - 1;
}

static void ReadEvent(const unsigned char *bytes, WtWireEvent *event)
{
    event->sequence = ReadNumber(bytes, 8); event->kind = bytes[8];
    memcpy(event->trade, bytes + 9, 16); memcpy(event->operation, bytes + 25, 16);
    memcpy(event->asset, bytes + 41, 16); memcpy(event->digest, bytes + 57, 32);
    memcpy(event->receipt, bytes + 89, 16); event->state = bytes[105];
}

static int DecodeSuccess(const unsigned char *request, const unsigned char *body, size_t size,
    WtWireHash hash, WtWireReply *reply)
{
    const unsigned char *sent = request + 80;
    const unsigned char *snapshot;
    uint64_t event, cursor, end;
    unsigned expectedState, count, more, i;
    switch (reply->operation) {
    case WT_OP_ENROLL:
        if (size != 16 || IsZero(body, 16)) return -1;
        memcpy(reply->body.saveID, body, 16);
        return 0;
    case WT_OP_SUBMIT: case WT_OP_CANCEL: case WT_OP_ACKNOWLEDGE:
        if (size != 146 || reply->nextCounter <= ReadNumber(request + 72, 8)) return -1;
        event = ReadNumber(body, 8); snapshot = body + 8;
        expectedState = reply->operation == WT_OP_SUBMIT ? WT_TRADE_QUEUED
            : reply->operation == WT_OP_CANCEL ? WT_TRADE_RETURN_READY : WT_TRADE_ACKNOWLEDGED;
        if (!SnapshotKnown(snapshot, reply->highwater) || snapshot[16] != expectedState
            || event == 0 || event != ReadNumber(snapshot + 130, 8)) return -1;
        if (reply->operation == WT_OP_SUBMIT) {
            if (memcmp(snapshot + 34, sent + 144, 32) != 0) return -1;
        } else if (memcmp(snapshot, sent, 16) != 0) return -1;
        if (reply->operation == WT_OP_ACKNOWLEDGE
            && (memcmp(snapshot + 114, sent + 16, 16) != 0 || memcmp(snapshot + 82, sent + 32, 32) != 0)) return -1;
        reply->body.mutation.event = event;
        ReadSnapshot(snapshot, &reply->body.mutation.snapshot);
        return 0;
    case WT_OP_STATUS:
        if (size != 138 || !SnapshotKnown(body, reply->highwater)
            || (!IsZero(sent, 16) && memcmp(body, sent, 16) != 0)) return -1;
        ReadSnapshot(body, &reply->body.status);
        return 0;
    case WT_OP_COLLECT:
        if (size != 274 || !SnapshotKnown(body, reply->highwater) || body[16] < WT_TRADE_MATCHED
            || memcmp(body, sent, 16) != 0 || !CanonicalPokemon(body + 138)
            || !DigestMatches(body + 138, 136, body + 82, hash)) return -1;
        ReadSnapshot(body, &reply->body.collection.snapshot);
        memcpy(reply->body.collection.pokemon, body + 138, 136);
        return 0;
    case WT_OP_RECONCILE:
        if (size < 10) return -1;
        count = body[0]; more = body[1]; cursor = ReadNumber(sent, 8);
        if (count > sent[8] || more > 1 || size != 10 + count * 106
            || cursor > reply->highwater || count > reply->highwater - cursor) return -1;
        end = cursor + count;
        if (ReadNumber(body + 2, 8) != end || more != (end < reply->highwater) || (more && count != sent[8])) return -1;
        for (i = 0; i < count; i++) {
            const unsigned char *bytes = body + 10 + i * 106;
            if (ReadNumber(bytes, 8) != cursor + i + 1 || !EventKnown(bytes)) return -1;
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
        || packetSize < 44 || packetSize > WT_WIRE_REPLY_MAX
        || memcmp(packet, "WTDS\1", 5) != 0 || packet[5] != request[5]
        || ReadNumber(packet + 6, 2) != packetSize - 44 || memcmp(packet + 8, request + 8, 16) != 0
        || !IsZero(packet + 26, 2)) return -1;
    reply->operation = packet[5]; reply->result = (uint16_t)ReadNumber(packet + 24, 2);
    reply->nextCounter = ReadNumber(packet + 28, 8); reply->highwater = ReadNumber(packet + 36, 8);
    if (reply->result < WT_RESULT_OK || reply->result > WT_RESULT_UNAVAILABLE
        || ((reply->nextCounter == 0) != (reply->highwater == 0))) goto invalid;
    if (reply->result != WT_RESULT_OK) {
        if (packetSize != 44 || ((reply->result == WT_RESULT_UNAUTHENTICATED || reply->result == WT_RESULT_PROCESSING)
            && reply->nextCounter != 0)) goto invalid;
        return 0;
    }
    if (reply->nextCounter == 0 || DecodeSuccess(request, packet + 44, packetSize - 44, hash, reply) != 0) goto invalid;
    return 0;
invalid:
    Clear(reply, sizeof(*reply));
    return -1;
}
