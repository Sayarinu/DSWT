# Platinum Wonder Trade

This project aims to patch Pokémon Platinum with Wonder Trade support and run a
custom Wonder Trade server.

The [ROM compatibility and patch contract](patches/README.md) records verified
inputs and pending targets. No Wonder Trade patch has been released yet.

## Build the game ROM

Project additions and the integration patch are in [`client`](client). The
upstream game source and build inputs stay in an ignored local workspace.
The build script prepares pinned source automatically; see
[`client/UPSTREAM.md`](client/UPSTREAM.md) for provenance and local-edit handling.

The simplest build uses Docker and does not install the cross-compiler on your
host. Local preparation requires Python 3.12 or newer; set `PYTHON=python3.12`
when needed to select a separately installed interpreter. Docker uses
`linux/amd64` for the ROM toolchain, including on Apple Silicon.
First create a local public endpoint/pins profile using the
[secure connection test guide](client/tests/README.md), then pass its path:

```sh
WONDER_TRADE_PROFILE=game/device-test/connection.json ./scripts/build-rom.sh
```

The output is `game/pokeplatinum/build/pokeplatinum.us.nds`. To select North
American revision 0, run `ROM_REVISION=0 ./scripts/build-rom.sh`. The image
definition is in `tools/rom-builder/Dockerfile`. This project script builds the
modified ROM. To reproduce and check the unmodified US baseline, run
`./scripts/build-rom.sh --pristine` (also accepts `ROM_REVISION=0`). This uses
`game/pokeplatinum-pristine/` and runs all upstream pristine checksum checks.
Both build modes default to one compiler job; set `ROM_BUILD_JOBS=2` (a positive
integer without leading zeros) to opt into parallel compilation. Serial builds
avoid the compiler failures observed with parallel macOS Docker builds.
ROMs and downloaded inputs stay local; CI publishes neither ROMs nor game caches.

## Server ledger

`docker build -f server/Dockerfile -t platinumwt-server .` builds the C++ server and runs its
tests, including a disposable PostgreSQL 16 cluster with synthetic data.
The [private image audit](tools/image-audit/README.md) checks local OCI artifacts,
source/notices, SBOMs and scan findings without publishing them.
Final trading remains gated on off-site recovery. The image also contains an operator tool:

```sh
platinumwt-ledger migrate /usr/local/share/platinumwt/migrations
platinumwt-ledger verify
```

Configure its database with libpq's `PGSERVICE`/`PGPASSFILE` settings or the
`PLATINUMWT_DATABASE_URL` environment variable; keep credentials outside Git.
Run migrations as the database owner. They are transactional, ordered and
checksum-verified. Replay uses a consistent snapshot and temporary row images;
it checks live state without repairing it. Audit evidence includes private
verifiers and payloads and stays in the database/backups; the tool prints counts
and SQLSTATE errors. Local WAL durability does not satisfy the off-site recovery
barrier required before final trade responses.

`platinumwt-backend` serves enrollment/trading on loopback with
`PLATINUMWT_DATABASE_URL` and the pinned `WT_LEGALITY_POLICY`; the validator
defaults to `127.0.0.1:50052`. Operations stay `PROCESSING` until their WAL is
archived, read from `pg_stat_archiver`. That is only sound with
[`server/wal-archive.sh`](server/wal-archive.sh) as `archive_command`: it refuses
gaps, so the archive stays a contiguous prefix on independently recoverable
storage. Nothing ships until its `init` records the chain start, and a base
backup must follow before serving. The backend's database role needs `EXECUTE`
on `pg_switch_wal()`. It forces WAL out only while a request waits on the open
segment, with no backlog and no archive failure. Its database URL must use a
socket path or IP address, because a hostname keeps the barrier closed.
For the HTTPS gateway, set `WT_TRADE_TARGET=127.0.0.1:50053` and provide
`WT_TLS_CERTIFICATE` / `WT_TLS_PRIVATE_KEY` file paths outside Git. Without TLS,
`POST /wt/v1` is refused. Keep every internal service in the same private network
namespace; only HTTPS may be exposed. The gateway bounds connections to 1,024,
with separate four-worker submission and recovery pools. Admission budgets are:

| Resource | Total concurrent/queued | Per key concurrent | Burst / refill per second |
| --- | ---: | ---: | ---: |
| Gateway connections | 1,024 | 64 | 128 / 64 |
| Gateway enrollment/submission jobs | 8 | 8 | 64 / 32 |
| Gateway status/cancel/collect/ack/reconcile jobs | 16 | 8 | 256 / 128 |
| Backend enrollment/submission calls | 4 | 2 | 16 / 4 |
| Backend recovery calls | 8 | 2 | 64 / 16 |

Gateway keys use the direct IPv4 peer or IPv6 /64; forwarded headers do not
override them. Backend identity budgets admit only verified credentials, with a
separate global concurrency lease held throughout each request. Each budget
holds at most 4,096 keys, evicts inactive keys older than 60 seconds under pressure
and rejects new keys when full. Limits are per process; account
for all replicas in database/validator/node capacity and complete the soak gate
before production. These limits are resource bounds, not throughput guarantees.

## Private telemetry

Set `WT_METRICS_PORT` to a nonzero port to enable `GET /metrics` on IPv4 loopback;
zero (the default) disables export. The local stack uses gateway port 9091 and
backend port 9092 inside their shared namespace, publishing only HTTPS 8443.
The game endpoint never serves metrics. Scrapes have 1 KiB headers, no body,
two-second I/O deadlines and at most sixteen concurrent connections.

Prometheus labels are fixed service, operation and result enums. Request,
operation and trade IDs appear only as bounded hexadecimal correlation fields
in JSON stderr logs. Credentials, save IDs, hints, trainer/device information,
Pokémon data and payload hashes are excluded. Dropped log records are counted;
ownership evidence remains in the protected PostgreSQL ledger.

The backend independently samples aggregate queue/delivery counts, ages,
matches, completed pairs and database health every five seconds through a
read-only connection with a one-second client connect deadline, 500 ms client
query deadline and 250 ms server statement timeout. Sampling requires a Unix
socket, numeric host or explicit numeric `hostaddr`; DNS/service discovery
configurations report unavailable to keep observation and shutdown bounded.
Migration 0003 timestamps new audit records without changing ownership images
or replay. Pre-existing records retain unknown times: affected ages are `NaN`
with explicit unknown-age counts. Database outages also produce unknown gauges.
Matched pairs, pairs with both sides acknowledged and acknowledged offers are
separate gauges. RPC latency excludes Wi-Fi/TLS establishment and HTTP queue
wait; use client timings for capacity acceptance.

Restrict collector/dashboard access and apply seven-day log and fifteen-day
metric retention in the storage layer. Operational telemetry cannot authorize
delivery or replace retained ownership/recovery evidence.

## Upstream source

The pinned upstream revision is recorded in [`client/UPSTREAM.md`](client/UPSTREAM.md).
Filtered project history preserves the original authors, dates and messages;
[`licensing/migration.json`](licensing/migration.json) records the source boundary
and commit mapping. The complete original repository remains private.
