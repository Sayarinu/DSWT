"""Review unprivileged workflows and compare read-only GitHub policy evidence."""

import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess

import yaml

PUBLISHER = "publish-images.yml"
DEPLOYER = "deploy.yml"
RELEASER = "release.yml"
RELEASE_INPUTS = {"inputs": {"version": {"description": "Release version, such as 0.1.0, of an existing draft release",
                                         "required": "true"}}}
RELEASE_WRITERS = ("fetch", "publish")
RELEASE_BUILDERS = ("linux", "macos", "windows")
MAIN_ONLY = "github.repository == 'Sayarinu/DSWT' && github.ref == 'refs/heads/main'"
DEPLOY_SECRETS = ("DISCORD_WEBHOOK_URL", "DISCORD_ALERT_USER_ID", "DSWT_BACKUP_PASSWORD", "R2_ACCOUNT_ID", "DSWT_BACKUPS_KEY",
                  "DSWT_CONNECTION_JSON", "DSWT_KNOWN_HOSTS", "DSWT_SSH_KEY", "DSWT_TAILNET_HOST")
# Everything the deployed images, the files synced to the server and the deploy itself are built from.
DEPLOY_PATHS = ["server/**", "validator/**", "LICENSE", "NOTICE", "deploy/**", "!deploy/README.md", "!deploy/terraform/**",
                "protocol/**", ".github/workflows/deploy.yml", ".github/workflows/publish-images.yml"]
TAILSCALE = "tailscale/github-action@064b10558093f38325e838ddea35486bdc6b1d5c"
TAILSCALE_INPUTS = {"oauth-client-id": "${{ secrets.TS_OAUTH_CLIENT_ID }}", "oauth-secret": "${{ secrets.TS_OAUTH_SECRET }}",
                    "tags": "tag:ci"}


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
        if "${{" in text:
            # Conservative review aid: quoted `}}` cannot hide a later context reference.
            require(not re.search(r"\bsecrets\b", text, re.I), "Secret context in contribution CI")
            for context in re.finditer(r"\bgithub\b", text, re.I):
                field = re.match(r"\s*\.\s*([a-zA-Z_][a-zA-Z_0-9]*)", text[context.end():])
                require(field and field[1].lower() not in ("token", "github_token"), "Token or unreviewed GitHub context access")
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
            require(not action.lower().startswith("tailscale/"), "Only the deploy job may join the tailnet")
            if action.lower().startswith("actions/checkout@"):
                options = step.get("with", {})
                require(isinstance(options, dict) and options.get("persist-credentials") == "false", "Disable persisted checkout credentials")
        name = job.get("name", key)
        require(isinstance(name, str) and "${{" not in name, "Use stable check names")
        names.append(name)
    return names


def publisher(data):
    """Run by hand or by the deployer, from main only, able to write packages and nothing else."""
    require(data.get("on") == {"workflow_dispatch": "", "workflow_call": ""},
            "Publishing must only run by hand or from the deploy workflow, without inputs or secrets")
    jobs = data.get("jobs")
    require(isinstance(jobs, dict) and len(jobs) == 1, "Publishing needs exactly one job")
    job = next(iter(jobs.values()))
    require(isinstance(job, dict) and job.get("if") == "github.ref == 'refs/heads/main'", "Publishing must only run from main")
    require(job.get("permissions") == {"contents": "read", "packages": "write"}, "Publishing may only write packages")
    # What remains must satisfy every contribution rule, without the two exceptions granted here.
    reviewed = copy.deepcopy(data)
    reviewed["on"] = "workflow_dispatch"
    job = next(iter(reviewed["jobs"].values()))
    del job["permissions"]
    for step in job.get("steps", []):
        if isinstance(step, dict) and isinstance(step.get("env"), dict) and step["env"].get("GH_TOKEN") == "${{ github.token }}":
            del step["env"]["GH_TOKEN"]
    workflow(reviewed)


def releaser(data):
    """Run by hand from main with one version input; only fetch and publish may write, and only contents."""
    require(data.get("on") == {"workflow_dispatch": RELEASE_INPUTS},
            "Releasing must only run by hand, with only the version input")
    jobs = data.get("jobs")
    require(isinstance(jobs, dict) and set(jobs) == {*RELEASE_WRITERS, *RELEASE_BUILDERS},
            "Releasing needs exactly the fetch, build and publish jobs")
    for name in RELEASE_WRITERS:
        job = jobs[name]
        require(isinstance(job, dict) and job.get("if") == "github.ref == 'refs/heads/main'", "Releases must only run from main")
        require(job.get("permissions") == {"contents": "write"}, "Release writers may only write contents")
    for name in RELEASE_BUILDERS:
        require(isinstance(jobs[name], dict) and "permissions" not in jobs[name] and jobs[name].get("needs") == "fetch",
                "Builds read source only and start from the verified archive")
    require(jobs["publish"].get("needs") == list(RELEASE_BUILDERS), "Publish only after every build")
    # What remains must satisfy every contribution rule, without the exceptions granted here.
    reviewed = copy.deepcopy(data)
    reviewed["on"] = "workflow_dispatch"
    for name in RELEASE_WRITERS:
        job = reviewed["jobs"][name]
        del job["permissions"]
        for step in job.get("steps", []):
            if isinstance(step, dict) and isinstance(step.get("env"), dict) and step["env"].get("GH_TOKEN") == "${{ github.token }}":
                require("run" in step, "Only commands may use the release token")
                del step["env"]["GH_TOKEN"]
    workflow(reviewed)


def deployer(data):
    """Publish then deploy each push to main. Only the production job may read secrets, and only these."""
    require(data.get("on") == {"push": {"branches": ["main"], "paths": DEPLOY_PATHS}, "workflow_dispatch": ""},
            "Deploying must only run on pushes to main that change the deployment, or by hand")
    require(data.get("concurrency") == {"group": "production-release", "cancel-in-progress": "false"},
            "Releases must run one at a time, in order")
    jobs = data.get("jobs")
    require(isinstance(jobs, dict) and set(jobs) == {"publish", "deploy"}, "Deploying needs exactly a publish and a deploy job")
    require(jobs["publish"] == {"name": "Publish images", "if": MAIN_ONLY,
                                "permissions": {"contents": "read", "packages": "write"},
                                "uses": "./.github/workflows/" + PUBLISHER}, "Publish only through the reviewed publisher")
    job = jobs["deploy"]
    require(isinstance(job, dict) and job.get("if") == MAIN_ONLY, "Deploying must only run from main of this repository")
    require(job.get("needs") == "publish", "Deploy only images published for this commit")
    require(job.get("environment") == "production", "Secrets are only for the production environment")
    require(job.get("concurrency") == {"group": "production-deploy", "cancel-in-progress": "false"},
            "Deploys must never overlap or be cancelled midway")
    require(job.get("permissions") == {"contents": "read", "packages": "read"}, "Deploying may only read source and packages")
    # What remains must satisfy every contribution rule. Secrets may only be named whole in a step's env.
    reviewed = copy.deepcopy(data)
    del reviewed["jobs"]["publish"], reviewed["concurrency"]
    job = reviewed["jobs"]["deploy"]
    for key in ("if", "needs", "environment", "concurrency", "permissions"):
        del job[key]
    steps = job.get("steps") if isinstance(job.get("steps"), list) else []
    # Tailscale joins once, with only its OAuth client and tag, before any step runs commands.
    joins = [index for index, step in enumerate(steps) if isinstance(step, dict) and step.get("uses") == TAILSCALE]
    require(len(joins) == 1 and set(steps[joins[0]]) == {"name", "uses", "with"}
            and steps[joins[0]]["with"] == TAILSCALE_INPUTS, "Join the tailnet once, only as tag:ci")
    require(all(isinstance(step, dict) and "run" not in step for step in steps[:joins[0]]),
            "Join the tailnet before running commands")
    del steps[joins[0]]
    for step in steps:
        if not isinstance(step, dict):
            continue
        # Checkout and the reviewed Tailscale step are the only actions beside the secrets.
        require("uses" not in step or str(step["uses"]).startswith("actions/checkout@"), "Deploying runs no other actions")
        variables = step.get("env")
        if not isinstance(variables, dict) or "uses" in step:
            continue
        for name, value in list(variables.items()):
            if name in DEPLOY_SECRETS and value == "${{ secrets.%s }}" % name:
                del variables[name]
            elif name == "DSWT_REGISTRY_TOKEN" and value == "${{ github.token }}":
                del variables[name]
    workflow(reviewed)


def audit(root):
    settings = json.loads((root / ".github/repository-settings.json").read_text())
    files, names = {}, []
    directory = root / ".github/workflows"
    for path in sorted(directory.iterdir()):
        require(path.is_file() and not path.is_symlink() and path.suffix in (".yml", ".yaml"), "Unexpected workflow input")
        data = path.read_bytes()
        require(len(data) <= 256 * 1024, "Workflow too large to review")
        loaded = yaml.load(data, Loader=WorkflowLoader)
        if path.name == PUBLISHER:
            publisher(loaded)  # Never a required PR check: it does not run on pull requests.
        elif path.name == DEPLOYER:
            deployer(loaded)  # Neither is this one.
        elif path.name == RELEASER:
            releaser(loaded)  # Nor this one.
        else:
            names.extend(workflow(loaded))
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
