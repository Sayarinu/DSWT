#pragma once

#include <cstddef>
#include <string_view>

// Byte offsets and sizes of the v1 DS envelope described in
// https://github.com/Sayarinu/DSWT/wiki/Protocol.
// Mirrors client/include/wonder_trade/wire_layout.h name for name (WT_WIRE_SNAPSHOT_STATE
// is kSnapshotState); scripts/tests/test_wire_layout.py checks both against
// protocol/wire.py. Offsets are relative to the start of the named header, body or record.
namespace dswt::server::wire {

inline constexpr std::string_view kMagic{"WTDS\1", 5}; // ASCII magic followed by version 1

// Operations
inline constexpr unsigned char kEnroll = 1, kSubmit = 2, kStatus = 3, kCancel = 4, kCollect = 5, kAcknowledge = 6, kReconcile = 7;

// Server-only bound on a DS frame in either direction (protocol/wire.py MAX_PACKET)
inline constexpr std::size_t kMaxPacket = 4096;

// Field sizes
inline constexpr std::size_t kIdSize = 16;
inline constexpr std::size_t kDigestSize = 32;
inline constexpr std::size_t kCredentialSize = 32;
inline constexpr std::size_t kPk4Size = 136;

// Request header
inline constexpr std::size_t kRequestOperation = 5;
inline constexpr std::size_t kRequestBodyLength = 6;
inline constexpr std::size_t kRequestId = 8;
inline constexpr std::size_t kRequestSaveId = 24;
inline constexpr std::size_t kRequestCredential = 40;
inline constexpr std::size_t kRequestCounter = 72;
inline constexpr std::size_t kRequestHeaderSize = 80;

// Response header
inline constexpr std::size_t kReplyOperation = 5;
inline constexpr std::size_t kReplyBodyLength = 6;
inline constexpr std::size_t kReplyRequestId = 8;
inline constexpr std::size_t kReplyResult = 24;
inline constexpr std::size_t kReplyReserved = 26;
inline constexpr std::size_t kReplyNextCounter = 28;
inline constexpr std::size_t kReplyHighwater = 36;
inline constexpr std::size_t kReplyHeaderSize = 44;

// Enroll request body: build u32, hint length u8, hints
inline constexpr std::size_t kEnrollBuild = 0;
inline constexpr std::size_t kEnrollHintLength = 4;
inline constexpr std::size_t kEnrollHints = 5;
inline constexpr std::size_t kEnrollHintsMax = 64;

// Submit request body
inline constexpr std::size_t kSubmitGeneration = 0;
inline constexpr std::size_t kSubmitPokemon = 8;
inline constexpr std::size_t kSubmitDigest = 144;
inline constexpr std::size_t kSubmitSize = 176;

// Status, Cancel and Collect request body: trade ID
inline constexpr std::size_t kTradeRequestSize = 16;

// Acknowledge request body
inline constexpr std::size_t kAckTrade = 0;
inline constexpr std::size_t kAckReceipt = 16;
inline constexpr std::size_t kAckDeliveredDigest = 32;
inline constexpr std::size_t kAckAppliedDigest = 64;
inline constexpr std::size_t kAckGeneration = 96;
inline constexpr std::size_t kAckSize = 104;

// Reconcile request body
inline constexpr std::size_t kReconcileAfter = 0;
inline constexpr std::size_t kReconcileLimit = 8;
inline constexpr std::size_t kReconcileSize = 9;
inline constexpr std::size_t kReconcileLimitMax = 8;

// Trade snapshot record
inline constexpr std::size_t kSnapshotTrade = 0;
inline constexpr std::size_t kSnapshotState = 16;
inline constexpr std::size_t kSnapshotDeliveryKind = 17;
inline constexpr std::size_t kSnapshotOfferedAsset = 18;
inline constexpr std::size_t kSnapshotOfferedDigest = 34;
inline constexpr std::size_t kSnapshotReceivedAsset = 66;
inline constexpr std::size_t kSnapshotReceivedDigest = 82;
inline constexpr std::size_t kSnapshotReceipt = 114;
inline constexpr std::size_t kSnapshotLastEvent = 130;
inline constexpr std::size_t kSnapshotSize = 138;

// Submit, Cancel and Acknowledge response body: outcome event u64, snapshot
inline constexpr std::size_t kOutcomeEvent = 0;
inline constexpr std::size_t kOutcomeSnapshot = 8;
inline constexpr std::size_t kOutcomeSize = 146;

// Collect response body: snapshot, incoming PK4
inline constexpr std::size_t kCollectionSnapshot = 0;
inline constexpr std::size_t kCollectionPokemon = 138;
inline constexpr std::size_t kCollectionSize = 274;

// Reconcile response body: count u8, has-more u8, page-end u64, events
inline constexpr std::size_t kPageCount = 0;
inline constexpr std::size_t kPageHasMore = 1;
inline constexpr std::size_t kPageEnd = 2;
inline constexpr std::size_t kPageEvents = 10;

// Event record
inline constexpr std::size_t kEventSequence = 0;
inline constexpr std::size_t kEventKind = 8;
inline constexpr std::size_t kEventTrade = 9;
inline constexpr std::size_t kEventOperation = 25;
inline constexpr std::size_t kEventAsset = 41;
inline constexpr std::size_t kEventDigest = 57;
inline constexpr std::size_t kEventReceipt = 89;
inline constexpr std::size_t kEventState = 105;
inline constexpr std::size_t kEventSize = 106;

// Canonical PK4: zero sanity u16, checksum u16 over the blocks, unused bytes in block C
inline constexpr std::size_t kPk4Sanity = 4;
inline constexpr std::size_t kPk4Checksum = 6;
inline constexpr std::size_t kPk4Blocks = 8;
inline constexpr std::size_t kPk4Unused = 0x64;
inline constexpr std::size_t kPk4UnusedSize = 4;

} // namespace dswt::server::wire
