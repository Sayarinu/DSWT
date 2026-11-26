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
                                         "required": "true"},
                             "companion": {"description": "Attach the DS Companion (requires hardware testing and translation review)",
                                           "type": "boolean", "default": "false"}}}
RELEASE_WRITERS = ("fetch", "publish")
RELEASE_BUILDERS = ("patcher", "companion")
PATCHER_RUNNERS = {"linux": "ubuntu-22.04", "macos": "macos-15", "windows": "windows-2025"}
MAIN_ONLY = "github.repository == 'Sayarinu/DSWT' && github.ref == 'refs/heads/main'"
DEPLOY_SECRETS = ("DISCORD_WEBHOOK_URL", "DISCORD_ALERT_USER_ID", "DSWT_BACKUP_PASSWORD", "R2_ACCOUNT_ID", "DSWT_BACKUPS_KEY",
                  "DSWT_CONNECTION_JSON", "DSWT_KNOWN_HOSTS", "DSWT_SSH_KEY", "DSWT_TAILNET_HOST")
# Everything the deployed images, the files synced to the server and the deploy itself are built from.
DEPLOY_PATHS = ["server/**", "validator/**", "LICENSE", "NOTICE", "deploy/**", "!deploy/terraform/**",
                "protocol/**", ".github/workflows/deploy.yml", ".github/workflows/publish-images.yml"]
# The publisher hands the deployer the digests it audited and pushed, so a moved tag can't change what deploys.
PUBLISHED_DIGESTS = {f"{image}_digest": "${{ steps.push.outputs.%s }}" % image for image in ("server", "validator")}
PUBLISHED_OUTPUTS = {f"{image}_digest": {"description": f"Manifest digest of the published {image} image",
                                         "value": "${{ jobs.publish.outputs.%s_digest }}" % image}
                     for image in ("server", "validator")}
DEPLOYED_DIGESTS = {f"DSWT_{image.upper()}_DIGEST": "${{ needs.publish.outputs.%s_digest }}" % image
                    for image in ("server", "validator")}
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
            if isinstance(action, str) and re.fullmatch(r"docker://ghcr\.io/gitleaks/gitleaks@sha256:[0-9a-f]{64}", action):
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
    require(data.get("on") == {"workflow_dispatch": "", "workflow_call": {"outputs": PUBLISHED_OUTPUTS}},
            "Publishing must only run by hand or from the deploy workflow, without inputs or secrets")
    jobs = data.get("jobs")
    require(isinstance(jobs, dict) and set(jobs) == {"publish"}, "Publishing needs exactly one job")
    job = jobs["publish"]
    require(isinstance(job, dict) and job.get("if") == "github.ref == 'refs/heads/main'", "Publishing must only run from main")
    require(job.get("permissions") == {"contents": "read", "packages": "write"}, "Publishing may only write packages")
    require(job.get("outputs") == PUBLISHED_DIGESTS, "Publishing must hand the deploy the digests it pushed")
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
    """Run by hand from main; companion downloads are opt-in, only fetch and publish may write contents."""
    require(data.get("on") == {"workflow_dispatch": RELEASE_INPUTS},
            "Releasing must only run by hand, with a version and a default-off companion input")
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
    companion = jobs["companion"]
    require("if" not in companion, "Every release must build and check the companion")
    builds = [step for step in companion.get("steps", []) if step.get("run") == "scripts/build/build-companion.sh"]
    require(len(builds) == 1 and "if" not in builds[0]
            and builds[0].get("env") == {"CONNECTION_PROFILE": "client/companion/production-connection-profile.json"},
            "Build the companion from source with only the tracked public production profile")
    uploads = [step for step in companion.get("steps", [])
               if str(step.get("uses", "")).startswith("actions/upload-artifact@")]
    require(len(uploads) == 1 and uploads[0].get("if") == "inputs.companion",
            "Only hand the companion to publish when explicitly requested")
    publish_steps = jobs["publish"].get("steps", [])
    publishes = [step for step in publish_steps if ' publish --version ' in step.get("run", "")]
    require(len(publishes) == 1 and publishes[0].get("env", {}).get("COMPANION") == "${{ inputs.companion }}"
            and '--companion "$COMPANION"' in publishes[0]["run"], "Publish must honor the companion opt-in")
    # What remains must satisfy every contribution rule, without the exceptions granted here.
    reviewed = copy.deepcopy(data)
    reviewed["on"] = "workflow_dispatch"
    patcher = reviewed["jobs"]["patcher"]
    matrix = [{"platform": platform, "runner": runner} for platform, runner in PATCHER_RUNNERS.items()]
    require(patcher.get("strategy") == {"matrix": {"include": matrix}}
            and patcher.get("runs-on") == "${{ matrix.runner }}", "Build only on the reviewed hosted runners")
    # The matrix is literal and complete. Check the rest of the job under each runner.
    for name in RELEASE_WRITERS:
        job = reviewed["jobs"][name]
        del job["permissions"]
        for step in job.get("steps", []):
            if isinstance(step, dict) and isinstance(step.get("env"), dict) and step["env"].get("GH_TOKEN") == "${{ github.token }}":
                require("run" in step, "Only commands may use the release token")
                del step["env"]["GH_TOKEN"]
    for runner in PATCHER_RUNNERS.values():
        patcher["runs-on"] = runner
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
    pinned = set()
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
            elif DEPLOYED_DIGESTS.get(name) == value:
                pinned.add(name)
    require(pinned == set(DEPLOYED_DIGESTS), "Deploy exactly the digests the publish job pushed")
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
        if path.name == "validate.yml":
            # Review the composite's actions under the same restrictions as the caller.
            action = ".github/actions/build-image/action.yml"
            content = (root / action).read_bytes()
            composite = yaml.load(content, Loader=WorkflowLoader)
            require(composite["runs"].get("using") == "composite", "Image builds use a composite action")
            files[action] = hashlib.sha256(content).hexdigest()
            for job in loaded["jobs"].values():
                steps = []
                for step in job["steps"]:
                    if step.get("uses") == "./.github/actions/build-image":
                        require(set(step) == {"name", "uses", "with"} and all(
                            isinstance(value, str) and "${{" not in value for value in step["with"].values()
                        ), "Local image builds take only literal inputs")
                        steps.extend(composite["runs"]["steps"])
                    else:
                        steps.append(step)
                job["steps"] = steps
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
