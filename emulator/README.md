# Durable Linux emulator

Use a pinned melonDS 1.1 build with checked host persistence. Install the Linux
build dependencies listed in `Dockerfile`, then run `bash scripts/build-emulator.sh`.
The source preparer verifies the archive/revision and preserves local edits.
`docker build -f emulator/Dockerfile -t platinumwt-emulator-builder .` also builds
the integration and runs the synthetic fault suite without ROMs or saves.

Launch a separately provisioned save through `scripts/run-emulator.py`, adding
`--store-tool game/save-store-build/platinumwt-save-store` before `--` and using
the built emulator path. Its configured save filename must match `--save`.
Each save owns its private `.platinumwt` recovery directory and lock; keep these
with the save. Do not mix snapshots or restore an older installation directory.
Savestates and live save imports are refused in managed sessions.

Every emulated flash write publishes two checked snapshots, a monotonic head and
the raw save using file/directory fsync before the DS can continue. I/O failure
stops the emulator. Restart repairs an interrupted publication or truncated raw
save; foreign owners, stale snapshot pairs, unknown formats and ambiguous files
fail closed. This build is optional: the game also trades on stock emulators and
hardware, where a crash before the host saves can roll back behind the server.
It exposes a virtual read-only page; the normal save's address remains untouched. Managed sessions require
ext4, Btrfs or XFS; RAM/network/unknown filesystems are refused. Storage must honor fsync;
these process/syscall fault tests do not physically power-cycle a host.

Rebuild the save-store tool with launcher updates. Its read-only preflight and
native seed refresh share the game's journal codec, retaining active credentials,
escrow and exact retry packets. The game routes unresolved journals through its
recovery screen and engine; unknown or contradictory evidence stays blocked.
Full interruption, reconciliation and end-to-end conservation coverage remain
release gates for actual transfers. Physical DS/TWiLight support remains
deferred. Preserve the
[GPL notice and matching source](NOTICE) with any emulator binary distribution.
