# Secure connection test

This is a ping-only probe. Trading remains disabled. Physical DS and melonDS
results are required before LUNE-300 is complete; host tests do not satisfy that
gate. Never commit or upload ROMs, saves, test keys or provisioned seeds.

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

## Local test

Use the test machine's IPv4 address reachable from the DS, and a DS-compatible
Wi-Fi access point configured in the game's WFC settings. Keep the original ROM
and save separately. Provision a new save copy for each test installation;
restoring or cloning an older seeded save can repeat random state.

```sh
python3 scripts/tls-test-server.py prepare --directory game/device-test --ipv4 192.0.2.1
WONDER_TRADE_PROFILE=game/device-test/connection.json scripts/build-rom.sh
python3 scripts/identify-rom.py /path/to/original.nds
python3 scripts/provision-save.py /path/to/original.sav game/device-test/test.sav --target us-rev1
python3 scripts/tls-test-server.py serve --directory game/device-test --bind 0.0.0.0
```

Replace the example IP and paths; use the identified US revision (and
`ROM_REVISION=0` for a revision-0 build). The game output is
`game/pokeplatinum/build/pokeplatinum.us.nds`. Copy it and the separately seeded
save under matching filenames for TWiLight Menu or melonDS. The provisioning
tool verifies both retail block types have a valid copy, requires both reserved
64 KiB banks to be blank, and exclusively creates a mode-0600 output. It never
updates an existing seed or transaction record. Region/layout validation is
limited to the verified US targets.

Choose Wonder Trade and a party Pokémon to send a ping. No Pokémon or save
credential is sent. After returning to the menu, Info reports connection time
(including seed I/O), peak TLS bytes, worker stack use and maximum main-loop
frame gap. Errors report a connection code. The TLS arena is capped at 65,536
bytes; the worker stack is 32 KiB, and the application heap is 320 KiB.

Record device/model, TWiLight/melonDS version, ROM target, AP setup, build commit,
each Info value, total failure/cancellation time and whether normal gameplay and
ordinary saves still work. Test both keys (`serve --key next`), reject an unknown
key (`--key untrusted`), reject truncation (`--truncate`), time out a stalled
response (`--stall`), disconnect Wi-Fi, cancel during connect/TLS/read, reconnect
repeatedly, and reboot between successful requests. Compare saved retail ranges
before/after; only the two reserved sectors may change. Retain private save
evidence locally. Do not use this fixture for production.

## Seed durability and host checks

The transport section at offset 1440 has format u16=1, length u16=32 and a
32-byte host CSPRNG seed, with remaining bytes zero. It currently accepts only
EMPTY journals. Mbed TLS's nonvolatile seed mechanism updates the seed before
external RNG use. Each update commits a new journal generation to both isolated
flash sectors, with the hash/body verified and `CMIT` programmed last. A write
failure prevents ClientHello; equal conflicting generations, unknown formats or
missing seeds block the connection. Torn unknown headers require inspection;
this probe never initializes or overwrites them. The trade journal and boot
recovery implementation remain separately ticketed.

```sh
tls_source="$(python3 scripts/prepare-tls.py)"
cmake -S client/tests -B game/tls-tests -DMBEDTLS_SOURCE="$tls_source"
cmake --build game/tls-tests --parallel 2
ctest --test-dir game/tls-tests --output-on-failure
python3 client/tests/test_tls.py game/tls-tests/tls_probe
python3 -m unittest discover -s scripts/tests
```

On a debugger/ptrace host, run ctest with `ASAN_OPTIONS=detect_leaks=0`; address
and undefined-behavior instrumentation still apply to the HTTP parser. The TLS
probe separately checks that all arena allocations are freed. Host conformance
does not emulate flash redirection, ARM7 scheduling or physical Wi-Fi timing.
