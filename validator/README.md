The isolated validator accepts only canonical 136-byte PK4 data and its SHA-256
through `PokemonLegality.Validate`. It returns a boolean, the exact payload hash,
policy `pkhex-26.08.26-74b88906-pwt2` and up to eight stable reason codes. It never
returns a Pokémon or repairs data. No ROM, save, UI, sprite or package icon is
included in the build context or runtime.

Build and test from the repository root:

```sh
docker build -f validator/Dockerfile -t platinumwt-validator .
```

Run as a sidecar sharing the C++ service's network namespace, with no published
port. The only listener is `127.0.0.1:50052`, HTTP/2 gRPC. Use container limits of
512 MiB memory, two CPUs and 128 PIDs, a read-only filesystem and no external
ingress. The deployment tickets supply those controls; this change deploys no
service. Each analysis has a separate process, a one-second lifetime, 128 MiB
managed-heap limit and a 1,024-byte output bound. Admission allows two workers
and no waiting queue; the C++ caller has a 1.5-second deadline and no retries.
The whole-container memory/PID limits also cover native memory and runtime
threads; a managed-heap setting alone is insufficient.

The policy uses the pinned core's Gen IV legality analysis, physical Gen III
transfer rules and invalid verdicts for missing Gen III/IV RNG frames. Gen IV
names use historical-format checks rather than later consoles' word lists.
Eggs, mail, invalid structures/species and forms incompatible with Platinum are
rejected. The game must reject attached capsules and protect its last usable
party Pokémon before escrow: those facts are absent from canonical PK4.

| Reason | Meaning |
| --- | --- |
| `INVALID_STRUCTURE`, `INVALID_SPECIES` | Canonical shape/checksum/reserved fields or species failed |
| `EGG_NOT_ALLOWED`, `MAIL_NOT_ALLOWED`, `INCOMPATIBLE_FORM` | Unsupported trade content |
| `ILLEGAL_ENCOUNTER`, `ILLEGAL_MOVES`, `ILLEGAL_RNG`, `ILLEGAL_DATA` | Pinned core legality checks failed |

RPC failures, timeouts, worker failures, capacity exhaustion and mismatched
policy/hash replies are unavailable, never definitive illegal verdicts. Only
new submission acceptance calls the validator. Already accepted ownership,
collection, cancellation, reconciliation and idempotent replay must remain
available when validation is down; lifecycle integration follows separately.
An illegal verdict alone cannot authorize local restoration: the server must
first durably record the matching rejection under the protocol's recovery rule.

Legality is consistency with known rules, not proof of authentic origin. Clones,
edits that satisfy those rules and undiscovered exploits can pass. False
positives can occur, particularly around events and RNG. Keep the offered
Pokémon in escrow pending a definitive durable outcome. Review a minimal
private reproduction, add a synthetic regression and review the core/resource
pin and policy version together; never auto-legalize, disable checks on failure
or retroactively block delivery of accepted assets.

`upstream.env` pins the source archive. `core-resources.json` inventories all
embedded core resources; they include encounter/event databases, factual game
tables, names, locations, localized messages and editor labels. No upstream UI
project/artwork/test fixture is selected. Upstream declares GPL-3.0-or-later for
the core; that declaration does not independently establish provenance for
every embedded game-derived resource. Keep image publication gated on the
existing artifact/rights review. The adapter is GPL-3.0-or-later and every
runtime image carries complete corresponding adapter/core source, build files,
schema, license and notices in `/usr/share/platinumwt-validator/source`, plus
hash-verified gRPC/Protobuf runtime source archives pinned in `runtime-sources.tsv`.
Ship that source with distributed images, preserving third-party notices.
