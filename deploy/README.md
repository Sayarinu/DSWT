# Deploying DSWT

DSWT runs on one VPS with Docker Compose. `dswt.py` loads the published images onto the
server, prepares the database and checks the public endpoint. CI runs it for each push to `main`
that changes the deployment; you can still run it by hand.

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

Pushing to `main` deploys when it changes what runs on the VPS. The **Deploy to production**
workflow runs for changes under:

- `server/**` and `validator/**`: the two images, including their Dockerfiles, migrations and
  `server/wal-archive.sh`, which `deploy` copies to the server.
- `LICENSE` and `NOTICE`: both images ship them.
- `deploy/**`, except `deploy/README.md` and `deploy/terraform/**`: the Compose file, monitoring
  configuration, `backup-uploader.sh` and `dswt.py` itself.
- `protocol/**`: `dswt.py` checks the live endpoint with `protocol/wire.py`.
- `.github/workflows/deploy.yml` and `.github/workflows/publish-images.yml`.

Other changes, such as the ROM patch in `client/`, docs and tooling, do not deploy. To deploy
anyway, run **Deploy to production** by hand on `main`.

The workflow runs the **Publish service images** workflow for the commit, which builds, tests and
audits both images and publishes those exact bytes to private GHCR packages tagged with the
commit. Then its deploy job, in the `production` environment, joins the tailnet and runs
`dswt.py deploy` for that commit. Read the scan results in the publish summary. Audit evidence
stays for 30 days. Releases run one at a time in push order, and a newer push replaces one still
waiting.

The deploy job reads only the `production` environment's secrets:

- the `.env` values `deploy` uses: `DISCORD_WEBHOOK_URL`, `DISCORD_ALERT_USER_ID`,
  `DSWT_BACKUP_PASSWORD`, `R2_ACCOUNT_ID` and `DSWT_BACKUPS_KEY`;
- `DSWT_CONNECTION_JSON`, the contents of `connection.json`;
- `DSWT_SSH_KEY`, a key used only for deploys, and `DSWT_KNOWN_HOSTS`, the server's `known_hosts`
  lines for its public IPv4;
- `TS_OAUTH_CLIENT_ID`, `TS_OAUTH_SECRET` and `DSWT_TAILNET_HOST`, the server's MagicDNS name or
  `100.x` address.

CI reaches SSH at `DSWT_TAILNET_HOST` but checks the host key recorded for the public IPv4, and
refuses any other. The public endpoint check still goes to the ROM's hostname on 443. CI never
holds `current.key`: it checks the certificate already on the server against the ROM pin instead,
so the first deploy to a new server must run by hand. CI deletes its pre-migration dump with the
runner, so only R2 keeps those copies for CI deploys.

To deploy by hand, update `main` here and run `deploy` from a clean checkout. It needs `gh` signed
in with `read:packages` (`gh auth refresh -s read:packages`). The server holds no registry login.
It reaches SSH at the public IPv4, which the firewall admits from your address. To go over the
tailnet instead, pass `--ssh-host` with the tailnet name, or set `DSWT_SSH_HOST`.

To roll back, deploy the older commit from a clean worktree. GHCR keeps the newest five
versions. `--build` builds on your machine instead, if GHCR is down or the commit is older.

To rotate the deploy key, make a new one (`ssh-keygen -t ed25519 -N '' -f dswt-deploy`), add
`dswt-deploy.pub` to `~dswt/.ssh/authorized_keys` on the server, replace `DSWT_SSH_KEY`, run the
workflow, then remove the old line from `authorized_keys` and delete the local copy. If the server's
host key changes, replace `DSWT_KNOWN_HOSTS` after checking the new key at the provider's console.

### Tailscale

CI joins the tailnet as an ephemeral `tag:ci` node that can reach only port 22 on the server, so
SSH stays closed to the internet. Set it up once:

1. Replace the tailnet policy's default allow-all rule with these rules, or `tag:ci` can reach the
   whole tailnet. Your own devices keep full access; CI reaches only the server's SSH port.

   ```json
   {
     "tagOwners": {
       "tag:ci": ["autogroup:admin"],
       "tag:dswt-server": ["autogroup:admin"]
     },
     "grants": [
       {"src": ["autogroup:member"], "dst": ["*"], "ip": ["*"]},
       {"src": ["tag:ci"], "dst": ["tag:dswt-server"], "ip": ["tcp:22"]}
     ]
   }
   ```

2. Create an OAuth client with the `auth_keys` write scope and tag `tag:ci`. Store its ID and
   secret as `TS_OAUTH_CLIENT_ID` and `TS_OAUTH_SECRET` in the `production` environment.
3. On the server, install Tailscale and run `sudo tailscale up --advertise-tags=tag:dswt-server`.
   Disable key expiry for it in the admin console, so deploys don't stop when it lapses.
   Tailscale connects outward, so the firewall needs no new rule.
4. Store the server's MagicDNS name or `100.x` address as `DSWT_TAILNET_HOST`.

To rotate the OAuth client, create a new one, replace both secrets, run the workflow, then
revoke the old client.

### When a deploy fails

Nothing rolls back automatically. What is left depends on where it stopped:

- Before anything reaches the server (publishing, credentials, pulling images): nothing changes.
- Loading images or syncing files: the previous release keeps serving, but `/opt/dswt` may
  already hold the new Compose file, configuration, secrets and `DSWT_VERSION`.
- Starting: the application services stop first, so trading is down until a deploy succeeds.
  If the backup fails, nothing was migrated. A failed migration rolls back in its transaction.
  Fix and redeploy, or deploy the previous commit.
- Verifying: the new release is running but unverified, and `tidy` is skipped, keeping the old
  WAL archive. Check it, then fix forward or deploy the previous commit.
- Tidying: the new release is running and verified. Run `tidy` again.

## Off-site backups

`backup-uploader` copies each WAL file and base backup to the private R2 bucket
`dswt-backups`, encrypted before it leaves the server. A WAL file only counts as archived once
R2 has it, so an R2 outage pauses trade completion. Pokémon stay in escrow, and it resumes by itself.
The recovery point is the last file R2 confirmed, normally seconds old.
R2 keeps 30 days of base backups, always including the newest, plus the WAL they need.

`.env` needs `R2_ACCOUNT_ID`, `DSWT_BACKUPS_KEY` (limited to that bucket) and
`DSWT_BACKUP_PASSWORD`. **Keep a copy of that password outside this machine.** Without it the
backups cannot be read. The key expires 2027-11-05: replace it in `.env` and in the `production`
environment, and deploy before then.

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

Critical alerts go to Discord and ping `DISCORD_ALERT_USER_ID`. `deploy` installs that and
`DISCORD_WEBHOOK_URL` from the ignored `.env` into `/opt/dswt/secrets/`. To change them, edit `.env`
and the `production` environment, and deploy.

## Known limitations

- Nothing alerts if the server or Prometheus itself is down. Use an outside uptime check.
- The local WAL archive grows until the next `deploy` or `tidy`. `DiskPressure` warns at 15%
  free.
