#!/usr/bin/env python3
"""Check real harness discovery in temporary homes, without a model turn.

Uses Codex skills/list, Pi get_commands and Claude's SDK initialize response.
No user prompt, generation request, login or real-user configuration is used.
On macOS, --container can run the Linux Control binary; --work-dir must then be
mounted at the identical absolute path inside that disposable container.
"""

import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 4 * 1024 * 1024


def environment(home, agent_dir=None):
    result = {key: value for key, value in os.environ.items() if key in ("PATH", "TMPDIR", "LANG", "LC_ALL")}
    result.update(HOME=str(home), CODEX_HOME=str(agent_dir or home / ".codex"),
                  CLAUDE_CONFIG_DIR=str(agent_dir or home / ".claude"),
                  PI_CODING_AGENT_DIR=str(agent_dir or home / ".pi/agent"), PI_OFFLINE="1",
                  XDG_CONFIG_HOME=str(home / ".config"), XDG_STATE_HOME=str(home / ".local/state"),
                  XDG_DATA_HOME=str(home / ".local/share"), XDG_CACHE_HOME=str(home / ".cache"),
                  OMARCHY_PATH=str(home / "omarchy"), CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1",
                  DISABLE_AUTOUPDATER="1", ANTHROPIC_BASE_URL="http://127.0.0.1:1",
                  ANTHROPIC_API_KEY="unused-offline-discovery-fixture")
    return result


class Protocol:
    def __init__(self, argv, env, cwd):
        self.process = subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, start_new_session=True)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = b""

    def send(self, message):
        self.process.stdin.write(json.dumps(message).encode() + b"\n")
        self.process.stdin.flush()

    def receive(self, predicate):
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if line.strip():
                    message = json.loads(line)
                    if predicate(message):
                        return message
            if not self.selector.select(min(1, max(0, deadline - time.monotonic()))):
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("harness exited before discovery response")
            self.buffer += chunk
            if len(self.buffer) > LIMIT:
                raise RuntimeError("harness response exceeds limit")
        raise TimeoutError("harness discovery did not respond")

    def close(self):
        try:
            os.killpg(self.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=3)
        self.selector.close()
        self.process.stdin.close()
        self.process.stdout.close()


def discover(harness, executable, env, cwd):
    if harness == "codex":
        Path(env["CODEX_HOME"]).mkdir(parents=True, exist_ok=True)
        argv = [executable, "app-server", "--stdio"]
    elif harness == "pi":
        directory = Path(env["PI_CODING_AGENT_DIR"])
        directory.mkdir(parents=True, exist_ok=True)
        # Required for offline startup only; the endpoint has no listener and
        # no prompt is sent. This is not an inference fixture or a credential.
        (directory / "models.json").write_text(json.dumps({"providers": {"discovery-fixture": {
            "baseUrl": "http://127.0.0.1:1/v1", "api": "openai-completions", "apiKey": "unused-offline-discovery-fixture",
            "models": [{"id": "no-generation"}]}}}))
        argv = [executable, "--mode", "rpc", "--offline", "--no-session", "--no-extensions",
                "--no-prompt-templates", "--no-themes", "--no-tools", "--model", "discovery-fixture/no-generation"]
    else:
        Path(env["CLAUDE_CONFIG_DIR"]).mkdir(parents=True, exist_ok=True)
        argv = [executable, "--print", "--input-format", "stream-json", "--output-format", "stream-json",
                "--verbose", "--no-session-persistence"]
    protocol = Protocol(argv, env, cwd)
    try:
        if harness == "codex":
            protocol.send({"id": 1, "method": "initialize", "params": {
                "clientInfo": {"name": "omarchy_control_discovery_check", "version": "0.1"},
                "capabilities": {"experimentalApi": True}}})
            initialized = protocol.receive(lambda row: row.get("id") == 1)
            if "error" in initialized:
                raise RuntimeError(initialized["error"])
            protocol.send({"method": "initialized"})
            protocol.send({"id": 2, "method": "skills/list", "params": {"cwds": [str(cwd)], "forceReload": True}})
            response = protocol.receive(lambda row: row.get("id") == 2)
            if "error" in response:
                raise RuntimeError(response["error"])
            data = response["result"]["data"][0]
            if data.get("errors"):
                raise RuntimeError("harness reported skill discovery errors")
            rows = data["skills"]
        elif harness == "pi":
            protocol.send({"id": "commands", "type": "get_commands"})
            response = protocol.receive(lambda row: row.get("id") == "commands")
            if not response.get("success"):
                raise RuntimeError("Pi command discovery failed")
            rows = [dict(row, name=row["name"].removeprefix("skill:"))
                    for row in response["data"]["commands"] if row.get("source") == "skill"]
        else:
            protocol.send({"type": "control_request", "request_id": "init",
                           "request": {"subtype": "initialize", "hooks": None}})
            response = protocol.receive(lambda row: row.get("type") == "control_response"
                                         and row.get("response", {}).get("request_id") == "init")["response"]
            if response.get("subtype") != "success":
                raise RuntimeError("Claude initialization failed")
            rows = response.get("response", {}).get("commands", [])
        selected = [row for row in rows if row["name"] in ("omarchy", "omarchy-control")]
        if len({row["name"] for row in selected}) != len(selected):
            raise RuntimeError("duplicate Omarchy skill identities")
        for row in selected:
            if row.get("enabled") is False:
                raise RuntimeError("registered skill is disabled")
        return selected
    finally:
        protocol.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", required=True, help="Control binary path, inside --container when supplied")
    parser.add_argument("--container")
    parser.add_argument("--harness", choices=["codex", "pi", "claude"], action="append")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/agent-discovery")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    reports = []
    for harness in args.harness or ["codex", "pi", "claude"]:
        executable = shutil.which(harness)
        if not executable:
            reports.append({"harness": harness, "state": "unavailable"})
            continue
        version_result = subprocess.run([executable, "--version"], capture_output=True, text=True, timeout=10, check=True)
        version = (version_result.stdout or version_result.stderr).strip()
        if not version:
            raise RuntimeError("harness version was not reported")
        kinds = ["default", "generic", "custom-home", "conflict", "multi-root"]
        if harness == "codex":
            kinds.append("directory-alias")
        for kind in kinds:
            with tempfile.TemporaryDirectory(prefix=f"{harness}-{kind}-", dir=args.work_dir.resolve()) as temporary:
                home = Path(temporary)
                cwd = home / "work"
                cwd.mkdir()
                agent_dir = home / "custom-agent" if kind == "custom-home" else None
                env = environment(home, agent_dir)
                default = {"codex": home / ".codex", "pi": home / ".pi/agent", "claude": home / ".claude"}[harness]
                directory = (agent_dir or default) / "skills"
                if kind in ("generic", "directory-alias"):
                    directory = home / ".agents/skills"
                directory.mkdir(parents=True)
                directories = [directory]
                if kind == "multi-root":
                    generic = home / ".agents/skills"
                    generic.mkdir(parents=True)
                    directories.append(generic)
                if kind == "directory-alias":
                    default.mkdir(parents=True)
                    (default / "skills").symlink_to(directory)
                stock = home / "omarchy/default/agents/skills/omarchy"
                shutil.copytree(ROOT / "tests/fixtures/agent-skills/omarchy", stock)
                if kind == "conflict":
                    (directory / "omarchy").mkdir()
                    (directory / "omarchy/SKILL.md").write_text("---\nname: omarchy\ndescription: Custom ownership fixture.\n---\nPreserve this skill.\n")
                else:
                    (directory / "omarchy").symlink_to(stock)
                if kind == "multi-root":
                    (directories[1] / "omarchy").symlink_to(stock)

                def control(action, expected=0, target=True):
                    tail = [args.control, "skill", action, "--api-version", "1"]
                    if target:
                        for selected_directory in directories:
                            tail += ["--target", str(selected_directory)]
                    if args.container:
                        argv = ["docker", "exec", "--user", str(os.getuid())]
                        for key in ("HOME", "CODEX_HOME", "CLAUDE_CONFIG_DIR", "PI_CODING_AGENT_DIR", "XDG_CONFIG_HOME",
                                    "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "OMARCHY_PATH"):
                            argv += ["--env", key + "=" + env[key]]
                        argv += ["--env", "LANG=C.UTF-8", args.container, *tail]
                        child_env = None
                    else:
                        argv, child_env = tail, env
                    child = subprocess.run(argv, env=child_env, capture_output=True, text=True, timeout=20)
                    if child.returncode != expected:
                        raise RuntimeError("unexpected Control result: " + child.stdout + child.stderr)
                    return json.loads(child.stdout)

                installed = control("install", expected=int(kind == "conflict"))
                selected = discover(harness, executable, env, cwd)
                names = {row["name"] for row in selected}
                visible = names == {"omarchy", "omarchy-control"}
                if not visible and kind != "generic":
                    raise AssertionError(f"{harness} {kind} did not discover both registered skills")
                if kind == "conflict":
                    assert installed["data"]["hasConflicts"]
                    assert "Custom ownership fixture" in next(row["description"] for row in selected if row["name"] == "omarchy")
                elif visible:
                    assert installed["data"]["registrationComplete"]
                    # Check the linked body/resources, separately from metadata
                    # discovery. This does not prove model selection or execution.
                    bridge = (directory / "omarchy/SKILL.md").read_text()
                    assert "omarchy-control commands" in bridge
                    assert (directory / "omarchy/.upstream/plugins.md").read_bytes() == (stock / "plugins.md").read_bytes()
                    control("opt-out", target=False)
                    after = discover(harness, executable, env, cwd)
                    assert "omarchy-control" not in {row["name"] for row in after}
                    control("opt-in", target=False)
                    assert "omarchy-control" in {row["name"] for row in discover(harness, executable, env, cwd)}
                control("remove")
                remaining = discover(harness, executable, env, cwd)
                assert "omarchy-control" not in {row["name"] for row in remaining}
                if kind != "conflict":
                    assert (directory / "omarchy/SKILL.md").read_bytes() == (stock / "SKILL.md").read_bytes()
                reports.append({"harness": harness, "version": version, "case": kind,
                                "discovery": "verified" if visible else "not_discovered",
                                "registeredNames": sorted(names), "conflictPreserved": kind == "conflict",
                                "modelTurnStarted": False})
    report = {"cases": reports, "modelTaskExecutionVerified": False, "scope": "actual harness discovery in isolated homes; no model turns or desktop session"}
    output = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end="")
    return 0 if any(row.get("discovery") == "verified" for row in reports) else 1


if __name__ == "__main__":
    raise SystemExit(main())
