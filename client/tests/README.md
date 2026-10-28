# Wonder Trade client validation

The game client implements enrollment, deposit, collection, cancellation and
history reconciliation over the authenticated HTTPS trade endpoint. The small
`scripts/tls-test-server.py` fixture only serves the legacy ping acknowledgement;
it is not a backend for the game application and must not be used to test a
trade. Actual exchanges require an isolated local C++ backend, PostgreSQL ledger
and pinned legality service. Keep production's recovery provider closed until
off-site recovery is proven. The current release gate is full conservation and
recovery coverage across two independent melonDS saves; physical DS validation
is deferred to LUNE-326. Never commit or upload ROMs, saves, test keys or
provisioned seeds.

The pinned Mbed TLS 4.1.1 / TF-PSA-Crypto 1.2.0 profile uses TLS 1.2,
ECDHE/ECDSA P-256, ChaCha20-Poly1305 and SHA-256. Certificate verification is
required. The expected hostname and either of two SHA-256 SPKI pins must match;
the dedicated self-signed ECDSA peer is its own trust anchor. Public CA roots and
the untrusted DS calendar are not the trust source. Rotate by deploying the
already pinned next key, then distributing a profile containing the subsequent
key. A compromised key must be removed in a new patch. No resumption, early
data, renegotiation, redirects or plaintext fallback are enabled.

The endpoint profile contains only a hostname, fixed IPv4 address, port and
public pins. The fixed address avoids an unbounded SDK DNS call; changing the
service IP needs a new profile/build. No deployment authority is in the profile.

## Two-save emulator trade test

The [local acceptance record](acceptance.md) distinguishes actual ROM scenarios
from host/native tests and lists the remaining release gates.

Start the real local services with Docker Compose and Python 3.9 or newer:

```sh
python3 tools/local-stack/stack.py --workspace game/local-e2e init
python3 tools/local-stack/stack.py --workspace game/local-e2e up
python3 tools/local-stack/stack.py --workspace game/local-e2e status
python3 tools/local-stack/stack.py --workspace game/local-e2e verify
python3 tools/local-stack/stack.py --workspace game/local-e2e down
```

`init` exclusively creates private TLS keys and a workspace manifest. `up`
builds native arm64/amd64 services, migrates PostgreSQL and checks the real
validator policy, matcher and HTTPS trade route. Only `127.0.0.1:8443` is published;
gRPC shares the gateway's private network namespace and PostgreSQL uses a shared
Unix socket. Remote Docker contexts and inherited Compose/database overrides
cannot redirect this stack. The local-only test backend is absent from the final
production image; its same-host durability allowance is not an off-site claim.

Use `game/local-e2e/tls/connection.json` for the Linux melonDS ROM build: its
SLIRP host address is `10.64.0.1`, which reaches the host loopback port. Native
host clients use `localhost:8443` with the same certificate/pins. `down` preserves
database volumes and keys; subsequent `up` refuses missing volumes or a changed
database identity. Keep the workspace and volumes together with enrolled saves;
there is no reset command. `control/matcher-hold` pauses new matches for tests.
The manifest records the source revision, dirty-source flag and actual image IDs.
Ordinary readiness checks create no identities or trades.

Use a local, isolated gateway/backend/validator/database stack and two
independent disposable saves. Pin the local HTTPS gateway in
`WONDER_TRADE_PROFILE`; the ping-only `scripts/tls-test-server.py` fixture is
not a backend for the game application. The default backend recovery provider
remains closed, so local test services need the explicitly test-only recovery
barrier. Never point this build at production. Keep the original ROM and saves
separate; provision a new copy for each client because restoring or cloning an
older seeded save can repeat random state.

Build the strict-legality ROM test backend with
`docker build --target build -t platinumwt-local-tests server`. The executable is
`/build/tests/platinumwt-device-fixture`; it exists only in the test/build stage,
not the final service image. It refuses startup unless
`PLATINUMWT_DISPOSABLE_DEVICE_TEST=1`, `PGDATABASE=platinumwt_e2e` and
`PGHOST=/test/run`. Mount a **new disposable** test directory at `/test`, create a
PostgreSQL cluster with its Unix socket in `/test/run`, create that database and
run `/build/platinumwt-ledger migrate /src/migrations`. Use the cluster owner's
unprivileged UID for these commands. Keep every existing acceptance dataset
separate.

Run the pinned validator on `127.0.0.1:50052` in that same container network
namespace, then start the fixture. It serves enrollment/trading on loopback
50053 and a matcher on loopback 50054; `/test/matcher-hold` pauses new matches
for queue/cancel scenarios. Start `/build/platinumwt-server` with
`WT_TRADE_TARGET=127.0.0.1:50053`, `WT_HTTP_PORT` and both TLS file paths, and
expose only that HTTPS port to the ROM. This fixture's same-host barrier proves
local flow only. Production's backend still requires an independently
recoverable off-site prefix provider.

```sh
python3 scripts/identify-rom.py /path/to/original.nds
python3 scripts/provision-save.py /path/to/original.sav game/device-test/a.sav --target us-rev1
python3 scripts/provision-save.py /path/to/original.sav game/device-test/b.sav --target us-rev1
WONDER_TRADE_PROFILE=/path/to/local-gateway-profile.json scripts/build-rom.sh
```

Use the identified US revision (and `ROM_REVISION=0` for a revision-0 build).
The output is `game/pokeplatinum/build/pokeplatinum.us.nds`. The provisioning
tool verifies both retail block types have a valid copy, requires both reserved
64 KiB banks to be blank and exclusively creates each output. It never updates
an existing seed or transaction record. Region/layout validation is limited to
the verified US targets.

For melonDS on Linux, build the [durable emulator](../../emulator/README.md),
configure each instance to load its own save path and launch through:

```sh
python3 scripts/run-emulator.py --save game/device-test/a.sav --target us-rev1 --store-tool game/save-store-build/platinumwt-save-store -- /path/to/built/melonDS game/pokeplatinum/build/pokeplatinum.us.nds
```

The launcher locks the save, repairs checked host snapshots and commits fresh
entropy before starting the emulator. Its synchronous host store makes subsequent
flash writes durable before the DS continues. The game refuses transport without
that emulator capability. Native launch refresh preserves checked active journals;
unknown evidence enters the recovery gate. Launch both clients separately
through this wrapper, then exercise enrollment, deposit, reconnect, match/collect,
unmatched cancellation, reconciliation and restart behavior. Savestate/rewind
restoration and loading a different save inside a running emulator are
unsupported; exit and relaunch the intended save through this tool.

With the isolated trade services running, opening Wonder Trade connects and
reconciles the save identity/history. Confirming a Pokémon persists its escrow
before sending a deposit. Collection persists the received image and receipt
before acknowledgement. Do not use a production endpoint for these tests. Info
reports connection time (including seed I/O), peak TLS bytes, worker stack use
and maximum main-loop frame gap. Errors report a connection code. The TLS arena
is capped at 65,536 bytes; the worker stack is 32 KiB, and the application heap
is 320 KiB.

Cancellation, expired deadlines and failed exchanges asynchronously disconnect
the owned access point to interrupt the SDK's long TCP/ARP retries. The port
retains heap/overlay ownership until the worker, socket pipes, access point and
driver have drained. Actual cancellation/teardown latency remains a device gate.

Record emulator version, ROM target, build commit, each Info value, failure and
cancellation time, and whether normal gameplay and ordinary saves still work.
Exercise TLS key rotation, malformed responses, server unavailability, Wi-Fi
disconnects, cancellation during connect/TLS/read, repeated reconnects and
restart recovery against the isolated trade stack. The legacy ping fixture's
`--key`, `--truncate` and `--stall` options only test that fixture; they do not
exercise the game trade path. Compare saved retail ranges before/after and retain
private save evidence locally.

## Seed durability and host checks

The transport section at offset 1440 has format u16=1, length u16=32 and a
32-byte host CSPRNG seed, with remaining bytes zero. The ping-only seed API
accepts only EMPTY journals. Trade requests use the checked active-journal seed
path and must match the journal's identity and phase; enrollment and mutation
packets must match the recorded bytes.
Mbed TLS's nonvolatile seed mechanism updates the seed before
external RNG use. Each update commits a new journal generation to both isolated
flash sectors using the SDK flash API: invalidate the old `CMIT`, verify the
replacement body, then write and verify `CMIT` last. A write
failure prevents ClientHello; equal conflicting generations, unknown formats or
missing seeds block the connection. Torn unknown headers require inspection;
this probe never initializes or overwrites them. The boot guard routes unknown or
unsafe active journals through recovery; verified queued/received states can use
the trade engine. Full interruption and inventory/server recovery evidence remain
release gates.

```sh
tls_source="$(python3 scripts/prepare-tls.py)"
cmake -S client/tests -B game/tls-tests -DMBEDTLS_SOURCE="$tls_source"
cmake --build game/tls-tests --parallel 2
ctest --test-dir game/tls-tests --output-on-failure
python3 client/tests/test_tls.py game/tls-tests/tls_probe
python3 -m unittest discover -s scripts/tests
```

On a debugger/ptrace host, run ctest with `ASAN_OPTIONS=detect_leaks=0`; address
and undefined-behavior instrumentation still apply to the HTTP, journal and boot
policy tests. The TLS probe separately checks that all arena allocations are
freed. Host conformance
does not emulate flash redirection, ARM7 scheduling or physical Wi-Fi timing.

With the same local `WONDER_TRADE_PROFILE`, run
`python3 scripts/build-recovery-probe.py` to build a separate ignored ROM that
exercises the actual blocked retail save/erase/clear and asynchronous entrypoints.
It also prepares a party delivery twice after poisoning freed mail allocations,
checking that images/hashes agree and the save is unchanged. Use a disposable
save with at least one free party slot. It restores and verifies the normal ROM
afterward; no probe code is included in that normal output.
