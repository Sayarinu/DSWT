# Local release candidate

Assemble, audit and bind evidence to a local US Rev 0/1 candidate. Every
output stays under ignored `game/`; `publication_permitted` is always false.
Nothing here releases, uploads, deploys or changes repository visibility.

```sh
python3 tools/candidate/candidate.py assemble --bundle game/bundles/0.1.0-rc1 \
  --output game/candidate/dswt-0.1.0-rc1.tar
python3 tools/candidate/candidate.py verify game/candidate/dswt-0.1.0-rc1.tar
python3 tools/candidate/readiness.py source --output game/candidate/source-audit
python3 tools/candidate/readiness.py report --candidate game/candidate/dswt-0.1.0-rc1.tar \
  --stack-manifest game/local-e2e/manifest.json --evidence load game/local-e2e/load/RUN/report.json \
  --output game/candidate/readiness.json
```

The input is a [`tools/bps-patch`](../bps-patch/README.md) bundle generated
from the same clean commit. The archive contains:

- the bundle manifest and its BPS patches;
- the tracked instructions and notices listed in [`contents.json`](contents.json);
- `CANDIDATE.json`, which binds the source, upstream revision, profile and
  every file hash.

It never contains ROMs, saves, SDKs, build intermediates or the bundle's raw
`audit/` literals. The archive is a canonical ustar file (sorted names, zero
owners and times, mode 0644), so reruns are byte-identical. It is staged and
then renamed into place without replacing anything. `verify` reads it without
extracting and rejects:

- non-canonical headers, links and unsafe or prohibited paths;
- extra, missing or modified members, and documents not in the allowlist;
- invalid patches, and any manifest that permits publication.

`source` inventories every blob reachable from every local and remote-tracking
ref. It applies explicit path rules ([`classification.json`](classification.json)),
never extensions, and lists forbidden and unclassified paths and private
markers. It writes a reproducible `git archive` of HEAD and checks it against
the tree. It then runs the gitleaks release pinned in CI with the same policy.
The inventory is a review aid and grants no rights. `report` binds the
candidate and the reference stack's image IDs to each evidence report. A report
from a different source, a dirty tree or different images is marked `stale`,
never treated as applicable; missing kinds keep the report incomplete. The open publication, rights, hardware, hosted
and rehearsal gates are always listed.
