#!/usr/bin/env python3
"""Check ticket naming and keep private build inputs out of contributions."""

import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


def check_pull_request(pr, subjects):
    ticket = pr["head"]["ref"]
    if not re.fullmatch(r"[A-Z][A-Z0-9]*-[1-9][0-9]*", ticket):
        return ["Use only the ticket identifier for the branch, such as ABC-123"]
    errors = []
    for subject in subjects:
        if not subject.startswith(f"{ticket} - ") or not subject.removeprefix(f"{ticket} - ").strip():
            errors.append(f"Each new commit must use '{ticket} - Message'")
            break
    body = pr.get("body") or ""
    if not re.search(rf"\b(?:ignore|skip)\s+{re.escape(ticket)}\b", body, re.IGNORECASE):
        errors.append("Use the PR template's tracker automation opt-out with this ticket")
    metadata = (pr.get("title") or "") + "\n" + body
    if re.search(r"linear\.app(?:[/\s]|$)", metadata, re.IGNORECASE):
        errors.append("Keep private tracker URLs out of the PR; use the ticket identifier")
    return errors


def check_paths(paths):
    errors = []
    for name in paths:
        path = PurePosixPath(name)
        if path.parts[0] == "game" or path.suffix.lower() in {".nds", ".sav", ".dsv", ".nef"}:
            errors.append(f"Keep local game inputs and outputs out of Git: {name}")
        elif (
            path.name.lower() in {"bios7.bin", "bios9.bin", "firmware.bin", ".env", "id_rsa", "id_ed25519"}
            or path.name.startswith(".env.")
            or {".aws", ".kube", ".terraform"}.intersection(path.parts)
            or path.suffix in {".tfstate", ".tfplan"}
            or ".tfstate." in path.name
        ):
            errors.append(f"Keep private local inputs out of Git: {name}")
    return errors


def main():
    paths = subprocess.check_output(["git", "ls-files", "-z"], text=True).split("\0")
    historical = subprocess.check_output(
        ["git", "log", "--all", "--root", "-m", "--no-renames", "--format=", "--name-only", "--diff-filter=A", "-z"], text=True
    ).split("\0")
    errors = check_paths(sorted(set(filter(None, paths + historical))))
    if os.environ.get("GITHUB_EVENT_NAME") == "pull_request":
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
        pr = event["pull_request"]
        base, head = pr["base"]["sha"], pr["head"]["sha"]
        if not all(re.fullmatch(r"[0-9a-f]{40}", sha) for sha in (base, head)):
            errors.append("Invalid pull request commit identifiers")
        else:
            subjects = subprocess.check_output(
                ["git", "log", "--format=%s", f"{base}..{head}"], text=True
            ).splitlines()
            errors.extend(check_pull_request(pr, subjects))
    if errors:
        sys.exit("\n".join(errors))
    print("Contribution policy passed")


if __name__ == "__main__":
    main()
