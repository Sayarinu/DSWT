#!/usr/bin/env python3
"""Control disposable durable melonDS profiles against the owned local stack."""

import argparse
from contextlib import contextmanager, ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import struct
import sys
import tempfile
import time

import stack


PROVISION = stack.load_module("emulator_provision", stack.ROOT / "scripts/provision-save.py")
VERIFY = stack.load_module("emulator_verify", stack.ROOT / "scripts/verify-demo-trade.py")
OWNER_LABEL = "studio.nouvellelune.platinumwt.emulator-profile"
KEYS = {"A": 65, "B": 66, "X": 88, "Y": 89, "Start": 16777220, "Select": 16777248,
        "Up": 16777235, "Down": 16777237, "Left": 16777234, "Right": 16777236}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def save_files(directory):
    """The raw save and its host recovery store, as a snapshot names them."""
    return {"platinum.sav": directory / "platinum.sav",
            **{item: directory / "platinum.sav.platinumwt" / item for item in ("A", "B", "HEAD")}}


def read_private(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)  # Never wait on a planted FIFO.
    with os.fdopen(descriptor, "rb") as source:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("Profile inputs must be private owned files without links")
        return source.read()


def write_private(path, data):
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), "wb") as output:
        output.write(data); output.flush(); os.fsync(output.fileno())


@contextmanager
def private_lock(path, operation, *, create=True, timeout=0):
    descriptor = os.open(path, os.O_RDWR | os.O_NOFOLLOW | (os.O_CREAT if create else 0), 0o600)
    try:
        stack.private_file(path)
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(descriptor, operation | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline: raise
                time.sleep(0.05)
        yield
    finally:
        os.close(descriptor)


def journal(data):
    """Observe a coherent committed bank, retaining ambiguity as an error."""
    if len(data) != 524288:
        raise ValueError("Save is incomplete; run checked host recovery first")
    records = [data[offset:offset + 4096] for offset in (0x30000, 0x70000)]
    valid = [record for record in records if record[:6] == b"PWTJ\1\0" and record[-4:] == b"CMIT"
             and hashlib.sha256(record[:4060]).digest() == record[4060:4092]]
    if not valid:
        raise ValueError("No known committed journal; preserve the profile")
    valid.sort(key=lambda record: struct.unpack_from("<Q", record, 16)[0])
    if len(valid) == 2 and valid[0][16:24] == valid[1][16:24] and valid[0] != valid[1]:
        raise ValueError("Conflicting journal mirrors; preserve the profile")
    return valid[-1]


class Emulator:
    def __init__(self, local, name):
        if not re.fullmatch(r"[a-z][a-z0-9-]{0,31}", name):
            raise ValueError("Use a short lowercase profile name")
        self.local, self.name = local, name
        self.directory = local.workspace / "emulators" / name
        if self.directory.resolve() != self.directory: raise ValueError("Profile must not be linked")
        self.container = local.manifest["project"] + "-emulator-" + name

    def manifest(self):
        if self.directory.resolve() != self.directory:
            raise ValueError("Profile must not be linked")
        stack.private_file(self.directory / "manifest.json")
        value = json.loads((self.directory / "manifest.json").read_text())
        if value.get("version") != 1 or value.get("profile") != self.name or value.get("workspace") != str(self.local.workspace):
            raise ValueError("Profile does not belong to this local workspace")
        return value

    def image(self, name):
        return self.local.run(["image", "inspect", "--format", "{{.Id}}", name], capture=True, timeout=10).stdout.strip()

    def image_sources(self, image):
        names = ("emulator/durable_save.cpp", "emulator/journal_seed.cpp", "emulator/tests/faults.cpp",
                 "client/src/wonder_trade/journal.c", "scripts/run-emulator.py")
        result = self.local.run(["run", "--rm", "--network", "none", "--read-only", "--cap-drop", "ALL",
                                 "--user", f"{os.getuid()}:{os.getgid()}", image, "sha256sum",
                                 *("/src/" + name for name in names)], capture=True, timeout=15)
        actual = {line.split()[1].removeprefix("/src/"): line.split()[0] for line in result.stdout.splitlines()}
        expected = {name: digest((stack.ROOT / name).read_bytes()) for name in names}
        if actual != expected: raise ValueError("Rebuild the emulator image from these checked sources")
        return expected

    def create(self, source, rom, target, image):
        source, rom = source.resolve(strict=True), rom.resolve(strict=True)
        original, program = source.read_bytes(), rom.read_bytes()
        if len(program) != 134217728 or program[12:16] != b"CPUE" or program[30] != int(target[-1]):
            raise ValueError("Use the matching US Rev 0/1 built ROM")
        provisioned = PROVISION.provision(original, target)
        image = self.image(image)
        self.profiles_directory()
        self.directory.mkdir(mode=0o700)  # Never reinitialize an enrolled profile.
        for name, content in (("platinum.sav", provisioned), ("platinum.nds", program), ("baseline.sav", original)):
            with (self.directory / name).open("xb") as output: output.write(content)
        self.write_config(self.directory)
        self.local.run(["run", "--rm", "--network", "none", "--read-only", "--user", f"{os.getuid()}:{os.getgid()}",
                        "--mount", f"type=bind,source={self.directory},target=/profile", image,
                        "cp", "/build/melonDS", "/profile/melonDS"], capture=True, timeout=15)
        (self.directory / "melonDS").chmod(0o700)
        stack.save_manifest(self.directory, {"version": 1, "profile": self.name, "workspace": str(self.local.workspace),
            "target": target, "source_save": str(source), "source_sha256": digest(original), "rom_sha256": digest(program),
            "image": image, "emulator_sha256": digest((self.directory / "melonDS").read_bytes())})

    def profiles_directory(self):
        parent = self.directory.parent
        parent.mkdir(mode=0o700, exist_ok=True)
        if parent.resolve() != parent: raise ValueError("Profiles directory must not be linked")
        return parent

    @staticmethod
    def write_config(directory):
        (directory / "portable").mkdir(mode=0o700)
        mac = ":".join(f"{byte:02x}" for byte in b"\x02" + secrets.token_bytes(5))
        (directory / "portable/melonDS.toml").write_text(
            'LimitFPS = true\nAudioSync = false\nPauseLostFocus = false\n[Emu]\nDirectBoot = true\n'
            'ExternalBIOSEnable = false\n[Screen]\nUseGL = false\n[3D]\nRenderer = 0\n[JIT]\nEnable = true\n'
            '[Instance0]\nSaveFilePath = "/profile"\n[Instance0.Firmware]\nMAC = "' + mac + '"\n'
            '[Instance0.Keyboard]\n' + "".join(f"{key} = {value}\n" for key, value in KEYS.items()))

    def fork(self, source_name, snapshot_name):
        """Copy a checked snapshot into a new profile to test copied/older enrolled saves.

        The copy deliberately shares the source identity and credential; the
        server's counter fence, not this tool, must keep the two apart.
        """
        if source_name == self.name: raise ValueError("Fork into a different profile")
        if self.directory.exists() or self.directory.is_symlink(): raise FileExistsError(self.directory)
        source = Emulator(self.local, source_name)
        with private_lock(self.local.workspace / (".emulator-" + source.name + ".lock"), fcntl.LOCK_EX):
            origin = source.manifest()
            snapshot = source.directory / "snapshots" / snapshot_name
            if not re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", snapshot_name) or snapshot.resolve() != snapshot:
                raise ValueError("Use a checked snapshot of the source profile")
            recorded = json.loads(read_private(snapshot / "manifest.json"))
            if not isinstance(recorded, dict): raise ValueError("Snapshot does not belong to the source profile")
            hashes, profile = recorded.get("files"), recorded.get("profile")
            if (recorded.get("version") != 1 or not isinstance(profile, dict) or not isinstance(hashes, dict)
                    or set(hashes) != set(save_files(snapshot)) or profile.get("profile") != source.name
                    or any(profile.get(key) != origin[key] for key in ("workspace", "rom_sha256", "emulator_sha256"))):
                raise ValueError("Snapshot does not belong to the source profile")
            files = {item: read_private(snapshot / item) for item in hashes}
            if any(digest(data) != hashes[item] for item, data in files.items()):
                raise ValueError("Snapshot changed after it was checked; preserve it")
            binaries = {item: read_private(source.directory / item) for item in ("platinum.nds", "melonDS")}
            if digest(binaries["platinum.nds"]) != origin["rom_sha256"] or digest(binaries["melonDS"]) != origin["emulator_sha256"]:
                raise ValueError("Source profile binaries changed; preserve the original run")
        # Build privately, then publish complete; the caller holds this profile's lock.
        staging = self.profiles_directory() / (".fork-" + self.name + "-" + secrets.token_hex(8))
        staging.mkdir(mode=0o700)
        try:
            (staging / "platinum.sav.platinumwt").mkdir(mode=0o700)
            outputs = {**save_files(staging), "platinum.nds": staging / "platinum.nds", "melonDS": staging / "melonDS"}
            for item, data in {**files, **binaries}.items(): write_private(outputs[item], data)
            (staging / "melonDS").chmod(0o700)
            self.write_config(staging)
            stack.save_manifest(staging, {"version": 1, "profile": self.name, "workspace": str(self.local.workspace),
                **{key: origin[key] for key in ("target", "source_save", "source_sha256", "rom_sha256", "image", "emulator_sha256")},
                "forked_from": {"profile": source.name, "snapshot": snapshot_name, "files": hashes}})
            if self.directory.exists() or self.directory.is_symlink(): raise FileExistsError(self.directory)
            staging.rename(self.directory)
        except BaseException:
            shutil.rmtree(staging, ignore_errors=True)  # Only copies of retained, checked inputs.
            raise
        descriptor = os.open(self.directory.parent, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(descriptor)
        finally: os.close(descriptor)

    def owned_container(self):
        result = self.local.run(["container", "inspect", self.container], capture=True, timeout=10, check=False)
        if result.returncode:
            # An engine failure must not be mistaken for permission to replace a container.
            names = self.local.run(["container", "ls", "-a", "--format", "{{.Names}}"], capture=True, timeout=10).stdout.splitlines()
            if self.container in names: raise ValueError("Cannot inspect retained emulator container")
            return None
        value = json.loads(result.stdout)[0]
        labels = value["Config"].get("Labels") or {}
        if labels.get(stack.LABEL) != self.local.manifest["project"] or labels.get(OWNER_LABEL) != self.name:
            raise ValueError("Container name belongs to another owner")
        return value

    def support(self, image):
        source = Path(__file__).with_name("qt-control.cpp").read_bytes()
        key = digest(source)[:16] + "-" + image[-12:]
        with private_lock(self.local.workspace / (".emulator-support-" + key + ".lock"), fcntl.LOCK_EX, timeout=65):
            return self._support(image, source, key)

    def _support(self, image, source, key):
        directory = self.local.workspace / "emulator-support" / key
        if directory.resolve() != directory: raise ValueError("Emulator support must not be linked")
        if directory.exists():
            if (directory / "qt-control.cpp").is_symlink() or (directory / "qt-control.cpp").read_bytes() != source:
                raise ValueError("Preserve incomplete/changed emulator support")
            if (directory / "manifest.json").exists():
                stack.private_file(directory / "manifest.json")
                stack.private_file(directory / "qt-control.so")
                ready = json.loads((directory / "manifest.json").read_text())
                if ready != {"source_sha256": digest(source), "image": image, "library_sha256": digest((directory / "qt-control.so").read_bytes())}:
                    raise ValueError("Preserve changed emulator control library")
                return directory
        else:
            directory.mkdir(mode=0o700, parents=True)
            (directory / "qt-control.cpp").write_bytes(source)
        # Preserve failed compiler outputs. Only a completed, synced library and
        # readiness manifest may be reused by another independently started save.
        temporary = directory / ("qt-control-" + secrets.token_hex(8) + ".new")
        self.local.run(["run", "--rm", "--network", "none", "--read-only", "--tmpfs", "/tmp", "--user", f"{os.getuid()}:{os.getgid()}",
                        "--mount", f"type=bind,source={directory},target=/support", image, "sh", "-c",
                        "g++ -shared -fPIC /support/qt-control.cpp -o /support/" + temporary.name + " $(pkg-config --cflags --libs Qt6Widgets Qt6Test) -ldl"],
                       capture=True, timeout=60)
        temporary.chmod(0o600)
        with temporary.open("rb") as compiled: os.fsync(compiled.fileno())
        temporary.replace(directory / "qt-control.so")
        stack.save_manifest(directory, {"source_sha256": digest(source), "image": image,
                                         "library_sha256": digest((directory / "qt-control.so").read_bytes())})
        return directory

    def logs(self):
        value = self.owned_container()
        if not value: raise ValueError("No retained emulator container")
        result = self.local.run(["logs", value["Id"]], capture=True, timeout=10)
        path = self.directory / ("run-" + value["Id"][:12] + ".log")
        with tempfile.NamedTemporaryFile(mode="w", dir=self.directory, prefix=".run-log-", delete=False) as output:
            temporary = Path(output.name)
            output.write(result.stdout + result.stderr)
        temporary.replace(path)
        return path

    def start(self, *, fault_at=None, fault_mode="before", fault_phase=None, fast=False):
        manifest = self.manifest()
        manifest["image_source_sha256"] = self.image_sources(manifest["image"])
        prior = self.owned_container()
        if prior:
            if prior["State"]["Running"]: raise ValueError("Profile is already running")
            self.logs()
            self.local.run(["rm", prior["Id"]], capture=True, timeout=10)
        self.local.ready()
        if digest((self.directory / "platinum.nds").read_bytes()) != manifest["rom_sha256"] or digest((self.directory / "melonDS").read_bytes()) != manifest["emulator_sha256"]:
            raise ValueError("Profile binaries changed; preserve the original run")
        support = self.support(manifest["image"])
        config = self.directory / "portable/melonDS.toml"
        stack.private_file(config)
        config.write_text(re.sub(r"^LimitFPS = .*", "LimitFPS = " + ("false" if fast else "true"), config.read_text(), flags=re.M))
        (self.directory / "control.json").unlink(missing_ok=True)
        gateway = self.local.compose("ps", "-q", "gateway", capture=True, timeout=10).stdout.strip()
        child = {"LD_PRELOAD": "/support/qt-control.so"}
        if fault_at:
            child.update(LD_PRELOAD="/support/qt-control.so:/checks/libpersistence-faults.so", PWT_FAULT_ARMED="1",
                         PWT_FAULT_SAVE="/profile/platinum.sav", PWT_FAULT_AT=str(fault_at), PWT_FAULT_MODE=fault_mode)
            if fault_phase is not None: child["PWT_FAULT_JOURNAL_PHASE"] = str(fault_phase)
        script = "import json,os,sys; os.umask(0o077); os.environ.update(json.loads(sys.argv[1])); os.execv('/profile/melonDS',['/profile/melonDS','/profile/platinum.nds'])"
        arguments = ["run", "-d", "--name", self.container, "--label", stack.LABEL + "=" + self.local.manifest["project"],
                     "--label", OWNER_LABEL + "=" + self.name, "--network", "container:" + gateway, "--read-only", "--cap-drop", "ALL",
                     "--security-opt", "no-new-privileges:true", "--memory", "1g", "--pids-limit", "128", "--user", f"{os.getuid()}:{os.getgid()}",
                     "--tmpfs", "/tmp:rw,noexec,nosuid,nodev,size=128m", "--workdir", "/profile",
                     "--mount", f"type=bind,source={self.directory},target=/profile",
                     "--mount", f"type=bind,source={support},target=/support,readonly",
                     "--env", "QT_QPA_PLATFORM=offscreen", "--env", "SDL_AUDIODRIVER=dummy", "--env", "WT_CONTROL=/profile/control.json",
                     "--env", "XDG_CONFIG_HOME=/profile/config", "--env", "XDG_CACHE_HOME=/profile/cache", manifest["image"],
                     "python3", "/src/scripts/run-emulator.py", "--save", "/profile/platinum.sav", "--target", manifest["target"],
                     "--store-tool", "/checks/platinumwt-save-store", "--", "python3", "-c", script, json.dumps(child)]
        container = self.local.run(arguments, capture=True, timeout=20).stdout.strip()
        manifest["last_start"] = {"source_commit": stack.command(["git", "-C", str(stack.ROOT), "rev-parse", "HEAD"], capture=True).stdout.strip(),
                                  "source_dirty": bool(stack.command(["git", "-C", str(stack.ROOT), "status", "--porcelain"], capture=True).stdout.strip()),
                                  "fault_at": fault_at, "fault_mode": fault_mode, "fault_phase": fault_phase, "fast": fast,
                                  "control_sha256": digest((support / "qt-control.so").read_bytes()), "gateway": gateway, "container": container}
        stack.save_manifest(self.directory, manifest)

    def events(self, events):
        if not isinstance(events, list) or not 1 <= len(events) <= 100:
            raise ValueError("Use 1 to 100 test controls")
        if not self.owned_container() or not self.owned_container()["State"]["Running"]:
            raise ValueError("Profile is not running")
        actions = []
        for event in events:
            if not isinstance(event, dict) or type(event.get("ms", 0)) is not int or not 0 <= event.get("ms", 0) <= 300000:
                raise ValueError("Invalid control time")
            value = dict(event, ms=event.get("ms", 0))
            if value.get("op") == "key" and set(value) <= {"op", "key", "ms"} and value.get("key") in KEYS:
                value["key"] = KEYS[value["key"]]
            elif value.get("op") == "shot" and set(value) <= {"op", "name", "ms"} and re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", value.get("name", "")):
                value["path"] = "/profile/" + value.pop("name") + ".png"
            elif value.get("op") == "quit" and set(value) <= {"op", "ms"}: pass
            elif value.get("op") in ("menu", "text") and set(value) <= {"op", "text", "parent", "ms"} and isinstance(value.get("text"), str) and len(value["text"]) <= 512: pass
            else: raise ValueError("Unsupported test control")
            actions.append(value)
        path = self.directory / "control.json"
        if path.exists(): raise ValueError("Previous controls have not yet been consumed")
        temporary = self.directory / "control.new"
        with temporary.open("x") as output: json.dump(actions, output)
        temporary.replace(path)

    def kill(self):
        state = self.owned_container()
        if not state or not state["State"]["Running"]:
            raise ValueError("No running owned emulator")
        self.local.run(["kill", "--signal", "KILL", state["Id"]], capture=True, timeout=15)
        self.logs()

    def quit(self, timeout=30):
        self.events([{"op": "quit"}])
        self.wait_exit(expected=0, timeout=timeout)

    def wait_exit(self, *, expected, timeout=300):
        manifest = self.manifest()
        if not manifest.get("last_start"): raise ValueError("No recorded emulator run")
        deadline = time.monotonic() + timeout
        while True:
            state = self.owned_container()
            if not state or manifest["last_start"].get("container", state["Id"]) != state["Id"]:
                raise ValueError("Emulator instance changed before exit")
            if not state["State"]["Running"]: break
            if time.monotonic() >= deadline: raise TimeoutError("Emulator did not exit; profile retained")
            time.sleep(0.25)
        log = self.logs().read_text()
        if state["State"].get("OOMKilled") or state["State"]["ExitCode"] != expected:
            raise ValueError("Unexpected emulator exit; preserve its log")
        if expected == 74 and manifest["last_start"]["fault_mode"] == "error":
            if "Managed save failed; emulator stopped before continuing" not in log or (
                    manifest["last_start"]["fault_phase"] is not None and "PHASE_READY" not in log):
                raise ValueError("Save fault did not reach its selected checkpoint")
        manifest["last_start"]["observed_exit_code"] = expected
        stack.save_manifest(self.directory, manifest)

    def kill_at_fault(self, timeout):
        manifest = self.manifest()
        if not manifest.get("last_start", {}).get("fault_at") or manifest["last_start"]["fault_mode"] == "error":
            raise ValueError("No stopping fault was armed")
        deadline = time.monotonic() + timeout
        while True:
            state = self.owned_container()
            if not state or not state["State"]["Running"]:
                if state: self.logs()
                raise ValueError("Emulator exited before its fault marker")
            if manifest["last_start"].get("container", state["Id"]) != state["Id"]:
                raise ValueError("Emulator instance changed before its checkpoint")
            result = self.local.run(["logs", "--tail", "200", state["Id"]], capture=True, timeout=10)
            lines = result.stdout.splitlines() + result.stderr.splitlines()
            if "STOPPED" in lines:
                if manifest["last_start"]["fault_phase"] is not None and "PHASE_READY" not in lines:
                    raise ValueError("Stopped without the selected journal checkpoint")
                self.kill()
                manifest["last_start"]["observed_fault_and_sigkill"] = True
                stack.save_manifest(self.directory, manifest)
                return
            if time.monotonic() >= deadline: raise TimeoutError("Fault marker not reached; profile retained")
            time.sleep(0.25)

    def inspect(self):
        raw = (self.directory / "platinum.sav").read_bytes()
        entry = journal(raw)
        inventories = [VERIFY.inventory(raw, bank) for bank in (0, 0x40000)]
        return {"phase": struct.unpack_from("<H", entry, 6)[0], "journal_generation": struct.unpack_from("<Q", entry, 16)[0],
                "counter": struct.unpack_from("<Q", entry, 96)[0], "cursor": struct.unpack_from("<Q", entry, 104)[0],
                "save_sha256": digest(raw), "mirrors_equal": raw[0x30000:0x31000] == raw[0x70000:0x71000],
                "inventory": [{"party": len(party), "pc": len(boxes), "multiset_sha256": digest(b"".join(sorted(party + boxes)))} for party, boxes in inventories]}

    def snapshot(self, name):
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", name): raise ValueError("Invalid snapshot name")
        with private_lock(Path(str(self.directory / "platinum.sav") + ".platinumwt.lock"), fcntl.LOCK_SH, create=False):
            return self._snapshot(name)

    def _snapshot(self, name):
        state = self.owned_container()
        if state and state["State"]["Running"]: raise ValueError("Close the emulator before a checked snapshot")
        manifest = self.manifest()
        raw = (self.directory / "platinum.sav").read_bytes()
        _, generation = VERIFY.host(self.directory / "platinum.sav", raw)
        status = self.inspect()
        destination = self.directory / "snapshots" / name
        if destination.resolve() != destination: raise ValueError("Snapshots directory must not be linked")
        destination.mkdir(mode=0o700, parents=True)
        hashes = {}
        for item, path in save_files(self.directory).items():
            if path.is_symlink(): raise ValueError("Preserve linked snapshot inputs")
            data = path.read_bytes()
            write_private(destination / item, data)
            hashes[item] = digest(data)
        stack.save_manifest(destination, {"version": 1, "profile": manifest, "state": status,
                                          "host_generation": generation, "files": hashes})
        return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--profile", required=True)
    sub = parser.add_subparsers(dest="action", required=True)
    create = sub.add_parser("create")
    create.add_argument("--save", type=Path, required=True); create.add_argument("--rom", type=Path, required=True)
    create.add_argument("--target", choices=("us-rev0", "us-rev1"), required=True)
    create.add_argument("--image", default="platinumwt-emulator-builder")
    fork = sub.add_parser("fork", help="copy another profile's checked snapshot, sharing its identity")
    fork.add_argument("--source", required=True); fork.add_argument("--snapshot", required=True)
    start = sub.add_parser("start")
    start.add_argument("--fault-at", type=int, choices=range(1, 21)); start.add_argument("--fault-mode", choices=("before", "after", "error"), default="before")
    start.add_argument("--fault-phase", type=int, choices=range(10)); start.add_argument("--fast", action="store_true")
    events = sub.add_parser("events"); events.add_argument("json")
    snapshot = sub.add_parser("snapshot"); snapshot.add_argument("name")
    fault = sub.add_parser("kill-at-fault"); fault.add_argument("--timeout", type=int, choices=range(1, 301), default=180)
    exited = sub.add_parser("wait-exit"); exited.add_argument("--expected", type=int, choices=range(256), required=True)
    exited.add_argument("--timeout", type=int, choices=range(1, 601), default=300)
    for name in ("inspect", "logs", "kill", "quit"): sub.add_parser(name)
    args = parser.parse_args()
    os.umask(0o077)
    workspace = stack.workspace_path(args.workspace)
    with ExitStack() as locks:
        locks.enter_context(private_lock(workspace / ".lock", fcntl.LOCK_SH))
        emulator = Emulator(stack.Stack(workspace, stack.read_manifest(workspace)), args.profile)
        if args.action not in ("inspect", "logs"):
            locks.enter_context(private_lock(workspace / (".emulator-" + args.profile + ".lock"), fcntl.LOCK_EX))
        if args.action == "create": emulator.create(args.save, args.rom, args.target, args.image)
        elif args.action == "fork": emulator.fork(args.source, args.snapshot)
        elif args.action == "start":
            if args.fault_phase is not None and args.fault_at is None: parser.error("--fault-phase requires --fault-at")
            emulator.start(fault_at=args.fault_at, fault_mode=args.fault_mode, fault_phase=args.fault_phase, fast=args.fast)
        elif args.action == "events": emulator.events(json.loads(args.json))
        elif args.action == "inspect": print(json.dumps(emulator.inspect()))
        elif args.action == "snapshot": print(emulator.snapshot(args.name))
        elif args.action == "kill": emulator.kill()
        elif args.action == "quit": emulator.quit()
        elif args.action == "wait-exit": emulator.wait_exit(expected=args.expected, timeout=args.timeout)
        elif args.action == "kill-at-fault": emulator.kill_at_fault(args.timeout)
        elif args.action == "logs": print(emulator.logs())


if __name__ == "__main__":
    try: main()
    except (ValueError, OSError, stack.subprocess.SubprocessError) as error:
        raise SystemExit("Emulator test failed; retained profile: " + str(error))
