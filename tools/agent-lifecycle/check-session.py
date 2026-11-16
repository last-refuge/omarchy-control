#!/usr/bin/python3
"""Run the prototype against real user managers in a disposable systemd container.

Requires OC_AGENT_LIFECYCLE_LAB=1, root, Docker and PID 1 systemd. This stages
experimental system files in that container only; neither PKGBUILD ships them.
"""

import argparse
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
import time


def run(argv, **kwargs):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=45, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    if (os.getuid() != 0 or os.environ.get("OC_AGENT_LIFECYCLE_LAB") != "1"
            or not Path("/.dockerenv").exists() or Path("/proc/1/comm").read_text().strip() != "systemd"):
        parser.error("run only in an explicitly enabled disposable Docker systemd lab")
    mounts = [line.split() for line in Path("/proc/self/mountinfo").read_text().splitlines()]
    if not any(row[4] == "/proc/sys" and "ro" in row[5].split(",") for row in mounts):
        parser.error("the disposable lab must mount /proc/sys read-only before systemd starts")
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    files = {
        Path("/usr/bin/omarchy-control"): args.binary,
        Path("/usr/lib/omarchy-control/agent-integration/session-notify"): source / "session-notify.py",
        Path("/usr/lib/omarchy-control/agent-integration/user-recovery"): source / "user-recovery.py",
        Path("/usr/lib/systemd/user/omarchy-control-agent-reconcile.service"): source / "omarchy-control-agent-reconcile.service",
        Path("/usr/lib/systemd/user/omarchy-control-agent-remove.service"): source / "omarchy-control-agent-remove.service",
    }
    wanted = Path("/usr/lib/systemd/user/default.target.wants/omarchy-control-agent-reconcile.service")
    if any(p.exists() or p.is_symlink() for p in [*files, wanted]):
        parser.error("prototype system destinations already exist; use a fresh container")
    accounts = []
    for uid, name in [(1000, "active"), (1001, "inactive"), (1002, "optedout"), (1003, "custom"),
                      (1004, "quoted"), (1005, "edited"), (1006, "transaction"), (1007, "interrupted")]:
        try:
            account = pwd.getpwnam(name)
            if account.pw_uid != uid:
                raise RuntimeError("lab account UID mismatch")
        except KeyError:
            extra = ["-d", "/home/quoted % $ data"] if name == "quoted" else []
            run(["useradd", "-m", "-u", str(uid), *extra, name])
            account = pwd.getpwnam(name)
        accounts.append(account)
    checks = []
    def check(condition, label):
        if not condition:
            raise AssertionError(label)
        checks.append(label)

    def user(account, arguments, expected=0):
        home = Path(account.pw_dir)
        env = {"HOME": str(home), "PATH": "/usr/bin:/bin", "LANG": "C.UTF-8",
               "OMARCHY_PATH": "/usr/share/omarchy", "XDG_RUNTIME_DIR": f"/run/user/{account.pw_uid}"}
        result = subprocess.run([str(a) for a in arguments], capture_output=True, text=True, timeout=45,
                                env=env, user=account.pw_uid, group=account.pw_gid, extra_groups=[])
        if result.returncode != expected:
            raise RuntimeError("Unexpected user command result: " + result.stdout + result.stderr)
        return result

    def api(account, *arguments):
        return json.loads(user(account, ["/usr/bin/omarchy-control", "skill", *arguments, "--api-version", "1"]).stdout)

    def notify(action, expected):
        child = subprocess.run(["/usr/bin/python3", str(source / "session-notify.py"), action],
                               capture_output=True, text=True, timeout=120)
        result = json.loads(child.stdout)
        check(child.returncode == expected, action + " notification reports scoped outcome")
        return result

    def recovery_status(account):
        state = Path(account.pw_dir) / ".local/state/omarchy-control/agent-lifecycle/state.json"
        return json.loads(state.read_text())

    def wait_for(predicate, label):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.1)
        raise AssertionError(label)

    def service_failed(uid):
        result = run(["systemctl", "--user", f"--machine={uid}@.host", "show", "omarchy-control-agent-cleanup.service", "--property=Result", "--value"])
        return result.stdout.strip() == "exit-code"

    stock = Path("/usr/share/omarchy/default/agents/skills/omarchy")
    if stock.exists():
        parser.error("lab stock fixture already exists; use a fresh container")
    stock.mkdir(parents=True)
    (stock / "SKILL.md").write_text("---\nname: omarchy\ndescription: Read-only lab stock skill.\n---\nSee [guide](plugins.md).\n")
    (stock / "plugins.md").write_text("Read-only stock guide fixture.\n")
    prepare = """from pathlib import Path
import sys
h=Path.home()
for suffix in ['.agents/skills','.claude/skills','.codex/skills']:
    root=h/suffix; root.mkdir(parents=True,exist_ok=True)
    link=root/'omarchy'
    if not link.exists(): link.symlink_to('/usr/share/omarchy/default/agents/skills/omarchy')
if sys.argv[1]=='custom':
    link=h/'.claude/skills/omarchy'; link.unlink(); link.mkdir()
    (link/'SKILL.md').write_text('custom user skill')
"""
    try:
        for account in accounts:
            user(account, ["/usr/bin/python3", "-c", prepare, account.pw_name])
        for account in accounts:
            run(["systemctl", "stop", f"user@{account.pw_uid}.service"])
        for account in [accounts[0], accounts[2], accounts[3]]:
            run(["systemctl", "start", f"user@{account.pw_uid}.service"])
        for destination, origin in files.items():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(origin, destination)
            destination.chmod(0o755 if destination in list(files)[:3] else 0o644)
        wanted.parent.mkdir(parents=True, exist_ok=True)
        wanted.symlink_to("../omarchy-control-agent-reconcile.service")
        api(accounts[2], "opt-out")
        before = Path(accounts[0].pw_dir) / ".agents/skills/omarchy"
        dry = json.loads(run(["/usr/bin/python3", str(source / "session-notify.py"), "install", "--dry-run"]).stdout)
        check([u["uid"] for u in dry["users"]] == [1000, 1002, 1003], "root bridge lists live managers, excluding inactive user")
        check(os.readlink(before) == str(stock), "notification planning does not register skills")
        installed = notify("install", 1)  # The custom skill makes one user's unit fail explicitly.
        check(next(u for u in installed["users"] if u["uid"] == 1000)["state"] == "service_completed", "already-running user manager activates without another login")
        check(api(accounts[0], "status")["data"]["registrationComplete"], "active user has verified registrations")
        check(recovery_status(accounts[0])["phase"] == "registered", "activation retains a verified user cleanup runtime")
        check(before.lstat().st_uid == 1000, "registration links belong to the user, not root")
        check(api(accounts[2], "status")["data"]["optedOut"], "activation preserves user opt-out")
        check(not (Path(accounts[2].pw_dir) / ".local/share/omarchy-control/agent-runtime/control").exists(), "opt-out does not install a deferred runtime")
        custom = Path(accounts[3].pw_dir) / ".claude/skills/omarchy/SKILL.md"
        check(custom.read_text() == "custom user skill", "activation preserves custom stock-named skill")
        check(not (Path(accounts[1].pw_dir) / ".local/share/omarchy-control/agent-skills").exists(), "inactive user home was not registered by root")
        run(["systemctl", "start", "user@1001.service"])
        wait_for(lambda: api(accounts[1], "status")["data"]["registrationComplete"], "next-manager-start activation did not finish")
        check(True, "next user-manager start activates through the global unit dependency")
        run(["systemctl", "start", "user@1004.service"])
        wait_for(lambda: api(accounts[4], "status")["data"]["registrationComplete"], "quoted-home activation did not finish")
        check(recovery_status(accounts[4])["phase"] == "registered", "runtime supports home paths with spaces, percent and dollar signs")
        for account in accounts[5:]:
            run(["systemctl", "start", f"user@{account.pw_uid}.service"])
            wait_for(lambda: api(account, "status")["data"]["registrationComplete"], "extra recovery-user activation did not finish")
        relink = """from pathlib import Path
p=Path.home()/'.agents/skills/omarchy';p.unlink();p.symlink_to('/usr/share/omarchy/default/agents/skills/omarchy')
"""
        user(accounts[0], ["/usr/bin/python3", "-c", relink])
        notify("upgrade", 1)
        check(api(accounts[0], "status")["data"]["registrationComplete"], "upgrade notification repairs an upstream relink")
        check(api(accounts[2], "status")["data"]["optedOut"], "upgrade does not reset opt-out")
        run(["systemctl", "stop", "user@1001.service"])
        run(["systemctl", "stop", "user@1004.service"])
        run(["systemctl", "stop", "user@1005.service", "user@1006.service", "user@1007.service"])
        edit = """from pathlib import Path
h=Path.home();p=h/'.agents/skills/omarchy';p.unlink();custom=h/'personal-skill';custom.mkdir();(custom/'SKILL.md').write_text('keep this');p.symlink_to(custom)
"""
        user(accounts[5], ["/usr/bin/python3", "-c", edit])
        notify("remove", 0)
        check(os.readlink(before) == str(stock), "active-user pre-removal restores stock links")
        check(custom.read_text() == "custom user skill", "pre-removal preserves custom user content")
        check(recovery_status(accounts[0])["phase"] == "retired", "active-user pre-removal retires owned recovery runtime")
        for path in [wanted, *files]:
            path.unlink()
        run(["systemctl", "start", "user@1001.service"])
        fallback = Path(accounts[1].pw_dir) / ".agents/skills/omarchy"
        wait_for(lambda: recovery_status(accounts[1])["phase"] == "retired", "deferred inactive-user cleanup did not finish")
        check(os.readlink(fallback) == str(stock), "inactive user restores stock links after package payload loss")
        check(not (Path(accounts[1].pw_dir) / ".agents/skills/omarchy-control").is_symlink(), "inactive user removes the owned companion registration")
        check(not (Path(accounts[1].pw_dir) / ".local/share/omarchy-control/agent-runtime/control").exists(), "deferred cleanup removes its retained binary")
        check(not os.path.lexists(Path(accounts[1].pw_dir) / ".config/systemd/user/default.target.wants/omarchy-control-agent-cleanup.timer"), "deferred cleanup removes its autostart timer link")
        # Only the private container library is hidden. Python and systemd do
        # not depend on Qt; the retained Control process must fail conservatively.
        dependencies = run(["ldd", args.binary]).stdout.splitlines()
        matches = [line.split()[2] for line in dependencies if line.split()[:2] == ["libQt6Qml.so.6", "=>"] and len(line.split()) >= 3]
        if len(matches) != 1 or not Path(matches[0]).is_absolute() or not Path(matches[0]).exists():
            raise RuntimeError("the disposable lab cannot identify the binary's Qt QML dependency")
        library = Path(matches[0])
        hidden = library.with_name(library.name + ".agent-recovery-test")
        library.rename(hidden)
        try:
            run(["systemctl", "start", "user@1004.service"])
            wait_for(lambda: service_failed(1004), "missing-library refusal did not finish")
            check(recovery_status(accounts[4])["phase"] == "registered", "missing runtime libraries retain the recovery state")
            quoted = Path(accounts[4].pw_dir) / ".agents/skills/omarchy"
            check((quoted / ".upstream/plugins.md").exists(), "missing runtime libraries preserve usable stock fallback")
        finally:
            hidden.rename(library)
        run(["systemctl", "--user", "--machine=1004@.host", "start", "omarchy-control-agent-cleanup.service"])
        check(recovery_status(accounts[4])["phase"] == "retired", "deferred cleanup resumes after its runtime dependency returns")
        check(os.readlink(quoted) == str(stock), "quoted-home cleanup restores stock links")
        run(["systemctl", "start", "user@1005.service"])
        wait_for(lambda: service_failed(1005), "edited registration refusal did not finish")
        edited_home = Path(accounts[5].pw_dir)
        check(os.readlink(edited_home / ".agents/skills/omarchy") == str(edited_home / "personal-skill"), "deferred engine preserves a user-edited registration")
        check((edited_home / ".local/share/omarchy-control/agent-runtime/control").exists(), "unverified registration cleanup keeps its retained runtime")
        check((edited_home / ".local/share/omarchy-control/agent-skills/omarchy/.upstream/plugins.md").exists(), "partial cleanup keeps upstream fallback references")
        restore = """from pathlib import Path
h=Path.home();p=h/'.agents/skills/omarchy';p.unlink();p.symlink_to(h/'.local/share/omarchy-control/agent-skills/omarchy')
"""
        user(accounts[5], ["/usr/bin/python3", "-c", restore])
        run(["systemctl", "--user", "--machine=1005@.host", "start", "omarchy-control-agent-cleanup.service"])
        check(recovery_status(accounts[5])["phase"] == "retired", "deferred cleanup resumes after a reported registration conflict is resolved")
        package_lock = Path("/var/lib/pacman/db.lck")
        package_lock.parent.mkdir(parents=True, exist_ok=True)
        package_lock.touch()
        try:
            run(["systemctl", "start", "user@1006.service"])
            def transaction_checked():
                result = run(["systemctl", "--user", "--machine=1006@.host", "show", "omarchy-control-agent-cleanup.service", "--property=ExecMainStartTimestampMonotonic", "--value"])
                return int(result.stdout.strip() or "0") > 0
            wait_for(transaction_checked, "package transaction deferral did not run")
            check(recovery_status(accounts[6])["phase"] == "registered", "package-manager lock defers registration removal")
            timer = run(["systemctl", "--user", "--machine=1006@.host", "is-active", "omarchy-control-agent-cleanup.timer"])
            check(timer.stdout.strip() == "active", "deferred transaction keeps its retry timer armed")
        finally:
            package_lock.unlink()
        run(["systemctl", "--user", "--machine=1006@.host", "start", "omarchy-control-agent-cleanup.service"])
        check(recovery_status(accounts[6])["phase"] == "retired", "cleanup succeeds after package transaction completion")
        deny_retirement = """from pathlib import Path
h=Path.home();(h/'.config/systemd/user/default.target.wants').chmod(0o500);(h/'.config/systemd/user').chmod(0o500)
"""
        user(accounts[7], ["/usr/bin/python3", "-c", deny_retirement])
        run(["systemctl", "start", "user@1007.service"])
        wait_for(lambda: service_failed(1007), "retirement interruption did not finish")
        interrupted_home = Path(accounts[7].pw_dir)
        check(recovery_status(accounts[7])["phase"] == "registrations_removed", "interrupted retirement records that registrations were already restored")
        check(not (interrupted_home / ".local/share/omarchy-control/agent-runtime/control").exists(), "retirement can resume without the copied binary")
        check((interrupted_home / ".config/systemd/user/default.target.wants/omarchy-control-agent-cleanup.timer").is_symlink(), "interrupted retirement preserves the next-login recovery trigger")
        check(os.readlink(interrupted_home / ".agents/skills/omarchy") == str(stock), "retirement failure leaves the primary stock skill usable")
        allow_retirement = """from pathlib import Path
h=Path.home();(h/'.config/systemd/user').chmod(0o700);(h/'.config/systemd/user/default.target.wants').chmod(0o700)
"""
        user(accounts[7], ["/usr/bin/python3", "-c", allow_retirement])
        run(["systemctl", "stop", "user@1007.service"])
        run(["systemctl", "start", "user@1007.service"])
        wait_for(lambda: recovery_status(accounts[7])["phase"] == "retired", "retirement did not resume after manager restart")
        check(True, "next user-manager start completes interrupted runtime retirement")
        check(not Path("/root/.claude/skills/omarchy-control").exists(), "root was not registered as a desktop user")
        print(json.dumps({"ok": True, "checks": checks, "count": len(checks),
                          "os": Path("/etc/os-release").read_text().splitlines()[0],
                          "systemd": run(["systemctl", "--version"]).stdout.splitlines()[0],
                          "scope": "real Linux systemd managers and production CLI; package hooks are staged, not a pacman transaction",
                          "inactiveUserUnregistration": "prepared user runtime verified after package-payload loss; legacy unprepared users remain a gate",
                          "harnessTaskExecution": "not exercised"}, indent=2))
    finally:
        for account in accounts:
            subprocess.run(["systemctl", "stop", f"user@{account.pw_uid}.service"], capture_output=True, timeout=20)
        for path in [wanted, *files]:
            if path.exists() or path.is_symlink():
                path.unlink()


if __name__ == "__main__":
    main()
