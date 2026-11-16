#!/usr/bin/python3
"""Exercise real makepkg/pacman hooks in the disposable native systemd lab.

Default fixture packages carry the production binary and shared payload. An
isolated --production-source builds the actual recipe and declared dependencies.
The lab exercises managers and graphical target ordering, not a visible desktop,
complete dependency pruning or model behavior.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import subprocess
import tempfile
import time

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[1]


def run(argv, **kwargs):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=150, check=True, **kwargs)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_version(root):
    versions = re.findall(r"^pkgver=([0-9]+\.[0-9]+\.[0-9]+)$", (root / "pkgbuild/PKGBUILD").read_text(), re.M)
    if len(versions) != 1:
        raise ValueError("the source recipe must declare one concrete release version")
    return versions[0]


def notifications(output):
    """Read hook results even when libalpm splits a long JSON output line.

    libalpm forwards script output in bounded chunks, inserting line breaks
    inside JSON strings as well as between records. Our helper escapes actual
    newlines, so joining these physical continuations restores its record.
    """
    messages, pending = [], ""
    for line in output.splitlines():
        if not pending and not line.startswith("{"):
            continue
        pending += line
        if len(pending) > 1024 * 1024:
            raise ValueError("package hook result exceeds the fixture output bound")
        try:
            body = json.loads(pending)
        except ValueError:
            continue
        pending = ""
        if isinstance(body, dict) and (body.get("protocolVersion") == 1 or body.get("prototype") is True) and isinstance(body.get("users"), list):
            messages.append(body)
    if pending:
        raise ValueError("incomplete package hook result")
    return messages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--production-source", type=Path, help="isolated source copy under this checkout's build directory")
    args = parser.parse_args()
    if (os.getuid() != 0 or os.environ.get("OC_AGENT_LIFECYCLE_LAB") != "1"
            or not Path("/.dockerenv").exists() or Path("/proc/1/comm").read_text().strip() != "systemd"):
        parser.error("use an explicitly enabled disposable Docker systemd lab")
    mounts = [line.split() for line in Path("/proc/self/mountinfo").read_text().splitlines()]
    if not any(row[4] == "/proc/sys" and "ro" in row[5].split(",") for row in mounts):
        parser.error("the lab must mount /proc/sys read-only before systemd starts")
    for path in (Path("/usr/bin/omarchy-control"), Path("/usr/share/omarchy")):
        if path.exists() or path.is_symlink():
            parser.error("use a fresh lab without staged Control or Omarchy files")
    if args.production_source:
        args.production_source = args.production_source.resolve()
        if not args.production_source.is_relative_to(ROOT / "build") or not (args.production_source / "pkgbuild/PKGBUILD").is_file():
            parser.error("production source must be an isolated copy below the checkout's build directory")
    args.output.mkdir(parents=True, exist_ok=True)
    version = source_version(args.production_source or ROOT)
    checks, transactions = [], []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)

    accounts = []
    for uid, name in [(1000, "active"), (1001, "inactive"), (1002, "optedout"), (1003, "custom"),
                      (1004, "editedruntime"), (1005, "editedregistration"), (1006, "legacy")]:
        run(["useradd", "-m", "-u", uid, name])
        accounts.append(pwd.getpwnam(name))
    def user(account, argv, expected=0):
        env = {"HOME": account.pw_dir, "PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "OMARCHY_PATH": "/usr/share/omarchy",
               "XDG_RUNTIME_DIR": f"/run/user/{account.pw_uid}"}
        result = subprocess.run([str(a) for a in argv], env=env, user=account.pw_uid, group=account.pw_gid,
                                extra_groups=[], capture_output=True, text=True, timeout=65)
        if result.returncode != expected:
            raise RuntimeError("unexpected user result: " + result.stdout + result.stderr)
        return result
    def api(account, *arguments, binary="/usr/bin/omarchy-control"):
        return json.loads(user(account, [binary, "skill", *arguments, "--api-version", "1"]).stdout)
    def receipt(account):
        return json.loads((Path(account.pw_dir) / ".local/state/omarchy-control/agent-lifecycle/state.json").read_text())
    def wait_for(test, label):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if test():
                return
            time.sleep(0.1)
        raise AssertionError(label)

    work = Path(tempfile.mkdtemp(prefix="oc-real-pacman-"))
    builder = pwd.getpwnam("builder")
    work.chmod(0o755)
    os.chown(work, builder.pw_uid, builder.pw_gid)
    config = work / "pacman.conf"
    config.write_text("[options]\nArchitecture = auto\nSigLevel = Never\nLocalFileSigLevel = Never\nLogFile = " + str(args.output / "pacman.log") + "\n")
    makeconfig = work / "makepkg.conf"
    makeconfig.write_text("source /etc/makepkg.conf\nCARCH=" + shlex.quote(os.uname().machine) + "\nPKGEXT='.pkg.tar.zst'\n")
    def package(release):
        if args.production_source:
            directory = args.production_source / (".lifecycle-package-" + str(release))
            directory.mkdir()
            recipe = args.production_source / "pkgbuild/PKGBUILD"
            text = re.sub(r"(?m)^pkgrel=.*$", "pkgrel=" + str(release), recipe.read_text())
            (directory / "PKGBUILD").write_text(text)
            for asset in (recipe.parent).iterdir():
                if asset.is_file() and asset.name != "PKGBUILD" and not asset.name.endswith(".pkg.tar.zst"):
                    shutil.copyfile(asset, directory / asset.name)
            for path in (directory, *directory.iterdir()): os.chown(path, builder.pw_uid, builder.pw_gid)
            result = subprocess.run(["makepkg", "--config", str(makeconfig), "--noconfirm"], cwd=directory,
                                    env=dict(os.environ, HOME=builder.pw_dir), user=builder.pw_uid, group=builder.pw_gid,
                                    extra_groups=[], capture_output=True, text=True, timeout=300)
            (args.output / f"build-{release}.log").write_text(result.stdout + result.stderr)
            result.check_returncode()
            return next(directory.glob("*.pkg.tar.zst"))
        directory = work / ("version-" + str(release))
        directory.mkdir()
        binary = directory / "control"
        # The executable is unchanged, but a trailing inert fixture stamp makes
        # per-user runtime replacement observable across package versions.
        binary.write_bytes(args.binary.read_bytes() + f"\npackage-lifecycle-fixture-{release}\n".encode())
        binary.chmod(0o755)
        text = "\n".join([
            "pkgname=omarchy-control", f"pkgver={version}", f"pkgrel={release}",
            "pkgdesc='Unshipped Control lifecycle fixture'", "arch=('" + os.uname().machine + "')", "license=('MIT')",
            "options=('!strip' '!debug')", "source=()", "sha256sums=()", "package() {",
            "  /usr/bin/python3 " + shlex.quote(str(SOURCE / "stage-payload.py"))
            + ' --destination "$pkgdir" --binary "$startdir/control"', "}", ""])
        (directory / "PKGBUILD").write_text(text)
        for path in (directory, binary, directory / "PKGBUILD"):
            os.chown(path, builder.pw_uid, builder.pw_gid)
        result = run(["makepkg", "--config", makeconfig, "--nodeps", "--noconfirm"], cwd=directory,
                     env=dict(os.environ, HOME=builder.pw_dir), user=builder.pw_uid, group=builder.pw_gid, extra_groups=[])
        (args.output / f"build-{release}.log").write_text(result.stdout + result.stderr)
        return next(directory.glob("*.pkg.tar.zst"))

    def pacman(label, *arguments):
        result = run(["pacman", "--config", config, "--noconfirm", *arguments])
        (args.output / (label + ".log")).write_text(result.stdout + result.stderr)
        messages = notifications(result.stdout)
        transactions.append({"action": label, "returncode": result.returncode, "notifications": messages})
        return messages

    stock = Path("/usr/share/omarchy/default/agents/skills/omarchy")
    stock.mkdir(parents=True)
    shutil.copyfile(ROOT / "tests/fixtures/agent-skills/omarchy/SKILL.md", stock / "SKILL.md")
    shutil.copyfile(ROOT / "tests/fixtures/agent-skills/omarchy/plugins.md", stock / "plugins.md")
    prepare = """from pathlib import Path
import sys
h=Path.home()
for suffix in ['.agents/skills','.claude/skills','.codex/skills']:
    root=h/suffix;root.mkdir(parents=True);(root/'omarchy').symlink_to('/usr/share/omarchy/default/agents/skills/omarchy')
if sys.argv[1]=='custom':
    p=h/'.claude/skills/omarchy';p.unlink();p.mkdir();(p/'SKILL.md').write_text('user custom skill')
"""
    try:
        first, second = package(1), package(2)
        for account in accounts:
            user(account, ["/usr/bin/python3", "-c", prepare, account.pw_name])
        api(accounts[2], "opt-out", binary=args.binary)
        api(accounts[6], "install", binary=args.binary)
        for account in (accounts[0], accounts[2], accounts[3], accounts[4], accounts[5]):
            run(["systemctl", "start", f"user@{account.pw_uid}.service"])
        installed = pacman("install", "-U", first)
        check(bool(installed), "first install executes the self-shipped post-transaction hook")
        states = {row["uid"]: row["state"] for row in installed[-1]["users"]}
        check(states[1000] == "service_completed" and states[1003] == "unresolved", "per-user custom conflicts are visible without aborting other activations")
        check(set(states) == {1000, 1002, 1003, 1004, 1005}, "package hook contacts running regular-user managers only")
        check(api(accounts[0], "status")["data"]["registrationComplete"], "already-running user activates during the real install transaction")
        check(receipt(accounts[0])["phase"] == "registered", "real install prepares the inactive-user recovery runtime")
        retained = Path(accounts[0].pw_dir) / ".local/share/omarchy-control/agent-runtime/control"
        active_link = Path(accounts[0].pw_dir) / ".agents/skills/omarchy"
        check(retained.stat().st_uid == 1000 and active_link.lstat().st_uid == 1000,
              "package-triggered runtime and skill writes belong to the actual user")
        check(api(accounts[2], "status")["data"]["optedOut"], "real installation preserves prior opt-out")
        check(not (Path(accounts[2].pw_dir) / ".local/share/omarchy-control/agent-runtime/control").exists(), "opted-out user receives no retained runtime")
        check((Path(accounts[3].pw_dir) / ".claude/skills/omarchy/SKILL.md").read_text() == "user custom skill", "package install preserves custom skills")
        canonical = Path("/usr/share/omarchy-control/agent-skills/omarchy-control/SKILL.md")
        check(canonical.read_bytes() == (Path(accounts[0].pw_dir) / ".agents/skills/omarchy-control/SKILL.md").read_bytes(), "canonical package skill matches the production binary's registered skill")
        owned = run(["pacman", "--config", config, "-Qo", canonical]).stdout
        check("omarchy-control" in owned, "pacman owns the canonical skill payload")
        check(not (Path(accounts[1].pw_dir) / ".local/share/omarchy-control").exists(), "package install does not write an inactive user's home as root")
        run(["systemctl", "start", "user@1001.service"])
        wait_for(lambda: api(accounts[1], "status")["data"]["registrationComplete"], "next-login activation did not finish")
        check(True, "new user-manager start activates the installed package unit")
        check(api(accounts[0], "status")["data"]["automaticActivation"], "status detects root-owned activation payload separately from harness loading")
        # A real graphical target transaction runs after late environment import.
        # Changing the stock root makes this second reconciliation observable.
        alternate = Path(accounts[1].pw_dir) / "late-omarchy"
        prepare_alternate = "import pathlib,shutil; p=pathlib.Path.home()/'late-omarchy/default/agents/skills/omarchy'; p.parent.mkdir(parents=True); shutil.copytree('/usr/share/omarchy/default/agents/skills/omarchy',p)"
        user(accounts[1], ["/usr/bin/python3", "-c", prepare_alternate])
        manager = ["systemctl", "--user", "--machine=1001@.host"]
        run([*manager, "set-environment", "OMARCHY_PATH=" + str(alternate)])
        session_unit = "[Unit]\nDescription=Disposable graphical-session test\nBindsTo=graphical-session.target\nBefore=graphical-session.target\n\n[Service]\nType=oneshot\nExecStart=/usr/bin/true\nRemainAfterExit=yes\n"
        prepare_session = "from pathlib import Path; p=Path.home()/'.config/systemd/user/oc-agent-test-session.service'; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(" + repr(session_unit) + ")"
        user(accounts[1], ["/usr/bin/python3", "-c", prepare_session])
        run([*manager, "daemon-reload"])
        run([*manager, "start", "oc-agent-test-session.service"])
        upstream = Path(accounts[1].pw_dir) / ".local/share/omarchy-control/agent-skills/omarchy/.upstream"
        wait_for(lambda: upstream.is_symlink() and os.readlink(upstream) == str(alternate / "default/agents/skills/omarchy"), "graphical target did not reconcile the late environment")
        check(True, "graphical-session target reconciles after late user-manager environment import")
        run([*manager, "stop", "oc-agent-test-session.service"])
        run([*manager, "set-environment", "OMARCHY_PATH=/usr/share/omarchy"])
        run([*manager, "restart", "omarchy-control-agent-reconcile.service"])
        wait_for(lambda: api(accounts[1], "status")["data"]["registrationComplete"], "restoring the normal stock root did not reconcile")
        first_hash = digest(retained)
        upgrade = pacman("upgrade", "-U", second)
        check(bool(upgrade), "real upgrade executes the post-transaction hook")
        check(run(["pacman", "--config", config, "-Q", "omarchy-control"]).stdout.strip() == f"omarchy-control {version}-2", "pacman database records upgraded package version")
        check(digest(retained) == digest(Path("/usr/bin/omarchy-control")) and (args.production_source or digest(retained) != first_hash), "upgrade reconciles the retained binary to the installed package")
        check(api(accounts[2], "status")["data"]["optedOut"], "upgrade retains opt-out")
        downgrade = pacman("downgrade", "-U", first)
        check(bool(downgrade), "same-API downgrade executes the upgrade trigger")
        check(digest(retained) == first_hash, "same-API downgrade reconciles its retained binary")
        pacman("upgrade-again", "-U", second)
        # Preserve an explicit user edit to a private runtime across another
        # package upgrade. Root must not overwrite it to force readiness.
        edit_runtime = """from pathlib import Path
import shutil
h=Path.home();p=h/'.local/share/omarchy-control/agent-runtime/control';shutil.copyfile(p,h/'known-runtime');p.write_bytes(p.read_bytes()+b'edited')
"""
        user(accounts[4], ["/usr/bin/python3", "-c", edit_runtime])
        corrupt = Path(accounts[4].pw_dir) / ".local/share/omarchy-control/agent-runtime/control"
        edited_hash = digest(corrupt)
        pacman("upgrade-with-edit", "-U", second)
        check(digest(corrupt) == edited_hash, "package reinstall preserves an edited retained binary")
        # Force a real partial CLI removal and inspect the wrapper's outcome.
        edit_registration = """from pathlib import Path
h=Path.home();custom=h/'personal';custom.mkdir();(custom/'SKILL.md').write_text('keep');p=h/'.agents/skills/omarchy';p.unlink();p.symlink_to(custom)
"""
        user(accounts[5], ["/usr/bin/python3", "-c", edit_registration])
        partial = json.loads(user(accounts[5], ["/usr/bin/python3", "/usr/lib/omarchy-control/agent-integration/user-recovery", "remove"], expected=1).stdout)
        check(partial["outcome"] == "partial" and not partial["verified"], "wrapper reports a real unverified child write as possibly partial")
        run(["systemctl", "stop", "user@1001.service"])
        removed = pacman("remove", "-R", "omarchy-control")
        check(bool(removed), "real removal executes the pre-transaction hook before deleting its helper")
        check(not Path("/usr/bin/omarchy-control").exists() and not canonical.exists(), "pacman removes the owned package payload")
        check(os.readlink(active_link) == str(stock), "pre-removal restores an active user's stock link")
        check(receipt(accounts[0])["phase"] == "retired", "pre-removal retires an active user's private runtime")
        check(api(accounts[2], "status", binary=args.binary)["data"]["optedOut"], "package removal preserves the user's opt-out")
        check((Path(accounts[3].pw_dir) / ".claude/skills/omarchy/SKILL.md").read_text() == "user custom skill",
              "package removal preserves the custom stock-named skill")
        check(not os.path.lexists(Path("/usr/lib/systemd/user/default.target.wants/omarchy-control-agent-reconcile.service")), "package removal takes the global activation dependency")
        run(["systemctl", "start", "user@1001.service"])
        wait_for(lambda: receipt(accounts[1])["phase"] == "retired", "post-package inactive cleanup did not finish")
        check(os.readlink(Path(accounts[1].pw_dir) / ".agents/skills/omarchy") == str(stock), "inactive user cleans registrations after a real package removal")
        check(not os.path.lexists(Path(accounts[1].pw_dir) / ".config/systemd/user/default.target.wants/omarchy-control-agent-cleanup.timer"), "inactive-user cleanup removes its recovery timer link")
        check(digest(corrupt) == edited_hash, "removal leaves edited runtime content untouched")
        check((Path(accounts[4].pw_dir) / ".agents/skills/omarchy/.upstream/plugins.md").exists(), "unresolved runtime ownership retains stock fallback")
        restore_runtime = """from pathlib import Path
h=Path.home();p=h/'.local/share/omarchy-control/agent-runtime/control';p.write_bytes((h/'known-runtime').read_bytes())
"""
        user(accounts[4], ["/usr/bin/python3", "-c", restore_runtime])
        run(["systemctl", "--user", "--machine=1004@.host", "start", "omarchy-control-agent-cleanup.service"])
        check(receipt(accounts[4])["phase"] == "retired", "cleanup resumes after the reported runtime edit is resolved")
        restore_registration = """from pathlib import Path
h=Path.home();p=h/'.agents/skills/omarchy';p.unlink();p.symlink_to(h/'.local/share/omarchy-control/agent-skills/omarchy')
"""
        user(accounts[5], ["/usr/bin/python3", "-c", restore_registration])
        run(["systemctl", "--user", "--machine=1005@.host", "start", "omarchy-control-agent-cleanup.service"])
        check(receipt(accounts[5])["phase"] == "retired", "cleanup resumes after the reported registration edit is resolved")
        legacy = Path(accounts[6].pw_dir) / ".agents/skills/omarchy"
        check((legacy / ".upstream/plugins.md").exists(), "unprepared inactive legacy user retains usable stock fallback")
        check(not Path("/root/.claude/skills/omarchy-control").exists(), "package hooks never register root as the desktop user")
        report = {"ok": True, "checks": checks, "count": len(checks), "transactions": transactions, "packageVersion": version,
                  "pacmanVersion": run(["pacman", "--version"]).stdout.strip(),
                  "systemdVersion": run(["systemctl", "--version"]).stdout.splitlines()[0],
                  "distribution": Path("/etc/os-release").read_text().splitlines()[0],
                  "scope": "real makepkg/pacman transactions, installed-distribution libraries, user managers and graphical target ordering; no visible desktop/model tasks",
                  "productionRecipeTested": bool(args.production_source), "automaticActivationPackaged": True, "desktopOrModelTasksTested": False}
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    finally:
        for account in accounts:
            subprocess.run(["systemctl", "stop", f"user@{account.pw_uid}.service"], capture_output=True, timeout=30)


if __name__ == "__main__":
    main()
