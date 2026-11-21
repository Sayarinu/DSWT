The isolated validator accepts only canonical 136-byte PK4 or PK5 data and its
SHA-256 through `PokemonLegality.Validate`. The request's `format` names the
structure: `FORMAT_PK4` (0, the default, so older callers stay valid) or
`FORMAT_PK5` (1); other values are invalid arguments. It returns a boolean, the
exact payload hash, policy `pkhex-26.08.26-74b88906-dswt5`, up to eight stable reason codes and, when
legal, the destination games that can hold the Pokémon. It never
returns a Pokémon or repairs data. No ROM, save, UI, sprite or package icon is
included in the build context or runtime.

Build and test from the repository root:

```sh
docker build -f validator/Dockerfile -t dswt-validator .
```

Run as a sidecar sharing the C++ service's network namespace, with no published
port. The only listener is `127.0.0.1:50052`, HTTP/2 gRPC. Use container limits of
512 MiB memory, two CPUs and 128 PIDs, a read-only filesystem and no external
ingress. The deployment tickets supply those controls; this change deploys no
service. Analyses run in separate worker processes, each with a 128 MiB
managed-heap limit, a one-second budget per analysis and a 1,024-byte reply bound.
Each worker request is one format byte (0 PK4, 1 PK5) followed by the 136 bytes;
a worker given any other format byte exits without replying.
Starting a worker costs far more than an analysis, so a worker that answered
correctly serves later payloads, one at a time, up to 256; a timeout, crash or
malformed or mismatched reply kills it and it is never reused. Admission allows
two workers and no waiting queue; the C++ caller has a 1.5-second deadline and no retries.
The whole-container memory/PID limits also cover native memory and runtime
threads; a managed-heap setting alone is insufficient.

The policy uses the pinned core's Gen IV or Gen V legality analysis, physical
Gen III transfer rules and invalid verdicts for missing Gen III/IV RNG frames.
Legality never applies a word list, so a Gen IV or Gen V name is never evidence
of an illegal Pokémon. Names are checked separately, because other players see them:
the nickname, when the player gave one, and the original trainer's name must
pass the Nintendo Switch word list, the newest games' online rules. Exact
species names, official Pokémon terms in every Gen IV and Gen V language and the reviewed
`name-allowlist.txt` (innocent names the list's substring patterns catch, such
as Lulu or grape) are exempt.
Eggs, mail, invalid structures/species and forms no game of the format can hold
or trade are rejected, as are Pokémon the pinned core marks untradable (such as
Spiky-eared Pichu, fused Kyurem or battle-only forms). N's Pokémon trade
normally, as the core treats them. The game must reject attached capsules and protect its last usable
party Pokémon before escrow: those facts are absent from canonical PK4.

Analysis assumes no home game. Gen IV and Gen V are separate pools. A legal
PK4 reply's `destinations` sets bit 0 for Diamond/Pearl, bit 1 for Platinum and
bit 2 for HeartGold/SoulSilver; a legal PK5 reply sets bit 3 (8) for Black/White
and bit 4 (16) for Black 2/White 2. A bit is set when that game's pinned PKHeX
tables accept the species/form, held item and ball byte it reads
(`Policy.Destinations` cites each rule), so Black 2/White 2 forms such as the
Therian Formes fit only bit 4. A reply never sets the other format's bits.
Legal Pokémon that fit no game are rejected. An empty set never means every game: the server treats a legal reply
without destinations, such as one from an older validator, as unavailable.

| Reason | Meaning |
| --- | --- |
| `INVALID_STRUCTURE`, `INVALID_SPECIES` | Canonical shape/checksum/reserved fields or species failed |
| `EGG_NOT_ALLOWED`, `MAIL_NOT_ALLOWED`, `INCOMPATIBLE_FORM` | Unsupported trade content; `INCOMPATIBLE_FORM` also covers untradable Pokémon |
| `INCOMPATIBLE_GAME` | Legal, but no game of its format (Diamond/Pearl, Platinum, HeartGold/SoulSilver; Black/White, Black 2/White 2) can hold it |
| `ILLEGAL_ENCOUNTER`, `ILLEGAL_MOVES`, `ILLEGAL_RNG`, `ILLEGAL_DATA` | Pinned core legality checks failed |
| `INAPPROPRIATE_NAME` | The nickname or original trainer's name fails the word list |

RPC failures, timeouts, worker failures, capacity exhaustion and mismatched
policy/hash replies are unavailable, never definitive illegal verdicts. New
submissions call the validator, and so does the backend's name screening of
offers queued before the current name rule: only `INAPPROPRIATE_NAME` refuses
such an offer, which then never matches and goes back to its owner. Already
accepted ownership, collection, cancellation, reconciliation and idempotent
replay must remain available when validation is down.
An illegal verdict alone cannot authorize local restoration: the server must
first durably record the matching rejection under the protocol's recovery rule.

Legality is consistency with known rules, not proof of authentic origin. Clones,
edits that satisfy those rules and undiscovered exploits can pass. False
positives can occur, particularly around events and RNG. Keep the offered
Pokémon in escrow pending a definitive durable outcome. Review a minimal
private reproduction, add a synthetic regression and review the core/resource
pin and policy version together; never auto-legalize, disable checks on failure
or retroactively refuse an accepted asset for legality. Only a name screening
hands an accepted offer back.

`upstream.env` pins the source archive. `core-resources.json` inventories all
embedded core resources; they include encounter/event databases, factual game
tables, names, locations, localized messages and editor labels. No upstream UI
project/artwork/test fixture is selected. Upstream declares GPL-3.0-or-later for
the core; that declaration does not independently establish provenance for
every embedded game-derived resource. Keep image publication gated on the
existing artifact/rights review. The adapter is GPL-3.0-or-later and every
runtime image carries complete corresponding adapter/core source, build files,
schema, license and notices in `/usr/share/dswt-validator/source`, plus
hash-verified gRPC/Protobuf runtime source archives pinned in `runtime-sources.tsv`.
Ship that source with distributed images, preserving third-party notices.
