"""Review unprivileged workflows and compare read-only GitHub policy evidence."""

import hashlib
import json
from pathlib import Path
import re
import subprocess

import yaml


class WorkflowLoader(yaml.BaseLoader):
    # BaseLoader preserves GitHub's `on` key instead of treating it as YAML 1.1 true.
    def compose_node(self, parent, index):
        require(not self.check_event(yaml.AliasEvent), "Workflow aliases need separate review")
        return super().compose_node(parent, index)

    def construct_mapping(self, node, deep=False):
        result = {}
        for key, value in node.value:
            name = self.construct_object(key, deep=deep)
            if not isinstance(name, str) or name == "<<" or name in result:
                raise ValueError("Duplicate, merged or non-scalar workflow key")
            result[name] = self.construct_object(value, deep=deep)
        return result


def require(condition, message):
    if not condition:
        raise ValueError(message)


def permissions(value):
    require(isinstance(value, dict), "Declare explicit permission mappings")
    require(all(mode in ("read", "none") and (name != "id-token" or mode == "none")
                for name, mode in value.items()), "Write/OIDC permissions are forbidden in contribution CI")


def strings(value, ancestors=frozenset()):
    require(id(value) not in ancestors, "Recursive workflow alias")
    require(len(ancestors) < 50, "Workflow nesting is excessive")
    parents = ancestors | {id(value)}
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for key, child in value.items():
            yield key
            yield from strings(child, parents)
    elif isinstance(value, list):
        for child in value:
            yield from strings(child, parents)


def workflow(data):
    require(isinstance(data, dict), "Workflow must be a mapping")
    triggers = data.get("on")
    if isinstance(triggers, str):
        triggers = [triggers]
    require(isinstance(triggers, (dict, list)) and bool(triggers), "Missing workflow triggers")
    require(set(triggers) <= {"pull_request", "push", "workflow_dispatch"}, "Privileged or unreviewed workflow trigger")
    permissions(data.get("permissions"))
    for text in strings(data):
        for expression in re.findall(r"\$\{\{(.*?)\}\}", text, re.S):
            require(not re.search(r"\bsecrets\s*[.\[]|\bgithub\s*(?:\.\s*(?:token|github_token)\b|\[\s*['\"](?:token|github_token)['\"])", expression, re.I),
                    "Explicit secret/token expression in contribution CI")
    jobs = data.get("jobs")
    require(isinstance(jobs, dict) and bool(jobs), "Workflow needs jobs")
    names = []
    for key, job in jobs.items():
        require(isinstance(job, dict), "Job must be a mapping")
        require("uses" not in job and "secrets" not in job, "Reusable/secret-inheriting jobs need separate review")
        permissions(job.get("permissions", data["permissions"]))
        runner = job.get("runs-on", "")
        require(isinstance(runner, str) and re.fullmatch(r"(ubuntu|windows|macos)-(latest|[0-9]+(?:\.[0-9]+)?)", runner),
                "Use a literal GitHub-hosted runner")
        require(not any(name in job for name in ("container", "services", "environment")),
                "Job containers/services/environments need separate review")
        steps = job.get("steps")
        require(isinstance(steps, list) and bool(steps), "Job needs steps")
        for step in steps:
            require(isinstance(step, dict), "Step must be a mapping")
            action = step.get("uses")
            if action is None:
                require(isinstance(step.get("run"), str), "Step needs run or uses")
                continue
            require(isinstance(action, str) and re.fullmatch(r"[\w.-]+/[\w./-]+@[0-9a-fA-F]{40}", action),
                    "Pin every action to a full commit")
            if action.lower().startswith("actions/checkout@"):
                options = step.get("with", {})
                require(isinstance(options, dict) and options.get("persist-credentials") == "false", "Disable persisted checkout credentials")
        name = job.get("name", key)
        require(isinstance(name, str) and "${{" not in name, "Use stable check names")
        names.append(name)
    return names


def audit(root):
    settings = json.loads((root / ".github/repository-settings.json").read_text())
    files, names = {}, []
    directory = root / ".github/workflows"
    for path in sorted(directory.iterdir()):
        require(path.is_file() and not path.is_symlink() and path.suffix in (".yml", ".yaml"), "Unexpected workflow input")
        data = path.read_bytes()
        require(len(data) <= 256 * 1024, "Workflow too large to review")
        names.extend(workflow(yaml.load(data, Loader=WorkflowLoader)))
        files[path.name] = hashlib.sha256(data).hexdigest()
    required = settings["main_protection"]["required_status_checks"]["contexts"]
    require(len(names) == len(set(names)) and len(required) == len(set(required)), "Check names must be unique")
    require(set(names) == set(required), "Tracked required checks differ from workflow jobs")
    return {"status": "pass", "workflows": files, "required_checks": sorted(required), "yaml_parser": "PyYAML " + yaml.__version__,
            "settings_sha256": hashlib.sha256((root / ".github/repository-settings.json").read_bytes()).hexdigest()}


def github(settings, repository, run=subprocess.run):
    require(re.fullmatch(r"[\w.-]+/[\w.-]+", repository), "Invalid GitHub repository")
    base = "repos/" + repository
    evidence = {}

    def read(name, suffix):
        try:
            result = run(["gh", "api", "--hostname", "github.com", base + suffix], text=True, capture_output=True, check=False, timeout=30)
        except (OSError, subprocess.SubprocessError):
            evidence[name] = {"status": "unavailable", "reason": "GitHub request did not complete"}
            return None
        if result.returncode:
            match = re.search(r"HTTP (\d+)", result.stderr)
            status = int(match[1]) if match else None
            evidence[name] = {"status": "denied" if status in (401, 403) else "unavailable", "http_status": status}
            return None
        try:
            data = json.loads(result.stdout)
            require(isinstance(data, dict), "Expected a GitHub object")
            return data
        except (ValueError, TypeError):
            evidence[name] = {"status": "unavailable", "reason": "invalid response"}
            return None

    def compare(name, actual, expected):
        if actual is not None:
            evidence[name] = {"status": "pass" if actual == expected else "mismatch", "actual": actual, "expected": expected}

    repo = read("repository", "")
    if repo is not None:
        compare("repository", {k: repo.get(k) for k in settings["repository"]}, settings["repository"])
    for name, suffix in (("actions", "/actions/permissions/workflow"),
                         ("private_fork_workflows", "/actions/permissions/fork-pr-workflows-private-repos")):
        data = read(name, suffix)
        if data is not None:
            compare(name, {k: data.get(k) for k in settings[name]}, settings[name])
    protection = read("main_protection", "/branches/main/protection")
    if protection is not None:
        actual = {}
        for key, expected in settings["main_protection"].items():
            value = protection.get(key)
            if isinstance(expected, bool) and isinstance(value, dict):
                value = value.get("enabled")
            if isinstance(expected, dict) and isinstance(value, dict):
                value = {k: value.get(k) for k in expected}
            actual[key] = value
        if isinstance(actual.get("required_status_checks"), dict):
            actual["required_status_checks"]["contexts"] = sorted(actual["required_status_checks"].get("contexts") or [])
        expected = json.loads(json.dumps(settings["main_protection"]))
        expected["required_status_checks"]["contexts"].sort()
        compare("main_protection", actual, expected)
    if repo is not None and repo.get("private") is True:
        evidence["public_fork_approval"] = {"status": "not_applicable", "reason": "repository is private; public-fork behavior untested"}
    elif repo is not None and repo.get("private") is False:
        data = read("public_fork_approval", "/actions/permissions/fork-pr-contributor-approval")
        if data is not None:
            compare("public_fork_approval", {k: data.get(k) for k in settings["public_fork_approval"]}, settings["public_fork_approval"])
    else:
        evidence["public_fork_approval"] = {"status": "unavailable", "reason": "visibility was not verified"}
    return evidence
