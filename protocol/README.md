# Wonder Trade v1 contract

This defines the exchange contract. `server/proto/trade.proto` is the internal
gRPC contract; `wire.py` is a small envelope conformance oracle for test clients.
It checks envelope shape; endpoints must also enforce the body, authorization
and state rules here. The game client implements the trade endpoints over
DS-compatible HTTPS, while the server uses gRPC internally. Conservation,
recovery, off-site durability and release acceptance remain separate gates.

The X-menu Wonder Trade path uses DWC access-point setup, authenticated HTTPS
`POST /wt/v1`, the C++ gateway and internal gRPC services. Retail GTS instead
uses `network_handler.c`'s Post/PostFinish and Get/Return/Delete flows, stores
the offered image in `GlobalTrade`, and interleaves the network finish step with
`SAVE_RESULT_PROCEED_FINAL`/`SAVE_RESULT_OK`. Wonder Trade uses the independent
ownership/journal sequence below and leaves those GTS paths and data intact.

## Identity and commitment

The client first persists a random 16-byte enrollment ID and independent
32-byte credential from an approved CSPRNG. Enrollment returns a random
server-issued 16-byte save ID. The server stores a credential verifier and an
idempotent enrollment mapping; the same enrollment ID and credential return the
same identity. A conflicting credential never reveals that identity. Save the
identity in both journal banks before submitting anything. Device/MAC/trainer
IDs, emulator hints, game PRNG output and copied save counters are not secrets.
TLS entropy and seed persistence require the LUNE-300 review; no fallback.

The server stores SHA-256 of ASCII `DSWT save credential v1`, one zero byte,
the 16 save-ID bytes and the 32 credential bytes, and compares verifiers in
constant time. This scheme is for CSPRNG credentials, not human passwords. The
identity module returns local database records; endpoint integration must still
apply the external recovery barrier before publishing a final enrollment reply.

Every mutation has a nonzero 16-byte operation ID and a 64-bit counter, persisted
before sending. Counters start at 1. Under an identity lock, look up the operation
before checking the counter. An identical retry returns its recorded outcome;
changed opcode/counter/body under the same ID conflicts. Hash the canonical
request as SHA-256 of frame bytes 0..39 followed by bytes 72..end, excluding only
the credential. A
recorded acceptance or definitive rejection consumes one counter. Authentication,
format/rate/capacity failures do not. Reject gaps/stale counters and exhaustion;
never wrap. One in-flight mutation and one pending trade per identity serialize
copied-save clients. Reads do not consume counters.

An externally committed outcome requires the payload, ownership changes,
operation result and per-identity event in one durable database transaction.
WAL must be flushed; production also needs an off-site recovery watermark covering
the transition before advertising it as final. Until that barrier is satisfied,
return `PROCESSING` without a body; retain escrow and prevent dependent matching,
cancellation or acknowledgement. A bounded response may precede the durability
barrier, but a final success may not. The backup/recovery implementation must
prove this barrier before launch; an incomplete restore keeps trading paused.

The private C++ backend registers enrollment and lifecycle services. Its default
recovery provider attests archived WAL on the current cluster and timeline; absence fails closed. A bounded in-memory cache ties conservative
WAL requirements to committed per-identity event highwaters, so retry row locks
cannot keep moving the target. Restart services after recovery. The archive
target decides whether attestation is really off-site. The HTTPS gateway routes only
the seven versioned DS operations, with no matcher RPC access or plaintext fallback.

The worker-only matcher has no network endpoint. It serializes each short pair transaction with
a database advisory lock shared across replicas. It anchors the oldest unlocked
eligible offer by its immutable admission event and randomly chooses a distinct
unlocked owner. Busy workers return `PROCESSING`; reuse that operation ID. A
final `NOT_FOUND` records no eligible pair, so later polling needs a new ID.
Pairs and no-pair outcomes survive retry/restart and require recovery coverage.
The one-hour capacity gate must measure contention before production promotion.

Retain final operation outcomes, payloads, receipts and contiguous ownership
events for supported replay, including after acknowledgement. Reconciliation
pages at most eight events after a per-identity cursor; reject a cursor beyond
the durable highwater. Validate a page in RAM, then atomically persist its folded
state changes and final cursor in one mirrored journal record before discarding
it; raw pages need not remain in the save. Unrepresentable effects cannot advance
the cursor. A missing event, contradictory receipt or unknown state stops trading for reconciliation;
it never authorizes deleting escrow, restoring an offered Pokémon or reenrolling.

## DS messages

Use authenticated HTTPS with HTTP/1.1 `POST /wt/v1`,
`Content-Type: application/octet-stream`, one `Content-Length`, and connection
close. No redirects, chunking, compression, conflicting lengths or sensitive
plaintext requests. Bound headers to 2 KiB, bodies to 4 KiB, TLS/connect to 30 s
and an established request to 15 s, with a 5 s no-progress limit. Poll the socket
without blocking frames; cancellation closes it and leaves the durable operation
available for retry. Internal gRPC messages also have a 4 KiB receive/send cap;
field/count bounds below still apply. Raw headers/bodies/credentials are not logs.

All integers are unsigned little-endian; IDs and SHA-256 digests are raw 16/32
bytes. Serialize fields explicitly, never a C struct. Reject unknown versions,
operations, result codes, nonzero reserved fields and inconsistent lengths.
Zero IDs mean absence only where stated. Response opcode and request ID must
match the outstanding request before its result can affect a save.

| Request header offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | ASCII `WTDS` |
| 4 | 1 | Version `1` |
| 5 | 1 | Operation below |
| 6 | 2 | Body length |
| 8 | 16 | Request ID; mutation operation ID or enrollment ID |
| 24 | 16 | Save ID; zero only for enrollment |
| 40 | 32 | Credential; nonzero, including enrollment |
| 72 | 8 | Mutation counter; zero for enrollment/reads |

The response header is `WTDS`, version, opcode, body length, request ID (first
24 bytes), then result u16, reserved-zero u16, current next-counter u64 and durable
event-highwater u64: 44 bytes total. Result numbers are those in `ResultCode`.
Only `OK` carries a body. Unauthenticated replies expose zero counters/highwater.
An HTTP 503 is retryable: keep all escrow/evidence and reuse the exact recorded
operation ID, counter and bytes after backoff of at least one second with jitter.
It does not authorize restoring or discarding a Pokémon.
Errors require HTTP failure handling or this bounded envelope, never HTML parsing
as a successful exchange. Error codes alone do not prove counter consumption:
`ILLEGAL_POKEMON` is a recorded rejection; `CONFLICT` may instead be an unrecorded
operation-ID/body conflict. Before retiring a rejected operation or restoring
escrow, reconcile its durable `REJECTED` event by operation ID and offered digest
(zero digest for operations without a Pokémon body). Counter-mismatch and other
transient/protocol errors leave escrow intact; a Submit's mismatch only starts
the supersession lookup below. Headers describe live counter and
highwater values; a retried mutation's outcome/event remain its original result.

| Op | Request body in order | Successful response body |
| --- | --- | --- |
| 1 Enroll | Build u32, hint length u8, 0..64 hint bytes | Save ID (16) |
| 2 Submit | Escrow generation u64, canonical PK4 (136), digest (32) | Outcome event u64, snapshot (138) |
| 3 Status | Trade ID (16); zero selects current pending trade | Snapshot (138); all zero when none |
| 4 Cancel | Nonzero trade ID (16) | Outcome event u64, snapshot (138) |
| 5 Collect | Nonzero trade ID (16) | Snapshot (138), canonical incoming PK4 (136) |
| 6 Acknowledge | Trade ID, receipt ID, delivered digest, applied digest, journal generation u64 | Outcome event u64, snapshot (138) |
| 7 Reconcile | After-event u64, limit u8 (1..8) | Count u8, has-more u8 (0/1), page-end u64, events |

A snapshot is trade ID (16), state u8, delivery kind u8, offered asset ID (16), offered digest (32),
received asset ID (16), received digest (32), receipt ID (16), last event u64.
Delivery kind is NONE=0 before a receipt, MATCH=1 or RETURN=2 thereafter. It is
immutable with the receipt and survives live state ACKNOWLEDGED. A return's
received asset/digest equal its offered asset/digest; never infer delivery kind
from equal payloads. Absent received fields are zero. An event is sequence u64, kind u8, trade ID,
operation ID, asset ID, payload digest, receipt ID, state u8: 106 bytes. Fields
without an applicable object are zero. Sequences are contiguous, increasing and
at most the highwater; page-end equals the last sequence or the request cursor
for an empty page. `has-more` means additional durable events exist. The largest
page is 902 bytes including its header. Matchmaking has no DS operation.

Canonical PK4 is exactly 136 decrypted, unshuffled bytes: personality u32,
zero sanity u16, checksum u16, then four 32-byte blocks in A/B/C/D order. The
checksum is the sum of 64 little-endian u16 values in bytes 8..135 modulo 65536;
the SHA-256 covers all 136 bytes. Party suffixes, encrypted/shuffled guesses and
automatic repairs are forbidden. Full original local images remain in escrow.
The pinned isolated PKHeX service checks strict Gen IV legality and returns a
verdict, exact payload hash, bounded policy/reason codes and, for legal Pokémon,
the Gen IV games that can hold it, never a replacement payload. RPC
failure is unavailable, not an illegal verdict. The server rejects Eggs and mail;
the game rejects attached capsules and protects its last usable party Pokémon
before escrow, since those facts are outside canonical PK4. The pinned policy,
failure handling and source obligations are in `validator/README.md`.

## Ownership and save ordering

Assets have random server IDs; a payload digest identifies bytes, not a globally
unique Pokémon. Distinct legitimate Pokémon may have identical bytes. Each asset
has one authoritative owner/location. All trade IDs, receipt IDs and assets in a
request must belong to the authenticated identity; guessed foreign IDs produce
no ownership information. Matcher RPCs require a separate internal worker role;
the gateway cannot invoke them. Validator RPCs grant no ownership authority.

| Transition | Authoritative change and required local ordering |
| --- | --- |
| Local selection → escrow | Keep original source unchanged until complete original image, canonical payload, source locator, operation and before/after inventory hashes are verified in both journal banks. |
| Escrow → submit-ready | Remove source, force and verify full saves into both retail backup copies; retain original in both journal banks. Only then send Submit. |
| Submit → QUEUED | Validate first; atomically create owned asset/offer, outcome and event. Definitive rejection is recorded and permits restoring escrow through the same two-copy save sequence. |
| QUEUED → MATCHED | Lock two distinct owners/offers in a consistent order; atomically exchange asset ownership and create both immutable delivery receipts/events. Never expose half a pair. |
| QUEUED → RETURN_READY | Cancellation races under the same locks. A committed match wins; otherwise keep the original asset owned by its depositor and issue a return receipt. |
| Ready → Collect | Read-only replay of the same receipt/payload. No removal, expiry or new assignment, including after an acknowledgement. Full local storage leaves it pending. |
| Collect → locally applied | Verify receipt/digest; persist incoming bytes and destination pre/post images in both journals. Import once into a free slot and apply ordinary game trade effects for a match (none for a return). Verify full saves into both retail copies, then mirror the applied receipt and final digest in both journals before gameplay resumes. |
| Applied → ACKNOWLEDGED | Send the persisted acknowledgement with receipt, delivered/applied digests and journal generation. Record it idempotently; do not delete replay data. Only its final outcome releases the identity for another deposit. |

Never restore a submitted original from a timeout or `NOT_FOUND` alone. Only a
recorded rejection for that exact operation, its return receipt, or proof that
another operation used its counter permits it. That proof is the durable event
immediately after the client's cursor carrying a different operation ID: every
counted operation records one event with its ID and counters never repeat.
A stale response cannot roll back the client's persisted event/counter. A later
receipt must not be mistaken for an earlier applied one. Server code never trusts
the applied digest/generation as proof of untampered offline play; it binds the
acknowledgement to the immutable delivered asset and revalidates future deposits.

## Journal placement and migration

[`save-layout.json`](save-layout.json) records the pinned US layout. Each 0x40000
half contains general data at 0..0xCF2C, boxes through 0x1F110, Hall of Fame at
0x20000, Battle Hall at 0x23000 and four recordings at 0x24000/26000/28000/2A000,
ending at 0x2BD60. Existing GTS data is inside the general block and stays intact.
Reserve a complete 64 KiB flash sector at 0x30000 and 0x70000 for independent
journal banks; the SDK's 256-byte programming pages/64 KiB erase sectors do not
overlap retail content there. Reject smaller flash, unknown layouts, foreign
journal magic and unsupported versions without writing. Europe needs its own
layout validation before it can join this list.

Each bank begins with one explicitly serialized 4096-byte record. Header:
`DSWJ`, version u16=1, phase u16, record-size u32=4096, reserved u32=0,
generation u64, reserved-zero 8 bytes. Remaining section offsets are in the JSON:
identity holds save ID/credential/enrollment ID (16/32/16); progress holds next
counter u64, event cursor u64, last applied receipt (16), last finalized operation
(16), its counter u64 and zero padding (8). Transaction holds operation ID (16),
counter u64, trade/receipt IDs (16 each), source/destination locators (4 each:
kind/box/slot/image-length), immutable delivery kind u8, global/general/box counters (u32 each), inventory
before/after digests and original/incoming/applied digests (32 each), then 19
zero bytes. Images are original/destination-before/destination-after (236 each),
canonical outgoing/incoming (136 each), then 44 zero bytes. Locators use kind
0=absent, 1=party, 2=box; zero-based party slots are 0..5, boxes 0..17 and box
slots 0..29. Images preserve the original encrypted stored game encoding;
box images zero-pad their 136 bytes to 236. The transport-private section has a u16 format and u16
length (at most 508) followed by private bytes and zero padding; its seeded format
uses format 1 / length 32 for the separately provisioned nonvolatile CSPRNG seed
(see `client/tests/README.md`). The pending-request section is
a u16 frame length, six zero bytes, then up to 256 exact request bytes with zero
padding. Persist enrollment/mutation packets before their first send, and keep
them unchanged until their final outcome is proven. Escrow/applied generation
values inside those packets are independent of later journal generations, seed
updates and reconciliation. Clear the section only when retiring that operation.
Reserved sections are zero.
SHA-256 covers bytes 0..4059; the final four bytes are ASCII `CMIT`, written last.

Phases are EMPTY=0, ENROLLING=1, IDLE=2, ESCROW=3, SUBMIT_READY=4, QUEUED=5,
RECEIVED=6, APPLYING=7, APPLIED=8, RESTORING=9 and RECOVERY_REQUIRED=10.
Write an inactive bank, verify its body/hash, commit tag and readback, then mirror
it before any destructive action. Never erase the only valid bank. Choose the
highest valid u64 generation; equal generations with different contents fail
closed, and generations cannot wrap. Repair a single bad bank before proceeding.

Migration leaves retail tables, offsets, footers and GTS unchanged. Recognize a
valid supported retail save first; initialize journals only in verified blank
reserved sectors (uniform 0xFF or 0x00), with no network mutation. Nonblank unknown
sectors require inspection. Recovery runs before ordinary gameplay/saves while a
transition is incomplete. Compare the loaded affected-inventory digest with the
persisted pre/post images and save counters using the retail loader's wrap rules;
complete only the uniquely proven
transition. A mismatch preserves evidence and blocks trading. Two successful
full saves make both retail fallback copies reflect the transition before the
next network boundary; one `SAVE_RESULT_OK` alone is insufficient. Ordinary
play may resume while queued, since both copies already exclude the original.

Layout evidence: pinned upstream [`savedata.c`](https://github.com/pret/pokeplatinum/blob/1dc13dcea40c04ab53f1f607de46023dc409ef20/src/savedata.c),
[`save_table.c`](https://github.com/pret/pokeplatinum/blob/1dc13dcea40c04ab53f1f607de46023dc409ef20/src/savedata/save_table.c),
and locally compiled size functions (136/236-byte Pokémon, extra bodies
0x2AB0/0xBA0/0x1D50 plus 16-byte footers). Cross-check:
[`SAV4Pt`](https://github.com/kwsch/PKHeX/blob/641c26ba100225f3605e5a44b61b3b16e15a3774/PKHeX.Core/Saves/SAV4Pt.cs).
These are format facts; no upstream implementation or save/ROM fixture is copied.
The client journal, inventory adapter and boot/save guards are implemented;
fault injection and end-to-end conservation evidence continue to be tracked
separately. Do not treat emulator SRAM readback alone as proof of host durability.

## Failure and replay cases

| Failure boundary | Required recovery |
| --- | --- |
| Before either escrow bank commits | Source remains live; no submission allowed. |
| Between journal mirrors or during any bank/page/tag write | Preserve the other valid bank; repair it before advancing. Both invalid blocks trading. |
| During either full retail save, before submission | Boot into recovery; recognize pre/post inventory, retain escrow and finish both copies. No network mutation yet. |
| Submit reaches server but reply is lost | Repeat the same persisted operation/counter/body; never restore on timeout. |
| Invalid payload or definitive legality failure | Recorded rejection consumes its counter; restore the exact original once, with journal and both retail copies. |
| Validator/storage/durability service unavailable | Preserve escrow; processing/unavailable cannot authorize restoration or matching. |
| Match/cancel/worker retry races | One atomic outcome under locks and unique operation IDs; cancellation cannot reclaim a matched asset. |
| Receipt arrives, storage full or incoming journal write fails | Keep server receipt/payload and local escrow; no acknowledgement or overwrite. |
| Power loss during import/evolution/save or before applied mirror | Boot recovery compares persisted pre/post inventory; finish or recognize the import once, then mirror both saves/journals. |
| Acknowledgement commits but reply is lost, then server restarts | Replay acknowledgement and reconcile retained receipt/events; never insert a second copy. |
| Retail loader falls back or loads mixed block copies | Follow the journal's proven transition or pause; do not infer completion from one block or a slot alone. |
| Copied client or old post-enrollment save reconnects | Same identity/history; fence counters and reconcile before trading. Ambiguous inventory requires recovery. |
| Copy's Submit loses its counter to another copy | Reconcile; restore its escrow once only if the next durable event is another operation, then stay fenced. Copies sharing a journal seed outside managed sessions may reuse an operation ID: identical Submits share the one offer; differing ones stay blocked. |
| Database restored behind acknowledged events | Pause writes until the off-site durable watermark is recovered; never blindly requeue or reissue. |

Whole-save deletion, pre-enrollment rollback, out-of-band editing/cloning and
loss of every local credential/escrow copy are outside automatic recovery. They
cannot be detected or undone reliably by a DS patch; never promise otherwise.
Each identity has one operation counter, so copies of an enrolled save cannot
both mutate with the same counter. A losing Submit restores its exact escrow
only after reconciliation shows another operation durably used its counter;
the restored copy, like any older copy, then stops at verification and never
collects or cancels offers it did not escrow. Copies still keep whatever Pokémon
they held, and Collect is a recovery read outside the fence, so copies taken
after submission can each import the same delivery. A pre-enrollment backup
re-enrolls as an unrelated identity and can offer its Pokémon again; payloads
are not deduplicated because distinct legitimate Pokémon may share bytes.
Savestates and live save imports are refused only in managed emulator sessions.
Hardware/emulator recovery tests, strict legality, backup rehearsal and capacity
evidence remain release gates, with no timeout-based deletion of pending assets.
