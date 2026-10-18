#ifndef WONDER_TRADE_WIRE_H
#define WONDER_TRADE_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WT_WIRE_REQUEST_MAX 256
#define WT_WIRE_REPLY_MAX 902

enum WtWireOperation {
    WT_OP_ENROLL = 1, WT_OP_SUBMIT, WT_OP_STATUS, WT_OP_CANCEL,
    WT_OP_COLLECT, WT_OP_ACKNOWLEDGE, WT_OP_RECONCILE
};

enum WtWireResult {
    WT_RESULT_OK = 1, WT_RESULT_PROCESSING, WT_RESULT_INVALID_REQUEST,
    WT_RESULT_UNAUTHENTICATED, WT_RESULT_CONFLICT, WT_RESULT_NOT_FOUND,
    WT_RESULT_CAPACITY, WT_RESULT_ILLEGAL_POKEMON, WT_RESULT_COUNTER_MISMATCH,
    WT_RESULT_CURSOR_AHEAD, WT_RESULT_UNAVAILABLE
};

enum WtWireTradeState {
    WT_TRADE_NONE, WT_TRADE_QUEUED, WT_TRADE_MATCHED,
    WT_TRADE_RETURN_READY, WT_TRADE_ACKNOWLEDGED
};

enum WtWireDeliveryKind { WT_DELIVERY_NONE, WT_DELIVERY_MATCH, WT_DELIVERY_RETURN };
enum WtWireEventKind {
    WT_EVENT_ENROLLED = 1, WT_EVENT_SUBMITTED, WT_EVENT_MATCHED,
    WT_EVENT_CANCELLED, WT_EVENT_ACKNOWLEDGED, WT_EVENT_REJECTED
};

/* Values, never packed wire structures. Requests may contain private credentials. */
typedef struct WtWireRequest {
    unsigned char operation, requestID[16], saveID[16], credential[32];
    uint64_t counter;
    union {
        struct { uint32_t build; unsigned char hintLength, hints[64]; } enroll;
        struct { uint64_t generation; unsigned char pokemon[136]; } submit;
        unsigned char trade[16]; /* Status (zero selects pending), Cancel, Collect. */
        struct {
            unsigned char trade[16], receipt[16], deliveredDigest[32], appliedDigest[32];
            uint64_t generation;
        } acknowledge;
        struct { uint64_t after; unsigned char limit; } reconcile;
    } body;
} WtWireRequest;

typedef struct WtWireSnapshot {
    unsigned char trade[16], state, deliveryKind;
    unsigned char offeredAsset[16], offeredDigest[32], receivedAsset[16], receivedDigest[32], receipt[16];
    uint64_t lastEvent;
} WtWireSnapshot;

typedef struct WtWireEvent {
    uint64_t sequence;
    unsigned char kind, trade[16], operation[16], asset[16], digest[32], receipt[16], state;
} WtWireEvent;

typedef struct WtWireReply {
    unsigned char operation;
    uint16_t result;
    uint64_t nextCounter, highwater;
    union {
        unsigned char saveID[16];
        WtWireSnapshot status;
        struct { uint64_t event; WtWireSnapshot snapshot; } mutation;
        struct { WtWireSnapshot snapshot; unsigned char pokemon[136]; } collection;
        struct { unsigned char count, hasMore; uint64_t end; WtWireEvent events[8]; } page;
    } body;
} WtWireReply;

/* SHA-256 callback: zero on success, nonzero on failure. */
typedef int (*WtWireHash)(const unsigned char *, size_t, unsigned char[32]);

/* Failure sets size to zero and leaves output bytes untouched. */
int WtWire_Encode(const WtWireRequest *, WtWireHash, unsigned char *, size_t capacity, size_t *size);
int WtWire_CheckRequest(const unsigned char *, size_t, WtWireHash);
/* Bind to the exact outstanding frame, including persisted retries. Buffers must
 * not overlap reply. Zero means a valid envelope, including non-OK results;
 * it never authorizes a save transition or retires an operation. Failure clears
 * reply. The recovery engine must prove durable history and fold it separately. */
int WtWire_Decode(const unsigned char *request, size_t requestSize,
    const unsigned char *packet, size_t packetSize, WtWireHash, WtWireReply *reply);

#ifdef __cplusplus
}
#endif
#endif
