#!/usr/bin/env python3
"""Check GUI operation accounting independently of the CLI registry.

Reads source and reviewed metadata only. It never runs a desktop command.
"""

import argparse
import collections
import json
import pathlib
import re
import sys


def audit(root):
    operations = json.loads((root / "src/data/cli-operations.json").read_text())
    actual = set()
    for header in sorted((root / "src").glob("*.h")):
        owner = None
        for line in header.read_text().splitlines():
            match = re.match(r"class\s+(\w+)\s*:\s*public", line)
            if match:
                owner = match[1]
            match = re.search(r"Q_INVOKABLE\s+(?:static\s+)?[\w:<>&* ]+\s+(\w+)\s*\(", line)
            if "Q_INVOKABLE" in line and not line.lstrip().startswith("//") and not match:
                raise ValueError(f"Unrecognized invokable declaration in {header.name}: {line.strip()}")
            if match:
                if not owner:
                    raise ValueError(f"Cannot identify the class for {header.name}: {line}")
                actual.add(f"{owner}.{match[1]}")
    mapped = collections.Counter()
    problems = []
    ids = set()
    classifications = {"legacy-cli", "api-cli", "cli-gap", "ui-only"}
    for row in operations:
        if row["id"] in ids:
            problems.append(f"Duplicate operation id: {row['id']}")
        ids.add(row["id"])
        if row.get("classification") not in classifications or not row.get("reason"):
            problems.append(f"Missing classification or reason: {row['id']}")
        implemented = row.get("classification") in {"legacy-cli", "api-cli"}
        if implemented and not row.get("invocations"):
            problems.append(f"Missing invocation: {row['id']}")
        if not implemented and row.get("invocations"):
            problems.append(f"Unimplemented/excluded operation advertises a command: {row['id']}")
        if row.get("apiVersions") and not implemented:
            problems.append(f"An unavailable operation advertises an API version: {row['id']}")
        if row.get("classification") == "api-cli" and (row.get("apiVersions") != [1] or "parameters" not in row):
            problems.append(f"Missing API version or parameters: {row['id']}")
        mapped.update(row.get("guiMethods", []))
    problems += [f"Unclassified GUI method: {name}" for name in sorted(actual - mapped.keys())]
    problems += [f"Stale GUI method: {name}" for name in sorted(mapped.keys() - actual)]
    problems += [f"GUI method accounted for twice: {name}" for name, count in mapped.items() if count != 1]
    settings = json.loads((root / "src/data/settings.json").read_text())
    workflows = json.loads((root / "src/data/workflows.json").read_text())
    catalog_ids = set()
    launch_fields = {"page": "page", "config": "file", "url": "url", "panel": "target",
                     "exec": "argv", "app": "argv", "terminal": "command", "shell": "command"}
    for entry in settings + workflows:
        identifier = entry["id"]
        if identifier in catalog_ids:
            problems.append(f"Duplicate catalog id: {identifier}")
        catalog_ids.add(identifier)
    for entry in settings:
        if entry.get("read", {}).get("kind") not in {"cmd", "hypr", "exists", "blur", "filetext", "json", "symlink", "icons", "iconlayer", "toml", "blurstatus", "automatic-timezone"}:
            problems.append(f"Unclassified setting reader: {entry['id']}")
        if entry.get("write") and entry["write"].get("kind") not in {"cmd", "hypr", "json", "icons", "automatic-timezone"}:
            problems.append(f"Unclassified setting writer: {entry['id']}")
    for entry in workflows:
        launch = entry.get("launch", {})
        kind = launch.get("kind")
        if kind not in launch_fields or not launch.get(launch_fields[kind]):
            problems.append(f"Unclassified catalog destination: {entry['id']}")
        native = launch.get("nativeSetting")
        if native and not any(row["id"] == native and row.get("write") for row in settings):
            problems.append(f"Invalid native setting alternative: {entry['id']}")
        for operation in launch.get("nativeOperations", []):
            if operation not in ids:
                problems.append(f"Invalid native operation alternative: {entry['id']}")
        if kind in {"exec", "terminal", "shell"}:
            alternative = bool(native or launch.get("nativeOperations"))
            expected = "native-alternative" if alternative else "handoff"
            if launch.get("automationStatus") != expected or not launch.get("handoffReason"):
                problems.append(f"Missing workflow automation review: {entry['id']}")
    return {
        "ok": not problems,
        "complete": False,
        "inventoryComplete": not problems,
        "guiMethods": len(actual),
        "providerOperations": len(operations),
        "classifications": dict(collections.Counter(row["classification"] for row in operations)),
        "catalogEntries": len(settings) + len(workflows),
        "problems": problems,
        "note": "GUI method and catalog accounting only. External workflow destinations are not parameterized native operations; physical desktop and harness/model evidence is separate.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = audit(pathlib.Path(__file__).resolve().parents[1])
    except (ValueError, KeyError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"GUI methods: {result['guiMethods']}; provider operations: {result['providerOperations']}")
        print(f"Catalog entries: {result['catalogEntries']}; classifications: {result['classifications']}")
        for problem in result["problems"]:
            print(problem, file=sys.stderr)
        print(result["note"])
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
