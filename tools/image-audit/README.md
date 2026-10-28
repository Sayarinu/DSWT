# Private service image audit

Use a separately installed **Python 3.12+**, Docker with Buildx, and a local Unix
Docker socket. Keep the repository private. This workflow never pushes images,
uses package credentials, uploads reports, or changes a retained local stack.

```sh
python3.12 tools/image-audit/audit.py run \
  --workspace game/image-audit-arm64 --platform linux/arm64
python3.12 tools/image-audit/audit.py verify --workspace game/image-audit-arm64
python3.12 -m unittest discover -s tools/image-audit/__tests__
```

Use `--platform linux/amd64` on Linux for separate architecture evidence. Each
run requires a new directory below ignored `game/`; an existing run is never
overwritten. Failures retain logs and `report.json`. A successful exit means the
audit completed, including scans; it does **not** mean there are no findings or
that publication is permitted. The final console summary includes severity
counts; the full Trivy reports retain every finding and license record.

Both Dockerfiles use the repository root and literal file allowlists in their
`Dockerfile.dockerignore`. Review and list each new tracked service file there;
the tooling test rejects stale lists. Before a build, a scratch export checks
the real Docker ignore rules against nested private `.cpp`/`.cs` sentinels.
Only the reviewed files are then staged for the audited build. Normal README,
CI and Compose builds use the same allowlists.

The owned digest-pinned BuildKit builder has two CPUs, a 4 GiB memory limit and
two parallel steps. It is removed before scanners run. The validator test image
remains locally tagged with the run's unique `platinumwt-audit-...` name for
independent inspection. No existing builder, image, container or volume is reset.
The exported OCI archives include maximum BuildKit provenance. Reports separately
record the layout wrapper, deployable image index, platform manifest and
configuration digests; the last is
the image ID, not an image manifest suitable for deployment pinning.

Trivy is pinned by the multi-platform image checksum in `pins.json`. One advisory
database download is recorded by its exact bytes and metadata; all subsequent
OS and .NET runtime scans use that same database with networking disabled.
CycloneDX generation explicitly enables vulnerabilities. JSON and SBOM coverage
must match every installed OS package name/version from the exported dpkg database
and include the validator's actual `app/Validator.deps.json`
runtime dependencies. An empty or failed scanner is an audit failure. .NET
`deps.json` does not provide license detection: the full loose-file license
inventory and independently checked exact notices/source remain separate evidence.

Every OCI blob and uncompressed layer digest is checked. All layers are scanned
before whiteouts are applied, then the final filesystem inventory is recorded.
Checks reject prohibited game/save/SDK paths, private-key markers and test
sentinels; they do not establish the origin or rights of arbitrary bytes.
Canonical project notices, selected PKHeX source against its hash-pinned upstream
archive, and complete matching gRPC/Protobuf source archives are verified. The
existing validator test target independently opens the exported image's actual
PKHeX.Core.dll and compares all 1,731 embedded resource hashes with packaged
source and the pinned category inventories. `verify` rechecks the archive,
filesystem, notices/source and all digest-bound evidence without Docker/network.

Publication stays blocked by the embedded-resource provenance review, applicable
corresponding-source obligations, unresolved vulnerability/license findings and
the parent image-publication ticket. A license inventory is not legal clearance.
No SDK, emulator, physical-device or hosted-deployment acceptance is claimed.

Tool pins are from the official [Trivy release](https://github.com/aquasecurity/trivy/releases/tag/v0.76.1)
and [BuildKit release](https://github.com/moby/buildkit/releases/tag/v0.34.1).
The C++ image uses the fixed Ubuntu `20261027T000000Z`
[package snapshot](https://snapshot.ubuntu.com/) and checksum-pinned Ubuntu base.
Its minimal base has no CA bundle, so a checksum-pinned certificate package from
that same snapshot bootstraps HTTPS. APT signatures remain mandatory; the fixed
historical indexes are allowed past their freshness dates. Package downloads
never fall back to the live archive.
Updating these inputs or the advisory database creates a new audit; retained
evidence is never relabeled as a newer result.
