# Install Wonder Trade

DSWT adds Wonder Trade to your own copy of a Pokémon DS game. It changes your
game; no game files are included. The full guide is at
https://github.com/Sayarinu/DSWT/wiki.

## What you need

- A dump of your own cartridge. Trimmed dumps work.
  - English, US or European, and French, German, Italian or Spanish: Pokémon
    Platinum, Diamond or Pearl (Rev 5), HeartGold or SoulSilver.
  - Japanese: Pokémon Platinum, HeartGold or SoulSilver.
  - Korean: Pokémon Platinum, Diamond, Pearl, HeartGold or SoulSilver.
  - Pokémon Black 2 or White 2, the English cartridge sold in the US and Europe.

  Other versions and languages are recognised but not supported yet.
- melonDS 1.1. Physical DS hardware isn't supported yet. Run a patched Black 2
  or White 2 in DS mode: in melonDS's **Config** > **Emu settings**, set
  **Console type** to **DS**. DSi mode doesn't work with the patched game.
- DSWT Patcher for your computer: the one file to download from the release page.
  It carries every patch. Other patching tools such as Flips don't work.

## Patch your game

1. Back up your save file.
2. Open DSWT Patcher, choose your ROM and save the patched copy. It checks your
   ROM, explains any problem and verifies the result. Your original file is
   never changed.
3. Give the patched ROM the same name as your save, then open it in melonDS:
   `Platinum.nds` uses `Platinum.sav`.

The apps aren't code-signed. On Windows choose **More info**, then **Run
anyway**. On macOS (Apple silicon) Control-click the app, choose **Open**, then
confirm. On Linux, mark `DSWT Patcher` as executable. `SHA256SUMS` on the release
page lists every file's checksum.

## Trade

Open the menu and choose **Wonder Trade**. In HeartGold and SoulSilver it's on the
touch-screen menu. In Black 2 and White 2, open the menu with X, then press
Select. In other languages the entry uses your game's name for it, such
as **ECHANGE MIRACLE**, **WUNDERTAUSCH** or **ミラクルこうかん**, and its screens
use your game's language. A brand-new save may ask
you to keep playing for a moment first, to gather randomness that secures your
connection.

1. Choose **Wonder Trade**, pick a party Pokémon and confirm. It leaves your
   party and waits on the server. You can offer one Pokémon at a time.
2. Keep playing, and come back whenever you like.
3. Choose **Collect result** to receive your Pokémon in your party, or your PC if
   your party is full. If nobody has matched yet, your Pokémon keeps waiting.
4. Before a match, **Cancel offer** gives your Pokémon back.

Every game trades with every other, and a Pokémon is only sent to a game that can
hold it. Trade evolutions happen when you collect. If your party and PC are full,
your result waits until you make space.

You can't offer an Egg, a Pokémon holding mail, one with a ball capsule, your last
Pokémon that can battle, or an exact copy of one you already have. The server
refuses illegal Pokémon, and Pokémon whose nickname or trainer name breaks the
online name rules of the newest Pokémon games, and gives them back. It can't promise every Pokémon you
receive is legitimate.

## If something goes wrong

Every step of a trade is saved, so an interrupted trade picks up where it left off.

- **Connection error:** choose Retry. Your Pokémon and your place are kept.
- **The game closed or crashed mid-trade:** start it normally. It finishes what it
  can by itself, and the rest finishes the next time you open Wonder Trade. You can
  play and save as usual meanwhile.
- **"Trade recovery required" at startup:** choose **Connect and retry**.
- **Error -52000 on melonDS:** quit melonDS completely and open it again.
- **Don't** restore an old save, load a savestate or copy a save to get around an
  error. An older copy can lose its pending trade.
- Let the game finish saving and close melonDS normally. A crash before your save
  reaches the disk can put it behind the server.

## Privacy

The server keeps a random ID for your save, a protected credential, the Pokémon
you trade with their trainer details, and your trade history, to prove ownership
and recover trades. Your save file is never uploaded.

## Notices

See `LICENSE`, `NOTICE` and the third-party notices in this bundle. Pokémon
belongs to its owners; this project is unofficial and includes no game data.
