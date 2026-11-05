# Install Wonder Trade

Wonder Trade is a patch for your own copy of Pokémon Platinum. It trades a party
Pokémon with a random player through this project's server. No ROM is included.

## What you need

- A full, decrypted dump of US English Pokémon Platinum, Rev 0 or Rev 1. It must
  match the size and SHA-256 listed for your revision in `manifest.json`.
- melonDS 1.1, which is the tested emulator.
- The patch for your revision from this bundle, and a BPS patcher such as
  [Flips](https://github.com/Alcaro/Flips).

Europe and physical DS hardware are not supported yet.

## Patch your game

1. Copy your ROM and keep the original somewhere safe.
2. Back up your save file before your first Wonder Trade.
3. Check that your ROM's SHA-256 matches `manifest.json`. A different value means
   an unsupported, trimmed, encrypted or already patched ROM, and it will not work.
4. Apply `platinum-wonder-trade-us-rev0.bps` or `platinum-wonder-trade-us-rev1.bps`
   to the copy with your BPS patcher, then
   check the output against the output SHA-256 in `manifest.json`.
5. Name the patched ROM so it uses your existing save, then load it in melonDS.

Developers can use [`tools/bps-patch`](../tools/bps-patch/README.md) instead,
which runs these checks automatically but needs Python and Docker.

## Trade

Open the X menu and choose **Wonder Trade**. A new save may ask you to keep
playing first, because the game gathers randomness from your play to secure it.

1. Choose **Wonder Trade**, then pick a party Pokémon and confirm. It leaves your
   party and waits on the server. You can offer one Pokémon at a time per save.
2. Keep playing, and come back to Wonder Trade whenever you like.
3. Choose **Collect result**. The game shows what you received and saves it to
   your party or PC. If no one has matched yet, your Pokémon keeps waiting.
4. Before a match, **Cancel offer** returns your Pokémon.

Traded Pokémon that evolve by trade do so when collected, and keep their current
moves. If your party and PC are full, your result stays pending until you make
space and collect again.

You cannot offer an Egg, a Pokémon holding mail, one with a capsule, or your last
usable Pokémon. The server also refuses Pokémon that fail its legality check, and
returns them to you. An exact copy of a Pokémon you already hold cannot be offered.

## If something goes wrong

Your trade is saved at each step, so interrupted trades are designed to recover.

- **Connection errors:** choose Retry. Your Pokémon and your place are kept. A
  trade can wait a few seconds while the server secures it, and longer if the
  server is down.
- **"Trade recovery required" at startup:** connect and let it finish. Normal
  saving stays blocked until then, to protect the trade.
- **Do not** restore an old save, load a savestate, or copy a save to get around
  an error. The server tracks each save's progress, so an older copy stops at
  verification and can lose access to its pending trade.
- **Error -52000 on melonDS:** quit and reopen melonDS completely.

Patching only changes the ROM. Keep your save with any recovery files beside it.

## Known limits

- On a stock emulator, a crash before the emulator writes your save to disk can
  roll the save back behind the server, which can duplicate or lose a Pokémon.
  Let the game finish saving, and close the emulator normally. A
  [durable Linux build](../emulator/README.md) of melonDS guards against this.
- Copies of a save are outside what the game can protect. Backups taken before
  your first Wonder Trade enroll as an unrelated player.
- The legality check screens offers; it does not guarantee every Pokémon you
  receive is legitimate.

## Privacy

The server stores a random ID for your save, a hash-protected credential, the
Pokémon you trade with their trainer details, and trade history. It keeps these
to prove ownership and recover trades. Your save file itself is not uploaded.

## Notices

See `LICENSE`, `NOTICE` and the third-party notices in this bundle. Pokémon
belongs to its owners; this project is unofficial and includes no game data.
