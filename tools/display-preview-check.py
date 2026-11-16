#!/usr/bin/env python3
"""Production-process display preview checks in an explicitly enabled Docker lab.

Every compositor reply and mutation is a fixture in a private temporary home.
This verifies the CLI/watchdog protocol, not a physical display or real Hyprland.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

COMPOSITOR = r'''#!/usr/bin/python3
import json, os, pathlib, re, sys, time
root=pathlib.Path(os.environ['HOME']); a=sys.argv[1:]
state=root/'monitor.json'; mode=root/'mode'
def emit(v): print(json.dumps(v))
def setting(): return mode.read_text() if mode.exists() else ''
if a==['-j','version']: emit({'version':'0.56.2'}); sys.exit(0)
if a==['-j','monitors','all']:
    if setting()=='bad-read': print('{}'); sys.exit(0)
    if setting()=='read-down': sys.exit(3)
    emit([json.loads(state.read_text())]); sys.exit(0)
if a and a[0]=='eval':
    with (root/'eval.log').open('a') as f: f.write(a[1]+'\n')
    if setting()=='ignore': sys.exit(0)
    d=json.loads(state.read_text()); rule=a[1]
    position=re.search(r'position = "(-?\d+)x(-?\d+)"',rule)
    if position: d.update(x=int(position[1]),y=int(position[2]))
    scale=re.search(r'scale = ([0-9.]+)',rule)
    if scale: d['scale']=float(scale[1])
    transform=re.search(r'transform = ([0-7])',rule)
    d['transform']=int(transform[1]) if transform else 0
    d['disabled']='disabled = true' in rule
    temporary=state.with_suffix('.tmp'); temporary.write_text(json.dumps(d)); temporary.replace(state)
    sys.exit(0)
sys.exit(9)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if os.environ.get('OC_DISPLAY_PREVIEW_LAB') != '1' or not Path('/.dockerenv').exists():
        parser.error('enable OC_DISPLAY_PREVIEW_LAB=1 inside a disposable Docker container')
    control = str(args.control.resolve())
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    with tempfile.TemporaryDirectory(prefix='oc-display-') as temporary:
        root = Path(temporary); home = root/'home'; home.mkdir(); runtime = root/'runtime'; runtime.mkdir(mode=0o700)
        binaries = root/'bin'; binaries.mkdir(); compositor=binaries/'hyprctl'; compositor.write_text(COMPOSITOR); compositor.chmod(0o700)
        config=home/'.config/hypr/hyprland.lua'; config.parent.mkdir(parents=True)
        original='require("default.hypr.omarchy")\nrequire("hypr.monitors")\nrequire("hypr.autostart")\nrequire("default.hypr.toggles")\n'
        config.write_text(original)
        env=dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home/'.config'), XDG_STATE_HOME=str(home/'.local/state'), XDG_CACHE_HOME=str(home/'.cache'), XDG_RUNTIME_DIR=str(runtime), OMARCHY_PATH=str(root/'omarchy'), HYPRLAND_INSTANCE_SIGNATURE='oc-display-fixture', PATH=str(binaries)+':'+os.environ['PATH'])
        monitor=home/'monitor.json'; initial=dict(id=0,name='eDP-1',description='Fixture panel',make='Test',model='Panel',serial='one',width=1920,height=1200,refreshRate=60,x=0,y=0,scale=1,transform=0,disabled=False,focused=True,mirrorOf='none',availableModes=['1920x1200@60.00Hz'])
        monitor.write_text(json.dumps(initial))
        def call(*arguments, ok=True, record=True):
            result=subprocess.run([control,*arguments],env=env,text=True,capture_output=True,timeout=55)
            try: body=json.loads(result.stdout)
            except ValueError: raise AssertionError((arguments,result.returncode,result.stdout,result.stderr))
            if ok is not None:
                valid=body.get('ok') is ok and (result.returncode==0) is ok
                if record: check(valid,' '.join(arguments)+' result')
                elif not valid: raise AssertionError((arguments,body,result.stderr))
            return body
        def status(token=None):
            return call('display','preview','status',*([token] if token else []),record=False)['data'].get('receipt',{})
        def wait_phase(token, phase, timeout=22):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                value=status(token)
                if value.get('phase')==phase: return value
                time.sleep(.1)
            raise AssertionError((phase,value))
        def layout(x):
            path=root/'layout.json'; path.write_text(json.dumps([dict(output='eDP-1',x=x)])); return str(path)
        check(call('display','status')['data']['monitors'][0]['serial']=='one','reported serial retained')
        call('display','check','--dry-run')
        check(not (home/'.local/state/omarchy-control/display-recovery-check.json').exists(),'dry-run does not unlock editing')
        check(call('display','check','--yes')['outcome']=='passed','real detached recovery check passes fixture')
        check(json.loads(monitor.read_text())['x']==0,'recovery check restored original geometry')
        call('display','layout',layout(40),'--dry-run')
        check(not (home/'eval.log').read_text().count('40x0'),'plan sends no layout mutation')
        first=call('display','layout',layout(40),'--yes')['data']['receipt']; token=first['token']
        check(first['phase']=='preview' and json.loads(monitor.read_text())['x']==40,'originating CLI exited and preview remains active')
        call('display','preview','keep',token,ok=False)
        call('display','preview','revert',token)
        check(wait_phase(token,'reverted')['restoreVerified'],'cross-process revert independently verified')
        check(json.loads(monitor.read_text())['x']==0,'revert returned geometry')
        token=call('display','layout',layout(60),'--yes')['data']['receipt']['token']
        call('display','preview','keep',token,'--yes')
        receipt=status(token)
        check(receipt['phase']=='kept' and not receipt['physicalAppearanceVerified'],'kept scope excludes physical appearance')
        module=home/'.config/hypr/omarchy-control.lua'
        check('60x0' in module.read_text() and 'omarchy-control' in config.read_text(),'detached owner persisted layout and import')
        call('display','preview','revert',token,ok=False)
        token=call('display','layout',layout(80),'--yes')['data']['receipt']['token']
        config.write_text(config.read_text()+'-- later edit\n'); edited=config.read_text()
        call('display','preview','keep',token,'--yes',ok=False)
        check(wait_phase(token,'reverted')['phase']=='reverted','failed Keep restored runtime')
        check(config.read_text()==edited and '60x0' in module.read_text(),'hand edit and previous persisted layout survived failed Keep')
        check(json.loads(monitor.read_text())['x']==60,'failed Keep returned to last kept runtime')
        token=call('display','layout',layout(100),'--yes')['data']['receipt']['token']
        wait_phase(token,'reverted')
        check(json.loads(monitor.read_text())['x']==60,'automatic expiry survives originating CLI exit')
        call('display','preview','keep',token,'--yes',ok=False)
        (home/'mode').write_text('ignore')
        call('display','layout',layout(120),'--yes',ok=False)
        check(json.loads(monitor.read_text())['x']==60,'zero-exit ignored writer is not a preview success')
        (home/'mode').unlink()
        if status().get('phase') not in ('reverted','not_started','kept'): call('display','preview','revert',status()['token'])
        token=call('display','layout',layout(140),'--yes')['data']['receipt']['token']
        other=dict(env,HYPRLAND_INSTANCE_SIGNATURE='another-fixture-session')
        result=subprocess.run([control,'display','preview','keep',token,'--yes'],env=other,text=True,capture_output=True,timeout=10)
        check(result.returncode!=0 and json.loads(result.stdout)['error']['code']=='stale_session','changed session cannot keep an old token')
        call('display','preview','revert',token)
        check(not status(token).get('physicalAppearanceVerified'),'recovery never claims physical verification')
    report=dict(scope='production CLI and real detached watchdog against stateful fixture compositor; no physical display verification',checks=len(checks),passed=checks)
    if args.output: args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
