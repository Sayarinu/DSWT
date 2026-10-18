import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ValidatorInputsTests(unittest.TestCase):
    def test_source_evidence_tracks_the_locked_runtime_packages_and_policy(self):
        folder = ROOT / "validator"
        sources = {}
        for line in (folder / "runtime-sources.tsv").read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            name, repository, revision, digest, version = line.split()
            self.assertRegex(repository, r"^[a-zA-Z0-9-]+/[a-zA-Z0-9-]+$")
            self.assertRegex(revision, r"^[0-9a-f]{40}$")
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            sources[name] = version
        locked = json.loads((folder / "packages.lock.json").read_text())["dependencies"]["net10.0"]
        for name, dependency in locked.items():
            if dependency["type"] == "Project" or name == "Grpc.Tools":
                continue  # The core is built from source; Tools is build-only.
            component = "protobuf" if name == "Google.Protobuf" else "grpc-dotnet" if name.startswith("Grpc.") else None
            self.assertIsNotNone(component, f"Unreviewed runtime dependency: {name}")
            self.assertEqual(dependency["resolved"], sources[component], f"Source evidence is stale for {name}")
        pin = dict(line.split("=", 1) for line in (folder / "upstream.env").read_text().splitlines())
        resources = json.loads((folder / "core-resources.json").read_text())
        self.assertEqual(resources["revision"], pin["PKHEX_REVISION"])
        policy = re.search(r'const string Version = "([^"]+)"', (folder / "Policy.cs").read_text())[1]
        self.assertIn(pin["PKHEX_VERSION"], policy)
        self.assertIn(pin["PKHEX_REVISION"][:8], policy)
        self.assertLessEqual(len(policy), 64)
