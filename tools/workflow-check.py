#!/usr/bin/env python3
"""Exercise explicit workflow adapters with pinned upstream helpers in a private lab.

The shell's qs IPC endpoint is simulated. No compositor, plugin code or real
desktop session is used. Run as an unprivileged user inside a disposable Docker
container with OC_WORKFLOW_LAB=1, jq and the production Control binary.
"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


IPC = r'''#!/usr/bin/python3
import json, os, pathlib, sys
h = pathlib.Path(os.environ['HOME'])
a = sys.argv[1:]
assert a[:6] == ['ipc', '-n', '-p', os.environ['OMARCHY_PATH'] + '/shell', 'call', '--'], a
a = a[6:]
assert a[0] == 'shell', a
method, args = a[1], a[2:]
c, l = h / '.config/omarchy/shell.json', h / 'live.json'
def emit(value): print(json.dumps(value))
if method == 'ping': print('pong')
elif method == 'listShellConfig': emit(json.loads(l.read_text()))
elif method == 'listPlugins':
    emit([dict(id='omarchy.bar', name='Omarchy Bar', kinds=['bar'], firstParty=True,
               canDisable=False, active=True, enabled=True, clonedFrom=''),
          dict(id='omarchy.power', name='Power', kinds=['bar-widget'], firstParty=True,
               canDisable=True, active=False, enabled=True, clonedFrom='')])
elif method == 'setBarWidget':
    identity, key, value, placement = args
    value, p = json.loads(value), json.loads(placement)
    assert identity == 'omarchy.power' and key == 'showPercentage' and type(value) is bool
    assert p == {'fromSection': 'right', 'fromIndex': 1}, p
    with (h / 'writes.jsonl').open('a') as f: f.write(json.dumps(a) + '\n')
    d = json.loads(c.read_text())
    target = d['bar']['layout'][p['fromSection']][p['fromIndex']]
    assert isinstance(target, dict) and target['id'] == identity
    if not (h / 'noop').exists():
        target[key] = value
        if (h / 'collateral').exists(): d['bar']['background'] = 'none'
        c.write_text(json.dumps(d))
        l.write_text(json.dumps(d))
    print('ok')
else: raise AssertionError(a)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', type=Path, required=True)
    parser.add_argument('--omarchy-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-version', help='require this application version in every API response')
    args = parser.parse_args()
    if os.environ.get('OC_WORKFLOW_LAB') != '1' or not Path('/.dockerenv').exists() or os.geteuid() == 0:
        parser.error('requires OC_WORKFLOW_LAB=1 and a non-root disposable Docker container')
    if not shutil.which('jq'):
        parser.error('jq is required by the real Omarchy bar helper')
    root = Path(__file__).resolve().parents[1]
    pins = json.loads((root / 'src/data/compat.json').read_text())['commands']
    helpers = ['omarchy-theme-bg-intro', 'omarchy-bar', 'omarchy-shell',
               'omarchy-shell-config', 'omarchy-plugin-list', 'omarchy-crash-mute',
               'omarchy-toggle', 'omarchy-toggle-enabled']
    checked = []
    sources = {}
    for name in helpers:
        content = (args.omarchy_source / 'bin' / name).read_bytes()
        assert hashlib.sha256(content).hexdigest() == pins[name], name
        sources[name] = content

    with tempfile.TemporaryDirectory(prefix='control-workflows-') as private:
        h = Path(private)
        bins, stock, runtime = h / 'bin', h / 'stock', h / 'run'
        for path in [bins, stock / 'shell', stock / 'config/omarchy', runtime, h / '.config/omarchy']:
            path.mkdir(parents=True, mode=0o700, exist_ok=True)
        (stock / 'shell/shell.qml').write_text('// IPC fixture; never executed\n')
        (stock / 'version').write_text('4.5.0\n')
        def command(name, content):
            path = bins / name
            path.write_bytes(content if isinstance(content, bytes) else content.encode())
            path.chmod(0o700)
        for name, content in sources.items(): command(name, content)
        command('qs', IPC)
        command('omarchy', '''#!/bin/bash
case "$*" in
  'version') echo 4.5.0;;
  'theme bg intro '*) shift 3; exec omarchy-theme-bg-intro "$@";;
  'plugin list --json') shift 2; exec omarchy-plugin-list "$@";;
  'bar set '*) shift; exec omarchy-bar "$@";;
  'crash mute -- '*) shift 2; exec omarchy-crash-mute "$@";;
  *) echo "unexpected private dispatch: $*" >&2; exit 93;;
esac
''')
        command('omarchy-i18n', '#!/bin/bash\nprintf "%s\\n" "$1"\n')
        command('omarchy-notification-send', '#!/bin/bash\n[[ ! -f "$HOME/notification-fails" ]]\n')
        env = dict(os.environ, HOME=str(h), PATH=str(bins) + ':/usr/bin', OMARCHY_PATH=str(stock),
                   XDG_CONFIG_HOME=str(h / '.config'), XDG_STATE_HOME=str(h / '.local/state'),
                   XDG_DATA_HOME=str(h / '.local/share'), XDG_CACHE_HOME=str(h / '.cache'),
                   XDG_DATA_DIRS=str(h / 'shared'), XDG_RUNTIME_DIR=str(runtime), QT_QPA_PLATFORM='offscreen')
        for key in ['DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'HYPRLAND_INSTANCE_SIGNATURE',
                    'HYPRLAND_CMD', 'LD_PRELOAD', 'LD_LIBRARY_PATH']:
            env.pop(key, None)
        def api(*argv, code=0, versioned=True):
            options = ['--json', '--api-version', '1'] if versioned else ['--json']
            child = subprocess.run([str(args.control.resolve()), *argv, *options],
                                   env=env, capture_output=True, text=True, timeout=30)
            assert child.returncode == code, (argv, child.returncode, child.stdout, child.stderr)
            body = json.loads(child.stdout)
            assert body['apiVersion'] == 1 and body['ok'] == (code == 0), (argv, body)
            if args.expected_version:
                assert body['appVersion'] == args.expected_version, (argv, body['appVersion'])
            checked.append(' '.join(argv))
            return body
        identifier = 'appearance.background.intros'
        flag = h / '.local/state/omarchy/toggles/background-intros-off'
        assert api('get', identifier)['data']['value'] == 'true'
        assert api('preferences', 'pin', 'missing.catalog.entry', code=1)['outcome'] == 'blocked'
        assert api('describe', 'appearance.background.intro')['data']['nativeSetting'] == identifier
        assert any(r['id'] == identifier for r in api('search', 'background introductions')['data'])
        assert api('plan', identifier, 'false')['outcome'] == 'planned' and not flag.exists()
        changed = api('apply', identifier, 'false')
        assert changed['verified'] and flag.read_bytes() == b''
        assert api('apply', identifier, 'false')['verified']
        transaction = changed['data']['id']
        assert api('undo', transaction)['verified'] and not flag.exists()
        (h / 'notification-fails').touch()
        assert api('apply', identifier, 'false')['verified'] and flag.exists()
        flag.write_text('custom data')
        assert api('plan', identifier, 'true', code=1)['error']['code'] == 'ownership_conflict'
        assert flag.read_text() == 'custom data'
        flag.unlink()

        mute = h / '.local/state/omarchy/toggles/crash-ignore/hyprland'
        assert api('preferences', 'crash-mutes')['data']['items'] == []
        assert api('describe', 'crash.mute')['data']['nativeOperations'] == ['preferences.crash-mutes']
        assert any(r['id'] == 'crash.mute' and r['nativeOperations'] == ['preferences.crash-mutes']
                   for r in api('search', 'mute crash')['data'])
        assert api('preferences', 'crash-mute', 'hyprland')['data']['muted'] is False
        assert api('preferences', 'crash-mute', 'hyprland', 'true', '--dry-run')['outcome'] == 'planned'
        assert not mute.exists()
        assert api('preferences', 'crash-mute', 'hyprland', 'true')['verified'] and mute.read_bytes() == b''
        assert api('preferences', 'crash-mute', 'hyprland', 'true')['verified']
        assert api('preferences', 'crash-mutes')['data']['items'][0]['program'] == 'hyprland'
        assert api('preferences', 'crash-mute', 'hyprland', 'false')['verified'] and not mute.exists()
        assert api('preferences', 'crash-mute', '', code=1)['error']['code'] == 'invalid_arguments'
        assert api('preferences', 'crash-mute', 'hyprland', '', code=1)['error']['code'] == 'invalid_arguments'
        assert api('preferences', 'crash-mute', 'hyprland', 'yes', code=1)['outcome'] == 'blocked'
        mute.write_text('custom')
        assert api('preferences', 'crash-mute', 'hyprland', 'false', code=1)['error']['code'] == 'ownership_conflict'
        assert mute.read_text() == 'custom'

        d = dict(version=1, bar=dict(id='omarchy.bar', background='opaque', layout={
            'left': [], 'centerBegin': [], 'center': [], 'centerEnd': [], 'tray': [],
            'right': ['omarchy.clock', dict(id='omarchy.power', customOption='preserve')]}),
            plugins=[], dock=dict(pinned=[]), custom=dict(preserve=True))
        config, live = h / '.config/omarchy/shell.json', h / 'live.json'
        def reset(value):
            for path in [config, live, stock / 'config/omarchy/shell.json']:
                path.write_text(json.dumps(value))
        reset(d)
        def widget():
            return next(w for w in api('bar', 'status')['data']['bar']['widgets'] if w['id'] == 'omarchy.power')
        w = widget()
        assert w['options']['showPercentage']['value'] is False and w['options']['showPercentage']['inherited']
        assert api('describe', 'battery.percentage')['data']['nativeOperations'] == ['widgets.option']
        assert any(r['id'] == 'battery.percentage' and r['nativeOperations'] == ['widgets.option']
                   for r in api('search', 'battery percentage')['data'])
        coverage = api('coverage', '--cli')['data']['entries']
        script_rows = [r for r in coverage if r.get('surface') == 'catalog' and r['classification'] == 'external-workflow']
        assert len(script_rows) == 344 and all(r.get('handoffReason') for r in script_rows)
        assert next(r for r in script_rows if r['id'] == 'battery.percentage')['nativeOperations'] == ['widgets.option']
        assert api('describe', 'widgets.option')['data']['optionSchema']['type'] == 'boolean'
        before = config.read_bytes()
        assert api('widget', 'option', w['selector'], 'showPercentage', 'true', '--dry-run')['outcome'] == 'planned'
        assert config.read_bytes() == before and not (h / 'writes.jsonl').exists()
        assert api('widget', 'option', w['selector'], 'showPercentage', 'true', versioned=False)['verified']
        expected = copy.deepcopy(d)
        expected['bar']['layout']['right'][1]['showPercentage'] = True
        assert json.loads(config.read_text()) == expected == json.loads(live.read_text())
        assert api('widget', 'option', w['selector'], 'showPercentage', 'false', code=1)['error']['code'] == 'stale_target'
        assert len((h / 'writes.jsonl').read_text().splitlines()) == 1
        assert api('widget', 'option', widget()['selector'], 'showPercentage', 'false')['verified']
        for failure in ['noop', 'collateral']:
            reset(d)
            (h / failure).touch()
            partial = api('widget', 'option', widget()['selector'], 'showPercentage', 'true', code=1)
            assert partial['outcome'] == 'partial' and not partial['verified']
            tx = partial['data']['transaction']
            assert tx['phase'] == 'partial'
            assert Path(tx['files'][str(config)]['backup']).exists()
            (h / failure).unlink()
        reset(d)
        changed_helper = bins / 'omarchy-bar'
        changed_helper.write_bytes(changed_helper.read_bytes() + b'\n# unreviewed helper\n')
        assert api('widget', 'option', widget()['selector'], 'showPercentage', 'true', '--dry-run', code=1)['error']['code'] == 'compatibility_blocked'
        report = dict(ok=True, checks=len(checked), calls=checked, helpers={name: pins[name] for name in helpers},
                      control=str(args.control.resolve()), controlSha256=hashlib.sha256(args.control.read_bytes()).hexdigest(),
                      expectedVersion=args.expected_version,
                      scope='Production CLI with pinned Omarchy helpers and simulated qs IPC; preference/configuration readback only. No rendering, playback or physical-session claim.')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({key: report[key] for key in ['ok', 'checks', 'scope']}))


if __name__ == '__main__':
    main()
