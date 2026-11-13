<div align="center">

# DSWT

**Wonder Trade for Pokémon Platinum.**

DSWT is a patch for your own copy of Pokémon Platinum that adds Wonder Trade to
the X menu, plus the server that runs it. Offer a party Pokémon, keep playing,
and come back later to collect whatever another player sent. Trades are checked
for legality and are designed to survive dropped connections and crashes.

[![CI](https://github.com/Sayarinu/DSWT/actions/workflows/validate.yml/badge.svg?branch=main)](https://github.com/Sayarinu/DSWT/actions/workflows/validate.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

[Features](#features) · [Getting the patch](#getting-the-patch) · [How it works](#how-it-works) · [FAQ](#faq) · [Contributing](#contributing)

</div>

---

## Features

- **Wonder Trade from the X menu.** Pick a party Pokémon and confirm. It waits on
  the server while you keep playing.
- **Collect when you're ready.** Choose **Collect result** to receive your match,
  or **Cancel offer** before a match to get your Pokémon back.
- **Trade evolutions** happen when you collect, as in a normal trade.
- **Legality checks.** The server screens every offer with
  [PKHeX.Core](https://github.com/kwsch/PKHeX) and returns rejected Pokémon to you.
- **Built for interruptions.** Every step of a trade is saved, so a lost
  connection resumes the same trade instead of losing or duplicating a Pokémon.
- **Secure connection.** The game talks to the server over TLS with a pinned
  server key; no plaintext fallback.
- **Optional durable emulator build.** A [patched melonDS](emulator/README.md)
  for Linux also protects trades against emulator or host crashes.

## Getting the patch

Download **DSWT Patcher** for Windows, macOS or Linux from the
[releases](https://github.com/Sayarinu/DSWT/releases), choose your own ROM and
save the patched copy. It recognises your cartridge, accepts trimmed dumps and
verifies the result. Each release also has the BPS patches, installation
instructions and a `manifest.json` of checksums. **DSWT never distributes ROMs.**

Supported today:

| Game | Revisions | Emulator |
| --- | --- | --- |
| Pokémon Platinum, US English | Rev 0 and Rev 1 | melonDS 1.1 |

Other versions and languages (Japanese, Korean, European English, French, German,
Italian and Spanish) and the other Gen IV games (Diamond, Pearl, HeartGold and
SoulSilver) are recognised but each needs its own ported and tested target first. Physical DS hardware is not supported yet.

- [Install and trade guide](patches/INSTALL.md): what players do with a released bundle.
- [ROM compatibility](patches/README.md): the verified inputs and the bundle contract.
- [BPS patch tooling](tools/bps-patch/README.md) and [player app](tools/patcher-app/README.md): build, check and release patches.
- [Building from source](docs/building.md): build the patched ROM yourself.

## How it works

The DS cannot speak gRPC, so the game sends small fixed-layout frames over HTTPS
to a gateway. Everything behind the gateway talks gRPC on a private network, and
only HTTPS is exposed.

```text
 Patched Platinum (melonDS)
            │  HTTPS POST /wt/v1, TLS 1.2 with a pinned key
            ▼
 HTTPS gateway (C++) ──gRPC──► Trade backend (C++) ──gRPC──► Legality validator
                                      │                      (.NET, PKHeX.Core)
                                      ▼
                   Matcher (C++) ──► PostgreSQL 16 ledger ──► off-site backups

 Prometheus and Grafana monitor the services; critical alerts go to Discord.
```

- [Wire protocol](protocol/README.md): the DS message layout and recovery rules.
- [Operating the services](docs/operations.md): configuration, limits and telemetry.
- [Deployment](deploy/README.md): Docker Compose on a single VPS.

## FAQ

**Do you provide the game?**
No. DSWT ships no ROMs, BIOS files, saves or game assets. You patch a copy of a
game you own.

**Is my save uploaded?**
No. The server stores a random ID for your save, a hash-protected credential,
the Pokémon you trade with their trainer details, and trade history, to prove
ownership and recover trades. See [Privacy](patches/INSTALL.md#privacy).

**What happens if the server is down?**
Your offered Pokémon stays safely in escrow. Choose Retry later; the game keeps
your place.

**Can I use save states or restore an old save?**
Don't. The server tracks each save's progress, so an older copy stops at
verification and can lose access to its pending trade.

**Can I run my own server?**
Yes. The server address and key pins are built into the ROM, so you build your
own patched ROM for your server. See [Deployment](deploy/README.md) and
[Building from source](docs/building.md).

## Contributing

Bug reports and pull requests are welcome. Open an
[issue](https://github.com/Sayarinu/DSWT/issues) first, then read
[AGENTS.md](AGENTS.md) for the repository layout, local checks and branch,
commit and pull request rules. It applies to people and coding agents alike.

## License

Original DSWT work is under the [MIT License](LICENSE). The legality validator
in [`validator/`](validator/) is GPL-3.0-or-later, and the melonDS patch in
[`emulator/`](emulator/) carries melonDS's GPL; their own notices apply. Neither
license covers the game, its assets or other third-party components. See
[NOTICE](NOTICE) and [`licensing/`](licensing/).

## Disclaimer

DSWT is an independent, unofficial fan project. It is not affiliated with,
endorsed by or sponsored by Nintendo, Game Freak, Creatures or The Pokémon
Company. Pokémon and Nintendo DS are trademarks of their respective owners.
This repository and its releases contain no ROMs, BIOS files, saves or original
game assets.
