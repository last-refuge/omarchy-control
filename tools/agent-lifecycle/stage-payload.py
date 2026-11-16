#!/usr/bin/python3
"""Stage the shared agent-integration payload into an explicit package root.

Used by production and fixture PKGBUILDs.
There are no writes to user homes and no service-manager/package operations.
"""

import argparse
from pathlib import Path
import shutil
import sys

SOURCE = Path(__file__).resolve().parents[2]


def payload(binary):
    source = Path(__file__).resolve().parent
    return {
        "usr/bin/omarchy-control": (Path(binary), 0o755),
        "usr/lib/omarchy-control/agent-integration/session-notify": (source / "session-notify.py", 0o755),
        "usr/lib/omarchy-control/agent-integration/user-recovery": (source / "user-recovery.py", 0o755),
        "usr/lib/systemd/user/omarchy-control-agent-reconcile.service": (source / "omarchy-control-agent-reconcile.service", 0o644),
        "usr/lib/systemd/user/omarchy-control-agent-remove.service": (source / "omarchy-control-agent-remove.service", 0o644),
        "usr/share/libalpm/hooks/90-omarchy-control-agent-install.hook": (source / "90-omarchy-control-agent-install.hook", 0o644),
        "usr/share/libalpm/hooks/90-omarchy-control-agent-remove.hook": (source / "90-omarchy-control-agent-remove.hook", 0o644),
        "usr/share/omarchy-control/agent-skills/omarchy-control/SKILL.md": (SOURCE / "src/data/skill.md", 0o644),
    }


def stage(destination, binary):
    destination = Path(destination)
    if not destination.is_absolute() or destination.resolve() == Path("/") or destination.is_symlink():
        raise ValueError("select an absolute package staging directory, never the live root")
    mapping = payload(binary)
    wanted = [destination / ("usr/lib/systemd/user/" + target + ".wants/omarchy-control-agent-reconcile.service")
              for target in ("default.target", "graphical-session.target")]
    for relative in [*mapping, *(str(path.relative_to(destination)) for path in wanted)]:
        target = destination / relative
        for parent in [target, *target.parents]:
            if parent == destination:
                break
            if parent.is_symlink():
                raise ValueError("a package staging path is linked: " + str(parent))
        if target.exists():
            raise ValueError("package staging destination already exists: " + str(target))
    for relative, (origin, mode) in mapping.items():
        if not origin.is_file() or origin.is_symlink():
            raise ValueError("missing or linked payload source: " + str(origin))
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
        target.chmod(mode)
    for path in wanted:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to("../omarchy-control-agent-reconcile.service")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    try:
        stage(args.destination, args.binary)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
