# Local ROM trading acceptance

LUNE-311 / LUNE-312, 2026-10-26. This is local emulator evidence, not a hosted
or physical-DS release approval. The ticketed trading branches are contained in
`main` at `c8693a938d416fab8aa4fa229290f48ebaeaf3dd`; the LUNE-311 follow-up
adds the LUNE-399 receive-replay fix, tests, local fixtures and build fixes.

The pinned US Rev 1 workspace was prepared afresh with all 41 current
integration/overlay files verified. Initial modified ROM SHA-256:
`c90d414c1d23576cec4598e3d6d657c91da38ef1f3e6da40c183d1fb5fc47a6a`.
The item-evolution run exposed LUNE-399: unused mail sentence IDs inherited
heap contents, making party receive replay fail before any retail write or ACK.
The fixed normal ROM clears these IDs for new deliveries and preserves the
committed IDs of existing prepared images. Final modified ROM SHA-256:
`02e9b66b29d85095700dd6d23dec9d64b91bd59d3274916e9275025bd508eab1`.
A separate SDK probe fails against the old code and passes with the fix after
poisoning freed mail allocations with two patterns. Neither probe changes the
retail save; the normal ROM is restored after each probe build.

Durable melonDS 1.1 SHA-256:
`ba0ff2ff4eda980121b3fe8c18688a4b83b1216dd4bf0fd357175a0dd266e0bd`.
The C++ gateway/backend/matcher were rebuilt from this checkout. The pinned
pwt2 validator image's included adapter source was compared with this checkout.
A fresh PostgreSQL 16 cluster and separate local TLS files were used; original
ROM/save assets and the previous acceptance database were kept separate.

| ROM scenario | Evidence |
| --- | --- |
| Independent enrollment and reciprocal match | Distinct journals/credentials, owned receipts and retained ledger outcomes |
| Termination at ESCROW | Restart removed the offer from both retail copies before resuming the exact Submit |
| Lost committed Submit response | Connection error retained pending request; retry recovered the same queued offer |
| Server unavailable during cancellation | Connection error preserved the queued offer |
| Match wins against pending Cancel | Durable Cancel held before transmission; match committed first; retry recorded conflict and collected the match |
| Full party and PC | Six party/540 PC slots unchanged, incoming payload retained, no ACK; preserved through termination/restart |
| Only final PC slot available | Delivery to box 18/slot 30, all other slots unchanged, held item retained |
| Trade evolution | Graveler became Golem; Porygon holding Up-Grade became Porygon2 with the item consumed; strict legality passed |
| Termination at APPLIED and lost committed ACK response | Retry returned to IDLE with every retail body byte unchanged; effects occurred once |
| Corrupted TLS response and database outage | Pending Submit bytes and retail inventory retained; database/backend restart and retries produced exactly the four intended submissions |
| Concurrent captured ACK replays | Two simultaneous callers, six ACK replays per client; same outcomes/counters and original Collect payloads |
| Legacy APPLYING image | LUNE-399 fix recovered Porygon2 without editing the save; full prepared image/inventory hash preserved |
| Ordinary gameplay and retail Save | Returned to the field, saved normally and relaunched; inventory, trade effects, receipt and counter preserved |
| Fresh exchange after LUNE-399 | Both clients used the final fixed ROM; new mail IDs zero, exact reciprocal inventories, stable identity/history and owned ACKs |

The run completed three reciprocal matches and six acknowledged offers across
three participating save identities. Every saved result was bound to its owned
receipt and matching canonical/applied hashes; ledger replay passed. The final
fresh pair completed on the fixed normal ROM at mutation counters 8 and 5.
Earlier rows record the initial-code scenarios; the item-evolution recovery and
fresh pair verify the fix rather than relabeling earlier runs as post-fix tests.

The full-storage setup was constructed offline in disposable copies of a queued
save, then of a received/pending save. Freeing the final PC slot was another
explicit fixture edit. These are capacity/recovery tests, not evidence of a
natural capture/PC interaction or protection against arbitrary offline editing.
All subsequent collection, save and ACK steps ran in the unmodified test ROM.

Native checks passed: 31 server CTests plus the new test-fixture startup refusal;
10 client CTests in Debug and Release with assertions enabled; 60 tooling tests;
7 TLS conformance tests; both host persistence suites. Native tests cover more
message, authorization, cancellation and persistence permutations than this ROM
run. They must not be described as additional actual SDK interruption cases.

Private screenshots, journals, save snapshots, source hashes, request captures
and verifier reports remain in ignored `game/LUNE-311-current/`. ROM/save/key or
Pokémon bytes must not be attached to the PR. Durable save steps take minutes on
this host; this run measures correctness, not release latency or capacity.

Remaining gates include the exhaustive SDK interruption/copied-save and AP
failure matrix, physical host power interruption, physical DS support
(LUNE-326), hosted off-site recovery/deployment and exact patch-release
rehearsal. Earlier same-ROM evidence for unmatched cancellation and selection
rejections remains separate from this new run. Keep the parent acceptance
issues open until their remaining criteria are met.
