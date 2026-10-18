#include "wonder_trade/wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psa/crypto.h>

static unsigned char frame[256], packet[903], saved[903];
static size_t frameSize, packetSize;

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

static int FailedHash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    (void)bytes; (void)size; (void)digest;
    return -1;
}

static void Put(unsigned char *bytes, uint64_t value, int size)
{
    int i;
    for (i = 0; i < size; i++) { bytes[i] = (unsigned char)value; value >>= 8; }
}

static int Zero(const void *pointer, size_t size)
{
    const unsigned char *bytes = pointer;
    size_t i;
    for (i = 0; i < size; i++) if (bytes[i]) return 0;
    return 1;
}

static void Pokemon(unsigned char *bytes, unsigned species)
{
    memset(bytes, 0, 136);
    Put(bytes, 0x12345678, 4); Put(bytes + 6, species, 2); Put(bytes + 8, species, 2);
}

static WtWireRequest Request(unsigned operation)
{
    WtWireRequest request;
    unsigned char pokemon[136];
    memset(&request, 0, sizeof(request));
    request.operation = (unsigned char)operation;
    memset(request.requestID, 0x11, 16); memset(request.saveID, 0x22, 16); memset(request.credential, 0x33, 32);
    request.counter = operation == 2 || operation == 4 || operation == 6 ? 1 : 0;
    switch (operation) {
    case WT_OP_ENROLL:
        memset(request.saveID, 0, 16); request.body.enroll.build = 0x01020304;
        break;
    case WT_OP_SUBMIT:
        request.body.submit.generation = 7; Pokemon(request.body.submit.pokemon, 25);
        break;
    case WT_OP_CANCEL: case WT_OP_COLLECT:
        memset(request.body.trade, 0x44, 16);
        break;
    case WT_OP_ACKNOWLEDGE:
        memset(request.body.acknowledge.trade, 0x44, 16); memset(request.body.acknowledge.receipt, 0x77, 16);
        Pokemon(pokemon, 2); assert(Hash(pokemon, 136, request.body.acknowledge.deliveredDigest) == 0);
        memset(request.body.acknowledge.appliedDigest, 0x99, 32); request.body.acknowledge.generation = 12;
        break;
    case WT_OP_RECONCILE: request.body.reconcile.limit = 8; break;
    }
    return request;
}

static void Encode(WtWireRequest request)
{
    assert(WtWire_Encode(&request, Hash, frame, sizeof(frame), &frameSize) == 0);
    assert(WtWire_CheckRequest(frame, frameSize, Hash) == 0);
}

static void Snapshot(unsigned char *bytes, unsigned state, unsigned kind, unsigned event)
{
    unsigned char pokemon[136];
    memset(bytes, 0, 138);
    memset(bytes, 0x44, 16); bytes[16] = (unsigned char)state; bytes[17] = (unsigned char)kind;
    memset(bytes + 18, 0x55, 16); Pokemon(pokemon, 25); assert(Hash(pokemon, 136, bytes + 34) == 0);
    if (kind) {
        memset(bytes + 66, 0x66, 16); Pokemon(pokemon, 2); assert(Hash(pokemon, 136, bytes + 82) == 0);
        memset(bytes + 114, 0x77, 16);
        if (kind == WT_DELIVERY_RETURN) memcpy(bytes + 66, bytes + 18, 48);
    }
    Put(bytes + 130, event, 8);
}

static void Event(unsigned char *bytes, unsigned sequence, unsigned kind)
{
    memset(bytes, 0, 106); Put(bytes, sequence, 8); bytes[8] = (unsigned char)kind;
    if (kind == WT_EVENT_ENROLLED) return;
    if (kind == WT_EVENT_REJECTED) { memset(bytes + 25, 0x11, 16); return; }
    memset(bytes + 9, 0x44, 16); memset(bytes + 41, 0x55, 16); memset(bytes + 57, 0x88, 32);
    if (kind != WT_EVENT_MATCHED) memset(bytes + 25, 0x11, 16);
    if (kind != WT_EVENT_SUBMITTED) memset(bytes + 89, 0x77, 16);
    bytes[105] = (unsigned char)(kind - 1);
}

static void Reply(unsigned operation)
{
    unsigned char *body = packet + 44;
    unsigned kinds[8] = { 1, 2, 3, 5, 2, 4, 5, 6 };
    size_t size = 0;
    unsigned i;
    memset(packet, 0, sizeof(packet)); memcpy(packet, "WTDS\1", 5); packet[5] = (unsigned char)operation;
    memcpy(packet + 8, frame + 8, 16); Put(packet + 24, WT_RESULT_OK, 2);
    Put(packet + 28, 2, 8); Put(packet + 36, 3, 8);
    switch (operation) {
    case WT_OP_ENROLL:
        size = 16; memset(body, 0x22, 16); Put(packet + 28, 1, 8); Put(packet + 36, 1, 8);
        break;
    case WT_OP_SUBMIT:
        size = 146; Put(body, 2, 8); Snapshot(body + 8, WT_TRADE_QUEUED, WT_DELIVERY_NONE, 2);
        Put(packet + 36, 2, 8);
        break;
    case WT_OP_STATUS: size = 138; Snapshot(body, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3); break;
    case WT_OP_CANCEL:
        size = 146; Put(body, 3, 8); Snapshot(body + 8, WT_TRADE_RETURN_READY, WT_DELIVERY_RETURN, 3);
        break;
    case WT_OP_COLLECT:
        size = 274; Snapshot(body, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3); Pokemon(body + 138, 2);
        break;
    case WT_OP_ACKNOWLEDGE:
        size = 146; Put(body, 4, 8); Snapshot(body + 8, WT_TRADE_ACKNOWLEDGED, WT_DELIVERY_MATCH, 4);
        Put(packet + 36, 4, 8);
        break;
    case WT_OP_RECONCILE:
        size = 858; body[0] = 8; Put(body + 2, 8, 8); Put(packet + 36, 8, 8);
        for (i = 0; i < 8; i++) Event(body + 10 + 106 * i, i + 1, kinds[i]);
        break;
    }
    Put(packet + 6, size, 2); packetSize = 44 + size;
}

static WtWireReply Accept(void)
{
    WtWireReply reply;
    assert(WtWire_Decode(frame, frameSize, packet, packetSize, Hash, &reply) == 0);
    return reply;
}

static void Reject(void)
{
    WtWireReply reply;
    memset(&reply, 0xa5, sizeof(reply));
    assert(WtWire_Decode(frame, frameSize, packet, packetSize, Hash, &reply) != 0);
    assert(Zero(&reply, sizeof(reply)));
}

static void BadByte(size_t offset, unsigned char value)
{
    unsigned char old = packet[offset]; packet[offset] = value; Reject(); packet[offset] = old;
}

static void TestEnvelopes(void)
{
    unsigned operation, result;
    size_t i, originalSize, length;
    WtWireReply reply;
    WtWireRequest request;
    for (operation = 1; operation <= 7; operation++) {
        request = Request(operation); Encode(request); Reply(operation);
        reply = Accept(); assert(reply.operation == operation && reply.result == WT_RESULT_OK);
        originalSize = frameSize;
        for (i = 0; i < originalSize; i++) {
            assert(WtWire_CheckRequest(frame, i, Hash) != 0);
            memset(saved, 0xa5, sizeof(saved)); length = 999;
            assert(WtWire_Encode(&request, Hash, saved, i, &length) != 0 && length == 0);
            for (length = 0; length < sizeof(saved); length++) assert(saved[length] == 0xa5);
        }
        memcpy(saved, frame, frameSize); saved[frameSize] = 0;
        assert(WtWire_CheckRequest(saved, frameSize + 1, Hash) != 0);
        originalSize = packetSize;
        for (i = 0; i < originalSize; i++) { packetSize = i; Reject(); }
        packetSize = originalSize + 1; Reject(); packetSize = originalSize;
        BadByte(0, 0); BadByte(4, 2); BadByte(5, 0); BadByte(8, 0); BadByte(6, 0);
        BadByte(24, 0); BadByte(24, 12); BadByte(25, 1); BadByte(26, 1); BadByte(27, 1);
        Put(packet + 28, 0, 8); Reject(); Put(packet + 28, 2, 8);
        Put(packet + 36, 0, 8); Reject(); Reply(operation);
        for (result = WT_RESULT_PROCESSING; result <= WT_RESULT_UNAVAILABLE; result++) {
            memset(packet + 44, 0, sizeof(packet) - 44);
            Put(packet + 24, result, 2); Put(packet + 6, 0, 2); packetSize = 44;
            Put(packet + 28, 0, 8); Put(packet + 36, 0, 8);
            reply = Accept(); assert(reply.result == result && Zero(&reply.body, sizeof(reply.body)));
            Put(packet + 28, 7, 8); Reject(); Put(packet + 36, 9, 8);
            if (result == WT_RESULT_PROCESSING || result == WT_RESULT_UNAUTHENTICATED) Reject();
            else assert(Accept().result == result);
            Put(packet + 28, 0, 8); Put(packet + 36, 0, 8);
            Put(packet + 6, 1, 2); packetSize = 45; Reject();
        }
    }
    Encode(Request(WT_OP_STATUS)); Reply(WT_OP_STATUS);
    assert(WtWire_CheckRequest(NULL, 96, Hash) != 0);
    assert(WtWire_CheckRequest(frame, 257, Hash) != 0);
    assert(WtWire_CheckRequest(frame, frameSize, NULL) != 0);
    assert(WtWire_Decode(frame, frameSize, NULL, packetSize, Hash, &reply) != 0 && Zero(&reply, sizeof(reply)));
    assert(WtWire_Decode(NULL, frameSize, packet, packetSize, Hash, &reply) != 0 && Zero(&reply, sizeof(reply)));
    assert(WtWire_Decode(frame, frameSize, packet, 903, Hash, &reply) != 0 && Zero(&reply, sizeof(reply)));
    assert(WtWire_Decode(frame, frameSize, packet, packetSize, Hash, NULL) != 0);
    request = Request(WT_OP_STATUS);
    assert(WtWire_Encode(NULL, Hash, frame, 256, &length) != 0 && length == 0);
    assert(WtWire_Encode(&request, Hash, NULL, 256, &length) != 0 && length == 0);
    assert(WtWire_Encode(&request, Hash, frame, 256, NULL) != 0);
}

static void TestRequestFields(void)
{
    WtWireRequest request;
    size_t size;
    unsigned operation;
    int i;
    for (operation = 1; operation <= 7; operation++) {
        request = Request(operation);
        memset(request.requestID, 0, 16); assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
        request = Request(operation);
        memset(request.credential, 0, 32); assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
        request = Request(operation);
        memset(request.saveID, operation == WT_OP_ENROLL ? 1 : 0, 16);
        assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
        request = Request(operation); request.counter = UINT64_MAX;
        assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
        request.counter = operation == 2 || operation == 4 || operation == 6 ? 0 : 1;
        assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    }
    request = Request(WT_OP_ENROLL); request.body.enroll.hintLength = 64; memset(request.body.enroll.hints, 0xa5, 64);
    Encode(request); assert(frameSize == 149 && frame[84] == 64 && frame[148] == 0xa5);
    frame[84] = 63; assert(WtWire_CheckRequest(frame, frameSize, Hash) != 0);
    request.body.enroll.hintLength = 65; assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request.operation = 0; assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request.operation = 8; assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request = Request(WT_OP_SUBMIT); Encode(request);
    assert(WtWire_CheckRequest(frame, frameSize, FailedHash) != 0);
    assert(WtWire_Encode(&request, FailedHash, frame, 256, &size) != 0);
    request.body.submit.generation = 0; assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    for (i = 4; i <= 6; i++) {
        request = Request(WT_OP_SUBMIT); request.body.submit.pokemon[i] ^= 1;
        assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    }
    request = Request(WT_OP_SUBMIT); request.body.submit.pokemon[0x64] = 1;
    Put(request.body.submit.pokemon + 6, 26, 2);
    assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    Encode(Request(WT_OP_SUBMIT)); frame[224] ^= 1; assert(WtWire_CheckRequest(frame, frameSize, Hash) != 0);
    request = Request(WT_OP_CANCEL); memset(request.body.trade, 0, 16);
    assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request = Request(WT_OP_COLLECT); memset(request.body.trade, 0, 16);
    assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request = Request(WT_OP_ACKNOWLEDGE); memset(request.body.acknowledge.receipt, 0, 16);
    assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request = Request(WT_OP_ACKNOWLEDGE); request.body.acknowledge.generation = 0;
    assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request = Request(WT_OP_RECONCILE); request.body.reconcile.limit = 0;
    assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
    request.body.reconcile.limit = 9; assert(WtWire_Encode(&request, Hash, frame, 256, &size) != 0);
}

static void TestTradeReplies(void)
{
    unsigned operation;
    WtWireReply reply;
    WtWireRequest request;
    const unsigned snapshotOffsets[] = { 16, 17, 130 };
    size_t i;
    for (operation = WT_OP_SUBMIT; operation <= WT_OP_ACKNOWLEDGE; operation += 2) {
        Encode(Request(operation)); Reply(operation);
        Put(packet + 28, 1, 8); Reject();
        Put(packet + 28, UINT64_MAX, 8); Put(packet + 36, 100, 8);
        assert(Accept().nextCounter == UINT64_MAX); /* Replay keeps its original outcome. */
        BadByte(44, 0);
        for (i = 0; i < 3; i++) BadByte(52 + snapshotOffsets[i], 255);
        if (operation == WT_OP_SUBMIT) BadByte(52 + 34, 0);
        else BadByte(52, 0);
        if (operation == WT_OP_ACKNOWLEDGE) { BadByte(52 + 114, 0); BadByte(52 + 82, 0); }
    }
    Encode(Request(WT_OP_STATUS)); Reply(WT_OP_STATUS);
    BadByte(44 + 16, 5); BadByte(44 + 17, 3);
    memcpy(saved, packet, packetSize);
    memset(packet + 44, 0, 16); Reject(); memcpy(packet, saved, packetSize);
    memset(packet + 44 + 18, 0, 16); Reject(); memcpy(packet, saved, packetSize);
    memset(packet + 44 + 66, 0, 16); Reject(); memcpy(packet, saved, packetSize);
    memset(packet + 44 + 114, 0, 16); Reject(); memcpy(packet, saved, packetSize);
    memcpy(packet + 44 + 66, packet + 44 + 18, 16); Reject(); memcpy(packet, saved, packetSize);
    Snapshot(packet + 44, WT_TRADE_RETURN_READY, WT_DELIVERY_RETURN, 3);
    assert(Accept().body.status.deliveryKind == WT_DELIVERY_RETURN);
    BadByte(44 + 66, 0); BadByte(44 + 82, 0);
    packet[44 + 16] = WT_TRADE_MATCHED; Reject(); packet[44 + 16] = WT_TRADE_ACKNOWLEDGED; Accept();
    memset(packet + 44, 0, 138); assert(Accept().body.status.state == WT_TRADE_NONE);
    request = Request(WT_OP_STATUS); memset(request.body.trade, 0x44, 16); Encode(request); Reject();
    Encode(Request(WT_OP_COLLECT)); Reply(WT_OP_COLLECT);
    reply = Accept(); assert(reply.body.collection.pokemon[8] == 2);
    assert(WtWire_Decode(frame, frameSize, packet, packetSize, FailedHash, &reply) != 0 && Zero(&reply, sizeof(reply)));
    BadByte(44 + 138 + 8, 3); BadByte(44 + 82, 0); BadByte(44, 0);
    packet[44 + 138 + 4] = 1; assert(Hash(packet + 44 + 138, 136, packet + 44 + 82) == 0); Reject();
    Reply(WT_OP_COLLECT); packet[44 + 138 + 6] ^= 1;
    assert(Hash(packet + 44 + 138, 136, packet + 44 + 82) == 0); Reject();
    Reply(WT_OP_COLLECT); Snapshot(packet + 44, WT_TRADE_QUEUED, WT_DELIVERY_NONE, 2); Reject();
    Reply(WT_OP_COLLECT); packet[44 + 16] = WT_TRADE_ACKNOWLEDGED; Accept();
}

static void TestPages(void)
{
    WtWireRequest request = Request(WT_OP_RECONCILE);
    WtWireReply reply;
    unsigned i;
    Encode(request); Reply(WT_OP_RECONCILE);
    reply = Accept(); assert(reply.body.page.count == 8 && reply.body.page.events[7].kind == WT_EVENT_REJECTED);
    BadByte(44, 9); BadByte(45, 2); BadByte(46, 7);
    BadByte(54, 2); BadByte(54 + 106, 1); BadByte(54 + 8, 7);
    BadByte(54 + 9, 1); BadByte(54 + 105, 1);
    for (i = 1; i < 8; i++) {
        unsigned char *event = packet + 54 + 106 * i;
        memcpy(saved, packet, packetSize);
        event[105] = 255; Reject(); memcpy(packet, saved, packetSize);
    }
    Reply(WT_OP_RECONCILE);
    memset(packet + 54 + 106 + 25, 0, 16); Reject();
    Reply(WT_OP_RECONCILE); packet[54 + 2 * 106 + 25] = 1; Reject();
    Reply(WT_OP_RECONCILE); packet[54 + 106 + 89] = 1; Reject();
    Reply(WT_OP_RECONCILE); memset(packet + 54 + 7 * 106 + 25, 0, 16); Reject();
    Reply(WT_OP_RECONCILE); packet[54 + 7 * 106 + 41] = 1; Reject();
    Reply(WT_OP_RECONCILE); packet[54 + 7 * 106 + 89] = 1; Reject();
    request.body.reconcile.limit = 2; Encode(request); Reply(WT_OP_RECONCILE); Reject();
    packet[44] = 2; packet[45] = 1; Put(packet + 46, 2, 8); Put(packet + 6, 222, 2); packetSize = 266;
    reply = Accept(); assert(reply.body.page.hasMore == 1 && reply.body.page.end == 2);
    packet[44] = 1; Put(packet + 46, 1, 8); Put(packet + 6, 116, 2); packetSize = 160; Reject();
    request.body.reconcile.after = UINT64_MAX; Encode(request); Reply(WT_OP_RECONCILE);
    packet[44] = 0; packet[45] = 0; Put(packet + 46, UINT64_MAX, 8);
    Put(packet + 6, 10, 2); packetSize = 54; Put(packet + 36, UINT64_MAX, 8);
    assert(Accept().body.page.end == UINT64_MAX);
    packet[44] = 1; Put(packet + 46, 0, 8); Put(packet + 6, 116, 2); packetSize = 160; Reject();
    packet[44] = 0; Put(packet + 46, UINT64_MAX, 8); Put(packet + 6, 10, 2); packetSize = 54;
    Put(packet + 36, 7, 8); Reject();
}

/* Binary test bridge. Only synthetic test requests are emitted; decoded replies
 * produce one result-code byte (zero for malformed), never credential logs. */
static int Bridge(const char *mode)
{
    unsigned char header[4];
    unsigned operation;
    size_t count, requestLength, responseLength;
    WtWireReply reply;
    if (strcmp(mode, "requests") == 0) {
        for (operation = 1; operation <= 7; operation++) {
            Encode(Request(operation)); Put(header, frameSize, 2);
            if (fwrite(header, 1, 2, stdout) != 2 || fwrite(frame, 1, frameSize, stdout) != frameSize) return 2;
        }
        return 0;
    }
    if (strcmp(mode, "decode") != 0) return 2;
    while ((count = fread(header, 1, 4, stdin)) != 0) {
        if (count != 4) return 2;
        requestLength = header[0] | ((size_t)header[1] << 8); responseLength = header[2] | ((size_t)header[3] << 8);
        if (requestLength > 256 || responseLength > 902
            || fread(frame, 1, requestLength, stdin) != requestLength
            || fread(packet, 1, responseLength, stdin) != responseLength) return 2;
        if (fputc(WtWire_Decode(frame, requestLength, packet, responseLength, Hash, &reply) == 0 ? reply.result : 0, stdout) == EOF) return 2;
    }
    return ferror(stdin) ? 2 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2) return Bridge(argv[1]);
    if (argc != 1) return 2;
    TestEnvelopes(); TestRequestFields(); TestTradeReplies(); TestPages();
    puts("Bounded trade envelopes, request bindings, digests, snapshots and recovery pages passed");
    return 0;
}
