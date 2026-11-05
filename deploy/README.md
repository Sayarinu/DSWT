# Deploying DSWT

DSWT runs on one VPS with Docker Compose. `dswt.py` builds images on your machine,
loads them onto the server, prepares the database and checks the public endpoint.

Prepare the server first: Docker, a `dswt` user in the `docker` group, an empty
`/opt/dswt` it owns, and a firewall allowing 443 from anyone and 22 from your address
only. Docker's published ports bypass `ufw`, so use the provider's firewall. Point a
DNS-only A record at the server. A proxied record breaks the ROM's pinned TLS.

```sh
python3 deploy/dswt.py keys --hostname HOSTNAME --ipv4 ADDRESS
python3 deploy/dswt.py deploy
python3 deploy/dswt.py verify
```

`keys` runs once and writes to ignored `game/dswt-production/tls/`:

- `connection.json` is the public ROM profile. Pass it as `WONDER_TRADE_PROFILE`.
- `current.key` and `current.pem` go to the server.
- `next.key` is the rotation key. Keep a copy off this machine, and never put it on
  the server.

The ROM builds in the hostname, address, port and both pins. Changing the hostname or
address, or losing both keys, means new patches for every player.

`deploy` needs a clean commit, and deploys the images published for it. It stops the services
briefly. It cannot delete data: the volumes are external, so no Compose command
removes them. Before migrating, it saves a dump to ignored
`game/dswt-production/backups/`. Dumps hold private verifiers, so keep them private.

Once the new release is verified, `deploy` frees disk: the WAL archive restarts from a
fresh base backup, dropping older WAL, and images no container uses are removed. Run
`python3 deploy/dswt.py tidy` to do just this between releases. Trades keep running.
To go back, deploy the older commit, and restore a dump if its schema needs it.

## Releasing

1. Merge to `main` and wait for its checks.
2. Run the **Publish service images** workflow on `main`. It builds, tests and audits both
   images, then publishes those exact bytes to private GHCR packages, tagged with the
   commit. Read the scan results in its summary. Audit evidence stays for 30 days.
3. Update `main` here and run `deploy`. It needs `gh` signed in with `read:packages`
   (`gh auth refresh -s read:packages`). The server holds no registry login.

To roll back, deploy the older commit from a clean worktree. GHCR keeps the newest five
versions. `--build` builds on your machine instead, if GHCR is down or the commit is older.

## Off-site backups

`backup-uploader` copies each WAL file and base backup to the private R2 bucket
`dswt-backups`, encrypted before it leaves the server. A WAL file only counts as archived once
R2 has it, so an R2 outage pauses trade completion. Pokémon stay in escrow, and it resumes by itself.
The recovery point is the last file R2 confirmed, normally seconds old.
R2 keeps 30 days of base backups, always including the newest, plus the WAL they need.

`.env` needs `R2_ACCOUNT_ID`, `DSWT_BACKUPS_KEY` (limited to that bucket) and
`DSWT_BACKUP_PASSWORD`. **Keep a copy of that password outside this machine.** Without it the
backups cannot be read. The key expires 2027-11-05: replace it in `.env` and deploy before then.

`python3 deploy/dswt.py restore-check` rebuilds the ledger from R2 alone in throwaway volumes and
prints what it holds. Run it after changing this setup, and from time to time.

To recover: stop trading first. Restore the newest base and all WAL into a new server as
`restore-check` does, confirm the ledger with `dswt-ledger verify`, compare it to what players
last saw, and only then serve it. A restore behind what players saw stays paused. It never reissues or
discards Pokémon. There is no automatic failover.

## Infrastructure

`deploy/terraform` manages the Hetzner server, firewall and IP addresses, with state in the
private R2 bucket `dswt-terraform`. Run it through `dswt.py`, which reads credentials from the ignored `.env`:

```sh
python3 deploy/dswt.py terraform init
python3 deploy/dswt.py terraform plan
```

`.env` needs `HETZNER_API_TOKEN`, `R2_ACCOUNT_ID`, `DSWT_TERRAFORM_KEY` and `DSWT_ADMIN_CIDRS`
(comma-separated). A read-only Hetzner token is enough to plan. To apply, make a Read & Write
token, read the plan first, and revoke the token afterwards. A plan must never show the server
or its addresses being replaced. Nothing here runs in CI.

## Monitoring

```sh
ssh -i ~/.ssh/dswt -L 3000:127.0.0.1:3000 dswt@ADDRESS
```

Then open `http://localhost:3000` and log in as `admin`, with the password from
`/opt/dswt/secrets/grafana-admin-password`. Read logs with
`docker compose -p dswt logs SERVICE` in `/opt/dswt`.

Alerts go to Discord. `deploy` installs `DISCORD_WEBHOOK_URL` from the ignored `.env` into
`/opt/dswt/secrets/`. To rotate it, change `.env` and deploy. Nothing alerts if the server
or Prometheus itself is down, so use an outside uptime check before public launch.

## Before public launch

- The local WAL archive grows until the next `deploy` or `tidy`. `DiskPressure` warns at 15%
  free.
