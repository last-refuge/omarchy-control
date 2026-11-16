#!/usr/bin/python3
"""Per-user cleanup runtime for package-loss recovery.

Registration cleanup is always delegated to a fingerprinted copy of Control.
This module owns only its private runtime and user service/timer files. It never
implements another parser/writer for the skill-registration manifest.
"""

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

LIMIT = 1024 * 1024
BINARY_LIMIT = 64 * LIMIT
UNIT = "omarchy-control-agent-cleanup"
PATH_KEYS = ("HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "OMARCHY_PATH",
             "CODEX_HOME", "CLAUDE_CONFIG_DIR", "PI_CODING_AGENT_DIR")
MISSING = {"type": "missing"}


class RecoveryError(Exception):
    def __init__(self, code, message, data=None):
        super().__init__(message)
        self.code = code
        self.data = data


def absolute(value):
    if not isinstance(value, str) or not value.startswith("/") or os.path.normpath(value) != value or re.search(r"[\x00-\x1f\x7f]", value):
        raise RecoveryError("unsafe_path", "Expected an absolute normalized path without control characters.")
    return Path(value)


def digest(contents):
    return hashlib.sha256(contents).hexdigest()


def file_state(contents):
    return {"type": "file", "sha256": digest(contents)}


def describe(path, limit=BINARY_LIMIT):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return MISSING
    if info.st_uid != os.getuid():
        return {"type": "unowned"}
    if stat.S_ISLNK(info.st_mode):
        return {"type": "link", "target": os.readlink(path)}
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > limit
            or info.st_mode & 0o022):
        return {"type": "conflict"}
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino, opened.st_size) != (info.st_dev, info.st_ino, info.st_size):
            raise RecoveryError("stale_path", "A runtime file changed while being opened.")
        contents = stream.read(limit + 1)
    if len(contents) > limit:
        raise RecoveryError("state_unavailable", "Runtime file exceeds its read limit.")
    return file_state(contents)


def quote(value, argument=False):
    # Unit specifiers and ExecStart dollar expansion are separate from ordinary
    # quoting. Environment= does not expand dollar signs.
    value = value.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")
    if argument:
        value = value.replace("$", "$$")
    return '"' + value + '"'


class Runtime:
    def __init__(self, environment=None, control="/usr/bin/omarchy-control", state_path=None,
                 runner=None, package_lock="/var/lib/pacman/db.lck"):
        env = environment if environment is not None else os.environ
        self.home = absolute(env.get("HOME", str(Path.home())))
        data = absolute(env.get("XDG_DATA_HOME") or str(self.home / ".local/share"))
        state = absolute(env.get("XDG_STATE_HOME") or str(self.home / ".local/state"))
        config = absolute(env.get("XDG_CONFIG_HOME") or str(self.home / ".config"))
        self.directory = data / "omarchy-control/agent-runtime"
        self.state_path = state / "omarchy-control/agent-lifecycle/state.json"
        if state_path is not None and absolute(str(state_path)) != self.state_path:
            raise RecoveryError("context_mismatch", "The recovery state does not match the current home/XDG context.")
        self.unit_dir = config / "systemd/user"
        self.binary = self.directory / "control"
        self.helper = self.directory / "recovery.py"
        self.service = self.unit_dir / (UNIT + ".service")
        self.timer = self.unit_dir / (UNIT + ".timer")
        self.wanted = self.unit_dir / "default.target.wants" / self.timer.name
        self.paths = (self.binary, self.helper, self.service, self.timer, self.wanted)
        self.control = absolute(str(control))
        self.package_lock = Path(package_lock)
        self.environment = {"HOME": str(self.home), "XDG_DATA_HOME": str(data), "XDG_STATE_HOME": str(state),
                            "XDG_CONFIG_HOME": str(config), "OMARCHY_PATH": env.get("OMARCHY_PATH") or "/usr/share/omarchy"}
        for key in PATH_KEYS:
            if key in env and env[key]:
                absolute(env[key])
                self.environment[key] = env[key]
        self.session_environment = {key: env[key] for key in ("XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS") if env.get(key)}
        self.session_environment.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
        self.runner = runner or self.invoke
        self.changed = False
        self.receipt = None
        self.state_before = None
        for path in (data, state, config, self.directory, self.state_path, self.unit_dir):
            self.safe(path)

    @staticmethod
    def invoke(argv, environment, timeout):
        return subprocess.run(argv, env=environment, capture_output=True, text=True, timeout=timeout, check=False)

    def safe(self, path, directory=False):
        absolute(str(path))
        if path != self.home and self.home not in path.parents:
            raise RecoveryError("unsafe_path", "Recovery paths must remain inside the user's home.")
        end = path if directory else path.parent
        if end != self.home and self.home not in end.parents:
            raise RecoveryError("unsafe_path", "Recovery paths must remain inside the user's home.")
        current = self.home
        for part in ("", *end.relative_to(self.home).parts):
            if part:
                current /= part
            try:
                info = current.lstat()
            except FileNotFoundError:
                continue
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
                raise RecoveryError("ownership_conflict", "Linked, shared or unowned runtime directory was preserved: " + str(current))

    def mkdir(self, directory):
        self.safe(directory, True)
        if not directory.exists():
            self.mkdir(directory.parent)
            directory.mkdir(mode=0o700)
        self.safe(directory, True)

    @contextlib.contextmanager
    def lock(self):
        if os.getuid() == 0 or os.geteuid() != os.getuid():
            raise RecoveryError("user_context_required", "Run recovery as the desktop user, without sudo.")
        self.mkdir(self.state_path.parent)
        path = self.state_path.parent / "runtime.lock"
        self.safe(path)
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or info.st_mode & 0o077:
                raise RecoveryError("ownership_conflict", "Unsafe recovery lock was preserved.")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RecoveryError("recovery_busy", "Another lifecycle action is running.") from None
            yield
        finally:
            os.close(fd)

    def load(self, refresh_environment=False):
        self.safe(self.state_path)
        self.state_before = describe(self.state_path, LIMIT)
        if self.state_before == MISSING:
            self.receipt = {"version": 1, "home": str(self.home), "environment": self.environment,
                            "controlPath": str(self.control), "phase": "new", "artifacts": {}}
            return
        if self.state_before.get("type") != "file":
            raise RecoveryError("state_unavailable", "Unsafe lifecycle state was preserved.")
        with self.state_path.open("rb") as stream:
            raw = stream.read(LIMIT + 1)
        if file_state(raw) != self.state_before:
            raise RecoveryError("state_unavailable", "Lifecycle state changed while being read.")
        try:
            receipt = json.loads(raw)
        except (ValueError, UnicodeError):
            raise RecoveryError("state_unavailable", "Malformed lifecycle state was preserved.") from None
        if (not isinstance(receipt, dict) or type(receipt.get("version")) is not int or receipt.get("version") != 1 or receipt.get("home") != str(self.home)
                or receipt.get("controlPath") != str(self.control)
                or receipt.get("phase") not in ("new", "prepared", "registered", "registrations_removed", "retired")
                or not isinstance(receipt.get("artifacts"), dict) or len(receipt["artifacts"]) > len(self.paths)):
            raise RecoveryError("state_unavailable", "Incompatible lifecycle state or home/XDG context was preserved.")
        recorded_environment = receipt.get("environment")
        storage = ("HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME")
        if (not isinstance(recorded_environment, dict) or set(recorded_environment) - set(PATH_KEYS)
                or any(recorded_environment.get(key) != self.environment.get(key) for key in storage)
                or "OMARCHY_PATH" not in recorded_environment):
            raise RecoveryError("state_unavailable", "Incompatible lifecycle storage context was preserved.")
        for value in recorded_environment.values(): absolute(value)
        if receipt["phase"] == "retired" and receipt["artifacts"]:
            raise RecoveryError("state_unavailable", "Retired lifecycle state still claims owned artifacts; inspect it before repair.")
        for name, entry in receipt["artifacts"].items():
            path = Path(name)
            if path not in self.paths or not isinstance(entry, dict) or "current" not in entry:
                raise RecoveryError("state_unavailable", "Unsupported lifecycle artifact was preserved.")
            self.safe(path)
            for key in ("current", "pending"):
                if key not in entry:
                    continue
                value = entry[key]
                valid = value == MISSING
                if isinstance(value, dict):
                    if path == self.wanted:
                        valid |= value == {"type": "link", "target": "../" + self.timer.name}
                    else:
                        sha = value.get("sha256")
                        valid |= value.get("type") == "file" and isinstance(sha, str) and bool(re.fullmatch(r"[0-9a-f]{64}", sha))
                if not valid:
                    raise RecoveryError("state_unavailable", "Malformed lifecycle ownership descriptor was preserved.")
        self.receipt = receipt
        # Graphical-session activation can import a new stock/harness context
        # after default.target. Storage paths and ownership stay fixed. Older
        # cleanup units use the recorded context if activation updated it.
        if refresh_environment:
            self.receipt["environment"] = self.environment
        else:
            self.environment = recorded_environment

    def replace(self, path, before, after, contents=b"", mode=0o600):
        self.safe(path)
        if describe(path) != before:
            raise RecoveryError("stale_path", "A runtime path changed before replacement: " + str(path))
        if after == MISSING:
            if before != MISSING:
                path.unlink()
                self.changed = True
        else:
            fd, temporary = tempfile.mkstemp(prefix=".oc-recovery-", dir=path.parent)
            try:
                os.fchmod(fd, mode)
                if after["type"] == "link":
                    os.close(fd)
                    fd = -1
                    os.unlink(temporary)
                    os.symlink(after["target"], temporary)
                else:
                    with os.fdopen(fd, "wb") as stream:
                        fd = -1
                        stream.write(contents)
                        stream.flush()
                        os.fsync(stream.fileno())
                self.safe(path)
                if describe(path) != before:
                    raise RecoveryError("stale_path", "A runtime path changed before commit: " + str(path))
                os.replace(temporary, path)
                self.changed = True
            finally:
                if fd >= 0:
                    os.close(fd)
                if os.path.lexists(temporary):
                    os.unlink(temporary)
        if describe(path) != after:
            raise RecoveryError("verification_failed", "Runtime readback did not match: " + str(path))

    def save(self):
        self.receipt["artifacts"] = self.receipt.get("artifacts", {})
        contents = (json.dumps(self.receipt, sort_keys=True, indent=2) + "\n").encode()
        if len(contents) > LIMIT:
            raise RecoveryError("state_unavailable", "Lifecycle state exceeds its size limit.")
        self.replace(self.state_path, self.state_before, file_state(contents), contents)
        self.state_before = file_state(contents)

    def owned(self, path, actual=None):
        entry = self.receipt["artifacts"].get(str(path), {})
        actual = describe(path) if actual is None else actual
        return actual == MISSING or actual == entry.get("current") or actual == entry.get("pending")

    def preflight(self):
        for path in self.paths:
            self.safe(path)
            if not self.owned(path):
                raise RecoveryError("ownership_conflict", "Custom or edited recovery artifact was preserved: " + str(path))

    def put(self, path, after, contents=b"", mode=0o600):
        self.safe(path)
        if not self.owned(path):
            raise RecoveryError("ownership_conflict", "Edited recovery artifact was preserved: " + str(path))
        self.mkdir(path.parent)
        before = describe(path)
        entry = self.receipt["artifacts"].get(str(path), {"current": MISSING})
        if before == after and entry.get("current") == after and "pending" not in entry:
            return
        entry["pending"] = after
        self.receipt["artifacts"][str(path)] = entry
        self.save()
        self.replace(path, before, after, contents, mode)
        self.receipt["artifacts"][str(path)] = {"current": after}
        self.save()

    def child_environment(self):
        return dict(self.environment, **self.session_environment, PATH="/usr/bin:/bin", LANG="C.UTF-8")

    def cli(self, binary, action):
        # A child writer can change registrations before returning a failure,
        # malformed response or timeout. Never label that attempt as no-effect.
        if action in ("reconcile", "remove"):
            self.changed = True
        try:
            result = self.runner([str(binary), "skill", action, "--api-version", "1"], self.child_environment(), 35)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RecoveryError("cli_unavailable", "Control did not finish; ownership state and stock fallback were retained: " + str(error)) from None
        if len(result.stdout.encode()) > LIMIT or len(result.stderr.encode()) > LIMIT:
            raise RecoveryError("unverified_result", "Control output exceeded its limit; inspect current state.")
        try:
            body = json.loads(result.stdout)
        except ValueError:
            raise RecoveryError("unverified_result", "Control returned no valid API response; runtime libraries may be unavailable. Nothing was retired.") from None
        if not isinstance(body, dict):
            raise RecoveryError("unverified_result", "Control returned an invalid response shape; nothing was retired.")
        if result.returncode or body.get("apiVersion") != 1 or body.get("ok") is not True:
            raise RecoveryError("registration_unresolved", "Control did not confirm the requested operation; inspect its state before retrying.", body)
        if action == "remove" and (body.get("outcome") != "removed" or body.get("verified") is not True):
            raise RecoveryError("unverified_result", "Control did not verify removal; fallback and runtime were retained.")
        return body

    def manager(self, *arguments):
        try:
            result = self.runner(["/usr/bin/systemctl", "--user", "--no-ask-password", *arguments], self.child_environment(), 10)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RecoveryError("user_manager_unavailable", str(error)) from None
        if result.returncode:
            raise RecoveryError("user_manager_unavailable", "The user manager did not confirm: " + " ".join(arguments))

    def units(self):
        environment = "\n".join("Environment=" + quote(key + "=" + value) for key, value in sorted(self.environment.items()))
        service = ("[Unit]\nDescription=Retire owned Omarchy Control agent registrations after package loss\nConditionUser=!root\n\n"
                   "[Service]\nType=oneshot\nExecStart=/usr/bin/python3 " + quote(str(self.helper), True)
                   + " cleanup --state " + quote(str(self.state_path), True) + " --control " + quote(str(self.control), True)
                   + "\n" + environment + "\nTimeoutStartSec=60\nNoNewPrivileges=yes\nUMask=0077\n")
        timer = ("[Unit]\nDescription=Check deferred Omarchy Control agent cleanup\nConditionUser=!root\n\n"
                 "[Timer]\nOnStartupSec=5s\nOnUnitInactiveSec=5min\nAccuracySec=1s\nUnit=" + self.service.name
                 + "\n\n[Install]\nWantedBy=default.target\n")
        return service.encode(), timer.encode()

    def prepare(self, allow_deferred_manager=False):
        self.preflight()
        info = self.control.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_mode & 0o022 or info.st_size > BINARY_LIMIT:
            raise RecoveryError("unsafe_binary", "The installed Control binary is not a safe regular file.")
        with self.control.open("rb") as stream:
            binary = stream.read(BINARY_LIMIT + 1)
        if not binary.startswith(b"\x7fELF") or len(binary) > BINARY_LIMIT:
            raise RecoveryError("unsafe_binary", "Deferred recovery requires the installed Linux Control binary.")
        after = self.control.lstat()
        if (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (
                info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns):
            raise RecoveryError("stale_path", "The installed binary changed during capture.")
        helper = Path(__file__).read_bytes()
        service, timer = self.units()
        for path, contents, mode in [(self.binary, binary, 0o700), (self.helper, helper, 0o600),
                                     (self.service, service, 0o600), (self.timer, timer, 0o600)]:
            self.put(path, file_state(contents), contents, mode)
        self.put(self.wanted, {"type": "link", "target": "../" + self.timer.name})
        self.receipt["phase"] = "prepared"
        self.save()
        try:
            self.manager("daemon-reload")
            self.manager("start", self.timer.name)
        except RecoveryError as error:
            if not allow_deferred_manager or error.code != "user_manager_unavailable":
                raise
            # The complete owned service/timer and default.target link are
            # already durable. A future manager start can load them without a
            # live daemon-reload; registration itself requires no user bus.
            return False
        return True

    def retire(self):
        self.preflight()
        # Stop scheduling before removing its owned definitions. If interrupted
        # during retirement, registrations are already restored; reinstall or
        # explicit recovery can finish any remaining private artifacts.
        self.manager("stop", self.timer.name)
        # Retire the large binary first. The durable phase allows retries
        # without it, while the helper and scheduling definitions still exist.
        for path in (self.binary, self.wanted, self.timer, self.service, self.helper):
            if str(path) not in self.receipt["artifacts"]:
                continue
            self.put(path, MISSING)
            self.receipt["artifacts"].pop(str(path))
            self.save()
        self.receipt["phase"] = "retired"
        self.save()
        self.manager("daemon-reload")

    def report(self, outcome, verified=False, error=None):
        return {"protocolVersion": 1, "prototype": False, "ok": error is None, "outcome": outcome, "verified": verified,
                "phase": self.receipt.get("phase") if self.receipt else None, "statePath": str(self.state_path),
                "artifacts": [{"path": str(path), "current": describe(path), "owned": self.owned(path)}
                              for path in self.paths] if self.receipt else [],
                "error": error, "scope": "owned registration links plus private recovery runtime; no harness/model verification",
                "runtimeDependencies": "The retained Control binary still requires its Linux shared libraries and /usr/bin/python3."}

    def package_present(self):
        return os.path.lexists(self.control) or os.path.lexists(self.package_lock)

    def run(self, action):
        if action == "status":
            self.load()
            return self.report("inspected")
        if os.getuid() == 0 or os.geteuid() != os.getuid():
            raise RecoveryError("user_context_required", "Run recovery as the desktop user, without sudo.")
        # A newly started timer can fire while activation still holds the
        # lifecycle lock. Package presence needs no write or lock acquisition.
        if action == "cleanup" and self.package_present():
            return self.report("deferred")
        with self.lock():
            self.load(refresh_environment=action == "activate")
            if action == "activate":
                status = self.cli(self.control, "status").get("data")
                if not isinstance(status, dict) or not isinstance(status.get("optedOut"), bool):
                    raise RecoveryError("unverified_result", "Control did not report a known opt-out preference.")
                if status["optedOut"]:
                    return self.report("opted_out")
                manager_ready = self.prepare(allow_deferred_manager=True)
                response = self.cli(self.control, "reconcile")
                if not response.get("verified"):
                    raise RecoveryError("unverified_result", "Reconciliation was not verified; recovery runtime was retained.")
                if not isinstance(response.get("data"), dict) or response["data"].get("registrationComplete") is not True:
                    raise RecoveryError("registration_incomplete", "Control did not establish complete stock-skill forwarding; recovery runtime was retained.", response)
                self.receipt["phase"] = "registered"
                self.save()
                result = self.report("activated", True)
                result["userManagerActivation"] = "confirmed" if manager_ready else "deferred"
                return result
            if action == "prepare":
                self.prepare()
                return self.report("prepared", True)
            if action not in ("cleanup", "remove"):
                raise RecoveryError("invalid_arguments", "Unsupported recovery action.")
            if self.receipt["phase"] == "retired":
                return self.report("retired", True)
            if action == "cleanup" and self.package_present():
                return self.report("deferred")
            self.preflight()
            if self.receipt["phase"] != "registrations_removed":
                binary = self.control if action == "remove" else self.binary
                if action == "cleanup" and (str(self.binary) not in self.receipt["artifacts"]
                                             or describe(self.binary).get("type") != "file"):
                    raise RecoveryError("runtime_unavailable", "No owned retained binary is available; fallback was preserved.")
                if action == "cleanup" and self.package_present():
                    return self.report("deferred")
                self.cli(binary, "remove")
                self.receipt["phase"] = "registrations_removed"
                self.save()
            if action == "cleanup" and self.package_present():
                return self.report("deferred_after_registration_removal")
            if self.receipt["artifacts"]:
                self.retire()
            else:
                self.receipt["phase"] = "retired"
                self.save()
            return self.report("retired", True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["status", "prepare", "activate", "cleanup", "remove"])
    parser.add_argument("--control", default="/usr/bin/omarchy-control")
    parser.add_argument("--state")
    args = parser.parse_args()
    runtime = None
    try:
        runtime = Runtime(control=args.control, state_path=args.state)
        result = runtime.run(args.action)
    except (RecoveryError, OSError, ValueError) as error:
        code = error.code if isinstance(error, RecoveryError) else "recovery_unavailable"
        result = {"protocolVersion": 1, "prototype": False, "ok": False, "outcome": "partial" if runtime and runtime.changed else "blocked",
                  "verified": False, "error": {"code": code, "message": str(error), "data": getattr(error, "data", None)}}
    print(json.dumps(result))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
