"""The disposable backend must refuse connection overrides before connecting."""
import os
import subprocess
import sys

binary = sys.argv[1]
environment = {key: value for key, value in os.environ.items()
               if not key.startswith("PG") and key not in ("DSWT_DATABASE_URL", "DATABASE_URL")}
environment.update(DSWT_DISPOSABLE_DEVICE_TEST="1", PGHOST="/test/run", PGDATABASE="dswt_e2e")
cases = [{key: value} for key, value in (
    ("DSWT_DISPOSABLE_DEVICE_TEST", "0"), ("PGHOST", "elsewhere.invalid"),
    ("PGDATABASE", "other_database"), ("PGPORT", "5433"), ("PGHOSTADDR", "192.0.2.1"),
    ("PGSERVICE", "other"), ("PGSERVICEFILE", "/private/service"),
    ("DSWT_DATABASE_URL", "other"), ("DATABASE_URL", "other"))]
for changed in cases:
    result = subprocess.run([binary, "--check"], env=dict(environment, **changed), capture_output=True, timeout=5)
    assert result.returncode == 2, (changed.keys(), result.returncode)
    assert not result.stdout and not result.stderr, "Refused configuration must not expose private settings"
