#ifndef WONDER_TRADE_WIRE_LAYOUT_H
#define WONDER_TRADE_WIRE_LAYOUT_H

/* Byte offsets and sizes of the v1 DS envelope described in protocol/README.md.
 * server/src/wire_layout.h mirrors these values for the gateway, and
 * scripts/tests/test_wire_layout.py checks both against protocol/wire.py.
 * Offsets are relative to the start of the named header, body or record. */

#define WT_WIRE_MAGIC "WTDS\1" /* ASCII magic followed by version 1 */
#define WT_WIRE_MAGIC_SIZE 5
#define WT_WIRE_ID_SIZE 16
#define WT_WIRE_DIGEST_SIZE 32
#define WT_WIRE_CREDENTIAL_SIZE 32
#define WT_WIRE_PK4_SIZE 136

/* Request header */
#define WT_WIRE_REQUEST_OPERATION 5
#define WT_WIRE_REQUEST_BODY_LENGTH 6
#define WT_WIRE_REQUEST_ID 8
#define WT_WIRE_REQUEST_SAVE_ID 24
#define WT_WIRE_REQUEST_CREDENTIAL 40
#define WT_WIRE_REQUEST_COUNTER 72
#define WT_WIRE_REQUEST_HEADER_SIZE 80

/* Response header */
#define WT_WIRE_REPLY_OPERATION 5
#define WT_WIRE_REPLY_BODY_LENGTH 6
#define WT_WIRE_REPLY_REQUEST_ID 8
#define WT_WIRE_REPLY_RESULT 24
#define WT_WIRE_REPLY_RESERVED 26
#define WT_WIRE_REPLY_NEXT_COUNTER 28
#define WT_WIRE_REPLY_HIGHWATER 36
#define WT_WIRE_REPLY_HEADER_SIZE 44

/* Enroll request body: build u32, hint length u8, hints */
#define WT_WIRE_ENROLL_BUILD 0
#define WT_WIRE_ENROLL_HINT_LENGTH 4
#define WT_WIRE_ENROLL_HINTS 5
#define WT_WIRE_ENROLL_HINTS_MAX 64

/* Submit request body */
#define WT_WIRE_SUBMIT_GENERATION 0
#define WT_WIRE_SUBMIT_POKEMON 8
#define WT_WIRE_SUBMIT_DIGEST 144
#define WT_WIRE_SUBMIT_SIZE 176

/* Status, Cancel and Collect request body: trade ID */
#define WT_WIRE_TRADE_REQUEST_SIZE 16

/* Acknowledge request body */
#define WT_WIRE_ACK_TRADE 0
#define WT_WIRE_ACK_RECEIPT 16
#define WT_WIRE_ACK_DELIVERED_DIGEST 32
#define WT_WIRE_ACK_APPLIED_DIGEST 64
#define WT_WIRE_ACK_GENERATION 96
#define WT_WIRE_ACK_SIZE 104

/* Reconcile request body */
#define WT_WIRE_RECONCILE_AFTER 0
#define WT_WIRE_RECONCILE_LIMIT 8
#define WT_WIRE_RECONCILE_SIZE 9
#define WT_WIRE_RECONCILE_LIMIT_MAX 8

/* Trade snapshot record */
#define WT_WIRE_SNAPSHOT_TRADE 0
#define WT_WIRE_SNAPSHOT_STATE 16
#define WT_WIRE_SNAPSHOT_DELIVERY_KIND 17
#define WT_WIRE_SNAPSHOT_OFFERED_ASSET 18
#define WT_WIRE_SNAPSHOT_OFFERED_DIGEST 34
#define WT_WIRE_SNAPSHOT_RECEIVED_ASSET 66
#define WT_WIRE_SNAPSHOT_RECEIVED_DIGEST 82
#define WT_WIRE_SNAPSHOT_RECEIPT 114
#define WT_WIRE_SNAPSHOT_LAST_EVENT 130
#define WT_WIRE_SNAPSHOT_SIZE 138

/* Submit, Cancel and Acknowledge response body: outcome event u64, snapshot */
#define WT_WIRE_OUTCOME_EVENT 0
#define WT_WIRE_OUTCOME_SNAPSHOT 8
#define WT_WIRE_OUTCOME_SIZE 146

/* Collect response body: snapshot, incoming PK4 */
#define WT_WIRE_COLLECTION_SNAPSHOT 0
#define WT_WIRE_COLLECTION_POKEMON 138
#define WT_WIRE_COLLECTION_SIZE 274

/* Reconcile response body: count u8, has-more u8, page-end u64, events */
#define WT_WIRE_PAGE_COUNT 0
#define WT_WIRE_PAGE_HAS_MORE 1
#define WT_WIRE_PAGE_END 2
#define WT_WIRE_PAGE_EVENTS 10

/* Event record */
#define WT_WIRE_EVENT_SEQUENCE 0
#define WT_WIRE_EVENT_KIND 8
#define WT_WIRE_EVENT_TRADE 9
#define WT_WIRE_EVENT_OPERATION 25
#define WT_WIRE_EVENT_ASSET 41
#define WT_WIRE_EVENT_DIGEST 57
#define WT_WIRE_EVENT_RECEIPT 89
#define WT_WIRE_EVENT_STATE 105
#define WT_WIRE_EVENT_SIZE 106

/* Canonical PK4: zero sanity u16, checksum u16 over the blocks, unused bytes in block C */
#define WT_WIRE_PK4_SANITY 4
#define WT_WIRE_PK4_CHECKSUM 6
#define WT_WIRE_PK4_BLOCKS 8
#define WT_WIRE_PK4_UNUSED 0x64
#define WT_WIRE_PK4_UNUSED_SIZE 4

#endif
