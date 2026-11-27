<div align="center">

# DSWT

**Wonder Trade for the Pokémon DS games.**

Offer a Pokémon from your own copy of Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White, Black 2 or White 2, keep playing, and collect whatever another player sent, from a game in the same generation.

[![CI](https://github.com/Sayarinu/DSWT/actions/workflows/validate.yml/badge.svg?branch=main)](https://github.com/Sayarinu/DSWT/actions/workflows/validate.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
![Platforms](https://img.shields.io/badge/patcher-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)

[**Download**](#download) · [Features](#features) · [Guide](https://github.com/Sayarinu/DSWT/wiki) · [FAQ](#faq)

</div>

---

## Features

- **Wonder Trade in the game's own menu.** Pick a party Pokémon and confirm.
- **Trade across games.** Diamond, Pearl, Platinum, HeartGold and SoulSilver trade with each other; Black, White, Black 2 and White 2 trade with each other. A Pokémon only goes to a game that can hold it.
- **Collect when you're ready.** Your offer waits on the server while you play. Cancel any time before a match.
- **Trade evolutions** happen when you collect.
- **Legality checks.** Every offer is checked with [PKHeX.Core](https://github.com/kwsch/PKHeX), and refused Pokémon come back to you.
- **Built for interruptions.** Every step is saved, so a crash or lost connection picks up the same trade instead of losing or duplicating a Pokémon, and the game still starts normally.
- **Secure connection** over TLS with a pinned server key.

## Download

DSWT is a patch: you need a dump of your own cartridge. **DSWT never distributes ROMs.**

DSWT is tested on melonDS with the US versions of Pokémon Platinum, Diamond, Pearl, HeartGold and SoulSilver, and the USA/Europe versions of Black, White, Black 2 and White 2 in DS mode only. Other supported languages are ports: see **[Language support](https://github.com/Sayarinu/DSWT/wiki/Language-support)**.

Get **DSWT Patcher** for Windows, macOS or Linux from the **[Releases page](https://github.com/Sayarinu/DSWT/releases)**, open it, choose your ROM and save the patched copy. Play it in melonDS 1.1.

For the companion on DSi or 3DS, choose the companion HTML installer from a release that includes it. Open it in your browser and select **Download SD card kit**. Your browser generates private seeds for first-time setup. Extract the kit and merge its folders into the root of your SD card.

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
On a DSi or 3DS, yes: the **[DSWT](https://github.com/Sayarinu/DSWT/wiki/DS-Companion)** trades from your Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White, Black 2 or White 2 save over normal Wi-Fi. The patched game itself is tested in melonDS 1.1.

**Can I run my own server or build it myself?**
Yes. See [Build from Source](https://github.com/Sayarinu/DSWT/wiki/Build-from-Source) and [Deployment](https://github.com/Sayarinu/DSWT/wiki/Deployment).

## Contributing

Bug reports and pull requests are welcome. Please open an [issue](https://github.com/Sayarinu/DSWT/issues) first, then read [AGENTS.md](AGENTS.md) for the layout, checks and branch, commit and pull request rules. Developer guides are in the [wiki](https://github.com/Sayarinu/DSWT/wiki).

## License

Original DSWT work is under the [MIT License](LICENSE). The legality validator in [`validator/`](validator/) is GPL-3.0-or-later and carries its own notices. These licenses do not cover the games, their assets or other third-party components. See [NOTICE](NOTICE) and the [license texts](LICENSES/) and [component notices](licensing/).

## Disclaimer

DSWT is an independent, unofficial fan project. It is not affiliated with, endorsed by or sponsored by Nintendo, Game Freak, Creatures or The Pokémon Company. Pokémon and Nintendo DS are trademarks of their respective owners.
