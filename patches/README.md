# ROM compatibility and patch bundle

Players: see [INSTALL.md](INSTALL.md).

[`compatibility.json`](compatibility.json) inventories verified English retail
dumps of the Gen IV games: Platinum records from No-Intro's Nintendo DS database
on the recorded date, and Diamond, Pearl, HeartGold and SoulSilver US dumps whose
SHA-1 matches pret's pinned build checksum (`reference`), hashed locally for the
other digests. Each entry records the game, header game code, revision, language,
size and digests. Each input is a decrypted full-size dump (Diamond and Pearl
67,108,864 bytes, the others 134,217,728); a trimmed dump is accepted only if
padding it back to its game's full size with `0xFF` reproduces a listed SHA-256
exactly. Filenames, cartridge labels and the shared game codes cannot select a
patch; use the complete SHA-256. Encrypted, corrupt, previously patched and
unlisted inputs are rejected. The header game code (`ADA`, `APA`, `CPU`, `IPK` or
`IPG` for Diamond, Pearl, Platinum, HeartGold or SoulSilver, then `J`, `E`, `P`,
`F`, `D`, `I`, `S` or `K` for Japanese, US English, European English, French,
German, Italian, Spanish or Korean) only explains a refusal. Every game and
language other than US English Platinum is recognised but not supported until it
has its own ported and tested target. No BIOS or ROM is bundled.

US Rev 0 and Rev 1 reproduce the pinned upstream SHA-1 and No-Intro SHA-256.
Run `ROM_REVISION=0 ./scripts/build-rom.sh --pristine` and repeat with `1`;
each must pass all five upstream checks. HeartGold and SoulSilver US reproduce
their pinned pret SHA-1s with `ROM_GAME=heartgold` or `ROM_GAME=soulsilver` and
`--pristine` (four upstream checks each). Europe has verified reference digests
but no compatible build yet; LUNE-301 tracks that target. Diamond, Pearl,
HeartGold and SoulSilver are verified dumps with no planned patch yet
(`planned_patch` is `null`). Additional regional labels sharing an exact listed
checksum use the same input. Any new checksum needs verification. Universal
English support requires every intended input to have its own validated target
while preserving that base's assets and behavior.

Check a legally supplied local dump without modifying it:

```sh
python3 scripts/identify-rom.py /path/to/rom.nds
```

The command returns a planned patch name only for a verified pristine baseline
with a planned patch. A known pending base or an unknown checksum exits
unsuccessfully and names the game when the header identifies one. A successful
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
