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

`deploy` needs a clean commit, and tags the images with it. It stops the services
briefly. It cannot delete data: the volumes are external, so no Compose command
removes them. Before migrating, it saves a dump to ignored
`game/dswt-production/backups/`. Dumps hold private verifiers, so keep them private.

Once the new release is verified, `deploy` frees disk: the WAL archive restarts from a
fresh base backup, dropping older WAL, and images no container uses are removed. Run
`python3 deploy/dswt.py tidy` to do just this between releases. Trades keep running.
To go back, deploy the older commit, and restore a dump if its schema needs it.

## Monitoring

```sh
ssh -i ~/.ssh/dswt -L 3000:127.0.0.1:3000 dswt@ADDRESS
```

Then open `http://localhost:3000` and log in as `admin`, with the password from
`/opt/dswt/secrets/grafana-admin-password`. Read logs with
`docker compose -p dswt logs SERVICE` in `/opt/dswt`.

## Before public launch

- Ship the WAL archive off the server. It is on the same disk as the database, so a
  lost server loses both.
- Choose an alert channel in `observability/alertmanager.yml`. Until then, alerts only
  show in Grafana and Alertmanager.
- The WAL archive grows until the next `deploy` or `tidy`. `DiskPressure` warns at 15%
  free, so it needs that alert channel.
