# ROM compatibility and patch bundle

[`compatibility.json`](compatibility.json) inventories the three English retail
records found in No-Intro's Nintendo DS database on the recorded date. Each input
is a full-size, decrypted 134,217,728-byte dump. Filenames, cartridge labels and
the shared `CPUE` game code cannot select a patch; use the complete SHA-256.
Encrypted, trimmed, corrupt, previously patched and unlisted inputs are rejected.
Dump format is separate from retail revision; encrypted dumps need a verified
local conversion before they can match a listed input. No BIOS or ROM is bundled.

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

The release contract is one bundle containing one
[BPS1](https://github.com/Alcaro/Flips/blob/master/bps_spec.md) patch per distinct
supported input, installation/recovery instructions, licenses and a
`manifest.json`. Each manifest entry must include the variant ID, input size and
SHA-256, patch filename/size/SHA-256, and output size/SHA-256. The manifest also
records the release version, source commit and pinned upstream revision. A
missing target blocks an all-English release. Generate each delta from its own
verified pristine base; conversion patches embedding another region's original
content are excluded. Review actual patch literals before distribution.

The release patcher must select exactly one manifest entry by input size and
SHA-256, verify the patch digest and all BPS input/patch/output CRC32 checks, and
verify the output SHA-256. Write to a new temporary file beside the requested
output, flush it, then publish it without replacing any existing path. Reject
input/output aliases (including symlinks and hard links), existing outputs,
unmatched inputs, malformed bundles, failed checks and interrupted writes while
preserving the original and removing only the patcher's temporary file. These
application and artifact checks belong to LUNE-291; the private local
implementation is [`tools/bps-patch`](../tools/bps-patch/README.md).
