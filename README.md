<div align="center">

# DSWT

**Wonder Trade for the Pokémon DS games.**

Offer a Pokémon from your own copy of Diamond, Pearl, Platinum, HeartGold or SoulSilver, keep playing, and collect whatever another player sent, from any of those games.

[![CI](https://github.com/Sayarinu/DSWT/actions/workflows/validate.yml/badge.svg?branch=main)](https://github.com/Sayarinu/DSWT/actions/workflows/validate.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
![Platforms](https://img.shields.io/badge/patcher-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)

[**Download**](#download) · [Features](#features) · [Guide](https://github.com/Sayarinu/DSWT/wiki) · [FAQ](#faq)

</div>

---

## Features

- **Wonder Trade in the game's own menu.** Pick a party Pokémon and confirm.
- **Trade across games.** Platinum, Diamond, Pearl, HeartGold and SoulSilver all trade with each other, and a Pokémon only goes to a game that can hold it.
- **Collect when you're ready.** Your offer waits on the server while you play. Cancel any time before a match.
- **Trade evolutions** happen when you collect.
- **Legality checks.** Every offer is checked with [PKHeX.Core](https://github.com/kwsch/PKHeX), and refused Pokémon come back to you.
- **Built for interruptions.** Every step is saved, so a crash or lost connection picks up the same trade instead of losing or duplicating a Pokémon, and the game still starts normally.
- **Secure connection** over TLS with a pinned server key.

## Download

DSWT is a patch: you need a dump of your own cartridge. **DSWT never distributes ROMs.**

| Game | Version |
|---|---|
| Pokémon Platinum | English: US Rev 0 and Rev 1, Europe |
| Pokémon Diamond and Pearl | English: US Rev 5, Europe Rev 5 |
| Pokémon HeartGold and SoulSilver | English: US, Europe |

Get **DSWT Patcher** for Windows, macOS or Linux from the **[Releases page](https://github.com/Sayarinu/DSWT/releases)** once the first release is out, open it, choose your ROM and save the patched copy. Play it in melonDS 1.1.

Step-by-step install and trading instructions are in the **[guide](https://github.com/Sayarinu/DSWT/wiki/Install)**.

## FAQ

**Do you provide the game?**
No. DSWT ships no ROMs, BIOS files, saves or game assets. You patch a copy of a game you own.

**Is my save uploaded?**
No. The server keeps a random ID for your save, a protected credential, the Pokémon you trade with their trainer details, and your trade history, to prove ownership and recover trades.

**What if the server is down, or the game crashes mid-trade?**
Your Pokémon stays safe. The game finishes what it can when it starts, and the rest finishes the next time you open Wonder Trade. See [Troubleshooting](https://github.com/Sayarinu/DSWT/wiki/Troubleshooting).

**Can I use save states or restore an old save?**
Please don't. The server tracks each save's progress, so an older copy can lose its pending trade.

**Does it work on a real DS?**
Not yet. melonDS 1.1 is the tested emulator.

**Can I run my own server or build it myself?**
Yes. See [Build from Source](https://github.com/Sayarinu/DSWT/wiki/Build-from-Source) and [Deployment](https://github.com/Sayarinu/DSWT/wiki/Deployment).

## Contributing

Bug reports and pull requests are welcome. Please open an [issue](https://github.com/Sayarinu/DSWT/issues) first, then read [AGENTS.md](AGENTS.md) for the layout, checks and branch, commit and pull request rules. Developer guides are in the [wiki](https://github.com/Sayarinu/DSWT/wiki).

## License

Original DSWT work is under the [MIT License](LICENSE). The legality validator in [`validator/`](validator/) is GPL-3.0-or-later, and the melonDS patch in [`emulator/`](emulator/) carries melonDS's GPL; their own notices apply. Neither covers the games, their assets or other third-party components. See [NOTICE](NOTICE) and [`licensing/`](licensing/).

## Disclaimer

DSWT is an independent, unofficial fan project. It is not affiliated with, endorsed by or sponsored by Nintendo, Game Freak, Creatures or The Pokémon Company. Pokémon and Nintendo DS are trademarks of their respective owners.
