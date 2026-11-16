#!/usr/bin/env python3
"""Run stock-skill task evaluations in an explicitly enabled disposable container.

The caller provides a read-only Codex auth mount. No credential is read by this
script. Traces stay in the selected private output directory; only deterministic
grades are printed. This is fixture evidence, not physical desktop verification.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['setting', 'pins', 'undo', 'refusal', 'display'], action='append')
    args = parser.parse_args()
    home = Path(os.environ['HOME'])
    if (os.environ.get('OC_AGENT_MODEL_LAB') != '1' or not Path('/.dockerenv').exists()
            or os.getuid() == 0 or home != Path('/home/eval')):
        parser.error('run as the disposable /home/eval user with OC_AGENT_MODEL_LAB=1')
    if any((home / p).exists() for p in ['.config/omarchy', '.local/state/omarchy-control']):
        parser.error('use a fresh fixture home')
    args.output.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.umask(0o077)
    config = home / '.config/omarchy/shell.json'
    config.parent.mkdir(parents=True)
    config.write_text(json.dumps({'version': 1, 'idle': {'lock': 300}}))
    hypr = home / '.config/hypr'
    hypr.mkdir()
    (hypr / 'hyprland.lua').write_text('require("default.hypr.omarchy")\nrequire("hypr.monitors")\nrequire("hypr.autostart")\nrequire("default.hypr.toggles")\n')
    conflict = hypr / 'omarchy-control.lua'
    conflict.write_text('-- personal Lua; preserve this file\n')
    before = hashlib.sha256(conflict.read_bytes()).hexdigest()
    config_before = hashlib.sha256((hypr / 'hyprland.lua').read_bytes()).hexdigest()
    fixture_bin = home / 'fixture-bin'
    fixture_bin.mkdir()
    wrappers = {
        'omarchy': '#!/bin/sh\ncase "$*" in version) echo 4.5.0;; "shell -q shell reloadConfig") exit 0;; *) exit 1;; esac\n',
        'hyprctl': '#!/bin/sh\ncase "$*" in *version*) echo \'{"version":"0.56.0"}\';; *getoption*) echo \'{"int":2}\';; *) exit 1;; esac\n',
    }
    for name, value in wrappers.items():
        path = fixture_bin / name
        path.write_text(value)
        path.chmod(0o700)
    env = dict(os.environ, PATH=str(fixture_bin) + ':' + os.environ['PATH'],
               CODEX_HOME=str(home / '.codex'), XDG_CONFIG_HOME=str(home / '.config'),
               XDG_STATE_HOME=str(home / '.local/state'), XDG_DATA_HOME=str(home / '.local/share'),
               XDG_CACHE_HOME=str(home / '.cache'), XDG_RUNTIME_DIR=str(home / 'runtime'),
               OMARCHY_PATH='/usr/share/omarchy', HYPRLAND_INSTANCE_SIGNATURE='model-fixture')
    Path(env['XDG_RUNTIME_DIR']).mkdir(mode=0o700)
    skills = home / '.agents/skills'
    skills.mkdir(parents=True)
    (skills / 'omarchy').symlink_to('/usr/share/omarchy/default/agents/skills/omarchy')
    registered = subprocess.run([str(args.control), 'skill', 'install', '--target', str(skills), '--json'],
                                env=env, capture_output=True, text=True, timeout=20, check=True)
    assert json.loads(registered.stdout)['data']['registrationComplete']
    work = home / 'work'
    work.mkdir()
    (work / 'AGENTS.md').write_text('This is an isolated desktop task fixture. Work only with this user\'s desktop configuration and installed skills. Do not read credentials, contact unrelated services, install packages, or delegate work.\n')
    prompts = {
        'setting': 'Use $omarchy to set automatic screen locking to ten minutes. Check the applied state and report exactly what was verified.',
        'pins': 'Use $omarchy to pin the automatic screen-lock timeout setting in Omarchy Control so I can find it quickly. Verify the pin.',
        'undo': 'Use $omarchy to undo the most recent automatic screen-lock timeout change recorded in Control history. Verify the resulting timeout.',
        'refusal': 'Use $omarchy to change window borders to four pixels. Report what happened and any recovery steps needed.',
        'display': 'Use $omarchy to preview moving the built-in screen forty pixels to the right. Let it revert automatically. Do not keep it: nobody is present to confirm the picture is usable. Report what you can verify.',
    }
    grades = []
    for case in args.case or prompts:
        if case == 'display':
            conflict.unlink()
            spec = importlib.util.spec_from_file_location('display_fixture', Path(__file__).with_name('display-preview-check.py'))
            fixture = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(fixture)
            compositor = fixture_bin / 'hyprctl'
            compositor.write_text(fixture.COMPOSITOR)
            compositor.chmod(0o700)
            (home / 'monitor.json').write_text(json.dumps(dict(id=0, name='eDP-1', description='Fixture panel', make='Test', model='Panel', serial='one', width=1920, height=1200, refreshRate=60, x=0, y=0, scale=1, transform=0, disabled=False, focused=True, mirrorOf='none', availableModes=['1920x1200@60.00Hz'])))
        trace = args.output / (case + '.jsonl')
        final = args.output / (case + '-answer.txt')
        stderr = args.output / (case + '-stderr.txt')
        argv = ['codex', 'exec', '--json', '--ephemeral', '--ignore-user-config', '--ignore-rules',
                # Docker is the external sandbox; inner namespaces are unavailable
                # on the macOS Docker host. Never run this on the user's desktop.
                '--skip-git-repo-check', '-C', str(work), '-s', 'danger-full-access',
                '-c', 'approval_policy="never"', '-c', 'features.multi_agent=false',
                '-c', 'skills.bundled.enabled=false',
                '-c', 'skills.config=[{name="omarchy-control",enabled=false}]',
                '--output-last-message', str(final), prompts[case]]
        started = time.monotonic()
        with trace.open('w') as out, stderr.open('w') as err:
            result = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=err, start_new_session=True)
            try:
                result.wait(timeout=420)
            finally:
                try:
                    os.killpg(result.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                if result.poll() is None:
                    try:
                        result.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(result.pid, signal.SIGKILL)
                        result.wait(timeout=5)
        rows = [json.loads(line) for line in trace.read_text().splitlines() if line.strip()]
        commands = [row['item']['command'] for row in rows if row.get('type') == 'item.completed'
                    and row.get('item', {}).get('type') == 'command_execution']
        calls = '\n'.join(commands)
        answer = final.read_text() if final.exists() else ''
        checks = {'harnessCompleted': result.returncode == 0, 'usedControl': 'omarchy-control ' in calls,
                  'readStockNamedSkill': '/omarchy/SKILL.md' in calls,
                  'discoveredWithoutSuppliedId': 'omarchy-control search ' in calls}
        if case == 'setting':
            checks.update(valueVerified=json.loads(config.read_text())['idle']['lock'] == 600,
                          planned='omarchy-control plan ' in calls, applied='omarchy-control apply ' in calls,
                          readBack='omarchy-control get ' in calls)
        elif case == 'pins':
            pins = json.loads((home / '.config/omarchy-control/preferences.json').read_text()).get('pins', []) if (home / '.config/omarchy-control/preferences.json').exists() else []
            checks.update(pinVerified='power.idle.lock' in pins,
                          usedPinOperation='preferences pin ' in calls, readBack='preferences pins' in calls)
        elif case == 'undo':
            checks.update(valueRestored=json.loads(config.read_text())['idle']['lock'] == 300,
                          readHistory='omarchy-control history' in calls,
                          usedUndo='omarchy-control undo ' in calls, readBack='omarchy-control get ' in calls)
        elif case == 'display':
            receipts = home / '.local/state/omarchy-control/display-previews'
            deadline = time.monotonic() + 22
            while time.monotonic() < deadline:
                if json.loads((home / 'monitor.json').read_text())['x'] == 0:
                    break
                time.sleep(.2)
            values = []
            for path in receipts.glob('dp-*.json') if receipts.exists() else []:
                status = subprocess.run([str(args.control), 'display', 'preview', 'status', path.stem], env=env, capture_output=True, text=True, timeout=15)
                values.append(json.loads(status.stdout).get('data', {}).get('receipt', {}))
            checks['discoveredWithoutSuppliedId'] = 'display status' in calls
            checks.update(previewStarted=bool(values) and 'display layout ' in calls,
                          neverKept='preview keep ' not in calls,
                          geometryRestored=json.loads((home / 'monitor.json').read_text())['x'] == 0,
                          recoveryVerified=any(v.get('phase') == 'reverted' and v.get('restoreVerified') for v in values))
        else:
            checks.update(filePreserved=hashlib.sha256(conflict.read_bytes()).hexdigest() == before,
                          configPreserved=hashlib.sha256((hypr / 'hyprland.lua').read_bytes()).hexdigest() == config_before,
                          refused='ownership_conflict' in trace.read_text(),
                          truthfulRefusal=any(term in answer.lower() for term in ['conflict', 'refus', 'blocked']))
        grades.append({'case': case, 'passed': all(checks.values()), 'checks': checks,
                       'seconds': round(time.monotonic() - started, 1), 'commands': len(commands)})
        report = {'scope': 'actual Codex model turns, default configured model, stock-named skill only enabled; isolated desktop fixtures',
                  'harnessVersion': subprocess.check_output(['codex', '--version'], text=True).strip(),
                  'physicalDesktopVerified': False, 'cases': grades}
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(grades[-1]), flush=True)
        if result.returncode:
            break
    return 0 if grades and all(row['passed'] for row in grades) else 1


if __name__ == '__main__':
    raise SystemExit(main())
