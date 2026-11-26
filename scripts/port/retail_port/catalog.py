"""Supported cartridges indexed by release variant ID."""

import json
from pathlib import Path


def variants_by_id(root: Path):
    catalog = json.loads((root / "client/games/compatibility.json").read_text())
    return {row["id"]: row for row in catalog["variants"]}
