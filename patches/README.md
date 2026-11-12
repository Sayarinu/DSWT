# ROM compatibility and patch bundle

Players: see [INSTALL.md](INSTALL.md).

[`compatibility.json`](compatibility.json) inventories the verified English retail
records found in No-Intro's Nintendo DS database on the recorded date. Each input
is a decrypted 134,217,728-byte dump; a trimmed dump is accepted only if padding it
back to full size with `0xFF` reproduces a listed SHA-256 exactly. Filenames,
cartridge labels and the shared `CPUE` game code cannot select a patch; use the
complete SHA-256. Encrypted, corrupt, previously patched and unlisted inputs are
rejected. The header game code only explains a refusal: Japanese (`CPUJ`), European
English (`CPUP`), French (`CPUF`), German (`CPUD`), Italian (`CPUI`), Spanish
(`CPUS`) and Korean (`CPUK`) cartridges are recognised but not supported until
each has its own ported and tested target. No BIOS or ROM is bundled.

US Rev 0 and Rev 1 reproduce the pinned upstream SHA-1 and No-Intro SHA-256.
Run `ROM_REVISION=0 ./scripts/build-rom.sh --pristine` and repeat with `1`;
each must pass all five upstream checks. Europe has verified reference digests
but no compatible build yet; LUNE-301 tracks that target. Additional regional
labels sharing an exact listed checksum use the same input. Any new checksum
needs verification. Universal English support requires every intended input to
have its own validated target while preserving that base's assets and behavior.

Check a legally supplied local dump without modifying it:

```sh
python3 scripts/identify-rom.py /path/to/platinum.nds
```

The command returns a planned patch name only for a verified pristine baseline.
A known pending base or an unknown checksum exits unsuccessfully. A successful
check identifies the base; the named patches have not been released yet.

The release contract is one archive containing one
[BPS1](https://github.com/Alcaro/Flips/blob/master/bps_spec.md) patch per distinct
supported input, installation/recovery instructions, licenses and a
`manifest.json`. Each manifest entry records the variant ID, cartridge game code,
revision and language, input size/SHA-256/CRC32, patch filename/size/SHA-256 and
output size/SHA-256/CRC32. The manifest also records the release version, source
commit, pinned upstream revision, connection profile digest and the release checks
that passed. Generate each delta from its own verified pristine base; patches
embedding retail content, another region's included, fail generation, as do
private keys, build-host paths and local or test endpoints.

The patcher selects exactly one manifest entry by SHA-256, verifies the patch
digest, every BPS action and CRC32, and the output SHA-256, then writes a new file
without replacing any existing path. Unmatched inputs, malformed bundles, failed
checks and interrupted writes leave the original untouched and no partial output.
[`tools/bps-patch`](../tools/bps-patch/README.md) implements generation, these
release checks and application.
