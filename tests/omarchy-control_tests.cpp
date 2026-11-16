// Every test runs against a temporary root with fake commands first on PATH. No
// test can reach the real desktop: the sandbox Env is the only way in.
#include <functional>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDirIterator>
#include <QProcess>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QLockFile>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <unistd.h>

#ifndef Q_MOC_RUN
#include "fake_scripts.h"
#endif

#include "appcore.h"
#include "blur.h"
#include "audioprovider.h"
#include "backend.h"
#include "bluetoothprovider.h"
#include "catalog.h"
#include "changecoordinator.h"
#include "cli.h"
#include "cliapi.h"
#include "commandrunner.h"
#include "compat.h"
#include "windowrulesprovider.h"
#include <QAccessible>
#include <QSysInfo>
#include "coverage.h"
#include "displayprovider.h"
#include "displaypreview.h"
#include "env.h"
#include "fileops.h"
#include "helperlauncher.h"
#include "hypromodule.h"
#include "iconprovider.h"
#include "iconthemeimporter.h"
#include "iconthemeprovider.h"
#include "iconthemes.h"
#include "networkprovider.h"
#include "networkpreview.h"
#include "pluginsprovider.h"
#include "pluginfiles.h"
#include <sys/stat.h>
#include "servicesprovider.h"
#include "settingsengine.h"
#include "shortcutsprovider.h"
#include "systemprovider.h"
#include "theme.h"
#include "translator.h"
#include "trayhost.h"
#include "iconlayer.h"
#include "iconarchive.h"
#include "launchereditor.h"
#include "profilefiles.h"
#include "profileprovider.h"

#include <archive.h>
#include <archive_entry.h>
#include "updater.h"

namespace {

void writeFile(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A throwaway machine: a temp root, an Env pointing into it, and helpers for
// writing the fake commands a test needs.
struct Sandbox {
    QTemporaryDir dir;
    Env env;
    Sandbox() { env = Env::sandbox(dir.path(), dir.path() + "/bin"); }
    QString bin(const QString &name) const { return dir.path() + "/bin/" + name; }
    QString log() const { return dir.path() + "/commands.log"; }
    void command(const QString &name, const QByteArray &body) const {
        const QByteArray script = "#!/bin/bash\necho \"" + name.toUtf8() + " $*\" >> '" + log().toUtf8() + "'\n" + body + "\n";
        writeFile(bin(name), script);
        QFile::setPermissions(bin(name), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QStringList calls() const {
        return QString::fromUtf8(readAll(log())).split('\n', Qt::SkipEmptyParts);
    }
    void minimalHyprland(const QString &extra = {}) const {
        command("hyprctl", (kScript0 + dir.path().toUtf8() + kScript1 + extra.toUtf8()).trimmed());
        writeFile(env.configHome + "/hypr/hyprland.lua", "dofile(\"x\")\nrequire(\"default.hypr.omarchy\")\nrequire(\"hypr.monitors\")\nrequire(\"hypr.autostart\")\nrequire(\"default.hypr.toggles\")\n-- custom\nrequire(\"hypr.mine\")\n");
    }
    void omarchy(const QByteArray &body) const { command("omarchy", body); }
    void debugHelper() const {
        command("omarchy-debug", "LOG_FILE=\"/tmp/omarchy-debug.log\"\nif [ \"$LOG_FILE\" = /tmp/omarchy-debug.log ]; then touch \"$HOME/public-log-used\"; exit 42; fi\nif [ -f \"$HOME/raw-limit\" ]; then head -c 3000000 /dev/zero > \"$LOG_FILE\"; exit $?; fi\nprintf 'private raw fixture\\n' > \"$LOG_FILE\"\nexec omarchy debug \"$@\"");
    }
    void bluetooth(const QByteArray &body) const {
        command("bluetoothctl", "case \"$1\" in list) echo 'Controller 00:11:22:33:44:55 Test'; exit 0;; show) echo 'Powered: yes'; exit 0;; esac\n" + body);
    }
};

struct DisplayDesk {
    Sandbox s; Env env;
    DisplayDesk() {
        s.minimalHyprland();
        writeFile(s.dir.path() + "/obeys", "1"); writeFile(s.dir.path() + "/x", "0");
        s.command("hyprctl", kStatefulCompositor);
        env = s.env; env.process.insert("OC_TEST_ROOT", s.dir.path());
        writeFile(env.appStateDir + "/display-recovery-check.json", R"({"passed":true,"hyprland":"0.56.2"})");
    }
    QString layout(int x) { const QString path = s.dir.path() + "/layout.json"; writeFile(path, QJsonDocument(QJsonArray{QJsonObject{{"output", "eDP-1"}, {"x", x}}}).toJson()); return path; }
    QJsonObject api(const QStringList &args) { return CliApi::execute(env, args).body; }
};

struct NetworkDesk {
    Sandbox s;
    NetworkDesk() {
        writeFile(s.env.home + "/radio-enabled", "true");
        writeFile(s.env.home + "/radio-hardware", "true");
        set("general", "enabled:full\n");
        set("devices", "wlan0:wifi:connected:Home\nlo:loopback:connected (externally):lo\nbr0:bridge:disconnected:\nfuture0:future-type:future-state:Odd\\: Name\n");
        set("saved", "Same:11111111-1111-1111-1111-111111111111:802-11-wireless:yes\nSame:22222222-2222-2222-2222-222222222222:802-11-wireless:no\nBridge:33333333-3333-3333-3333-333333333333:bridge:no\n");
        set("wifi", "*:Cafe\\: Lab:40:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan0\n :Cafe\\: Lab:90:WPA2:02\\:00\\:00\\:00\\:00\\:02:wlan0\n\n :Cafe\\: Lab:20:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan1\n ::0:--:02\\:00\\:00\\:00\\:00\\:03:wlan0\n");
        s.command("nmcli", R"SH(
[ "$LC_ALL" = C.UTF-8 ] && [ "$LANGUAGE" = C ] || { echo wrong-locale >&2; exit 5; }
case "$*" in
  *GENERAL.DBUS-PATH*)
    device="${@: -1}"
    if [ -f "$HOME/lookup-fail-$device" ]; then exit 8; fi
    if [ -f "$HOME/object-$device" ]; then cat "$HOME/object-$device"; else echo "/org/freedesktop/NetworkManager/Devices/$device"; fi
    exit 0;;
  *WIFI,CONNECTIVITY*) section=general;;
  *DEVICE,TYPE,STATE,CONNECTION*) section=devices;;
  *NAME,UUID,TYPE,ACTIVE*) section=saved;;
  *IN-USE,SSID,SIGNAL,SECURITY,BSSID,DEVICE*) section=wifi;;
  *) echo unexpected-command >&2; exit 9;;
esac
touch "$HOME/started-$section"
if [ -f "$HOME/delay-$section" ]; then
  cp "$HOME/read-$section" "$HOME/captured-$section"
  while [ ! -f "$HOME/release-reads" ]; do sleep .01; done
  cat "$HOME/captured-$section"
else
  cat "$HOME/read-$section"
fi
mode=$(cat "$HOME/mode-$section" 2>/dev/null)
case "$mode" in
  fail) echo fixture-failure >&2; exit 8;;
  truncated) head -c 1100000 /dev/zero | tr '\0' X;;
  stderr-truncated) head -c 1100000 /dev/zero | tr '\0' X >&2;;
  timeout) exec sleep 10;;
esac
exit 0)SH");
        s.command("busctl", R"SH(
case "$*" in
  *"get-property org.freedesktop.NetworkManager /org/freedesktop/NetworkManager org.freedesktop.NetworkManager WirelessEnabled WirelessHardwareEnabled")
    mode=$(cat "$HOME/radio-read-mode" 2>/dev/null)
    [ ! -f "$HOME/radio-written" ] || mode=$(cat "$HOME/radio-readback-mode" 2>/dev/null)
    case "$mode" in
      malformed) printf 'b false\nb maybe\n'; exit 0;;
      extra) printf 'b false\nb true\nb false\n'; exit 0;;
      timeout) exec sleep 8;;
    esac
    printf 'b %s\nb %s\n' "$(cat "$HOME/radio-enabled")" "$(cat "$HOME/radio-hardware")"
    case "$mode" in
      fail) echo fixture-read-failed >&2; exit 8;;
      truncated) head -c 70000 /dev/zero | tr '\0' X;;
      stderr-truncated) head -c 70000 /dev/zero | tr '\0' X >&2;;
    esac
    exit 0;;
  *"org.freedesktop.DBus.Properties Set ssv org.freedesktop.NetworkManager WirelessEnabled b "*)
    case "$*" in *--allow-interactive-authorization=no*) ;; *) exit 9;; esac
    value="${@: -1}"
    echo "$value" >> "$HOME/radio-calls"
    touch "$HOME/radio-started"
    mode=$(cat "$HOME/radio-write-mode" 2>/dev/null)
    case "$mode" in
      denied) echo 'Wi-Fi write not authorized' >&2; exit 1;;
      timeout) exec sleep 25;;
      held) while [ ! -f "$HOME/release-radio" ]; do sleep .01; done;;
    esac
    touch "$HOME/radio-written"
    if [ "$mode" != no-op ]; then
      printf '%s' "$value" > "$HOME/radio-enabled"
      if [ "$value" = true ]; then echo enabled:unknown; else echo disabled:unknown; fi > "$HOME/read-general"
    fi
    case "$mode" in
      output) echo unexpected;;
      truncated) head -c 70000 /dev/zero | tr '\0' X;;
      changed-then-failed) exit 1;;
    esac
    exit 0;;
esac
case "$*" in *--allow-interactive-authorization=no*"RequestScan a{sv} 0") ;; *) echo unexpected-command >&2; exit 9;; esac
for arg in "$@"; do case "$arg" in /org/freedesktop/NetworkManager/Devices/*) device="${arg##*/}";; esac; done
echo "$device" >> "$HOME/scan-calls"
touch "$HOME/scan-started"
mode=$(cat "$HOME/scan-mode-$device" 2>/dev/null)
case "$mode" in
  denied) echo 'Scan not authorized' >&2; exit 1;;
  output) echo unexpected;;
  truncated) head -c 70000 /dev/zero | tr '\0' X;;
  timeout) exec sleep 25;;
  held) while [ ! -f "$HOME/release-scan" ]; do sleep .01; done;;
esac
exit 0)SH");
    }
    void set(const QString &section, const QByteArray &bytes) { writeFile(s.env.home + "/read-" + section, bytes); }
    void mode(const QString &section, const QString &mode) { writeFile(s.env.home + "/mode-" + section, mode.toUtf8()); }
    Env ownedPreviewDriver() {
        previewReceipt();
        const QString script = s.env.home + "/preview-driver.py";
        writeFile(script, R"PY(import json,os,pathlib,sys,time
h=pathlib.Path(os.environ['HOME']); directory=pathlib.Path(os.environ['XDG_STATE_HOME'])/'omarchy-control/network-previews'
a=sys.argv[1:]; verb=a[2]; token=a[3]; path=directory/(token+'.json'); r=json.loads(path.read_text())
mode=(h/'owned-mode').read_text() if (h/'owned-mode').exists() else ''
if mode=='invalid': print('invalid'); sys.exit(0)
if mode=='slow': time.sleep(.3)
ok=True
if verb=='keep':
    assert '--yes' in a
    if mode=='keep-failed': r['phase']='unknown';ok=False
    else: r['phase']='kept'
elif verb=='revert': r['phase']='reverted'
elif mode=='daemon-changed': r['phase']='daemon_changed';ok=False
if mode=='unconfirmed': ok=False
path.write_text(json.dumps(r))
print(json.dumps(dict(apiVersion=1,ok=ok,outcome=r['phase'],verified=verb!='status' and ok,data=dict(receipt=r,targetStateVerified=ok,remainingSeconds=50))))
sys.exit(0 if ok else 1)
)PY");
        s.command("preview-control", "exec '" + QStandardPaths::findExecutable("python3").toUtf8() + "' '" + script.toUtf8() + "' \"$@\"");
        Env env = s.env; env.selfBinary = s.bin("preview-control"); return env;
    }
    QString previewReceipt(const QString &phase = "preview") {
        const QString path = s.env.appStateDir + "/network-previews";
        QDir().mkpath(path);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString token(32, 'a');
        const QJsonObject value{{"version", 1}, {"token", token}, {"phase", phase}, {"action", "activate"}, {"interface", "wlan0"},
            {"uuid", "22222222-2222-2222-2222-222222222222"}, {"beforeUuid", "11111111-1111-1111-1111-111111111111"}, {"beforeState", 100},
            {"device", "/org/freedesktop/NetworkManager/Devices/one"}, {"udi", "fixture-one"}, {"type", 2}, {"busId", QString(32, 'b')}, {"owner", ":1.2"},
            {"requestedBootMs", "1"}, {"checkpoint", "/org/freedesktop/NetworkManager/Checkpoint/1"}, {"createdBootMs", "1"}, {"expiresBootMs", "60001"},
            {"active", "/org/freedesktop/NetworkManager/ActiveConnection/2"}, {"message", ""}};
        FileOps::atomicWrite(path + "/" + token + ".json", QJsonDocument(value).toJson());
        FileOps::atomicWrite(path + "/current.json", QJsonDocument(QJsonObject{{"token", token}}).toJson());
        return path + "/" + token + ".json";
    }
};


// A compositor whose live bindings change only on reload. Module data is read
// as JSON text; neither fixture nor discovery evaluates any Lua.
struct HyprDesk {
    Sandbox s;
    HyprDesk() {
        s.minimalHyprland();
        writeFile(s.env.home + "/stock.json", "[]");
        writeFile(s.env.home + "/live.json", "[]");
        const QString backend = s.env.home + "/hypr.py";
        writeFile(backend, R"PY(import json, os, pathlib, sys
h=pathlib.Path(os.environ['HOME']); a=sys.argv[1:]
def mode(name): return (h/name).exists()
def emit(value): print(json.dumps(value))
def records():
    m=h/'.config/hypr/omarchy-control.lua'
    if not m.exists(): return {}
    for line in m.read_text().splitlines():
        if line.startswith('-- omarchy-control-data: '): return json.loads(line.split(': ',1)[1])
    return {}
n=int((h/'reloads').read_text()) if (h/'reloads').exists() else 0
if a==['-j','version']:
    emit({'version':'9.9.0' if mode('new-version') else '0.56.2'})
elif a==['-j','configerrors']:
    if mode('bad-errors'): print('{}'); sys.exit(0)
    if mode('post-bad-errors') and n>0: print('{}'); sys.exit(0)
    emit(['baseline warning'] + (['new rule error'] if mode('new-error') and records().get('rules') else []))
elif a==['-j','binds']:
    if mode('bad-binds'): sys.exit(1)
    rows=json.loads((h/'live.json').read_text())
    if mode('wrong-command') and n>0:
        for b in rows: b['dispatcher']='exec'; b['arg']='different command'
    emit(rows)
elif a==['reload']:
    n+=1; (h/'reloads').write_text(str(n))
    if mode('recovery-fail') and n>1: sys.exit(1)
    if mode('edit-after-reload') and n==1:
        m=h/'.config/hypr/omarchy-control.lua'
        m.write_text(m.read_text()+'\n-- concurrent external edit\n')
    if not mode('no-effect'):
        rows=json.loads((h/'stock.json').read_text())
        for record in records().get('binds',[]):
            parts=record['keys'].split(' + ')
            mask=sum({'SUPER':64,'CTRL':4,'ALT':8,'SHIFT':1}[v] for v in parts[:-1])
            if record.get('replaces'): rows=[b for b in rows if not (b['modmask']==mask and b['key'].upper()==parts[-1] and not b.get('submap'))]
            if not record.get('unbindOnly'): rows.append(dict(modmask=mask,key=parts[-1],description=record.get('description',''),dispatcher='__lua',arg=str(100+len(rows)),submap=''))
        (h/'live.json').write_text(json.dumps(rows))
else: sys.exit(91)
)PY");
        const QString python = QStandardPaths::findExecutable("python3");
        s.command("hyprctl", "exec '" + python.toUtf8() + "' '" + backend.toUtf8() + "' \"$@\"");
    }
    void mode(const QString &name) { writeFile(s.env.home + "/" + name, "1"); }
    void stock(const QJsonArray &rows) {
        writeFile(s.env.home + "/stock.json", QJsonDocument(rows).toJson());
        writeFile(s.env.home + "/live.json", QJsonDocument(rows).toJson());
    }
    QString selector(const QString &family) {
        const auto reply = CliApi::execute(s.env, family == QLatin1String("rule") ? QStringList{"rule", "list", "--api-version", "1"} : QStringList{"shortcut", "list"});
        const auto data = reply.body.value("data").toObject();
        const auto rows = family == QLatin1String("rule") ? data.value("rules").toArray() : data.value("owned").toObject().value("binds").toArray();
        return rows.isEmpty() ? QString() : rows.first().toObject().value("selector").toString();
    }
};


// Stateful shell preferences and a separate in-memory configuration. The
// fixture can leave either side unchanged, so process success cannot prove a write.
struct ShellDesk {
    Sandbox s;
    QString config() const { return s.env.configHome + "/omarchy/shell.json"; }
    void mode(const QString &name) { writeFile(s.env.home + "/" + name, "1"); }
    QJsonObject object() const { return QJsonDocument::fromJson(readAll(config())).object(); }
    void powerWidget(const QJsonValue &entry = QJsonObject{{"id", "omarchy.power"}, {"customOption", "preserve"}}) {
        mode("power-widget");
        auto d = object(); auto bar = d.value("bar").toObject(); auto layout = bar.value("layout").toObject(); auto right = layout.value("right").toArray();
        right.append(entry); layout["right"] = right; bar["layout"] = layout; d["bar"] = bar;
        const auto bytes = QJsonDocument(d).toJson(); writeFile(config(), bytes); writeFile(s.env.home + "/live-shell.json", bytes);
    }
    ShellDesk() {
        const QByteArray config = R"({"version":1,"bar":{"layout":{"left":[{"id":"omarchy.menu","unknownOption":"keep"},{"id":"omarchy.clock","format":"first"},{"id":"omarchy.clock","format":"second"}],"centerBegin":[],"center":[],"centerEnd":[],"right":["local.widget","local.tray"],"tray":[]}},"plugins":[{"id":"local.panel"}],"dock":{"pinned":["a","gone"]},"custom":{"keep":true}})";
        writeFile(this->config(), config); writeFile(s.env.home + "/live-shell.json", config);
        writeFile(s.env.omarchyPath + "/config/omarchy/shell.json", config);
        for (const QString &id : {QString("a"), QString("b")})
            writeFile(s.env.dataHome + "/applications/" + id + ".desktop", "[Desktop Entry]\nType=Application\nName=" + id.toUtf8() + "\nExec=app\n");
        const QString backend = s.env.home + "/shell-backend.py";
        writeFile(backend, R"PY(import copy, json, os, pathlib, sys, time, shutil
h=pathlib.Path(os.environ['HOME']); kind=sys.argv[1]; a=sys.argv[2:]
c=h/'.config/omarchy/shell.json'; l=h/'live-shell.json'
sections=['left','centerBegin','center','centerEnd','right','tray']
def mode(n): return (h/n).exists()
def emit(v): print(json.dumps(v))
def identity(v): return v if isinstance(v,str) else v['id']
def config(): return json.loads(c.read_text())
def live(): return json.loads(l.read_text())
def rows():
    d=live(); layout=d['bar']['layout']; disabled=d.get('disabledPlugins',[])
    table=[('omarchy.bar',['bar'],True,''),('local.alt-bar',['bar'],False,''),('omarchy.menu',['bar-widget'],True,''),('omarchy.clock',['bar-widget'],True,''),('local.widget',['bar-widget'],False,''),('local.tray',['bar-widget'],False,'omarchy.tray'),('omarchy.service',['service'],True,''),('local.panel',['panel'],False,'')]
    if mode('power-widget'): table.append(('omarchy.power',['bar-widget'],not mode('shadow-power'),''))
    if mode('lifecycle'):
        table=[v for v in table if v[2] or (h/'.config/omarchy/plugins'/v[0]).exists()]
        for manifest in (h/'.config/omarchy/plugins').glob('*/manifest.json'):
            if manifest.parent.name.startswith('.'): continue
            m=json.loads(manifest.read_text())
            if not any(v[0]==m['id'] for v in table): table.append((m['id'],m['kinds'],False,m.get('omarchy',{}).get('clonedFrom','')))
    out=[]
    for id,kinds,first,clone in table:
        enabled=(d['bar'].get('id','omarchy.bar')==id) if kinds==['bar'] else (any(identity(v)==id for s in sections for v in layout.get(s,[])) if kinds==['bar-widget'] else (id not in disabled if first else any(v['id']==id for v in d.get('plugins',[]))))
        out.append(dict(id=id,name=id,kinds=kinds,firstParty=first,clonedFrom=clone,canDisable=kinds!=['bar'],active=kinds==['bar'] and enabled,enabled=enabled))
    return out
if kind=='shell':
    if a==['shell','listShellConfig']:
        if mode('live-down'): sys.exit(1)
        if mode('malformed-live'): print('[]'); sys.exit(0)
        emit(live()); sys.exit(0)
    if a==['shell','rescanPlugins']: print('ok'); sys.exit(0)
    if a==['shell','reloadConfig']:
        l.write_bytes(c.read_bytes()); print('ok'); sys.exit(0)
    sys.exit(91)
if kind!='omarchy': sys.exit(92)
if a==['plugin','list','--json']:
    if mode('plugin-down'): sys.exit(1)
    if mode('bad-registry'): print('{}'); sys.exit(0)
    if mode('truncated-registry'): print('[]'); print('x'*4300000); sys.exit(0)
    r=rows()
    if mode('duplicate-registry'): r.append(r[0])
    if mode('unicode-registry'): r.append(dict(id='local.функция',name='Unicode',kinds=['panel'],firstParty=False,clonedFrom='',canDisable=True,active=False,enabled=False))
    if mode('false-registry') and (h/'changed').exists():
        for item in r:
            if item['id']=='omarchy.service': item['enabled']=False
    emit(r); sys.exit(0)
d=config()
if mode('noop'): sys.exit(0)
if a[:2]==['plugin','clone']:
    source=a[2]; new=os.environ['USER']+'.'+source.removeprefix('omarchy.')
    target=h/'.config/omarchy/plugins'/new
    shutil.copytree(pathlib.Path(os.environ['OMARCHY_PATH'])/'shell/plugins/clock',target)
    m=json.loads((target/'manifest.json').read_text()); m['id']=new; m['omarchy']={'clonedFrom':source}; (target/'manifest.json').write_text(json.dumps(m))
    for section in sections:
        for v in d['bar']['layout'].get(section,[]):
            if isinstance(v,dict) and v['id']==source: v['id']=new
elif a[:2]==['plugin','remove']:
    assert a[3:]==['--yes']
    target=h/'.config/omarchy/plugins'/a[2]
    manifest=json.loads((target/'manifest.json').read_text()); source=manifest.get('omarchy',{}).get('clonedFrom','')
    if target.is_symlink(): target.unlink()
    elif (target/'.git').is_dir(): shutil.rmtree(target)
    else: target.rename(target.parent/('.'+target.name+'.bak.fixture'))
    d['plugins']=[v for v in d.get('plugins',[]) if v['id']!=a[2]]
    for section in sections:
        for v in d['bar']['layout'].get(section,[]):
            if isinstance(v,dict) and v['id']==a[2]: v['id']=source
elif a[:2]==['plugin','disable']:
    id=a[2]
    if id=='omarchy.service': d.setdefault('disabledPlugins',[]).append(id)
    elif id=='local.panel': d['plugins']=[v for v in d['plugins'] if v['id']!=id]
    else:
        for s in sections: d['bar']['layout'][s]=[v for v in d['bar']['layout'].get(s,[]) if identity(v)!=id]
elif a[:2]==['plugin','enable']:
    id=a[2]
    if id=='omarchy.service': d['disabledPlugins']=[v for v in d.get('disabledPlugins',[]) if v!=id]
    elif id=='local.alt-bar' or id=='omarchy.bar': d['bar']['id']=id
    elif id=='local.panel': d['plugins'].append({'id':id})
    else: d['bar']['layout']['centerEnd'].append({'id':id})
elif a[:2]==['bar','move']:
    id=a[2]; destination=a[4]; index=int(a[6]); source=a[8]; original=int(a[10])
    entry=d['bar']['layout'][source].pop(original)
    assert identity(entry)==id
    if mode('lose-options'): entry={'id':id}
    d['bar']['layout'][destination].insert(index,entry)
elif a[:2]==['bar','set']:
    id,key,value=a[2:5]
    assert a[5]=='--json' and a[6]=='--from-section' and a[8]=='--from-index'
    source=a[7]; index=int(a[9]); entry=d['bar']['layout'][source][index]
    assert isinstance(entry,dict) and identity(entry)==id
    entry[key]=json.loads(value)
    if mode('lose-options'): d['bar']['layout'][source][index]={'id':id,key:json.loads(value)}
    if mode('change-bar'): d['bar']['background']='unexpected change'
elif a[:2]==['dock','pin']:
    if a[2] not in d['dock']['pinned']: d['dock']['pinned'].append(a[2])
elif a[:2]==['dock','unpin']: d['dock']['pinned']=[id for id in d['dock']['pinned'] if id!=a[2]]
else: sys.exit(93)
(h/'changed').write_text('1')
if mode('slow'): time.sleep(.4)
if mode('false-registry'): sys.exit(0)
if not mode('memory-only'): c.write_text(json.dumps(d))
if not mode('disk-only'): l.write_text(json.dumps(d))
if mode('concurrent-edit'):
    d['external-choice']='keep'; c.write_text(json.dumps(d)); sys.exit(4)
if mode('fail-after-write'): sys.exit(4)
)PY");
        const QString python = QStandardPaths::findExecutable("python3");
        for (const auto &pair : {qMakePair(QString("omarchy"), QString("omarchy")), qMakePair(QString("omarchy-shell"), QString("shell"))})
            s.command(pair.first, "exec '" + python.toUtf8() + "' '" + backend.toUtf8() + "' '" + pair.second.toUtf8() + "' \"$@\"");
    }
    QJsonObject api(const QStringList &args) { return CliApi::execute(s.env, args).body; }
    QString selector(const QString &id, int occurrence = 0) {
        const auto widgets = api({"bar", "status"}).value("data").toObject().value("bar").toObject().value("widgets").toArray();
        for (const auto &value : widgets) if (value.toObject().value("id").toString() == id && occurrence-- == 0) return value.toObject().value("selector").toString();
        return {};
    }
};

// Stateful IPC fixtures. Reads and writes use the same device facts, so a
// successful subprocess alone can never make the provider's verification pass.
struct DeviceDesk {
    Sandbox s;
    QString state() const { return s.env.home + "/devices.json"; }
    void mode(const QString &name) { writeFile(s.env.home + "/" + name, "1"); }
    QJsonObject data() const { return QJsonDocument::fromJson(readAll(state())).object(); }
    void set(const QString &key, const QJsonValue &value) {
        auto object = data(); object[key] = value; writeFile(state(), QJsonDocument(object).toJson());
    }
    DeviceDesk() {
        writeFile(state(), R"({"defaultOut":"dsp","defaultIn":"mic","speakersVolume":40,"dspVolume":100,"phonesVolume":30,"micVolume":60,"mic2Volume":50,"speakersMute":false,"dspMute":false,"phonesMute":false,"micMute":false,"mic2Mute":false,"streamSink":11,"radio":true,"otherRadio":false,"paired":true,"connected":false,"known":true})");
        const QString script = s.env.home + "/backend.py";
        writeFile(script, R"PY(import json, os, pathlib, sys, time
h=pathlib.Path(os.environ['HOME'])
state=h/'devices.json'
d=json.loads(state.read_text())
kind=sys.argv[1]; a=sys.argv[2:]
def mode(name): return (h/name).exists()
def emit(value): print(json.dumps(value))
def change():
    if not mode('noop'):
        state.write_text(json.dumps(d))
    (h/'changed').write_text('1')
    sys.exit(0)
def node(name,index,identity):
    return dict(name=name,index=index,description=name,mute=d[name+'Mute'],volume={'mono':{'value_percent':str(d[name+'Volume'])+'%'}},properties={'object.id':str(identity)})
if kind=='pactl':
    if a[:3]==['-f','json','list']:
        if mode('malformed'): print('not-json'); sys.exit(0)
        if mode('fail-after') and mode('changed'): sys.exit(2)
        if a[3]=='sinks':
            reads=int((h/'sink-reads').read_text()) if (h/'sink-reads').exists() else 0
            (h/'sink-reads').write_text(str(reads+1))
            emit([node('dsp',10,100),node('speakers',11,999 if mode('recycle-device') and reads else 101),node('phones',12,102)])
        elif a[3]=='sources': emit([node('mic',20,200),node('mic2',21,201)])
        else:
            reads=int((h/'stream-reads').read_text()) if (h/'stream-reads').exists() else 0
            (h/'stream-reads').write_text(str(reads+1))
            emit([dict(index=7,sink=d['streamSink'],properties={'application.name':'Player','object.serial':'701' if mode('recycle-stream') and reads else '700'})])
    elif a[0]=='get-default-sink': print(d['defaultOut'])
    elif a[0]=='get-default-source': print(d['defaultIn'])
    elif a[0] in ['set-sink-volume','set-source-volume']:
        d[a[1]+'Volume']=int(a[2].removesuffix('%')); change()
    elif a[0] in ['set-sink-mute','set-source-mute']:
        d[a[1]+'Mute']=a[2]=='1'; change()
    elif a[0]=='move-sink-input':
        d['streamSink']={'dsp':10,'speakers':11,'phones':12}[a[2]]; change()
    else: sys.exit(2)
elif kind=='omarchy':
    if a==['audio','output','sink']:
        if mode('slow-route'): time.sleep(.12)
        print('speakers' if d['defaultOut']=='dsp' else d['defaultOut'])
    elif a[:4] in [['audio','output','set','default'],['audio','input','set','default']]:
        d['defaultOut' if a[1]=='output' else 'defaultIn']=a[5]; change()
    elif a[:2]==['bluetooth','power']:
        d['radio']=a[2]=='on'; d['otherRadio']=a[2]=='on' and not mode('partial-power-on'); change()
    elif a[:2]==['bluetooth','device']:
        if a[2]=='connect': d['radio']=True; d['connected']=True
        elif a[2]=='disconnect': d['connected']=False
        elif a[2]=='forget': d['radio']=True; d['known']=False; d['paired']=False; d['connected']=False
        change()
    else: sys.exit(2)
elif kind=='bluetoothctl':
    if a[0]=='list': print('Controller 00:11:22:33:44:55 First\nController 00:11:22:33:44:66 Second')
    elif a[0]=='show':
        if mode('radio-read-fails') or (mode('radio-after-fails') and mode('changed')): sys.exit(2)
        print('Powered: '+('yes' if d['radio' if a[1].endswith('55') else 'otherRadio'] else 'no'))
    elif a[0]=='devices':
        if mode('list-after-fails') and mode('changed'): sys.exit(2)
        if d['known']: print('Device AA:BB:CC:DD:EE:01 Headset')
    elif a[0]=='info':
        if mode('info-after-fails') and mode('changed'): print('Connected: no'); sys.exit(2)
        if mode('empty-info-after') and mode('changed'): sys.exit(0)
        if not d['known']: sys.exit(1)
        print('Name: Headset\nPaired: '+('yes' if d['paired'] else 'no')+'\nConnected: '+('yes' if d['connected'] else 'no'))
    else: sys.exit(2)
)PY");
        const QString python = QStandardPaths::findExecutable("python3");
        for (const QString &kind : {QString("pactl"), QString("omarchy"), QString("bluetoothctl")})
            s.command(kind, "exec '" + python.toUtf8() + "' '" + script.toUtf8() + "' " + kind.toUtf8() + " \"$@\"");
    }
};

struct ServiceDesk {
    Sandbox s;
    QString state() const { return s.env.home + "/services.json"; }
    QJsonObject data() const { return QJsonDocument::fromJson(readAll(state())).object(); }
    void mode(const QString &name) { writeFile(s.env.home + "/" + name, "1"); }
    void set(const QString &scope, const QString &key, const QString &value) {
        auto all = data(); auto facts = all.value(scope).toObject(); facts[key] = value; all[scope] = facts;
        writeFile(state(), QJsonDocument(all).toJson());
    }
    ServiceDesk() {
        QJsonObject all;
        for (const QString &scope : {QStringLiteral("user"), QStringLiteral("system")})
            all[scope] = QJsonObject{{"Id", "agent.service"}, {"LoadState", "loaded"}, {"FragmentPath", "/usr/lib/systemd/" + scope + "/agent.service"},
                                    {"ActiveState", "inactive"}, {"SubState", "dead"}, {"UnitFileState", "disabled"}, {"Type", "simple"},
                                    {"Result", "success"}, {"InvocationID", "old"}, {"ExecMainStartTimestampMonotonic", "10"}};
        writeFile(state(), QJsonDocument(all).toJson());
        const QString script = s.env.home + "/service-backend.py";
        writeFile(script, R"SV(import json, os, pathlib, sys
h=pathlib.Path(os.environ['HOME']); state=h/'services.json'
d=json.loads(state.read_text()); a=sys.argv[1:]
scope='system' if '--system' in a else 'user'; f=d[scope]
def mode(name): return (h/name).exists()
def emit(value): print(json.dumps(value))
if mode(scope+'-unavailable'): sys.exit(2)
if 'list-units' in a:
    emit([dict(unit='agent.service',load=f['LoadState'],active=f['ActiveState'],sub=f['SubState'],description=scope+' agent')])
elif 'list-unit-files' in a:
    if mode('files-error'): sys.exit(2)
    if mode('malformed-list'): print('{}'); sys.exit(0)
    emit([dict(unit_file='agent.service',state=f['UnitFileState'])])
elif 'show' in a:
    count=int((h/'show-count').read_text()) if (h/'show-count').exists() else 0
    (h/'show-count').write_text(str(count+1))
    if mode('read-after-fails') and mode('changed'): sys.exit(2)
    if mode('empty-after') and mode('changed'): sys.exit(0)
    if a[-1]!='agent.service': f['Id']=a[-1]; f['LoadState']='not-found'
    if mode('stale-before-write') and count>=2: f['FragmentPath']='/different.service'
    if mode('identity-after') and mode('changed'): f['Id']='replacement.service'
    if mode('missing-field'): del f['ActiveState']
    for key,value in f.items(): print(key+'='+value)
else:
    action=next((v for v in a if v in ['start','stop','restart','enable','disable']),None)
    if not action: sys.exit(2)
    if mode('authorization-denied'):
        assert '--no-ask-password' in a
        print('Interactive authentication required.',file=sys.stderr); sys.exit(1)
    if not mode('noop'):
        if action in ['start','restart']:
            f['ExecMainStartTimestampMonotonic']=str(int(f['ExecMainStartTimestampMonotonic'])+1)
            f['InvocationID']='new'+f['ExecMainStartTimestampMonotonic']; f['Result']='success'
            f['ActiveState']='inactive' if f['Type']=='oneshot' else 'active'
        elif action=='stop': f['ActiveState']='inactive'
        elif action in ['enable','disable']: f['UnitFileState']='enabled' if action=='enable' else 'disabled'
        if mode('activating-after'): f['ActiveState']='activating'
        state.write_text(json.dumps(d))
    (h/'changed').write_text('1')
)SV");
        s.command("systemctl", "exec '" + QStandardPaths::findExecutable("python3").toUtf8() + "' '" + script.toUtf8() + "' \"$@\"");
    }
};

struct MimeDesk {
    Sandbox s;
    QString generic() const { return s.env.configHome + "/mimeapps.list"; }
    void mode(const QString &name) { writeFile(s.env.home + "/" + name, "1"); }
    void app(const QString &id, const QString &extra = {}, bool user = false) {
        writeFile((user ? s.env.dataHome : s.env.dataDirs.last()) + "/applications/" + id,
                  ("[Desktop Entry]\nType=Application\nName=" + id + "\nExec=true\nMimeType=text/plain;application/pdf;application/x-custom;\n" + extra).toUtf8());
    }
    MimeDesk() {
        app("a.desktop"); app("b.desktop", "NoDisplay=true\n");
        s.env.process.insert("XDG_CONFIG_DIRS", s.env.sys("/etc/xdg"));
        s.env.process.insert("XDG_CURRENT_DESKTOP", "Hyprland");
        writeFile(generic(), "# keep\n[Default Applications]\ntext/plain=a.desktop;\napplication/pdf=a.desktop;\n[Added Associations]\ntext/plain=a.desktop;b.desktop;\n");
        const QString script = s.env.home + "/mime-backend.py";
        writeFile(script, R"MI(import fcntl, os, pathlib, sys
h=pathlib.Path(os.environ['HOME']); a=sys.argv[1:]
def mode(name): return (h/name).exists()
if a[:2]!=['query','default']: sys.exit(2)
with (h/'query-count').open('a+') as counter:
    fcntl.flock(counter,fcntl.LOCK_EX); counter.seek(0); count=int(counter.read() or '0')
    counter.seek(0); counter.truncate(); counter.write(str(count+1)); counter.flush()
if mode('query-fails'): print('b.desktop'); sys.exit(2)
if mode('multiple-results'): print('a.desktop\nb.desktop'); sys.exit(0)
if mode('empty-query'): sys.exit(0)
config=pathlib.Path(os.environ['XDG_CONFIG_HOME'])
desktops=[v.lower() for v in os.environ.get('XDG_CURRENT_DESKTOP','').split(':') if v]
files=[config/(v+'-mimeapps.list') for v in desktops]+[config/'mimeapps.list']
answer='a.desktop'
for file in files:
    if not file.exists(): continue
    group=''
    for raw in file.read_text().splitlines():
        line=raw.strip()
        if line.startswith('['): group=line
        elif group=='[Default Applications]' and line.startswith(a[2]+'='):
            answer=line.split('=',1)[1].split(';')[0]; break
    else: continue
    break
if mode('app-replaced') and count>=2:
    target=pathlib.Path(os.environ['XDG_DATA_HOME'])/'applications/b.desktop'
    target.parent.mkdir(parents=True,exist_ok=True); target.write_text('[Desktop Entry]\nHidden=true\n')
if mode('unknown-handler'): answer='missing.desktop'
if mode('ignore-write'): answer='a.desktop'
if mode('fail-after-write') and answer=='b.desktop': print(answer); sys.exit(2)
print(answer)
)MI");
        s.command("xdg-mime", "exec '" + QStandardPaths::findExecutable("python3").toUtf8() + "' '" + script.toUtf8() + "' \"$@\"");
        s.command("gtk-launch", "exit 0");
    }
};

struct FactsDesk {
    Sandbox s;
    QString exports() const { return s.env.appStateDir + "/exports"; }
    void mode(const QString &name) { writeFile(s.env.home + "/" + name, "1"); }
    void battery(const QString &name, const QString &capacity, const QString &status = "Charging", const QString &cycles = "0") {
        const QString path = s.env.sys("/sys/class/power_supply/") + name;
        writeFile(path + "/type", "Battery\n"); writeFile(path + "/capacity", capacity.toUtf8() + "\n");
        writeFile(path + "/status", status.toUtf8() + "\n"); writeFile(path + "/cycle_count", cycles.toUtf8() + "\n");
    }
    FactsDesk() {
        s.debugHelper();
        QDir().mkpath(s.env.sys("/sys/class/power_supply"));
        writeFile(s.env.sys("/sys/class/dmi/id/product_name"), "Fixture laptop\n");
        writeFile(s.env.sys("/sys/class/dmi/id/sys_vendor"), "Fixture vendor\n");
        s.env.process.insert("XDG_SESSION_TYPE", "wayland");
        s.command("df", "printf 'Filesystem Type 1B-blocks Used Avail Use%% Mounted on\\n/dev/a btrfs 1000 420 580 42%% /\\n/dev/a btrfs 1000 420 580 42%% /home\\n'");
        s.command("hyprctl", "case \"$*\" in *version*) echo '{\"version\":\"0.56.2\"}';; *devices*) echo '{\"mice\":[]}' ;; esac");
        s.omarchy(R"SH(case "$1" in
version) if [ -f "$HOME/slow" ]; then sleep .15; fi; echo 4.5.0;;
shell) exit 0;;
debug)
    if [ -f "$HOME/timeout" ]; then exec sleep 40; fi
    if [ -f "$HOME/slow" ]; then sleep .15; fi
    if [ -f "$HOME/empty" ]; then exit 0; fi
    if [ -f "$HOME/truncated" ]; then head -c 1100000 /dev/zero | tr '\0' X; exit 0; fi
    if [ -f "$HOME/stderr-truncated" ]; then head -c 1100000 /dev/zero | tr '\0' X >&2; echo report; exit 0; fi
    printf 'Local debug report\npassword="two secret words"\n'
    printf 'token: hidden-token\n' >&2
    if [ -f "$HOME/fail" ]; then exit 7; fi;;
esac)SH");
    }
};

// A sandbox whose Hyprland holds blur as the real one would: the fake used by the
// screenshot sandbox, Omarchy's glass, and a stock hyprland.lua.
static void blurSandbox(Sandbox &s) {
    s.minimalHyprland();
    writeFile(s.bin("hyprctl"), readAll(PROJECT_ROOT "/tools/fake-hyprctl"));
    QFile::setPermissions(s.bin("hyprctl"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    writeFile(s.env.omarchyPath + "/default/hypr/glass.lua",
              "return function()\n  hl.config({\n    decoration = {\n      blur = {\n        enabled = true,\n        size = 4,\n        passes = 2,\n"
              "        contrast = 1.0,\n        brightness = 0.75,\n        noise = 0.01,\n        vibrancy = 0.35,\n        vibrancy_darkness = 0.35,\n"
              "        popups = true,\n        special = true,\n      },\n    },\n  })\nend\n");
    QDir().mkpath(s.env.stateHome + "/omarchy/toggles/hypr");
}
static void blurToggle(Sandbox &s, const QString &name, bool on) {
    const QString path = s.env.stateHome + "/omarchy/toggles/hypr/" + name + ".lua";
    if (on)
        writeFile(path, "-- toggle\n");
    else
        QFile::remove(path);
}
Transaction applySetting(SettingsEngine &engine, const QString &id, const QString &value) {
    Transaction result;
    bool done = false;
    engine.apply(id, value, [&](const Transaction &tx) { result = tx; done = true; });
    QTest::qWaitFor([&] { return done; }, 15000);
    return result;
}

ReadResult readSetting(SettingsEngine &engine, const QString &id) {
    ReadResult result;
    bool done = false;
    engine.read(id, [&](const ReadResult &r) { result = r; done = true; });
    QTest::qWaitFor([&] { return done; }, 8000);
    return result;
}

Transaction undoTx(SettingsEngine &engine, const QString &id) {
    Transaction result;
    bool done = false;
    engine.undo(id, [&](const Transaction &tx) { result = tx; done = true; });
    QTest::qWaitFor([&] { return done; }, 15000);
    return result;
}

QStringList argvOf(const QJsonObject &action) {
    QStringList out;
    for (const auto &part : action.value("argv").toArray())
        out << part.toString();
    return out;
}

const char kSvg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 48 48\"><rect width=\"48\" height=\"48\" fill=\"#ff0000\"/></svg>";

// A pack on disk, shaped as the spec describes. `icons` are placed in its scalable folder.
void addPack(const QString &root, const QString &id, const QStringList &icons = {"folder", "text-x-generic", "utilities-terminal", "web-browser"},
             const QString &inherits = {}, const QByteArray &extra = {}) {
    QByteArray index = "[Icon Theme]\nName=" + id.toUtf8() + "\n" + extra;
    if (!inherits.isEmpty())
        index += "Inherits=" + inherits.toUtf8() + "\n";
    index += "Directories=48x48/apps,scalable/apps\n\n[48x48/apps]\nSize=48\nType=Fixed\n\n[scalable/apps]\nSize=48\nMinSize=8\nMaxSize=512\nType=Scalable\n";
    writeFile(root + "/" + id + "/index.theme", index);
    QDir().mkpath(root + "/" + id + "/48x48/apps");
    for (const QString &icon : icons)
        writeFile(root + "/" + id + "/scalable/apps/" + icon + ".svg", kSvg);
}

// Icon packs and a fake session. Yaru-blue is what Omarchy's fallback names.
struct IconDesk {
    Sandbox &s;
    QString user, system;
    explicit IconDesk(Sandbox &sandbox) : s(sandbox), user(sandbox.env.dataHome + "/icons"), system(sandbox.env.dataDirs.last() + "/icons") {
        s.command("gsettings", kFakeGsettings);
        s.command("dconf", kFakeDconf);
        addPack(system, "Yaru-blue");
        addPack(system, "Papirus");
        addPack(system, "Numix");
    }
    QString effective() const { return QString::fromUtf8(readAll(s.env.home + "/.fake-icon-theme")); }
    void setEffective(const QString &value) const { writeFile(s.env.home + "/.fake-icon-theme", value.toUtf8()); }
    bool isUnset() const { return !QFile::exists(s.env.home + "/.fake-icon-theme"); }
    void themeAsks(const QString &id) const { writeFile(s.env.stateHome + "/omarchy/current/theme/icons.theme", id.toUtf8() + "\n"); }
    QString policy() const { return s.env.appConfigDir + "/icons.json"; }
    QString hook() const { return s.env.configHome + "/omarchy/hooks/theme-set.d/90-omarchy-control-icons"; }
    int writes() const {
        int count = 0;
        for (const QString &call : s.calls())
            if (call.startsWith("gsettings set") || call.startsWith("gsettings reset"))
                ++count;
        return count;
    }
};

// Omarchy's launcher as the layer expects it, the launchers it shows, and an app whose only
// icon is its own PNG in hicolor.
const char kAppLibrary[] =
    "  function iconSource(icon) {\n"
    "    var found = root.iconIndex[value]\n"
    "    var themed = Quickshell.iconPath(value, true)\n"
    "  }\n"
    "  function iconIndexScanCommand() {\n"
    "    return [\n"
    "      'dirs=\"$HOME/.icons $HOME/.local/share/icons\";',\n"
    "      'find \"$base\" -path \"*/apps/*\" -o -path \"*/devices/*\"',\n"
    "      'awk /.svg$/ { print; next } { png[++n] = $0 }'\n"
    "    ].join(' ')\n"
    "  }\n";

void addLauncher(const QString &folder, const QString &id, const QString &name, const QString &icon, const QByteArray &extra = {}) {
    writeFile(folder + "/" + id + ".desktop", "[Desktop Entry]\nType=Application\nName=" + name.toUtf8() + "\nIcon=" + icon.toUtf8() + "\nExec=true\n" + extra);
}

struct LauncherDesk {
    QString apps, layer;
    explicit LauncherDesk(Sandbox &s) : apps(s.env.dataDirs.last() + "/applications"), layer(s.env.home + "/.icons/omarchy-control") {
        writeFile(s.env.omarchyPath + "/shell/services/AppLibrary.qml", kAppLibrary);
        const QString hicolor = s.env.dataDirs.last() + "/icons/hicolor";
        writeFile(hicolor + "/index.theme", "[Icon Theme]\nName=Hicolor\nDirectories=256x256/apps\n\n[256x256/apps]\nSize=256\nContext=Applications\nType=Threshold\n");
        QDir().mkpath(hicolor + "/256x256/apps");
        QImage own(300, 200, QImage::Format_ARGB32);
        own.fill(Qt::blue);
        own.save(hicolor + "/256x256/apps/ownapp.png");
        addLauncher(apps, "terminal", "Terminal", "utilities-terminal");
        // Called by its last name in most packs.
        addLauncher(apps, "org.example.Browser", "Browser", "org.example.web-browser");
        addLauncher(apps, "own", "Own App", "ownapp");
        addLauncher(apps, "nothing", "Nothing", "no-such-icon");
        addLauncher(apps, "hidden", "Hidden Helper", "folder", "NoDisplay=true\n");
        // A path is the app's own file, and the launcher shows it as it is.
        addLauncher(apps, "path", "Path App", "/opt/app/icon.png");
    }
    QStringList entries(const Sandbox &s) const {
        return QDir(s.env.home + "/.icons").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    }
};

Transaction removeIconPack(IconThemeProvider &icons, const QString &id) {
    Transaction result;
    bool done = false;
    icons.removePack(id, [&](const Transaction &tx) { result = tx; done = true; });
    QTest::qWaitFor([&] { return done; }, 15000);
    return result;
}

Transaction launcherChange(const std::function<void(ChangeCoordinator::Finished)> &start) {
    Transaction result;
    bool done = false;
    start([&](const Transaction &tx) { result = tx; done = true; });
    QTest::qWaitFor([&] { return done; }, 15000);
    return result;
}

// An archive written entry by entry, so a test can make one no real tool would: paths that
// climb out, links to write through, and devices.
struct ArchiveItem {
    QString path;
    char type = 'f';  // f file, d folder, l link, h hard link, c device
    QByteArray data;
    QString target;
    qint64 declaredSize = -1;
};

void writeArchive(const QString &file, const QList<ArchiveItem> &items) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    struct archive *writer = archive_write_new();
    if (file.endsWith(".zip")) {
        archive_write_set_format_zip(writer);
    } else {
        archive_write_set_format_pax_restricted(writer);
        archive_write_add_filter_gzip(writer);
    }
    archive_write_open_filename(writer, QFile::encodeName(file).constData());
    for (const ArchiveItem &item : items) {
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.path.toUtf8().constData());
        archive_entry_set_perm(entry, item.type == 'd' ? 0755 : 0644);
        if (item.type == 'd') {
            archive_entry_set_filetype(entry, AE_IFDIR);
        } else if (item.type == 'l') {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, item.target.toUtf8().constData());
        } else if (item.type == 'h') {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_hardlink(entry, item.target.toUtf8().constData());
        } else if (item.type == 'c') {
            archive_entry_set_filetype(entry, AE_IFCHR);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_size(entry, item.declaredSize >= 0 ? item.declaredSize : item.data.size());
        }
        archive_write_header(writer, entry);
        if (item.type == 'f' && !item.data.isEmpty())
            archive_write_data(writer, item.data.constData(), size_t(item.data.size()));
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
}

QByteArray packIndex(const QString &name, const QString &inherits = {}) {
    return "[Icon Theme]\nName=" + name.toUtf8() + "\n" + (inherits.isEmpty() ? QByteArray() : "Inherits=" + inherits.toUtf8() + "\n")
           + "Directories=scalable/apps\n\n[scalable/apps]\nSize=48\nMinSize=8\nMaxSize=512\nType=Scalable\nContext=Applications\n";
}

QString importPack(IconThemeImporter &importer, const QString &folder) {
    QSignalSpy finished(&importer, &IconThemeImporter::finished);
    importer.choosePath(folder);
    if (importer.busy())
        finished.wait(30000);
    return importer.state();
}

// ---- My Omarchy profile helpers ------------------------------------------------------

// A fake Omarchy machine: its omarchy commands, Hyprland, installed browsers, and a
// shell.json holding a key of the user's own beside the idle timers.
struct ProfileMachine {
    Sandbox s;
    ProfileMachine() {
        s.minimalHyprland();
        s.command("hyprctl", kProfileHyprctl);
        s.omarchy(kProfileOmarchy);
        s.command("firefox", "exit 0");
        s.command("chromium", "exit 0");
        writeFile(s.env.omarchyPath + "/config/omarchy/shell.json", "{\"idle\":{\"lock\":300,\"screensaver\":150},\"bar\":{\"position\":\"top\"}}\n");
        writeFile(config(), "{\n  \"bar\": {\"position\": \"top\"},\n  \"idle\": {\"lock\": 300, \"screensaver\": 150},\n  \"mine\": {\"keep\": true}\n}\n");
    }
    QString config() const { return s.env.configHome + "/omarchy/shell.json"; }
    QString state(const QString &name) const { return s.env.home + "/.fake-omarchy/" + name; }
};

ProfileCapture::Result captureProfile(AppCore &core, const Profile *existing, const ProfileCapture::Selection &selection) {
    ProfileCapture::Result result;
    bool done = false;
    core.profileCapture.capture(existing, selection, [&](const ProfileCapture::Result &r) { result = r; done = true; });
    if (!QTest::qWaitFor([&] { return done; }, 30000))
        result.error = "capture timed out";
    return result;
}

bool saveProfile(AppCore &core, const QString &directory, const ProfileCapture::Selection &selection) {
    bool exists = false;
    const ProfileModel::Parsed saved = ProfileModel::load(directory, &exists);
    const ProfileCapture::Result result = captureProfile(core, exists ? &saved.profile : nullptr, selection);
    QString error;
    return result.ok && core.profileCapture.save(directory, result, &error);
}

ProfilePlan planProfile(AppCore &core, const QString &directory, const ProfilePlanner::Options &options = {}) {
    ProfilePlan plan;
    bool done = false;
    core.profilePlanner.plan(directory, options, [&](const ProfilePlan &p) { plan = p; done = true; });
    if (!QTest::qWaitFor([&] { return done; }, 30000))
        plan.errors << "plan timed out";
    return plan;
}

ProfileRun runPlan(AppCore &core, const ProfilePlan &plan) {
    ProfileRun run;
    bool done = false;
    core.profileRunner.apply(plan, [&](const ProfileRun &r) { run = r; done = true; });
    if (!QTest::qWaitFor([&] { return done; }, 60000))
        run.message = "restore timed out";
    return run;
}

ProfileRun resumeRun(AppCore &core, const QString &id) {
    ProfileRun run;
    bool done = false;
    core.profileRunner.resume(id, [&](const ProfileRun &r) { run = r; done = true; });
    if (!QTest::qWaitFor([&] { return done; }, 60000))
        run.message = "resume timed out";
    return run;
}

ProfilePlanItem planned(const ProfilePlan &plan, const QString &target) {
    for (const ProfilePlanItem &item : plan.items)
        if (item.target == target)
            return item;
    return {};
}

void writeProfile(const QString &directory, const QJsonObject &settings, const QString &name = "Test profile") {
    writeFile(ProfileModel::manifestPath(directory),
              QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", name}, {"settings", settings}}).toJson());
}

QJsonObject setTo(const QString &value) {
    return QJsonObject{{"mode", "set"}, {"value", value}};
}

int changesBy(const ChangeCoordinator &coordinator, const QString &runId) {
    int count = 0;
    for (const Transaction &tx : coordinator.history())
        if (tx.runId == runId)
            ++count;
    return count;
}

// A machine with apps: a repository, what is installed and why, and Omarchy's defaults.
// Firefox is offered but not installed; chromium and libreoffice-fresh are Omarchy's;
// my-local-tool is in no repository; mpv needs libmpv; omarchy needs hyprland.
struct PackageMachine : ProfileMachine {
    PackageMachine() {
        s.command("pacman", kFakePacman);
        s.command("sudo", kFakeSudo);
        QFile::remove(s.bin("firefox"));
        writeFile(pacman("repo"), "extra firefox 140.0-1 nss,libvpx\nextra nss 3.110-1 -\nextra libvpx 1.15-1 -\nextra ripgrep 14.1-1 -\n"
                                  "extra libreoffice-fresh 25.2-1 -\nextra chromium 138.0-1 -\nextra hyprland 0.56.2-4 -\nextra btop 1.4.7-1 -\n"
                                  "extra obsidian 1.8-1 -\nextra mpv 0.40-1 libmpv\nextra libmpv 0.40-1 -\nomarchy omarchy 4.5.0-3 hyprland\n");
        writeFile(pacman("installed"), "omarchy 4.5.0-3 explicit hyprland\nhyprland 0.56.2-4 dependency -\nchromium 138.0-1 explicit -\n"
                                       "libreoffice-fresh 25.2-1 explicit -\nbtop 1.4.7-1 explicit -\nlibmpv 0.40-1 dependency -\n"
                                       "mpv 0.40-1 explicit libmpv\nmy-local-tool 1.0-1 explicit -\nthermald 2.5-1 explicit -\n"
                                       "ex-vi-compat 1.0-1 explicit -\n");
        writeFile(pacman("baseline"), "chromium\nlibreoffice-fresh\nbtop\nmpv\nhyprland\nobsidian\ntypora\nvi\n");
        // Installed by Omarchy for this hardware, and an alternative that stands in for vi.
        writeFile(s.env.omarchyPath + "/install/omarchy-other.packages", "# Hardware\nthermald\nintel-media-driver\n");
        writeFile(pacman("provides"), "ex-vi-compat vi\n");
    }
    QString pacman(const QString &name) const { return s.env.home + "/.fake-pacman/" + name; }
    QString reason(const QString &name) const {
        for (const QString &line : QString::fromUtf8(readAll(pacman("installed"))).split('\n'))
            if (line.section(' ', 0, 0) == name)
                return line.section(' ', 2, 2);
        return {};
    }
    bool has(const QString &name) const { return !reason(name).isEmpty(); }
    QStringList transactions() const { return QString::fromUtf8(readAll(pacman("transactions"))).split('\n', Qt::SkipEmptyParts); }
};

QJsonObject app(const QString &name, const QString &intent = "present", const QString &source = "repository") {
    return QJsonObject{{"name", name}, {"source", source}, {"intent", intent}};
}

void writeAppProfile(const QString &directory, const QJsonObject &settings, const QJsonArray &packages) {
    writeFile(ProfileModel::manifestPath(directory),
              QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Apps"}, {"settings", settings}, {"packages", packages}}).toJson());
}

// A machine with desktop choices to carry: two apps with launchers, Omarchy's default dock,
// file associations kept by a fake xdg-mime, and the Hyprland binds list.
struct DesktopMachine : ProfileMachine {
    DesktopMachine() {
        s.command("xdg-mime", kFakeXdgMime);
        const QString apps = s.env.dataDirs.last() + "/applications";
        addLauncher(apps, "org.example.Browser", "Browser", "web-browser");
        addLauncher(apps, "org.example.TextEditor", "Text Editor", "accessories-text-editor", "MimeType=text/plain;\n");
        writeFile(s.env.omarchyPath + "/config/omarchy/shell.json",
                  "{\"idle\":{\"lock\":300,\"screensaver\":150},\"dock\":{\"pinned\":[\"org.gnome.Nautilus\",\"chromium\"]}}\n");
    }
    QString pins() const { return QString::fromUtf8(readAll(state("pins"))).trimmed(); }
    QString mime(const QString &type) const { return QString::fromUtf8(readAll(state("mime-" + QString(type).replace('/', '_')))).trimmed(); }
    void setMime(const QString &type, const QString &app) const { writeFile(state("mime-" + QString(type).replace('/', '_')), app.toUtf8() + "\n"); }
};

// A theme in use, with one background of its own and a folder for the user's.
void useTheme(Sandbox &s, const QString &theme, const QStringList &shipped = {"1.png", "2.png"}) {
    const QString current = s.env.stateHome + "/omarchy/current";
    writeFile(current + "/theme.name", theme.toUtf8() + "\n");
    for (const QString &name : shipped) {
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QDir().mkpath(current + "/theme/backgrounds");
        QDir().mkpath(s.env.omarchyPath + "/themes/" + theme + "/backgrounds");
        image.save(current + "/theme/backgrounds/" + name);
        image.save(s.env.omarchyPath + "/themes/" + theme + "/backgrounds/" + name);
    }
    QFile::remove(current + "/background");
    QFile::link(current + "/theme/backgrounds/" + shipped.first(), current + "/background");
}

QString pngFile(const QString &path, Qt::GlobalColor color) {
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(color);
    QDir().mkpath(QFileInfo(path).absolutePath());
    image.save(path);
    return path;
}

// Real git, which profile repositories are tested with, against local bare repositories
// standing in for GitHub at `hub`, and a fake GitHub CLI signed in as "tester".
void useGitHub(Sandbox &s, const QString &hub) {
    QFile::link(QStandardPaths::findExecutable("git"), s.bin("git"));
    s.env.gitHub = "file://" + hub;
    s.env.process.insert("FAKE_GITHUB", hub);
    s.command("gh", kFakeGh);
}

// Git run directly, as someone with their own copy of the repository would.
QByteArray gitIn(const QString &gitDir, const QStringList &arguments, const QByteArray &input = {}, const QString &indexFile = {}) {
    QProcess git;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *variable : {"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null", "GIT_AUTHOR_NAME=Someone", "GIT_AUTHOR_EMAIL=someone@example.com",
                                 "GIT_COMMITTER_NAME=Someone", "GIT_COMMITTER_EMAIL=someone@example.com"})
        environment.insert(QString::fromLatin1(variable).section('=', 0, 0), QString::fromLatin1(variable).section('=', 1));
    if (!indexFile.isEmpty())
        environment.insert("GIT_INDEX_FILE", indexFile);
    git.setProcessEnvironment(environment);
    git.start("git", QStringList{"--git-dir=" + gitDir} + arguments);
    git.write(input);
    git.closeWriteChannel();
    git.waitForFinished(30000);
    return git.readAllStandardOutput().trimmed();
}

QString bareRepository(const QString &hub, const QString &fullName) {
    const QString path = hub + "/" + fullName + ".git";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QProcess::execute("git", {"init", "--bare", "--quiet", path});
    return path;
}

// A commit made by hand in a repository, holding exactly these entries: mode, path, and
// contents (a link's target, or a submodule's commit).
struct GitEntry {
    QString mode, path;
    QByteArray contents;
};
QString commitByHand(const QString &gitDir, const QList<GitEntry> &entries, const QString &parent = {}) {
    QTemporaryDir scratch;
    QByteArray info;
    for (const GitEntry &entry : entries) {
        const QByteArray object = entry.mode == "160000" ? entry.contents : gitIn(gitDir, {"hash-object", "-w", "--stdin"}, entry.contents);
        info += entry.mode.toLatin1() + " " + object + "\t" + entry.path.toUtf8() + "\n";
    }
    gitIn(gitDir, {"update-index", "--add", "--index-info"}, info, scratch.path() + "/index");
    const QByteArray tree = gitIn(gitDir, {"write-tree"}, {}, scratch.path() + "/index");
    QStringList arguments{"commit-tree", QString::fromLatin1(tree), "-m", "By hand"};
    if (!parent.isEmpty())
        arguments << "-p" << parent;
    const QString commit = QString::fromLatin1(gitIn(gitDir, arguments));
    gitIn(gitDir, {"update-ref", "refs/heads/main", commit});
    return commit;
}

bool copyFolder(const QString &from, const QString &to) {
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString target = to + file.mid(from.size());
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (!QFile::copy(file, target))
            return false;
    }
    return true;
}

ProfileRepository::Result repositoryDo(std::function<void(ProfileRepository::Done)> start) {
    ProfileRepository::Result result;
    bool done = false;
    start([&](const ProfileRepository::Result &r) { result = r; done = true; });
    if (!QTest::qWaitFor([&] { return done; }, 60000))
        result.error = "timed out";
    return result;
}

ProfileRepository::Status repositoryStatus(AppCore &core) {
    ProfileRepository::Status status;
    bool done = false;
    core.profileRepository.status([&](const ProfileRepository::Status &s) { status = s; done = true; });
    QTest::qWaitFor([&] { return done; }, 30000);
    return status;
}

// Omarchy's own plugins, as their manifests declare them.
void omarchyPlugins(Sandbox &s, const QStringList &ids, const QString &kind = "bar-widget") {
    for (const QString &id : ids)
        writeFile(s.env.omarchyPath + "/shell/plugins/" + id.mid(8) + "/manifest.json",
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"id", id}, {"name", id}, {"kinds", QJsonArray{kind}}}).toJson());
}

QJsonObject barLayoutIn(const QString &shellJson) {
    return QJsonDocument::fromJson(readAll(shellJson)).object().value("bar").toObject().value("layout").toObject();
}

const char kGhosttyMine[] = "# Mine\nconfig-file = ?\"~/.local/state/omarchy/current/theme/ghostty.conf\"\nfont-size = 12\nwindow-padding-x = 4\n";
const char kGhosttyOmarchy[] = "config-file = ?\"~/.local/state/omarchy/current/theme/ghostty.conf\"\nfont-size = 9\n";
const char kAlacrittyMine[] = "general.import = [ \"~/.local/state/omarchy/current/theme/alacritty.toml\" ]\n\n[font]\nsize = 11\n";
const char kAlacrittyOmarchy[] = "general.import = [ \"~/.local/state/omarchy/current/theme/alacritty.toml\" ]\n\n[font]\nsize = 9\n";

void omarchyTerminalDefaults(Sandbox &s) {
    writeFile(s.env.omarchyPath + "/config/ghostty/config", kGhosttyOmarchy);
    writeFile(s.env.omarchyPath + "/config/alacritty/alacritty.toml", kAlacrittyOmarchy);
}

}  // namespace

Q_DECLARE_METATYPE(GitEntry)

class OmarchyControlTests : public QObject {
    Q_OBJECT

private slots:
    // ---- File editing ---------------------------------------------------------
    void jsonSetChangesOnlyTheTargetBytes() {
        QByteArray doc = "{\n  // keep me\n  \"zeta\": 1,\n  \"idle\": { \"lock\": 300, /* note */ \"screensaver\": 150 },\n  \"alpha\": [1, 2,]\n}\n";
        QVERIFY(FileOps::jsonSet(&doc, {"idle", "lock"}, 600));
        QCOMPARE(doc, QByteArray("{\n  // keep me\n  \"zeta\": 1,\n  \"idle\": { \"lock\": 600, /* note */ \"screensaver\": 150 },\n  \"alpha\": [1, 2,]\n}\n"));
    }
    void jsonSetCreatesMissingPathAndKeepsOrder() {
        QByteArray doc = "{\n  \"version\": 1,\n  \"plugins\": []\n}\n";
        QVERIFY(FileOps::jsonSet(&doc, {"idle", "lock"}, 60));
        const auto object = QJsonDocument::fromJson(doc).object();
        QCOMPARE(object.value("idle").toObject().value("lock").toInt(), 60);
        QVERIFY(doc.indexOf("version") < doc.indexOf("plugins"));
        QVERIFY(doc.indexOf("plugins") < doc.indexOf("idle"));
    }
    void jsonSetKeepsCommentsInAnEmptyObject() {
        QByteArray doc = "{\n  // Extend the menu.\n  // \"personal\": {},\n}\n";
        QVERIFY(FileOps::jsonSet(&doc, {"omarchy-control"}, QJsonObject{{"label", "Control"}}));
        QVERIFY(doc.contains("// Extend the menu."));
        QVERIFY(doc.contains("// \"personal\": {},"));
        QCOMPARE(FileOps::jsonGet(doc, {"omarchy-control", "label"}).toString(), QString("Control"));
    }
    void jsonRemoveKeepsTheRest() {
        QByteArray doc = "{\n  \"a\": 1,\n  \"b\": 2,\n  \"c\": 3\n}\n";
        QVERIFY(FileOps::jsonRemove(&doc, {"b"}));
        const auto object = QJsonDocument::fromJson(doc).object();
        QCOMPARE(object.size(), 2);
        QCOMPARE(object.value("c").toInt(), 3);
        QByteArray last = "{\n  \"a\": 1,\n  \"b\": 2\n}\n";
        QVERIFY(FileOps::jsonRemove(&last, {"b"}));
        QVERIFY(!QJsonDocument::fromJson(last).isNull());
        QCOMPARE(QJsonDocument::fromJson(last).object().size(), 1);
    }
    void jsonSetRefusesToReplaceANonObjectParent() {
        QByteArray doc = "{\"idle\": 5}";
        QString error;
        QVERIFY(!FileOps::jsonSet(&doc, {"idle", "lock"}, 1, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(doc, QByteArray("{\"idle\": 5}"));
    }
    void jsonSetRejectsCorruptDocuments() {
        QByteArray doc = "{\"a\": ";
        QString error;
        QVERIFY(!FileOps::jsonSet(&doc, {"a"}, 1, &error));
        QVERIFY(!error.isEmpty());
    }
    void jsonStringEscaping() {
        QByteArray doc = "{}";
        QVERIFY(FileOps::jsonSet(&doc, {"name"}, QStringLiteral("a\"b\\c\nd")));
        QCOMPARE(FileOps::jsonGet(doc, {"name"}).toString(), QStringLiteral("a\"b\\c\nd"));
    }
    void tomlReadsSectionsAndComments() {
        const QByteArray doc = "# top\n[bar]\nsize = 3 # keep\n\n[font]\nbase-size = 12 # size\nname = \"Mono # One\"\n";
        QCOMPARE(FileOps::tomlGet(doc, "font", "base-size"), QString("12"));
        QCOMPARE(FileOps::tomlGet(doc, "bar", "size"), QString("3"));
        QCOMPARE(FileOps::tomlGet(doc, "font", "name"), QString("Mono # One"));
        QVERIFY(FileOps::tomlGet(doc, "font", "missing").isEmpty());
    }
    void atomicWriteFollowsSymlinksAndKeepsMode() {
        QTemporaryDir dir;
        const QString target = dir.filePath("real/config.json"), link = dir.filePath("link.json");
        writeFile(target, "old");
        QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
        QVERIFY(QFile::link(target, link));
        QVERIFY(FileOps::atomicWrite(link, "new"));
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(readAll(target), QByteArray("new"));
        QCOMPARE(QFile::permissions(target) & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
    }
    void atomicWriteFailureLeavesTheOriginal() {
        if (geteuid() == 0)
            QSKIP("root ignores directory permissions");
        QTemporaryDir dir;
        const QString target = dir.filePath("sub/file");
        writeFile(target, "old");
        QFile::setPermissions(dir.filePath("sub"), QFile::ReadOwner | QFile::ExeOwner);
        QString error;
        QVERIFY(!FileOps::atomicWrite(target, "new", &error));
        QVERIFY(!error.isEmpty());
        QFile::setPermissions(dir.filePath("sub"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QCOMPARE(readAll(target), QByteArray("old"));
    }

    // ---- Process handling -----------------------------------------------------
    void runnerPassesArgumentsWithoutAShell() {
        Sandbox s;
        s.command("echoargs", "for a in \"$@\"; do echo \"[$a]\"; done");
        CommandRunner runner(s.env);
        const auto result = runner.runBlocking({"echoargs", "two words", "$(touch /tmp/oc-pwned)", "semi;colon"});
        QVERIFY(result.ok());
        QCOMPARE(QString::fromUtf8(result.out), QString("[two words]\n[$(touch /tmp/oc-pwned)]\n[semi;colon]\n"));
        QVERIFY(!QFile::exists("/tmp/oc-pwned"));
    }
    void runnerReportsMissingCommands() {
        Sandbox s;
        CommandRunner runner(s.env);
        const auto result = runner.runBlocking({"definitely-not-installed-xyz"});
        QCOMPARE(result.failure, CommandResult::NotFound);
        QVERIFY(result.describe("x").contains("not installed"));
    }
    void runnerTimesOut() {
        Sandbox s;
        s.command("slow", "sleep 20");
        CommandRunner runner(s.env);
        CommandOptions options;
        options.timeoutMs = 150;
        QElapsedTimer timer;
        timer.start();
        const auto result = runner.runBlocking({"slow"}, options);
        QCOMPARE(result.failure, CommandResult::Timeout);
        QVERIFY(timer.elapsed() < 5000);
    }
    void runnerBoundsOutput() {
        Sandbox s;
        s.command("loud", "head -c 200000 /dev/zero | tr '\\0' x");
        CommandRunner runner(s.env);
        CommandOptions options;
        options.maxOutputBytes = 1000;
        const auto result = runner.runBlocking({"loud"}, options);
        QVERIFY(result.out.size() <= 1000);
        QVERIFY(result.truncated);
    }
    void runnerReportsExitFailures() {
        Sandbox s;
        s.command("nope", "echo 'it broke' >&2; exit 3");
        CommandRunner runner(s.env);
        const auto result = runner.runBlocking({"nope"});
        QVERIFY(!result.ok());
        QCOMPARE(result.exitCode, 3);
        QCOMPARE(result.describe("nope"), QString("it broke"));
    }
    void runnerNeverBlocksTheEventLoop() {
        Sandbox s;
        s.command("slow", "sleep 0.5");
        CommandRunner runner(s.env);
        bool finished = false;
        QElapsedTimer timer;
        timer.start();
        runner.run({"slow"}, nullptr, [&](const CommandResult &) { finished = true; });
        QVERIFY(timer.elapsed() < 100);
        QVERIFY(!finished);
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    }
    void runnerDropsCallbackWhenContextIsGone() {
        Sandbox s;
        s.command("slow", "sleep 0.2");
        CommandRunner runner(s.env);
        bool called = false;
        {
            QObject context;
            runner.run({"slow"}, &context, [&](const CommandResult &) { called = true; });
        }
        QTest::qWait(500);
        QVERIFY(!called);
    }

    // ---- Theme and language ---------------------------------------------------
    void themeReadsThePalette() {
        Sandbox s;
        writeFile(s.env.stateHome + "/omarchy/current/theme/colors.toml", "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#fafafa\"\nforeground = \"#101010\"\n");
        writeFile(s.env.configHome + "/omarchy/shell.toml", "[font]\nbase-size = 16\n");
        Theme theme(s.env);
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.window(), QString("#fafafa"));
        QVERIFY(!theme.dark());
        QCOMPARE(theme.accentForeground(), QString("white"));
        QCOMPARE(theme.fontSize(), 16);
    }
    void themeFallsBackWithoutOmarchy() {
        Sandbox s;
        Theme theme(s.env);
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QVERIFY(theme.dark());
        QCOMPARE(theme.fontSize(), 13);
        QVERIFY(theme.animate());
    }
    void themeFollowsReplacementAndMotion() {
        Sandbox s;
        const QString current = s.env.stateHome + "/omarchy/current";
        writeFile(current + "/theme/colors.toml", "accent = \"#111111\"\n");
        Theme theme(s.env);
        QCOMPARE(theme.accent(), QString("#111111"));
        // A theme switch replaces the directory rather than editing the file.
        QVERIFY(QDir(current + "/theme").removeRecursively());
        writeFile(current + "/theme/colors.toml", "accent = \"#222222\"\n");
        QTRY_COMPARE_WITH_TIMEOUT(theme.accent(), QString("#222222"), 3000);
        writeFile(s.env.stateHome + "/omarchy/toggles/hypr/no-animations.lua", "x");
        QTRY_VERIFY_WITH_TIMEOUT(!theme.animate(), 3000);
    }
    void themeFollowsHyprlandRounding() {
        Sandbox s;
        s.command("hyprctl", "echo '{\"int\":6}'");
        Theme theme(s.env);
        QTRY_COMPARE_WITH_TIMEOUT(theme.radius(), 6, 3000);
    }
    void translatorFallsBackPerString() {
        QTemporaryDir dir;
        writeFile(dir.filePath("language"), "pt-BR\n");
        writeFile(dir.filePath("pt.json"), R"({"Save": "Guardar", "Open": "Abrir"})");
        writeFile(dir.filePath("pt-BR.json"), R"({"Save": "Salvar"})");
        Translator translator(dir.filePath("language"), dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("x", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("x", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("x", "Quit").isNull());
    }
    void translatorFollowsTheFileLive() {
        QTemporaryDir dir;
        writeFile(dir.filePath("da.json"), R"({"Save": "Gem"})");
        Translator translator(dir.filePath("language"), dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QSignalSpy spy(&translator, &Translator::changed);
        writeFile(dir.filePath("language"), "da\n");
        QTRY_COMPARE_WITH_TIMEOUT(translator.language(), QString("da"), 3000);
        QCOMPARE(translator.translate("x", "Save"), QString("Gem"));
        QVERIFY(spy.count() >= 1);
    }
    void translatorPlurals() {
        QCOMPARE(Translator::pluralCategory("ru", 21), QString("one"));
        QCOMPARE(Translator::pluralCategory("ru", 25), QString("many"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }
    void shippedCatalogsKeepPlaceholders() {
        // A translation that drops %1 shows the wrong text with no error anywhere.
        // {target} is filled in at runtime the same way.
        const QDir dir(PROJECT_ROOT "/src/i18n");
        static const QRegularExpression placeholder("%[0-9n]|\\{target\\}");
        for (const auto &name : dir.entryList({"*.json"})) {
            const auto object = QJsonDocument::fromJson(readAll(dir.filePath(name))).object();
            QVERIFY2(!object.isEmpty(), qPrintable(name));
            for (auto it = object.begin(); it != object.end(); ++it) {
                QSet<QString> expected;
                auto keys = placeholder.globalMatch(it.key());
                while (keys.hasNext())
                    expected << keys.next().captured();
                const auto check = [&](const QString &value) {
                    if (it.key().contains("%n") && !value.contains("%n"))
                        expected.remove("%n");   // Singular forms may spell the count out.
                    for (const auto &p : expected)
                        QVERIFY2(value.contains(p), qPrintable(name + ": " + it.key() + " -> " + value));
                };
                if (it.value().isString()) {
                    check(it.value().toString());
                } else {
                    const auto forms = it.value().toObject();
                    QVERIFY2(forms.contains("other"), qPrintable(name + ": " + it.key()));
                    check(forms.value("other").toString());
                }
            }
        }
    }

    // Omarchy's language registry, copied from default/i18n/locales.json; refresh it
    // when Omarchy changes its roster.
    static QJsonObject omarchyRegistry() {
        return QJsonDocument::fromJson(readAll(PROJECT_ROOT "/tests/fixtures/omarchy-locales.json")).object();
    }
    // The language codes Control has a catalog for, English included.
    static QStringList shippedLanguages() {
        QStringList codes;
        for (const auto &name : QDir(PROJECT_ROOT "/src/i18n").entryList({"*.json"}))
            codes << name.chopped(5);
        return codes;
    }
    void shippedCatalogsAreOmarchyLanguages() {
        // A catalog only loads when Omarchy's language file names its code, and the
        // file only ever holds a code from the registry.
        const QJsonObject registry = omarchyRegistry();
        QCOMPARE(registry.size(), 50);
        // Every language Omarchy can be set to has a catalog, so picking any of
        // them under Setup > Language changes this window too.
        for (const auto &code : registry.keys())
            QVERIFY2(shippedLanguages().contains(code), qPrintable(code + " has no catalog"));
        for (const auto &code : shippedLanguages()) {
            QVERIFY2(registry.contains(code), qPrintable(code));
            // Listed by hand in resources.qrc, so a new file can ship without loading.
            QVERIFY2(QFile::exists(":/i18n/" + code + ".json"), qPrintable(code + ".json is not in resources.qrc"));
        }
    }
    void pluralFormsCoverEachLanguage() {
        // Each counted string carries every form its language's counts reach: a
        // missing form falls back to `other`, which reads wrong (Arabic "two files"
        // as "files"). The categories come from the same rules the app uses.
        for (const auto &code : shippedLanguages()) {
            if (code == QLatin1String("en"))
                continue;
            QSet<QString> reached{"other"};
            for (long long n = 0; n <= 1200; ++n)
                reached << Translator::pluralCategory(code, n);
            reached << Translator::pluralCategory(code, 1000000);
            const auto object = QJsonDocument::fromJson(readAll(QString(PROJECT_ROOT "/src/i18n/") + code + ".json")).object();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!it.value().isObject())
                    continue;
                const auto forms = it.value().toObject();
                for (const auto &category : reached)
                    QVERIFY2(!forms.value(category).toString().isEmpty(), qPrintable(code + ": " + it.key() + " has no " + category));
            }
        }
    }
    void numbersFormatTheWayOmarchyDoes() {
        // The app formats with each language's formatLocale; its digits and decimal
        // sign have to be the ones Omarchy's registry promises the desktop uses.
        const QJsonObject registry = omarchyRegistry();
        QStringList wrong;
        for (auto it = registry.begin(); it != registry.end(); ++it) {
            const auto entry = it.value().toObject();
            const QLocale locale(entry.value("formatLocale").toString());
            if (locale.zeroDigit() != entry.value("zeroDigit").toString() || locale.decimalPoint() != entry.value("decimal").toString())
                wrong << QString("%1 (%2): Qt gives %3 and %4, Omarchy %5 and %6").arg(it.key(), entry.value("formatLocale").toString(), locale.zeroDigit(), locale.decimalPoint(),
                                                                                       entry.value("zeroDigit").toString(), entry.value("decimal").toString());
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join('\n')));
    }
    void shippedCatalogsMatchTheSource() {
        // Catalogs are keyed by the English, so rewording a string strands its
        // translations. The report finds those, and broken plural forms, from the
        // same extraction the coverage numbers use.
        const QString python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty())
            QSKIP("python3 is not installed");
        QProcess report;
        report.start(python, {"-I", PROJECT_ROOT "/tools/i18n-report.py", "--check"});
        QVERIFY(report.waitForFinished(60000));
        QVERIFY2(report.exitCode() == 0, report.readAllStandardError().constData());
    }
    void countsUseTheLanguagesDigits() {
        // Qt writes %n in Western digits; %Ln uses the language's, as the rest of
        // the window does (Bengali ২৫%), so a count matches the numbers around it.
        QTemporaryDir dir;
        writeFile(dir.filePath("language"), "bn\n");
        writeFile(dir.filePath("bn.json"), R"json({"%n file(s)": {"one": "%nটি ফাইল", "other": "%nটি ফাইল"}})json");
        const QLocale before;
        {
            Translator translator(dir.filePath("language"), dir.path(), PROJECT_ROOT "/tests/fixtures/omarchy-locales.json");
            QCoreApplication::installTranslator(&translator);
            QCOMPARE(QCoreApplication::translate("x", "%n file(s)", nullptr, 25), QString::fromUtf8("২৫টি ফাইল"));
            QCoreApplication::removeTranslator(&translator);
            QVERIFY(!translator.cursiveScript());
        }
        writeFile(dir.filePath("language"), "fa\n");
        Translator persian(dir.filePath("language"), dir.path(), PROJECT_ROOT "/tests/fixtures/omarchy-locales.json");
        QVERIFY(persian.cursiveScript());
        QLocale::setDefault(before);
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    }
    void pseudoLanguageMarksEveryString() {
        QTemporaryDir dir;
        writeFile(dir.filePath("language"), "en-XA\n");
        writeFile(dir.filePath("en.json"), R"json({"%n file(s)": {"one": "%n file", "other": "%n files"}})json");
        Translator translator(dir.filePath("language"), dir.path());
        QCOMPARE(translator.language(), QString("en-XA"));
        // Accented, bracketed, and longer, with placeholders and markup left working.
        const QString save = translator.translate("x", "Save %1 of %2 <b>now</b>");
        QVERIFY2(save.startsWith(QString::fromUtf8("⟦")) && save.endsWith(QString::fromUtf8("⟧")), qPrintable(save));
        QVERIFY(save.contains("%1") && save.contains("%2") && save.contains("<b>") && save.contains("</b>"));
        QVERIFY2(!save.contains(QRegularExpression("Save|now")), qPrintable(save));
        QVERIFY(save.size() >= QString("Save %1 of %2 <b>now</b>").size() + 4);
        // Counted strings take the English form first, so the count still gets its number.
        QVERIFY(translator.translate("x", "%n file(s)", nullptr, 2).contains("%Ln"));
        QVERIFY(translator.translate("x", "%n file(s)", nullptr, 2).contains(QString::fromUtf8("ƒîļéš")));
        QVERIFY(Translator::pseudo(QString()).isEmpty());
        // Never offered: the picker lists Omarchy's registry.
        QVERIFY(!omarchyRegistry().contains(Translator::PseudoLanguage));
        QCOMPARE(translator.formatLocale(translator.language()).name(), QString("en_US"));
    }

    // ---- Owned Lua module -----------------------------------------------------
    void luaStringsCannotEscape() {
        const QString evil = "x\")\nos.execute(\"touch /tmp/pwned\")\n--";
        const QString lua = HyprModule::luaString(evil);
        QVERIFY(!lua.contains('\n'));
        QVERIFY(lua.startsWith('"') && lua.endsWith('"'));
        QVERIFY(lua.contains("\\\""));
        QCOMPARE(HyprModule::luaString(QString("a") + QChar(0x01) + "b"), QString("\"ab\""));
    }
    void moduleRendersOptionsMonitorsAndBinds() {
        Sandbox s;
        HyprModule module(s.env);
        module.setOption({"input", "repeat_rate"}, 31);
        module.setOption({"input", "touchpad", "tap-to-click"}, false);
        module.setOption({"decoration", "dim_strength"}, 0.25);
        module.setMonitors(QJsonArray{QJsonObject{{"output", "DP-1"}, {"mode", "2560x1440@60.00"}, {"position", "0x0"}, {"scale", 1.25}, {"transform", 1}}, QJsonObject{{"output", "eDP-1"}, {"disabled", true}}});
        module.setBinds(QJsonArray{QJsonObject{{"keys", "SUPER + ALT + N"}, {"description", "Note"}, {"command", "notes --new"}, {"replaces", true}}});
        const QString lua = QString::fromUtf8(module.render());
        QVERIFY(lua.contains("repeat_rate = 31"));
        QVERIFY(lua.contains("[\"tap-to-click\"] = false"));
        QVERIFY(lua.contains("dim_strength = 0.25"));
        QVERIFY(lua.contains("hl.monitor({ output = \"DP-1\", mode = \"2560x1440@60.00\", position = \"0x0\", scale = 1.25, transform = 1 })"));
        QVERIFY(lua.contains("hl.monitor({ output = \"eDP-1\", disabled = true })"));
        QVERIFY(lua.contains("hl.unbind(\"SUPER + ALT + N\")"));
        QVERIFY(lua.contains("o.bind(\"SUPER + ALT + N\", \"Note\", \"notes --new\")"));
    }
    void moduleRoundTripsAndDetectsHandEdits() {
        Sandbox s;
        HyprModule module(s.env);
        QCOMPARE(module.state(), HyprModule::Absent);
        module.setOption({"input", "repeat_rate"}, 31);
        QVERIFY(module.save());
        QCOMPARE(module.state(), HyprModule::Ours);
        HyprModule reread(s.env);
        QVERIFY(reread.load());
        QCOMPARE(reread.option({"input", "repeat_rate"}).toInt(), 31);
        QVERIFY(reread.hasOption({"input", "repeat_rate"}));

        writeFile(module.path(), readAll(module.path()) + "-- my own line\n");
        QCOMPARE(module.state(), HyprModule::ModifiedByUser);
        HyprModule blocked(s.env);
        QString error;
        QVERIFY(!blocked.load(&error));
        QVERIFY(!error.isEmpty());
    }
    void moduleRemovalPrunesEmptyTables() {
        Sandbox s;
        HyprModule module(s.env);
        module.setOption({"input", "touchpad", "drag_3fg"}, 1);
        module.removeOption({"input", "touchpad", "drag_3fg"});
        QVERIFY(module.options().isEmpty());
        QVERIFY(!module.render().contains("hl.config"));
    }
    void importGoesAfterUserFilesAndBeforeToggles() {
        Sandbox s;
        s.minimalHyprland();
        HyprModule module(s.env);
        QVERIFY(!module.importInstalled());
        QVERIFY(module.installImport());
        QVERIFY(QFile::exists(module.path()));
        const QByteArray lua = readAll(module.hyprlandPath());
        const int autostart = lua.indexOf("require(\"hypr.autostart\")");
        const int ours = lua.indexOf("require(\"hypr.omarchy-control\")");
        const int toggles = lua.indexOf("require(\"default.hypr.toggles\")");
        const int custom = lua.indexOf("require(\"hypr.mine\")");
        QVERIFY(autostart < ours && ours < toggles && toggles < custom);
        QVERIFY(lua.contains("Managed by Omarchy Control"));
        // Installing twice must not stack up duplicates.
        QVERIFY(module.installImport());
        QCOMPARE(readAll(module.hyprlandPath()).count("hypr.omarchy-control"), qsizetype(1));
        QVERIFY(module.removeImport());
        QVERIFY(!module.importInstalled());
        QVERIFY(readAll(module.hyprlandPath()).contains("hypr.mine"));
    }
    void importRefusesACustomLayout() {
        Sandbox s;
        writeFile(s.env.configHome + "/hypr/hyprland.lua", "-- everything hand written\nhl.config({})\n");
        const QByteArray before = readAll(s.env.configHome + "/hypr/hyprland.lua");
        HyprModule module(s.env);
        QVERIFY(!module.canInstallImport());
        QString error;
        QVERIFY(!module.installImport(&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readAll(s.env.configHome + "/hypr/hyprland.lua"), before);
    }

    // ---- Blur ------------------------------------------------------------------
    void glassParsingKeepsOnlyBlurValues() {
        const auto values = Blur::parseGlass("-- Frosted glass\nreturn function()\n  hl.config({\n    decoration = {\n      blur = {\n"
                                             "        size = 4, -- small\n        brightness = 0.75,\n        xray = false,\n        name = \"x\",\n      },\n    },\n  })\nend\n");
        QCOMPARE(values.value("size"), QString("4"));
        QCOMPARE(values.value("brightness"), QString("0.75"));
        QCOMPARE(values.value("xray"), QString("false"));
        QVERIFY(!values.contains("name"));
        QVERIFY(Blur::parseGlass("nothing here").isEmpty());
    }
    void blurStatusFollowsOmarchysToggles() {
        Blur::Toggles none, windows, bar, both;
        windows.windows = true;
        bar.bar = true;
        both.windows = both.bar = true;
        QCOMPARE(Blur::status(none, false), QString("off"));
        QCOMPARE(Blur::status(windows, true), QString("windows"));
        QCOMPARE(Blur::status(both, true), QString("windows"));
        QCOMPARE(Blur::status(bar, true), QString("bar"));
        QCOMPARE(Blur::status(none, true), QString("on"));
        QVERIFY(Blur::statusReason(none, true).contains("Your own"));
        QVERIFY(Blur::statusReason(windows, true).isEmpty());
        // Toggles load in name order: no-animations turns off the bar's blur, but
        // Glassy windows load after it and turn blur back on.
        windows.noAnimations = bar.noAnimations = true;
        QVERIFY(Blur::statusReason(windows, true).contains("keep their blur"));
        QVERIFY(Blur::statusReason(bar, false).contains("not blurred"));
    }
    void strengthNamesItsPresetOrSaysTheParts() {
        const QJsonObject presets{{"light", QJsonObject{{"size", 3}, {"passes", 1}}}, {"omarchy", QJsonObject{}}, {"strong", QJsonObject{{"size", 6}, {"passes", 3}}}};
        const QHash<QString, QString> glass{{"size", "4"}, {"passes", "2"}};
        QCOMPARE(Blur::strengthName(presets, "3", "1", glass), QString("light"));
        QCOMPARE(Blur::strengthName(presets, "4", "2", glass), QString("omarchy"));
        QCOMPARE(Blur::strengthName(presets, "6", "3", glass), QString("strong"));
        QCOMPARE(Blur::strengthName(presets, "7", "2", glass), QString("7") + QChar(0x00D7) + "2");
    }
    void blurImportGoesAfterTogglesAndLeavesTheMainOneAlone() {
        Sandbox s;
        s.minimalHyprland();
        const QByteArray stock = readAll(s.env.configHome + "/hypr/hyprland.lua");
        HyprModule blur(s.env, HyprModule::Blur), main(s.env);
        QCOMPARE(blur.path(), s.env.configHome + "/hypr/omarchy-control-blur.lua");
        QVERIFY(blur.installImport());
        QVERIFY(blur.importInstalled());
        QVERIFY(!main.importInstalled());
        QString lua = QString::fromUtf8(readAll(s.env.configHome + "/hypr/hyprland.lua"));
        QVERIFY(lua.indexOf("require(\"default.hypr.toggles\")") < lua.indexOf("require(\"hypr.omarchy-control-blur\")"));
        QVERIFY(main.installImport());
        lua = QString::fromUtf8(readAll(s.env.configHome + "/hypr/hyprland.lua"));
        QVERIFY(lua.indexOf("require(\"hypr.omarchy-control\")") < lua.indexOf("require(\"default.hypr.toggles\")"));
        // Taking the blur file away leaves the main one's line where it was.
        QVERIFY(blur.removeEntirely());
        QVERIFY(!QFile::exists(blur.path()));
        QVERIFY(!blur.importInstalled());
        QVERIFY(main.importInstalled());
        QVERIFY(main.removeImport());
        QCOMPARE(readAll(s.env.configHome + "/hypr/hyprland.lua"), stock);
        // A config without Omarchy's toggles line gets no blur line at all.
        writeFile(s.env.configHome + "/hypr/hyprland.lua", "require(\"hypr.autostart\")\n");
        QVERIFY(!HyprModule(s.env, HyprModule::Blur).canInstallImport());
        QVERIFY(HyprModule(s.env).canInstallImport());
    }
    void blurRowsShowOmarchysGlassUntilChosenHere() {
        Sandbox s;
        blurSandbox(s);
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        // No Glassy toggle: blur is off, and the rows say what turning it on brings.
        ReadResult status = readSetting(core.engine, "appearance.blur.status");
        QCOMPARE(status.value, QString("off"));
        QVERIFY(status.reason.contains("Glassy"));
        ReadResult brightness = readSetting(core.engine, "appearance.blur.brightness");
        QCOMPARE(brightness.value, QString("0.75"));
        QCOMPARE(brightness.source, QString("Omarchy's glass"));
        QVERIFY(!brightness.isOverride);
        QCOMPARE(readSetting(core.engine, "appearance.blur.strength").value, QString("omarchy"));
        QCOMPARE(readSetting(core.engine, "appearance.blur.popups").value, QString("true"));
        blurToggle(s, "window-glassy", true);
        QCOMPARE(readSetting(core.engine, "appearance.blur.status").value, QString("windows"));
        QCOMPARE(readSetting(core.engine, "appearance.blur.brightness").value, QString("0.75"));
    }
    void blurChoicesLoadAfterTogglesAndLeaveNoTrace() {
        Sandbox s;
        blurSandbox(s);
        blurToggle(s, "window-glassy", true);
        const QByteArray stock = readAll(s.env.configHome + "/hypr/hyprland.lua");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        const QString file = s.env.configHome + "/hypr/omarchy-control-blur.lua";

        const ChangePlan plan = core.engine.plan("appearance.blur.strength", "strong");
        QVERIFY(plan.ok);
        QVERIFY(plan.needsOwnership);
        QVERIFY(plan.confirmText.contains("omarchy-control-blur.lua"));
        QCOMPARE(applySetting(core.engine, "appearance.blur.strength", "strong").phase, QString("committed"));
        QVERIFY(readAll(file).contains("size = 6"));
        QVERIFY(readAll(file).contains("passes = 3"));
        ReadResult strength = readSetting(core.engine, "appearance.blur.strength");
        QCOMPARE(strength.value, QString("strong"));
        QVERIFY(strength.isOverride);
        QCOMPARE(strength.extra.value("size").toString(), QString("6"));
        // Nothing went into the main file, which loads before Glassy would undo it.
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));

        // Color boost sets both of Hyprland's vibrancy options.
        QCOMPARE(applySetting(core.engine, "appearance.blur.vibrancy", "0.6").phase, QString("committed"));
        QVERIFY(readAll(file).contains("vibrancy = 0.6"));
        QVERIFY(readAll(file).contains("vibrancy_darkness = 0.6"));
        QCOMPARE(readSetting(core.engine, "appearance.blur.vibrancy").source, QString("Set here"));

        // A radius of its own makes the strength a custom pair, which undo can restore.
        QCOMPARE(applySetting(core.engine, "appearance.blur.size", "7").phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "appearance.blur.strength").value, QString("7") + QChar(0x00D7) + "3");
        QVERIFY(core.engine.plan("appearance.blur.strength", "7x3").ok);
        QVERIFY(!core.engine.plan("appearance.blur.strength", "30x3").ok);
        QVERIFY(!core.engine.plan("appearance.blur.strength", "loud").ok);

        // Omarchy's strength gives size and passes back to the glass.
        QCOMPARE(applySetting(core.engine, "appearance.blur.strength", "omarchy").phase, QString("committed"));
        QVERIFY(!readAll(file).contains("size ="));
        QCOMPARE(readSetting(core.engine, "appearance.blur.strength").value, QString("omarchy"));

        // Resetting the last choice takes the file and its line away again.
        Transaction reset;
        bool done = false;
        core.engine.reset("appearance.blur.vibrancy", [&](const Transaction &t) { reset = t; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
        QCOMPARE(reset.phase, QString("committed"));
        QVERIFY(!QFile::exists(file));
        QCOMPARE(readAll(s.env.configHome + "/hypr/hyprland.lua"), stock);
        QCOMPARE(readSetting(core.engine, "appearance.blur.vibrancy").value, QString("0.35"));
    }
    void blurChangesUndo() {
        Sandbox s;
        blurSandbox(s);
        blurToggle(s, "bar-glassy", true);
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        QCOMPARE(applySetting(core.engine, "appearance.blur.strength", "light").phase, QString("committed"));
        const Transaction tx = applySetting(core.engine, "appearance.blur.strength", "medium");
        QCOMPARE(tx.phase, QString("committed"));
        Transaction undone;
        bool done = false;
        core.engine.undo(tx.id, [&](const Transaction &t) { undone = t; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
        QCOMPARE(undone.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "appearance.blur.strength").value, QString("light"));
    }
    void handEditedBlurFilePausesOnlyBlur() {
        Sandbox s;
        blurSandbox(s);
        HyprModule module(s.env, HyprModule::Blur);
        module.setOption({"decoration", "blur", "noise"}, 0.05);
        QVERIFY(module.save());
        QVERIFY(module.installImport());
        writeFile(module.path(), readAll(module.path()) + "-- mine\n");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        const ChangePlan plan = core.engine.plan("appearance.blur.noise", "0.1");
        QVERIFY(!plan.ok);
        QVERIFY(plan.error.contains("omarchy-control-blur.lua"));
        QCOMPARE(readSetting(core.engine, "appearance.blur.noise").status, QString("custom"));
        // The main file is not affected by the blur file's state.
        QVERIFY(core.engine.plan("appearance.gaps_in", "6").ok);
    }

    // ---- Change coordinator ---------------------------------------------------
    void coordinatorCommitsAndJournals() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        ChangeCoordinator::Request request;
        request.settingId = "x";
        request.label = "X";
        request.after = "1";
        request.recovery = "auto-undo";
        request.apply = [](ChangeCoordinator::Done done) { done(true, {}); };
        request.verify = [](ChangeCoordinator::Done done) { done(true, {}); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("committed"));
        QVERIFY(result.undoable());
        QCOMPARE(coordinator.history().first().id, result.id);
        // Every phase reached the disk, so a crash at any point leaves a trace.
        const QString journal = QString::fromUtf8(readAll(coordinator.journalPath()));
        for (const char *phase : {"intent", "applying", "applied", "committed"})
            QVERIFY2(journal.contains(QString("\"phase\":\"%1\"").arg(phase)), phase);
        QCOMPARE(QFile::permissions(coordinator.journalPath()) & (QFile::ReadOther | QFile::ReadGroup), QFile::Permissions());
    }
    void coordinatorRestoresFilesOnFailedVerify() {
        Sandbox s;
        const QString file = s.env.configHome + "/thing.json";
        writeFile(file, "original");
        ChangeCoordinator coordinator(s.env);
        ChangeCoordinator::Request request;
        request.settingId = "x";
        request.label = "X";
        request.files << file;
        request.apply = [file](ChangeCoordinator::Done done) { done(FileOps::atomicWrite(file, "changed"), {}); };
        request.verify = [](ChangeCoordinator::Done done) { done(false, "did not take"); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("rolled-back"));
        QCOMPARE(readAll(file), QByteArray("original"));
        QVERIFY(result.message.contains("did not take"));
        QVERIFY(!result.undoable());
    }
    void coordinatorRemovesAFileItCreated() {
        Sandbox s;
        const QString file = s.env.configHome + "/new.json";
        ChangeCoordinator coordinator(s.env);
        ChangeCoordinator::Request request;
        request.files << file;
        request.apply = [file](ChangeCoordinator::Done done) { done(FileOps::atomicWrite(file, "x"), {}); };
        request.verify = [](ChangeCoordinator::Done done) { done(false, "no"); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("rolled-back"));
        QVERIFY(!QFile::exists(file));
    }
    void coordinatorReportsPartialWhenNothingCanBeRestored() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        ChangeCoordinator::Request request;
        request.apply = [](ChangeCoordinator::Done done) { done(false, "broke"); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("partial"));
        QVERIFY(result.message.contains("partly"));
    }
    void coordinatorRefusesExternallyEditedFiles() {
        Sandbox s;
        const QString file = s.env.configHome + "/c.json";
        writeFile(file, "v1");
        const QString planned = FileOps::fingerprint(file);
        writeFile(file, "someone else");
        ChangeCoordinator coordinator(s.env);
        bool applied = false;
        ChangeCoordinator::Request request;
        request.expectedFingerprints.insert(file, planned);
        request.files << file;
        request.apply = [&](ChangeCoordinator::Done done) { applied = true; done(true, {}); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("failed"));
        QVERIFY(!applied);
        QCOMPARE(readAll(file), QByteArray("someone else"));
    }
    void coordinatorSerializesConflictingChanges() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        QStringList order;
        for (int i = 0; i < 3; ++i) {
            ChangeCoordinator::Request request;
            request.settingId = QString::number(i);
            request.apply = [&order, i](ChangeCoordinator::Done done) {
                order << QString("begin%1").arg(i);
                QTimer::singleShot(30, [&order, i, done] { order << QString("end%1").arg(i); done(true, {}); });
            };
            coordinator.submit(request, nullptr);
        }
        QTRY_COMPARE(order.size(), 6);
        QCOMPARE(order, (QStringList{"begin0", "end0", "begin1", "end1", "begin2", "end2"}));
    }
    void coordinatorRefusesWhileAnotherInstanceHoldsTheLock() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        QProcess holder;
        // A real second process holding the lock, as another window would.
        QLockFile other(s.env.appStateDir + "/change.lock");
        QVERIFY(other.tryLock(0));
        // Same-process QLockFile also blocks, which models the other instance.
        bool applied = false;
        ChangeCoordinator::Request request;
        request.apply = [&](ChangeCoordinator::Done done) { applied = true; done(true, {}); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE_WITH_TIMEOUT(result.phase, QString("failed"), 5000);
        QVERIFY(!applied);
        QVERIFY(result.message.contains("Another"));
    }
    void historyRedactsSecrets() {
        QCOMPARE(ChangeCoordinator::redact("password=hunter2 ok"), QString("password=[redacted] ok"));
        QVERIFY(!ChangeCoordinator::redact("api_key: abc123").contains("abc123"));
        QVERIFY(!ChangeCoordinator::redact("nmcli con modify x wifi-sec.psk secretvalue").contains("secretvalue"));
        QCOMPARE(ChangeCoordinator::redact("nothing here"), QString("nothing here"));
    }

    // Crash at each journal phase, by writing the journal the crash would leave.
    void recoveryNeverOverwritesWhatItCannotAttribute() {
        // Writes are atomic, so this content is not half of the interrupted change. It
        // is an edit someone made after the crash, and it has to survive.
        Sandbox s;
        const QString file = s.env.configHome + "/shell.json";
        writeFile(file, "an edit made after the crash");
        ChangeCoordinator coordinator(s.env);
        const QString backup = coordinator.backupDirectory() + "/tx1/0";
        writeFile(backup, "original");
        Transaction tx;
        tx.id = "tx1";
        tx.settingId = "power.idle.lock";
        tx.label = "Lock after";
        tx.recovery = "auto-undo";
        tx.phase = "applying";
        tx.files.insert(file, QJsonObject{{"before", FileOps::fingerprintBytes("original")}, {"backup", backup}});
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n");
        const QStringList messages = coordinator.recoverInterrupted();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(readAll(file), QByteArray("an edit made after the crash"));
        QCOMPARE(coordinator.transaction("tx1").phase, QString("interrupted"));
        // The way back is offered in words, and the backup is still there to use.
        QVERIFY(coordinator.transaction("tx1").message.contains(backup));
        QVERIFY(QFile::exists(backup));
        // Running it again changes nothing and reports nothing.
        QVERIFY(coordinator.recoverInterrupted().isEmpty());
    }
    void recoveryKnowsWhenNothingChanged() {
        Sandbox s;
        const QString file = s.env.configHome + "/shell.json";
        writeFile(file, "same");
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "tx2";
        tx.label = "X";
        tx.phase = "intent";
        tx.files.insert(file, QJsonObject{{"before", FileOps::fingerprint(file)}});
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n");
        coordinator.recoverInterrupted();
        QCOMPARE(coordinator.transaction("tx2").phase, QString("not-applied"));
    }
    void recoveryKeepsACompletedWrite() {
        Sandbox s;
        const QString file = s.env.configHome + "/shell.json";
        writeFile(file, "new value");
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "tx3";
        tx.label = "X";
        tx.phase = "applied";
        tx.files.insert(file, QJsonObject{{"before", FileOps::fingerprintBytes("old")}, {"after", FileOps::fingerprint(file)}});
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n");
        coordinator.recoverInterrupted();
        QCOMPARE(coordinator.transaction("tx3").phase, QString("interrupted"));
        QCOMPARE(readAll(file), QByteArray("new value"));
    }
    void recoveryHandlesTransactionsWithoutFiles() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "tx4";
        tx.label = "X";
        tx.phase = "applying";
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n");
        QCOMPARE(coordinator.recoverInterrupted().size(), 1);
        QCOMPARE(coordinator.transaction("tx4").phase, QString("interrupted"));
    }
    void recoveryWaitsForARunningInstance() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "tx5";
        tx.phase = "applying";
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n");
        QLockFile other(s.env.appStateDir + "/change.lock");
        QVERIFY(other.tryLock(0));
        QVERIFY(coordinator.recoverInterrupted().isEmpty());
        QCOMPARE(coordinator.transaction("tx5").phase, QString("applying"));
    }
    void journalSurvivesATruncatedLastLine() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "good";
        tx.phase = "committed";
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n{\"id\":\"bad\",\"pha");
        QCOMPARE(coordinator.history().size(), 1);
        QCOMPARE(coordinator.history().first().id, QString("good"));
    }

    // ---- Settings engine ------------------------------------------------------
    void idleTimeoutFullPath() {
        // The plan's first work package: read, stage, scoped shell.json change,
        // verify, history, undo, with unknown keys and order preserved.
        Sandbox s;
        s.omarchy("exit 0");
        const QString config = s.env.configHome + "/omarchy/shell.json";
        const QByteArray original = "{\n  \"zzz\": {\"custom\": true},\n  \"idle\": {\n    \"lock\": 300,\n    \"screensaver\": 150\n  },\n  \"aaa\": 1\n}\n";
        writeFile(config, original);
        AppCore core(s.env);

        ReadResult current = readSetting(core.engine, "power.idle.lock");
        QCOMPARE(current.status, QString("ok"));
        QCOMPARE(current.value, QString("300"));

        const Transaction tx = applySetting(core.engine, "power.idle.lock", "600");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(tx.before, QString("300"));
        QVERIFY(tx.undoable());
        const QByteArray changed = readAll(config);
        QCOMPARE(changed, QByteArray(original).replace("300", "600"));
        QVERIFY(s.calls().join('\n').contains("omarchy shell -q shell reloadConfig"));
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("600"));

        const Transaction undone = undoTx(core.engine, tx.id);
        QCOMPARE(undone.phase, QString("committed"));
        QCOMPARE(readAll(config), original);
        QCOMPARE(core.coordinator.transaction(tx.id).undoneBy, undone.id);
        QVERIFY(!core.coordinator.transaction(tx.id).undoable());
    }
    void undoRefusesToOverwriteALaterEdit() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{\"idle\":{\"lock\":300}}");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "power.idle.lock", "600");
        QCOMPARE(tx.phase, QString("committed"));
        writeFile(config, "{\"idle\":{\"lock\":900}}");
        const Transaction undone = undoTx(core.engine, tx.id);
        QCOMPARE(undone.phase, QString("failed"));
        QVERIFY(undone.message.contains("changed after"));
        QCOMPARE(readAll(config), QByteArray("{\"idle\":{\"lock\":900}}"));
    }
    void missingShellJsonStartsFromDefaults() {
        Sandbox s;
        s.omarchy("exit 0");
        writeFile(s.env.omarchyPath + "/config/omarchy/shell.json", "{\"version\":1,\"idle\":{\"lock\":300,\"screensaver\":150},\"bar\":{\"position\":\"top\"}}");
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("300"));
        const Transaction tx = applySetting(core.engine, "power.idle.lock", "60");
        QCOMPARE(tx.phase, QString("committed"));
        const auto written = QJsonDocument::fromJson(readAll(s.env.configHome + "/omarchy/shell.json")).object();
        QCOMPARE(written.value("bar").toObject().value("position").toString(), QString("top"));
        QCOMPARE(written.value("idle").toObject().value("lock").toInt(), 60);
    }
    void corruptConfigIsLeftAlone() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{ this is not json");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "power.idle.lock", "60");
        QVERIFY(tx.phase != QLatin1String("committed"));
        QCOMPARE(readAll(config), QByteArray("{ this is not json"));
    }
    void planValidates_data() {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("ok");
        QTest::newRow("in range") << "appearance.textsize" << "16" << true;
        QTest::newRow("too large") << "appearance.textsize" << "99" << false;
        QTest::newRow("not a number") << "appearance.textsize" << "big" << false;
        QTest::newRow("float") << "input.sensitivity" << "0.5" << true;
        QTest::newRow("float nan") << "input.sensitivity" << "nan" << false;
        QTest::newRow("float out of range") << "input.sensitivity" << "5" << false;
        QTest::newRow("toggle") << "power.stayawake" << "true" << true;
        QTest::newRow("toggle junk") << "power.stayawake" << "maybe" << false;
        QTest::newRow("choice") << "appearance.corners" << "sharp" << true;
        QTest::newRow("choice junk") << "appearance.corners" << "; rm -rf /" << false;
        QTest::newRow("custom allowed") << "power.idle.lock" << "1234" << true;
        QTest::newRow("pattern refuses") << "power.idle.lock" << "12; reboot" << false;
        QTest::newRow("control chars") << "system.weather" << "a\nb" << false;
        QTest::newRow("status row") << "system.version" << "9" << false;
        QTest::newRow("unknown") << "no.such.setting" << "1" << false;
        QTest::newRow("scale flag") << "displays.scale" << "-h" << false;
        QTest::newRow("scale word") << "displays.scale" << "up" << false;
        QTest::newRow("scale empty") << "displays.scale" << "" << false;
        QTest::newRow("scale") << "displays.scale" << "1.25" << true;
        QTest::newRow("dns choice") << "network.dns" << "Cloudflare" << true;
        QTest::newRow("dns typed") << "network.dns" << "9.9.9.9" << false;
        QTest::newRow("dns custom") << "network.dns" << "Custom" << false;
    }
    void planValidates() {
        QFETCH(QString, id);
        QFETCH(QString, value);
        QFETCH(bool, ok);
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.env.sys("/sys/class/net/wlan0/wireless/x"), "");
        AppCore core(s.env);
        QCOMPARE(core.engine.plan(id, value).ok, ok);
    }
    void liveChoiceListsAreEnforced() {
        Sandbox s;
        s.omarchy("case \"$*\" in 'font list') echo 'Good Font';; esac");
        AppCore core(s.env);
        const QVariantList good{QVariantMap{{"value", "Good Font"}, {"label", "Good Font"}}};
        QVERIFY(core.engine.plan("appearance.font", "Good Font", good).ok);
        // omarchy-font-set feeds this into sed, so an unlisted value must not pass.
        QVERIFY(!core.engine.plan("appearance.font", "x/;s/.*/pwned/;", good).ok);
        // No list means no way to check, which also means no.
        QVERIFY(!core.engine.plan("appearance.font", "Good Font", {}).ok);
    }
    void commandWriteVerifiesByReadingBack() {
        Sandbox s;
        s.omarchy("state=\"$HOME/corners\"; case \"$*\" in 'hyprland corners') cat \"$state\" 2>/dev/null || echo theme;; 'hyprland corners '*) echo \"$3\" > \"$state\";; esac");
        s.command("hyprctl", "echo '{}'");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.corners", "sharp");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "appearance.corners").value, QString("sharp"));
    }
    void exitZeroWithNoEffectIsNotSuccess() {
        // Many Omarchy setters swallow failures, so a clean exit proves nothing.
        Sandbox s;
        s.omarchy("case \"$*\" in 'hyprland corners') echo theme;; esac");
        s.command("hyprctl", "echo '{}'");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.corners", "sharp");
        QVERIFY(tx.phase != QLatin1String("committed"));
        QVERIFY(!tx.message.isEmpty());
    }
    void failedCommandReportsItsError() {
        Sandbox s;
        s.omarchy("case \"$*\" in 'hyprland corners') echo theme;; *) echo 'denied' >&2; exit 4;; esac");
        s.command("hyprctl", "echo '{}'");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.corners", "sharp");
        QVERIFY(tx.phase != QLatin1String("committed"));
        QVERIFY(tx.message.contains("denied"));
    }
    void toggleOnlyCommandsOnlyRunWhenNeeded() {
        Sandbox s;
        s.omarchy("f=\"$HOME/.local/state/omarchy/toggles/screensaver-off\"; case \"$*\" in 'toggle screensaver') if [ -f \"$f\" ]; then rm \"$f\"; else mkdir -p \"$(dirname \"$f\")\"; touch \"$f\"; fi;; esac");
        AppCore core(s.env);
        // Already in the requested state: running a toggle would flip it the wrong way.
        const Transaction same = applySetting(core.engine, "power.screensaver", "true");
        QCOMPARE(same.phase, QString("committed"));
        QVERIFY(!s.calls().join('\n').contains("toggle screensaver"));
        QVERIFY(!QFile::exists(s.env.stateHome + "/omarchy/toggles/screensaver-off"));
        const Transaction flipped = applySetting(core.engine, "power.screensaver", "false");
        QCOMPARE(flipped.phase, QString("committed"));
        QVERIFY(QFile::exists(s.env.stateHome + "/omarchy/toggles/screensaver-off"));
    }
    void aNonzeroExitAfterTheWorkIsDoneIsNotFailure() {
        // Found on real scripts: they end by sending a notification, which fails when
        // the shell is down, so they exit 1 although the setting changed.
        Sandbox s;
        s.omarchy("f=\"$HOME/c\"; case \"$*\" in 'hyprland corners') cat \"$f\" 2>/dev/null || echo theme;; 'hyprland corners '*) echo \"$3\" > \"$f\"; exit 1;; esac");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.corners", "sharp");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "appearance.corners").value, QString("sharp"));
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "appearance.corners").value, QString("theme"));
    }
    void barToggleIsNotInverted() {
        // `omarchy toggle bar on` hides the bar, so "show" must send off.
        Sandbox s;
        s.omarchy("f=\"$HOME/.local/state/omarchy/toggles/bar-off\"; case \"$*\" in 'toggle bar off') rm -f \"$f\";; 'toggle bar on') mkdir -p \"$(dirname \"$f\")\"; touch \"$f\";; esac");
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "desktop.bar.visible").value, QString("true"));
        QCOMPARE(applySetting(core.engine, "desktop.bar.visible", "false").phase, QString("committed"));
        QVERIFY(s.calls().join('\n').contains("omarchy toggle bar on"));
        QCOMPARE(readSetting(core.engine, "desktop.bar.visible").value, QString("false"));
    }
    void defaultAppSettersOnlyOfferInstalledApps() {
        // A default setter installs whatever is missing; that must stay a separate,
        // explicit action.
        Sandbox s;
        s.command("firefox", "exit 0");
        s.omarchy("exit 0");
        AppCore core(s.env);
        QVariantList choices;
        bool done = false;
        core.engine.readChoices("apps.default.browser", [&](const QVariantList &c) { choices = c; done = true; });
        QTRY_VERIFY(done);
        QStringList values;
        for (const auto &choice : choices)
            values << choice.toMap().value("value").toString();
        QCOMPARE(values, QStringList{"firefox"});
    }
    void unavailableHardwareIsDistinct() {
        Sandbox s;
        AppCore core(s.env);
        const auto *entry = core.catalog.find("input.tp_natural");
        QVERIFY(entry);
        const auto availability = core.capabilities.availability(*entry);
        QVERIFY(!availability.available);
        QCOMPARE(availability.kind, QString("hardware"));
        QVERIFY(availability.reason.contains("touchpad"));
        QCOMPARE(readSetting(core.engine, "input.tp_natural").status, QString("unavailable"));
    }
    void missingToolsAreDistinct() {
        Sandbox s;
        AppCore core(s.env);
        const auto *entry = core.catalog.find("helper.printer");
        QVERIFY(entry);
        const auto availability = core.capabilities.availability(*entry);
        QVERIFY(!availability.available);
        QCOMPARE(availability.kind, QString("tool"));
    }
    void shellDownIsReportedNotHidden() {
        Sandbox s;
        s.omarchy("exit 1");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        QVERIFY(!core.capabilities.has("shell"));
        const auto availability = core.capabilities.availability(*core.catalog.find("desktop.dnd"));
        QVERIFY(!availability.available);
        QCOMPARE(availability.kind, QString("service"));
        QVERIFY(availability.reason.contains("shell"));
    }
    void readReportsUnknownRatherThanStaleValue() {
        Sandbox s;
        s.omarchy("echo oops >&2; exit 1");
        AppCore core(s.env);
        const ReadResult result = readSetting(core.engine, "appearance.corners");
        QCOMPARE(result.status, QString("unknown"));
        QVERIFY(result.value.isEmpty());
        QVERIFY(!result.reason.isEmpty());
    }
    void hyprOptionParsing() {
        bool ok = false;
        QCOMPARE(SettingsEngine::parseHyprOption(R"({"option":"x","int":40,"set":true})", &ok), QString("40"));
        QVERIFY(ok);
        QCOMPARE(SettingsEngine::parseHyprOption(R"({"bool":false})", &ok), QString("false"));
        QCOMPARE(SettingsEngine::parseHyprOption(R"({"float":0.400000})", &ok), QString("0.4"));
        QCOMPARE(SettingsEngine::parseHyprOption(R"({"float":0.000000})", &ok), QString("0"));
        QCOMPARE(SettingsEngine::parseHyprOption(R"({"str":"[[EMPTY]]"})", &ok), QString());
        QCOMPARE(SettingsEngine::parseHyprOption(R"({"css":"5 5 5 5"})", &ok), QString("5"));
        SettingsEngine::parseHyprOption("garbage", &ok);
        QVERIFY(!ok);
    }
    void hyprOverrideFullLifecycle() {
        Sandbox s;
        s.minimalHyprland();
        const QString option = s.dir.path() + "/hypr-input_repeat_rate";
        // The fake compositor picks the value up from the owned module on reload.
        s.command("hyprctl", (kScript2));
        const QByteArray stock = readAll(s.env.configHome + "/hypr/hyprland.lua");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        QCOMPARE(readSetting(core.engine, "input.repeat_rate").value, QString("40"));
        QVERIFY(!readSetting(core.engine, "input.repeat_rate").isOverride);

        const ChangePlan plan = core.engine.plan("input.repeat_rate", "31");
        QVERIFY(plan.ok);
        QVERIFY(plan.needsOwnership);
        QVERIFY(plan.confirmText.contains("hyprland.lua"));

        const Transaction tx = applySetting(core.engine, "input.repeat_rate", "31");
        QCOMPARE(tx.phase, QString("committed"));
        const ReadResult read = readSetting(core.engine, "input.repeat_rate");
        QCOMPARE(read.value, QString("31"));
        QVERIFY(read.isOverride);
        QVERIFY(readAll(s.env.configHome + "/hypr/omarchy-control.lua").contains("repeat_rate = 31"));
        // Second change needs no new ownership decision.
        QVERIFY(!core.engine.plan("input.repeat_rate", "33").needsOwnership);

        // "Use default" removes the override and reads the inherited value again.
        Transaction reset;
        bool done = false;
        core.engine.reset("input.repeat_rate", [&](const Transaction &t) { reset = t; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
        QCOMPARE(reset.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "input.repeat_rate").value, QString("40"));
        QVERIFY(!readAll(s.env.configHome + "/hypr/omarchy-control.lua").contains("repeat_rate"));
        QVERIFY(readAll(s.env.configHome + "/hypr/hyprland.lua").contains("hypr.mine"));
        Q_UNUSED(stock);
    }
    void hyprOverrideRollsBackOnNewConfigError() {
        Sandbox s;
        s.minimalHyprland();
        s.command("hyprctl", kScript3);
        const QByteArray stock = readAll(s.env.configHome + "/hypr/hyprland.lua");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        const Transaction tx = applySetting(core.engine, "input.repeat_rate", "31");
        QCOMPARE(tx.phase, QString("rolled-back"));
        QVERIFY(tx.message.contains("config error"));
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
        QCOMPARE(readAll(s.env.configHome + "/hypr/hyprland.lua"), stock);
    }
    void hyprOverrideToleratesExistingErrors() {
        Sandbox s;
        s.minimalHyprland();
        s.command("hyprctl", kScript4);
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        QCOMPARE(applySetting(core.engine, "input.repeat_rate", "31").phase, QString("committed"));
    }
    void hyprOverrideExplainsWhenCustomConfigWins() {
        Sandbox s;
        s.minimalHyprland();
        s.command("hyprctl", kScript5);
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        const Transaction tx = applySetting(core.engine, "input.repeat_rate", "31");
        QCOMPARE(tx.phase, QString("rolled-back"));
        QVERIFY(tx.message.contains("Another part"));
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }
    void focusFollowsMouseKnowsOmarchysFloatingWorkspaces() {
        Sandbox s;
        s.minimalHyprland();
        // Omarchy forces click focus on a floating workspace, so Hyprland reports 0 there
        // whatever is configured.
        writeFile(s.dir.path() + "/hypr-input_follow_mouse", "{\"int\":0}");
        writeFile(s.dir.path() + "/layout", "floating");
        s.command("omarchy-hyprland-workspace-layout-current", "cat '" + s.dir.path().toUtf8() + "/layout'");
        writeFile(s.env.omarchyPath + "/default/hypr/input.lua", "o.config({\n  input = {\n    follow_mouse = 1,\n  },\n})\n");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();

        // The configured value, Omarchy's here, with why Hyprland says otherwise.
        ReadResult read = readSetting(core.engine, "input.follow_mouse");
        QCOMPARE(read.status, QString("ok"));
        QCOMPARE(read.value, QString("1"));
        QVERIFY(read.reason.contains("Floating workspaces focus on click"));
        QVERIFY(read.extra.value("floatingWorkspace").toBool());
        // The user's own uncommented value wins over Omarchy's; a commented one does not count.
        writeFile(s.env.configHome + "/hypr/input.lua", "-- follow_mouse = 3,\no.config({ input = {\n  follow_mouse = 2, -- mine\n} })\n");
        QCOMPARE(readSetting(core.engine, "input.follow_mouse").value, QString("2"));

        // A change from a floating workspace lands in the file and a clean reload confirms it,
        // though Hyprland still reports Omarchy's 0.
        Transaction tx = applySetting(core.engine, "input.follow_mouse", "3");
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(readAll(s.env.configHome + "/hypr/omarchy-control.lua").contains("follow_mouse = 3"));
        read = readSetting(core.engine, "input.follow_mouse");
        QCOMPARE(read.value, QString("3"));
        QVERIFY(read.isOverride);

        // On a tiled workspace Hyprland's own answer decides, as for any other setting.
        writeFile(s.dir.path() + "/layout", "dwindle");
        read = readSetting(core.engine, "input.follow_mouse");
        QCOMPARE(read.value, QString("0"));
        QVERIFY(read.reason.isEmpty());
        tx = applySetting(core.engine, "input.follow_mouse", "2");
        QCOMPARE(tx.phase, QString("rolled-back"));
        QVERIFY(readAll(s.env.configHome + "/hypr/omarchy-control.lua").contains("follow_mouse = 3"));
    }
    void handEditedModulePausesChanges() {
        Sandbox s;
        s.minimalHyprland();
        HyprModule module(s.env);
        module.setOption({"input", "repeat_rate"}, 31);
        QVERIFY(module.save());
        QVERIFY(module.installImport());
        const QByteArray edited = readAll(module.path()) + "-- mine\n";
        writeFile(module.path(), edited);
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        const ChangePlan plan = core.engine.plan("input.repeat_rate", "33");
        QVERIFY(!plan.ok);
        QVERIFY(plan.error.contains("by hand"));
        QCOMPARE(readSetting(core.engine, "input.repeat_rate").status, QString("custom"));
        QCOMPARE(readAll(module.path()), edited);
    }
    void customLayoutGetsAdvancedRouteNotARewrite() {
        Sandbox s;
        writeFile(s.env.configHome + "/hypr/hyprland.lua", "-- entirely my own\n");
        s.command("hyprctl", "echo '{\"int\":40}'");
        AppCore core(s.env);
        const ChangePlan plan = core.engine.plan("input.repeat_rate", "33");
        QVERIFY(!plan.ok);
        QVERIFY(plan.error.contains("stock layout"));
    }

    // ---- Catalog invariants ---------------------------------------------------
    void luaOptionKeysUseHyprlandsLuaSpelling() {
        // Found on real Hyprland: getoption says tap-to-click, but the Lua key is
        // tap_to_click, and the dashed form is a config error.
        Catalog catalog;
        for (const auto &e : catalog.entries()) {
            if (e.write.value("kind").toString() != QLatin1String("hypr"))
                continue;
            for (const auto &part : e.write.value("path").toArray())
                QVERIFY2(!part.toString().contains('-'), qPrintable(e.id));
        }
    }
    void automaticTimezoneEnablesAndUpdatesThenDisables() {
        Sandbox s;
        s.command("systemctl", kAutomaticTimezone);
        AppCore core(s.env);
        const QString id = "system.timezone.auto";
        const auto *entry = core.catalog.find(id);
        QVERIFY(entry);
        QCOMPARE(entry->page, QString("system"));
        QCOMPARE(entry->section, QString("Language and time"));
        QCOMPARE(entry->type, QString("toggle"));
        QCOMPARE(readSetting(core.engine, id).value, QString("false"));
        QVERIFY(core.engine.plan(id, "true").ok);
        QVERIFY(!core.engine.plan(id, "yes").ok);
        const Transaction enabled = applySetting(core.engine, id, "true");
        QCOMPARE(enabled.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, id).value, QString("true"));
        QVERIFY(QFile::exists(s.env.home + "/timezone-update-ran"));
        const QString enable = "systemctl --system enable --now automatic-timezone.timer";
        const QString update = "systemctl --system start automatic-timezone.service";
        QVERIFY(s.calls().indexOf(enable) >= 0);
        QVERIFY(s.calls().indexOf(update) > s.calls().indexOf(enable));

        const Transaction disabled = applySetting(core.engine, id, "false");
        QCOMPARE(disabled.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, id).value, QString("false"));
        QCOMPARE(readAll(s.env.home + "/timezone-active").trimmed(), QByteArray("inactive"));
        QVERIFY(s.calls().contains("systemctl --system disable --now automatic-timezone.timer"));
        QCOMPARE(s.calls().count(update), 1);
        // Undo restores the scheduling preference; disabling does not revert the time zone.
        QVERIFY(disabled.undoable());
        QCOMPARE(undoTx(core.engine, disabled.id).phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, id).value, QString("true"));
        QCOMPARE(s.calls().count(update), 2);
    }
    void automaticTimezoneDoesNotHideAFirstUpdateFailure_data() {
        QTest::addColumn<bool>("alreadyEnabled");
        QTest::newRow("was off") << false;
        QTest::newRow("was already on") << true;
    }
    void automaticTimezoneDoesNotHideAFirstUpdateFailure() {
        QFETCH(bool, alreadyEnabled);
        Sandbox s;
        s.command("systemctl", kAutomaticTimezone);
        writeFile(s.env.home + "/timezone-enabled", alreadyEnabled ? "enabled" : "disabled");
        writeFile(s.env.home + "/timezone-active", alreadyEnabled ? "active" : "inactive");
        writeFile(s.env.home + "/timezone-update-fails", "");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "system.timezone.auto", "true");
        QCOMPARE(tx.phase, QString("rolled-back"));
        QVERIFY(tx.message.contains("first update failed"));
        QCOMPARE(readSetting(core.engine, "system.timezone.auto").value, alreadyEnabled ? QString("true") : QString("false"));
        QCOMPARE(readAll(s.env.home + "/timezone-active").trimmed(), alreadyEnabled ? QByteArray("active") : QByteArray("inactive"));
        QCOMPARE(s.calls().count("systemctl --system start automatic-timezone.service"), 1);
        QVERIFY(!tx.undoable());
    }
    void automaticTimezoneRequiresReadableInstalledUnits_data() {
        QTest::addColumn<QString>("marker");
        QTest::addColumn<QString>("state");
        QTest::newRow("not installed") << QString("missing") << QString("unavailable");
        QTest::newRow("no bus") << QString("read-fails") << QString("unknown");
        QTest::newRow("incomplete response") << QString("malformed") << QString("unknown");
        QTest::newRow("masked") << QString("masked") << QString("unavailable");
    }
    void automaticTimezoneRequiresReadableInstalledUnits() {
        QFETCH(QString, marker);
        QFETCH(QString, state);
        Sandbox s;
        s.command("systemctl", kAutomaticTimezone);
        if (marker == "masked")
            writeFile(s.env.home + "/timezone-enabled", "masked");
        else
            writeFile(s.env.home + "/timezone-" + marker, "");
        AppCore core(s.env);
        const ReadResult read = readSetting(core.engine, "system.timezone.auto");
        QCOMPARE(read.status, state);
        QVERIFY(!read.reason.isEmpty());
        QCOMPARE(applySetting(core.engine, "system.timezone.auto", "true").phase, QString("failed"));
        for (const auto &call : s.calls())
            QVERIFY2(call.startsWith("systemctl --system show "), qPrintable(call));
    }
    void automaticTimezoneVerifiesBothTimerStates_data() {
        QTest::addColumn<QString>("marker");
        QTest::newRow("authorization denied") << QString("denied");
        QTest::newRow("enable did not start timer") << QString("no-start");
        QTest::newRow("changed before write") << QString("stale");
    }
    void automaticTimezoneVerifiesBothTimerStates() {
        QFETCH(QString, marker);
        Sandbox s;
        s.command("systemctl", kAutomaticTimezone);
        writeFile(s.env.home + "/timezone-" + marker, "");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "system.timezone.auto", "true");
        QVERIFY2(tx.phase == "failed" || tx.phase == "rolled-back", qPrintable(tx.phase));
        QVERIFY(!tx.message.isEmpty());
        if (marker == "stale") {
            for (const auto &call : s.calls())
                QVERIFY2(call.startsWith("systemctl --system show "), qPrintable(call));
        } else {
            QCOMPARE(readSetting(core.engine, "system.timezone.auto").value, QString("false"));
            if (marker == "denied")
                QVERIFY(!s.calls().contains("systemctl --system start automatic-timezone.service"));
        }
    }
    void catalogIsStructurallySound() {
        Catalog catalog;
        QVERIFY(catalog.entries().size() > 400);
        QSet<QString> ids;
        QSet<QString> pages;
        for (const auto &page : catalog.pages())
            pages << page.id;
        QCOMPARE(catalog.pages().size(), 14);
        static const QSet<QString> types{"toggle", "choice", "int", "float", "slider", "text", "status", "action"};
        static const QSet<QString> modes{"native", "omarchy-panel", "omarchy-wizard", "external-helper", "advanced-config"};
        static const QSet<QString> recoveries{"auto-undo", "best-effort", "no-undo"};
        static const QRegularExpression domain("^C[0-9]{2}$");
        for (const auto &e : catalog.entries()) {
            const QByteArray name = e.id.toUtf8();
            QVERIFY2(!ids.contains(e.id), name);
            ids << e.id;
            QVERIFY2(pages.contains(e.page), name);
            QVERIFY2(types.contains(e.type), name);
            QVERIFY2(modes.contains(e.integration), name);
            QVERIFY2(recoveries.contains(e.recovery), name);
            QVERIFY2(domain.match(e.domain).hasMatch(), name);
            QVERIFY2(!e.label.trimmed().isEmpty(), name);
            QVERIFY2(!e.section.isEmpty(), name);
            if (e.isSetting())
                QVERIFY2(!e.read.isEmpty(), name);
            if (e.isAction())
                QVERIFY2(!e.launch.isEmpty(), name);
        }
    }
    void everyPageHasEverySection() {
        Catalog catalog;
        for (const auto &e : catalog.entries()) {
            const auto *page = catalog.page(e.page);
            QVERIFY(page);
            QVERIFY2(page->sections.contains(e.section), qPrintable(e.id + " -> " + e.section));
        }
    }
    void onlyValuePlaceholdersReachCommands() {
        Catalog catalog;
        static const QRegularExpression placeholder("\\{[a-z]+\\}");
        for (const auto &e : catalog.entries()) {
            for (const auto &action : {e.write, e.read}) {
                for (const auto &arg : argvOf(action)) {
                    auto matches = placeholder.globalMatch(arg);
                    while (matches.hasNext())
                        QVERIFY2(matches.next().captured() == QLatin1String("{value}"), qPrintable(e.id + ": " + arg));
                }
            }
            // The launch path has no substitution at all: every route is static.
            const QString launchText = QString::fromUtf8(QJsonDocument(e.launch).toJson());
            QVERIFY2(!launchText.contains("{value}"), qPrintable(e.id));
        }
    }
    void noCatalogCommandRunsAShellWithAValue() {
        Catalog catalog;
        for (const auto &e : catalog.entries()) {
            const QStringList argv = argvOf(e.write);
            QVERIFY2(!argv.contains("bash") && !argv.contains("sh") && !argv.contains("-c"), qPrintable(e.id));
            // Only an argument, never part of a command string, carries a value.
            for (const auto &arg : argv)
                // Superwhisper takes JSON, so its strings arrive wrapped in quotes, and a
                // pattern or choice list keeps quotes out of the value.
                if (arg.contains("{value}"))
                    QVERIFY2(arg == QLatin1String("{value}") || arg == QLatin1String("{value}%") || (arg == QLatin1String("\"{value}\"") && (!e.pattern.isEmpty() || !e.allowCustomValue)), qPrintable(e.id + ": " + arg));
        }
    }
    void noPrivilegeOrSecretRoutes() {
        Catalog catalog;
        for (const auto &e : catalog.entries()) {
            const QString text = QString::fromUtf8(QJsonDocument(e.write).toJson()) + QString::fromUtf8(QJsonDocument(e.read).toJson());
            QVERIFY2(!text.contains("sudo") && !text.contains("pkexec"), qPrintable(e.id));
            const QString launch = QString::fromUtf8(QJsonDocument(e.launch).toJson());
            // Printing a Wi-Fi password is deliberately not reachable from here.
            QVERIFY2(!launch.contains("network-password") && !launch.contains("\"network\",\n\"password\""), qPrintable(e.id));
            static const QRegularExpression sudo("(^|[\\s\";])sudo\\s");
            // The one exception: UFW has no other route, and these run visibly in a
            // terminal, where sudo asks for the password itself.
            QVERIFY2(!sudo.match(launch).hasMatch() || e.id.startsWith("firewall."), qPrintable(e.id));
        }
    }
    void destructiveRoutesAlwaysAsk() {
        Catalog catalog;
        static const QRegularExpression destructive("omarchy-(remove|reinstall|refresh|system-factory|setup-security-.*--remove)");
        for (const auto &e : catalog.entries()) {
            if (!e.isAction())
                continue;
            const QString command = e.launch.value("command").toString();
            const bool isRefreshApplications = command.contains("refresh-applications");
            if (e.id.startsWith("remove.") || e.id == "setup.reset" || (destructive.match(command).hasMatch() && !isRefreshApplications)) {
                QVERIFY2(e.confirm == QLatin1String("destructive") || e.confirm == QLatin1String("confirm"), qPrintable(e.id));
                QVERIFY2(!e.confirmText.isEmpty(), qPrintable(e.id));
            }
        }
    }
    void agentAndDictationAreNotPlainSetters() {
        // `omarchy default agent` starts the agent; it must say so before it runs.
        Catalog catalog;
        const auto *claude = catalog.find("setup.default.agent.claude");
        QVERIFY(claude);
        QVERIFY(claude->isAction());
        QVERIFY(!claude->confirm.isEmpty());
        QVERIFY(claude->confirmText.contains("starts"));
        QVERIFY(!catalog.find("apps.default.agent")->isEditable());
        QVERIFY(!catalog.find("apps.default.dictation")->isEditable());
    }
    void helperLaunchesResolveToArgv() {
        Sandbox s;
        QCOMPARE(HelperLauncher::resolve(QJsonObject{{"kind", "terminal"}, {"command", "omarchy-update"}}, s.env, true),
                 (QStringList{"omarchy-launch-floating-terminal-with-presentation", "omarchy-update"}));
        QCOMPARE(HelperLauncher::resolve(QJsonObject{{"kind", "panel"}, {"target", "omarchy.network"}, {"verb", "toggle"}}, s.env, true),
                 (QStringList{"omarchy", "shell", "shell", "toggle", "omarchy.network"}));
        QCOMPARE(HelperLauncher::resolve(QJsonObject{{"kind", "app"}, {"argv", QJsonArray{"gnome-disks"}}}, s.env, true),
                 (QStringList{"uwsm-app", "--", "gnome-disks"}));
        QCOMPARE(HelperLauncher::resolve(QJsonObject{{"kind", "app"}, {"argv", QJsonArray{"gnome-disks"}}}, s.env, false), QStringList{"gnome-disks"});
        QCOMPARE(HelperLauncher::resolve(QJsonObject{{"kind", "config"}, {"file", "~/.config/hypr/input.lua"}}, s.env, true),
                 (QStringList{"omarchy", "launch", "config", "editor", s.env.home + "/.config/hypr/input.lua"}));
    }
    void launchReportsOpenedNotChanged() {
        Sandbox s;
        s.omarchy("exit 0");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        HelperLauncher launcher(&core.runner, &core.capabilities);
        HelperLauncher::Outcome outcome;
        bool done = false;
        launcher.launch(*core.catalog.find("panel.network"), [&](const HelperLauncher::Outcome &o) { outcome = o; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(outcome.opened);
        QVERIFY(outcome.message.startsWith("Opened"));
        QVERIFY(core.coordinator.history().isEmpty());
    }
    void panelFailureIsReported() {
        Sandbox s;
        s.omarchy("case \"$*\" in 'shell shell ping') exit 0;; *) exit 1;; esac");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        HelperLauncher launcher(&core.runner, &core.capabilities);
        HelperLauncher::Outcome outcome;
        bool done = false;
        launcher.launch(*core.catalog.find("panel.network"), [&](const HelperLauncher::Outcome &o) { outcome = o; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(!outcome.opened);
        QVERIFY(outcome.message.contains("shell"));
    }
    void pageRoutesPointAtRealDestinations() {
        Catalog catalog;
        for (const auto &e : catalog.entries()) {
            if (e.launch.value("kind").toString() != QLatin1String("page"))
                continue;
            const auto *page = catalog.page(e.launch.value("page").toString());
            QVERIFY2(page, qPrintable(e.id));
            const QString focus = e.launch.value("focus").toString();
            if (focus.startsWith("block."))
                QVERIFY2(page->blocks.contains(focus.mid(6)), qPrintable(e.id + " -> " + focus));
            else if (!focus.isEmpty())
                QVERIFY2(catalog.find(focus), qPrintable(e.id + " -> " + focus));
            QVERIFY2(QFile::exists(":/Block" + (focus.isEmpty() ? QString("Audio") : focus.mid(6, 1).toUpper() + focus.mid(7)) + ".qml") || !focus.startsWith("block."), qPrintable(focus));
        }
    }

    // ---- Search ---------------------------------------------------------------
    void searchLandsOnTheRightSetting_data() {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("expected");
        QTest::newRow("sleeping") << "stop sleeping" << "power.stayawake";
        QTest::newRow("text") << "make text bigger" << "appearance.textsize";
        QTest::newRow("mic") << "change microphone" << "native.audio.input";
        QTest::newRow("login") << "start an app at login" << "native.startup";
        QTest::newRow("wallpaper") << "wallpaper" << "appearance.background";
        QTest::newRow("dnd") << "do not disturb" << "desktop.dnd";
        QTest::newRow("printers") << "printer" << "helper.printer";
        QTest::newRow("resolution") << "resolution" << "native.displays";
        QTest::newRow("volume") << "volume" << "native.audio.output";
        QTest::newRow("config filename") << "hyprsunset.conf" << "setup.config.hyprsunset";
        QTest::newRow("helper name") << "nmtui" << "helper.nmtui";
        QTest::newRow("shortcuts") << "keyboard shortcuts" << "native.shortcuts";
        QTest::newRow("snapshots") << "snapshot" << "snapshot.create";
        QTest::newRow("case") << "BLUETOOTH DEVICES" << "native.bluetooth";
    }
    void searchLandsOnTheRightSetting() {
        QFETCH(QString, query);
        QFETCH(QString, expected);
        Catalog catalog;
        const auto hits = catalog.search(query, 5);
        QVERIFY2(!hits.isEmpty(), qPrintable(query));
        QStringList ids;
        for (const auto &hit : hits)
            ids << hit.entry->id;
        QVERIFY2(ids.mid(0, 2).contains(expected), qPrintable(query + " -> " + ids.join(", ")));
    }
    void searchIsFastAndSideEffectFree() {
        Sandbox s;
        s.omarchy("touch \"$HOME/ran\"");
        AppCore core(s.env);
        QElapsedTimer timer;
        timer.start();
        for (const char *q : {"a", "theme", "stop sleeping", "install", "remove steam", "xyz nothing"})
            core.catalog.search(q);
        const auto perQuery = timer.elapsed() / 6;
        QVERIFY2(perQuery < 100, qPrintable(QString("%1 ms per query").arg(perQuery)));
        QVERIFY(!QFile::exists(s.env.home + "/ran"));
    }
    void searchEmptyQueryFindsNothing() {
        Catalog catalog;
        QVERIFY(catalog.search("").isEmpty());
        QVERIFY(catalog.search("   ").isEmpty());
        QVERIFY(catalog.search("zzzqqq").isEmpty());
    }

    // ---- Coverage -------------------------------------------------------------
    void everyDiscoveredEntryHasADestination() {
        // The release definition: zero unclassified first-party entries.
        const auto inventory = QJsonDocument::fromJson(readAll(PROJECT_ROOT "/docs/inventory-2026-11-05.json")).object();
        QVERIFY(!inventory.isEmpty());
        Catalog catalog;
        const auto result = Coverage::reconcile(catalog, inventory);
        QVERIFY2(result.unclassifiedCommands.isEmpty(), qPrintable(result.unclassifiedCommands.join(", ")));
        QVERIFY2(result.unclassifiedMenu.isEmpty(), qPrintable(result.unclassifiedMenu.join(", ")));
        QCOMPARE(result.claimedCommands + result.excludedCommands, 433);
        QVERIFY(result.claimedMenu > 300);
    }
    void aNewCommandFailsCoverage() {
        Catalog catalog;
        const QJsonArray commands{QJsonObject{{"route", "omarchy brand new feature"}, {"binary", "omarchy-brand-new-feature"}, {"hidden", false}}};
        const auto result = Coverage::reconcileLive(catalog, commands);
        QCOMPARE(result.unclassifiedCommands, QStringList{"omarchy brand new feature"});
        const QJsonArray hidden{QJsonObject{{"route", "omarchy brand new feature"}, {"binary", "x"}, {"hidden", true}}};
        QVERIFY(Coverage::reconcileLive(catalog, hidden).unclassifiedCommands.isEmpty());
    }
    void everyExclusionHasAReason() {
        Catalog catalog;
        for (const auto &group : {"commands", "menu"})
            for (const auto &value : catalog.exclusions().value(QLatin1String(group)).toArray()) {
                QVERIFY(!value.toObject().value("reason").toString().isEmpty());
                QVERIFY(!value.toObject().value("pattern").toString().isEmpty());
            }
    }
    void ledgerHasEveryRequiredField() {
        Catalog catalog;
        const auto rows = Coverage::ledger(catalog);
        QCOMPARE(rows.size(), catalog.entries().size());
        for (const auto &value : rows) {
            const auto row = value.toObject();
            for (const char *field : {"id", "domain", "source_references", "searchable_labels", "applicable_capability", "read_method", "integration_mode", "write_or_launch_method", "apply_restart_semantics", "recovery_class", "supported_versions", "implementation_status", "validation_evidence", "known_limitation"})
                QVERIFY2(row.contains(QLatin1String(field)), field);
            // Nothing claims "verified" without recorded evidence.
            QCOMPARE(row.value("implementation_status").toString(), QString("implemented"));
        }
    }
    void wildcards() {
        QVERIFY(Coverage::wildcardMatch("omarchy hw *", "omarchy hw laptop"));
        QVERIFY(!Coverage::wildcardMatch("omarchy hw *", "omarchy hwx"));
        QVERIFY(Coverage::wildcardMatch("a.b", "a.b"));
        QVERIFY(!Coverage::wildcardMatch("a.b", "axb"));
    }

    // ---- Device parsers -------------------------------------------------------
    void audioParsesAndFiltersMonitors() {
        const QByteArray sources = R"([{"name":"mic","description":"Mic","mute":true,"state":"IDLE","volume":{"mono":{"value_percent":"80%"}},"properties":{"object.id":"44"}},{"name":"x.monitor","description":"Monitor of x","volume":{},"properties":{"device.class":"monitor"}}])";
        const auto rows = AudioProvider::parseNodes(sources, "mic", true);
        QCOMPARE(rows.size(), 1);
        const auto mic = rows.first().toMap();
        QCOMPARE(mic.value("volume").toInt(), 80);
        QVERIFY(mic.value("isDefault").toBool());
        QVERIFY(mic.value("mute").toBool());
        QCOMPARE(mic.value("objectId").toString(), QString("44"));
    }
    void audioKeepsDspChainsOutOfTheStreamList() {
        const QByteArray inputs = R"([{"index":1,"sink":2,"volume":{},"properties":{"application.name":"Player"}},{"index":2,"sink":3,"volume":{},"properties":{}},{"index":3,"sink":3,"volume":{},"properties":{"application.name":"EasyEffects"}}])";
        const auto rows = AudioProvider::parseStreams(inputs);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("app").toString(), QString("Player"));
    }
    void audioDefaultMustActuallyChange() {
        Sandbox s;
        // Omarchy's setter exits 0 even when nothing happened.
        s.omarchy("exit 0");
        s.command("pactl", kScript6);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        AudioProvider audio(&runner, &coordinator);
        QSignalSpy messages(&audio, &AudioProvider::message);
        audio.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(audio.outputs().size(), 2, 5000);
        audio.setDefaultOutput("b");
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(coordinator.history().first().phase != QLatin1String("committed"));
    }
    void audioNamesComeFromTheCardAndPort() {
        // As PipeWire describes a laptop's own card, its HDMI ports, and a DSP sink.
        const QByteArray sinks = R"J([
          {"index":151,"name":"spk","description":"Maker Chip (Series 3) HD Audio Speaker","volume":{},"properties":{"object.id":"84","device.description":"Maker Chip (Series 3) HD Audio","device.profile.description":"Speaker","device.icon_name":"audio-speakers"}},
          {"index":153,"name":"hdmi1","description":"Maker Chip (Series 3) HD Audio HDMI / DisplayPort 1 Output (Stereo)","volume":{},"properties":{"object.id":"75","device.description":"Maker Chip (Series 3) HD Audio","device.profile.description":"HDMI / DisplayPort 1 Output","device.icon_name":"video-display"}},
          {"index":49,"name":"dsp","description":"Laptop Speakers","volume":{},"properties":{"object.id":"30","device.description":"Laptop Speakers"}},
          {"index":60,"name":"bt","description":"Earbuds","volume":{},"properties":{"object.id":"61","device.description":"Earbuds","device.profile.description":"High Fidelity Playback (A2DP Sink)","device.bus":"bluetooth"}},
          {"index":70,"name":"plain","description":"Some Device","volume":{},"properties":{"object.id":"71"}}])J";
        const auto rows = AudioProvider::parseNodes(sinks, "spk", false);
        QCOMPARE(rows.size(), 5);
        auto row = [&](int i) { return rows[i].toMap(); };
        QCOMPARE(row(0).value("title").toString(), QString("HD Audio Speaker"));
        QCOMPARE(row(0).value("subtitle").toString(), QString("Maker Chip (Series 3)"));
        QCOMPARE(row(0).value("kind").toString(), QString("speaker"));
        QCOMPARE(row(0).value("index").toInt(), 151);
        // The full description stays for what is read aloud.
        QCOMPARE(row(0).value("description").toString(), QString("Maker Chip (Series 3) HD Audio Speaker"));
        QCOMPARE(row(1).value("title").toString(), QString("HDMI / DisplayPort 1"));
        QCOMPARE(row(1).value("subtitle").toString(), QString::fromUtf8("Maker Chip (Series 3) · Stereo"));
        QCOMPARE(row(1).value("kind").toString(), QString("displays"));
        QCOMPARE(row(2).value("title").toString(), QString("Laptop Speakers"));
        QVERIFY(row(2).value("subtitle").toString().isEmpty());
        QCOMPARE(row(2).value("kind").toString(), QString("laptop"));
        QCOMPARE(row(3).value("title").toString(), QString("Earbuds"));
        QCOMPARE(row(3).value("subtitle").toString(), QString("High Fidelity Playback (A2DP Sink)"));
        QCOMPARE(row(3).value("kind").toString(), QString("bluetooth"));
        QCOMPARE(row(4).value("title").toString(), QString("Some Device"));
        // A microphone's port reads the same way.
        const auto mic = AudioProvider::parseNodes(R"J([{"index":156,"name":"mic","description":"Maker Chip (Series 3) HD Audio Microphones","volume":{},"properties":{"device.description":"Maker Chip (Series 3) HD Audio","device.profile.description":"Microphones"}}])J", "mic", true);
        QCOMPARE(mic.first().toMap().value("title").toString(), QString("HD Audio Microphones"));
        QCOMPARE(mic.first().toMap().value("kind").toString(), QString("mic"));
    }
    void streamsFindTheirOutputByIndex() {
        const auto outputs = AudioProvider::parseNodes(R"J([{"index":151,"name":"spk","description":"S","volume":{},"properties":{"object.id":"84"}},{"index":152,"name":"hp","description":"H","volume":{},"properties":{"object.id":"76"}}])J", "spk", false);
        // Two streams from one app stay two, and each finds its own output.
        auto streams = AudioProvider::parseStreams(R"J([{"index":1,"sink":151,"volume":{},"properties":{"application.name":"Browser","media.name":"Playback"}},{"index":2,"sink":152,"volume":{},"properties":{"application.name":"Browser","media.name":"Playback"}},{"index":3,"sink":76,"volume":{},"properties":{"application.name":"Old"}},{"index":4,"sink":999,"volume":{},"properties":{"application.name":"Lost"}}])J");
        AudioProvider::matchStreams(streams, outputs);
        QCOMPARE(streams.size(), 4);
        QCOMPARE(streams[0].toMap().value("output").toString(), QString("spk"));
        QCOMPARE(streams[1].toMap().value("output").toString(), QString("hp"));
        // Older PipeWire named the sink by its object id.
        QCOMPARE(streams[2].toMap().value("output").toString(), QString("hp"));
        // One that cannot be told is not guessed.
        QVERIFY(streams[3].toMap().value("output").toString().isEmpty());
    }
    void aStreamMoveIsCheckedAfterwards() {
        Sandbox s;
        s.omarchy("exit 0");
        // pactl takes the move and does nothing with it.
        s.command("pactl", R"SH(
case "$*" in
  "-f json list sinks") echo '[{"index":151,"name":"spk","description":"S","volume":{},"properties":{"object.id":"84"}},{"index":152,"name":"hp","description":"H","volume":{},"properties":{"object.id":"76"}}]';;
  "-f json list sources") echo '[]';;
  "-f json list sink-inputs") echo '[{"index":7,"sink":151,"volume":{},"properties":{"application.name":"Browser","object.serial":"700"}}]';;
  get-default-sink) echo spk;;
esac)SH");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        AudioProvider audio(&runner, &coordinator);
        QSignalSpy messages(&audio, &AudioProvider::message);
        audio.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(audio.streams().size(), 1, 5000);
        QCOMPARE(audio.streams().first().toMap().value("output").toString(), QString("spk"));
        audio.moveStream(7, "hp");
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(messages.first().at(0).toString().contains("did not move"));
        QVERIFY(s.calls().contains("pactl move-sink-input 7 hp"));
    }
    void aDefaultChangeIsShownAsPending() {
        Sandbox s;
        s.omarchy("sleep 0.4; exit 0");
        s.command("pactl", kScript6);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        AudioProvider audio(&runner, &coordinator);
        audio.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(audio.outputs().size(), 2, 5000);
        audio.setDefaultOutput("b");
        QCOMPARE(audio.pendingDefault(), QString("b"));
        QTRY_VERIFY_WITH_TIMEOUT(audio.pendingDefault().isEmpty(), 8000);
    }
    void aBluetoothFailureStaysWithItsDevice() {
        Sandbox s;
        s.omarchy(R"SH(case "$*" in "bluetooth power is-on") exit 0;; esac; exit 0)SH");
        s.bluetooth(R"SH(
case "$1" in
  devices) printf 'Device AA:BB:CC:DD:EE:03 Office Mouse\nDevice AA:BB:CC:DD:EE:04 Office Mouse\n';;
  info) printf 'Name: Office Mouse\nConnected: no\nPaired: yes\n';;
esac)SH");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        BluetoothProvider bluetooth(&runner, &coordinator);
        bluetooth.refresh();
        // Two devices with one name stay two.
        QTRY_COMPARE_WITH_TIMEOUT(bluetooth.devices().size(), 2, 5000);
        bluetooth.connectDevice("AA:BB:CC:DD:EE:04");
        QCOMPARE(bluetooth.pending().value("AA:BB:CC:DD:EE:04").toString(), QString("connect"));
        QVERIFY(!bluetooth.pending().contains("AA:BB:CC:DD:EE:03"));
        QTRY_VERIFY_WITH_TIMEOUT(bluetooth.pending().isEmpty(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(!bluetooth.failures().value("AA:BB:CC:DD:EE:04").toString().isEmpty(), 5000);
        QVERIFY(!bluetooth.failures().contains("AA:BB:CC:DD:EE:03"));
        // It outlives the refresh after it, and goes with the device.
        QTRY_COMPARE_WITH_TIMEOUT(bluetooth.devices().size(), 2, 5000);
        QVERIFY(bluetooth.failures().contains("AA:BB:CC:DD:EE:04"));
    }
    void soundToolsAppearOnceEach() {
        Catalog catalog;
        QStringList placed, blocks;
        for (const auto &row : catalog.panelLayout("sound"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    if (!panel.value("block").toString().isEmpty())
                        blocks << panel.value("block").toString();
                    for (const auto &id : panel.value("rows").toArray())
                        placed << id.toString();
                }
        placed.sort();
        QStringList tools{"audio.tuning", "helper.printer", "install.camera", "install.video.decode", "panel.audio", "panel.bluetooth", "restart.audio", "restart.bluetooth"};
        QCOMPARE(placed, tools);
        QCOMPARE(blocks, QStringList({"output", "microphone", "appAudio", "bluetooth"}));
        // Search lands on each native section by name, and each is on the page.
        const QHash<QString, QString> destinations{{"native.audio.output", "block.output"}, {"native.audio.input", "block.microphone"}, {"native.audio.apps", "block.appAudio"}, {"native.bluetooth", "block.bluetooth"}};
        for (auto it = destinations.cbegin(); it != destinations.cend(); ++it) {
            const CatalogEntry *entry = catalog.find(it.key());
            QVERIFY(entry);
            QCOMPARE(entry->launch.value("focus").toString(), it.value());
            QVERIFY(catalog.page("sound")->blocks.contains(it.value().mid(6)));
        }
    }

    void bluetoothInfoParsing() {
        const auto device = BluetoothProvider::parseInfo("Device AA:BB:CC:DD:EE:01 (public)\n\tName: Phones\n\tAlias: Phones\n\tIcon: audio-headset\n\tPaired: yes\n\tConnected: no\n\tBattery Percentage: 0x4b (75)\n", "AA:BB:CC:DD:EE:01");
        QCOMPARE(device.value("name").toString(), QString("Phones"));
        QVERIFY(device.value("paired").toBool());
        QVERIFY(!device.value("connected").toBool());
        QCOMPARE(device.value("battery").toInt(), 75);
        QVERIFY(BluetoothProvider::isValidAddress("AA:BB:CC:DD:EE:01"));
        QVERIFY(!BluetoothProvider::isValidAddress("AA:BB; rm -rf /"));
    }
    void bluetoothDeviceRemovalIsVerified() {
        Sandbox s;
        s.omarchy("exit 0");
        s.bluetooth("case \"$1\" in devices) echo 'Device AA:BB:CC:DD:EE:01 Headset';; info) printf 'Connected: no\\nPaired: yes\\n';; esac");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        BluetoothProvider bluetooth(&runner, &coordinator);
        QSignalSpy messages(&bluetooth, &BluetoothProvider::message);
        bluetooth.refresh();
        QTRY_COMPARE(bluetooth.status(), QString("ok"));
        bluetooth.connectDevice("AA:BB:CC:DD:EE:01");
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(messages.first().at(0).toString().contains("did not respond"));
    }
    void networkTerseEscapes() {
        QCOMPARE(NetworkProvider::splitTerse(R"(a\:b:c\\d:e)"), (QStringList{"a:b", "c\\d", "e"}));
    }
    void cliNetworkSnapshotRetainsReportedIdentitiesAndSeparatesUnknownState() {
        NetworkDesk desk;
        desk.s.env.process.insert("LC_ALL", "de_DE.UTF-8");
        desk.s.env.process.insert("CLICOLOR_FORCE", "1");
        const auto reply = CliApi::execute(desk.s.env, {"network", "status"});
        QCOMPARE(reply.exitCode, 0);
        QVERIFY(reply.body.value("verified").toBool());
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("complete").toBool());
        QCOMPARE(data.value("devices").toArray().size(), 4);
        const auto future = data.value("devices").toArray().last().toObject();
        QCOMPARE(future.value("connection").toString(), QString("Odd: Name"));
        QVERIFY(future.value("connected").isNull());
        QVERIFY(!future.value("stateKnown").toBool());
        QCOMPARE(data.value("saved").toArray().size(), 3);
        QCOMPARE(data.value("accessPoints").toArray().size(), 4);
        const auto points = data.value("accessPoints").toArray();
        QCOMPARE(points[0].toObject().value("ssid").toString(), QString("Cafe: Lab"));
        QVERIFY(points[3].toObject().value("ssidEmpty").toBool());
        QVERIFY(!points[3].toObject().value("secured").toBool());
        QVERIFY(!data.value("scanRequested").toBool());
        QVERIFY(!data.value("connectivityCheckRequested").toBool());
        QVERIFY(!data.value("accessPointAgeKnown").toBool());
        QVERIFY(!data.value("atomic").toBool());
        QCOMPARE(desk.s.calls().size(), 4);
        for (const auto &call : desk.s.calls()) {
            QVERIFY(call.contains("-c no -e yes"));
            QVERIFY(!call.contains("--ask") && !call.contains("--show-secrets") && !call.contains("connectivity check"));
            if (call.contains("wifi list")) QVERIFY(call.endsWith("--rescan no"));
        }
    }
    void networkDurablePreviewBlocksGuiRadioScansAndRestoreLeases() {
        NetworkDesk desk; desk.previewReceipt();
        ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        QVERIFY(!NetworkPreview::pendingReason(desk.s.env).isEmpty());
        QString error; QVERIFY(!coordinator.acquireLease("restore", &error)); QVERIFY(!error.isEmpty());
        bool applied = false, settled = false;
        ChangeCoordinator::Request request; request.settingId = "network.dns";
        request.apply = [&](auto done) { applied = true; done(true, {}); };
        coordinator.submit(std::move(request), [&](const auto &tx) { settled = true; QCOMPARE(tx.phase, QString("failed")); });
        QVERIFY(settled); QVERIFY(!applied);
        provider.activate("11111111-1111-1111-1111-111111111111", "Home");
        provider.deactivate("wlan0"); provider.setWifiEnabled(false);
        for (const auto &call : desk.s.calls()) QVERIFY(!call.contains("connection up") && !call.contains("device disconnect") && !call.contains("CheckpointCreate"));
        const auto radio = CliApi::execute(desk.s.env, {"network", "radio", "false", "--yes"});
        QCOMPARE(radio.exitCode, 1);
        QCOMPARE(radio.body.value("error").toObject().value("code").toString(), QString("busy"));
        QVariantMap scan; provider.requestScan({}, [&](const auto &value) { scan = value; });
        QTRY_VERIFY(!scan.isEmpty());
        QVERIFY(!scan.value("ok").toBool());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls") && !QFileInfo::exists(desk.s.env.home + "/scan-calls"));
    }
    void networkGuiAdoptsAndSettlesOwnedCliTokens_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("keep") << "keep";
        QTest::newRow("revert") << "revert";
    }
    void networkGuiAdoptsAndSettlesOwnedCliTokens() {
        QFETCH(QString, action);
        NetworkDesk desk; const Env env = desk.ownedPreviewDriver();
        CommandRunner runner(env); ChangeCoordinator coordinator(env); NetworkProvider provider(&runner, &coordinator);
        provider.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(provider.awaitingKeep() && provider.canKeep(), 5000);
        QCOMPARE(provider.ownedPreview().value("token").toString(), QString(32, 'a'));
        QSignalSpy messages(&provider, &NetworkProvider::message);
        if (action == "keep") provider.keepConnection(); else provider.revertConnection();
        QTRY_VERIFY_WITH_TIMEOUT(!provider.awaitingKeep() && !provider.busy(), 5000);
        QVERIFY(!messages.isEmpty()); QVERIFY(!messages.last().at(1).toBool());
        const auto receipt = NetworkPreview::localReceipt(env);
        QCOMPARE(receipt.value("phase").toString(), action == "keep" ? QString("kept") : QString("reverted"));
        QVERIFY(desk.s.calls().filter("preview-control network preview " + action + " " + QString(32, 'a')).size() == 1);
        for (const auto &call : desk.s.calls()) QVERIFY(!call.startsWith("busctl ") && !call.contains("connection up") && !call.contains("device disconnect"));
    }
    void networkGuiNeverKeepsAnUnconfirmedOwnedPreview() {
        NetworkDesk desk; const Env env = desk.ownedPreviewDriver();
        writeFile(env.home + "/owned-mode", "unconfirmed");
        CommandRunner runner(env); ChangeCoordinator coordinator(env); NetworkProvider provider(&runner, &coordinator);
        provider.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(desk.s.calls().filter("preview-control network preview status").size() >= 1, 5000);
        QVERIFY(provider.awaitingKeep()); QVERIFY(!provider.canKeep());
        provider.keepConnection();
        QTest::qWait(50);
        QVERIFY(desk.s.calls().filter("preview-control network preview keep").isEmpty());
        QFile::remove(env.home + "/owned-mode");
        QTRY_VERIFY_WITH_TIMEOUT(provider.canKeep(), 5000);
        writeFile(env.home + "/owned-mode", "keep-failed");
        QSignalSpy messages(&provider, &NetworkProvider::message);
        provider.keepConnection();
        QTRY_VERIFY_WITH_TIMEOUT(!provider.busy() && !messages.isEmpty(), 5000);
        QVERIFY(messages.last().at(1).toBool()); QVERIFY(provider.awaitingKeep()); QVERIFY(!provider.canKeep());
        QFile::remove(env.home + "/owned-mode");
        provider.revertConnection();
        QTRY_VERIFY_WITH_TIMEOUT(!provider.awaitingKeep(), 5000);
    }
    void networkDurablePreviewRejectsUnsafeReceipts_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"symlink", "public-file", "public-directory", "bad-json", "extra-field", "stale-file", "bad-token"}) QTest::newRow(mode) << QString(mode);
    }
    void networkDurablePreviewRejectsUnsafeReceipts() {
        QFETCH(QString, mode);
        NetworkDesk desk; const QString file = desk.previewReceipt();
        const QString directory = QFileInfo(file).absolutePath();
        if (mode == "symlink") { QFile::remove(file); QVERIFY(QFile::link(desk.s.env.home + "/radio-enabled", file)); }
        if (mode == "public-file") QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther);
        if (mode == "public-directory") QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther);
        if (mode == "bad-json") writeFile(file, "{");
        if (mode == "extra-field") { auto value = QJsonDocument::fromJson(readAll(file)).object(); value["unrecognized"] = "do-not-trust"; FileOps::atomicWrite(file, QJsonDocument(value).toJson()); }
        if (mode == "stale-file") QFile::remove(file);
        if (mode == "bad-token") FileOps::atomicWrite(directory + "/current.json", "{\"token\":\"../../other\"}");
        QVERIFY(!NetworkPreview::pendingReason(desk.s.env).isEmpty());
        const auto status = CliApi::execute(desk.s.env, {"network", "preview", "status"});
        QCOMPARE(status.exitCode, 1);
        QVERIFY(!status.body.value("verified").toBool());
        QVERIFY(desk.s.calls().isEmpty());
    }
    void networkTerminalReceiptDoesNotPretendToVerifyCurrentConnectivity() {
        NetworkDesk desk; desk.previewReceipt("kept");
        QVERIFY(NetworkPreview::pendingReason(desk.s.env).isEmpty());
        const auto status = CliApi::execute(desk.s.env, {"network", "preview", "status"});
        QCOMPARE(status.exitCode, 0);
        QCOMPARE(status.body.value("outcome").toString(), QString("kept"));
        QVERIFY(status.body.value("data").toObject().value("historicalReceipt").toBool());
        QVERIFY(!status.body.value("verified").toBool());
        QVERIFY(desk.s.calls().isEmpty());
    }
    void cliNetworkRadioPlansAndVerifiesOnlyTheSwitch() {
        NetworkDesk desk;
        const auto planned = CliApi::execute(desk.s.env, {"network", "radio", "false", "--dry-run"});
        QCOMPARE(planned.exitCode, 0);
        const auto plan = planned.body.value("data").toObject();
        QCOMPARE(plan.value("before").toString(), QString("true"));
        QCOMPARE(plan.value("after").toString(), QString("false"));
        QVERIFY(!plan.value("confirmText").toString().isEmpty());
        QCOMPARE(plan.value("details").toObject().value("devices").toArray(), QJsonArray{"wlan0"});
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls"));
        QVERIFY(!QFileInfo::exists(desk.s.env.appStateDir + "/journal.jsonl"));
        const auto unconfirmed = CliApi::execute(desk.s.env, {"network", "radio", "false"});
        QCOMPARE(unconfirmed.exitCode, 1);
        QCOMPARE(unconfirmed.body.value("error").toObject().value("code").toString(), QString("confirmation_required"));
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls"));
        const auto changed = CliApi::execute(desk.s.env, {"network", "radio", "false", "--yes"});
        QCOMPARE(changed.exitCode, 0);
        QCOMPARE(changed.body.value("outcome").toString(), QString("committed"));
        QVERIFY(changed.body.value("verified").toBool());
        const auto data = changed.body.value("data").toObject();
        QCOMPARE(data.value("verificationScope").toString(), QString("NetworkManager-radio-switch-only"));
        QVERIFY(!data.value("readback").toObject().value("enabled").toBool());
        QVERIFY(!data.value("connectionVerified").toBool() && !data.value("internetReachabilityVerified").toBool());
        QCOMPARE(readAll(desk.s.env.home + "/radio-calls"), QByteArray("false\n"));
        ChangeCoordinator history(desk.s.env);
        QCOMPARE(history.history().size(), 1);
        QVERIFY(!history.history().first().undoable());
        QCOMPARE(history.history().first().before, QString("true"));
        const auto enabled = CliApi::execute(desk.s.env, {"network", "radio", "true"});
        QCOMPARE(enabled.exitCode, 0);
        QVERIFY(enabled.body.value("data").toObject().value("readback").toObject().value("enabled").toBool());
        QCOMPARE(readAll(desk.s.env.home + "/radio-calls"), QByteArray("false\ntrue\n"));
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
    }
    void cliNetworkRadioRefusesUnknownStateAndHardwareBlocks_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("code");
        for (const auto &mode : {"fail", "malformed", "extra", "truncated", "stderr-truncated", "timeout", "devices-fail"})
            QTest::newRow(mode) << QString(mode) << QString("read_unavailable");
        QTest::newRow("no-adapter") << QString("no-adapter") << QString("unavailable");
        QTest::newRow("missing-tool") << QString("missing-tool") << QString("unavailable");
        QTest::newRow("hardware-off") << QString("hardware-off") << QString("hardware_blocked");
    }
    void cliNetworkRadioRefusesUnknownStateAndHardwareBlocks() {
        QFETCH(QString, mode); QFETCH(QString, code);
        NetworkDesk desk;
        if (mode == "no-adapter") desk.set("devices", "lo:loopback:unmanaged:\n");
        else if (mode == "devices-fail") desk.mode("devices", "fail");
        else if (mode == "missing-tool") QFile::remove(desk.s.bin("busctl"));
        else if (mode == "hardware-off") writeFile(desk.s.env.home + "/radio-hardware", "false");
        else writeFile(desk.s.env.home + "/radio-read-mode", mode.toUtf8());
        const auto result = CliApi::execute(desk.s.env, {"network", "radio", "true"});
        QCOMPARE(result.exitCode, 1);
        QCOMPARE(result.body.value("error").toObject().value("code").toString(), code);
        QVERIFY(!result.body.value("verified").toBool());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls"));
    }
    void cliNetworkRadioDoesNotConfuseTheHardwareSwitchWithItsOwnPreference() {
        NetworkDesk desk;
        writeFile(desk.s.env.home + "/radio-hardware", "false");
        // Hardware-blocked enabling is refused; disabling can still be verified.
        const auto disabled = CliApi::execute(desk.s.env, {"network", "radio", "false", "--yes"});
        QCOMPARE(disabled.exitCode, 0);
        QVERIFY(disabled.body.value("verified").toBool());
        QVERIFY(!disabled.body.value("data").toObject().value("readback").toObject().value("hardwareEnabled").toBool());
        desk.set("general", "malformed-but-unrelated\n");
        const auto planned = CliApi::execute(desk.s.env, {"network", "radio", "false", "--dry-run"});
        QCOMPARE(planned.exitCode, 0);
        QCOMPARE(planned.body.value("data").toObject().value("before").toString(), QString("false"));
    }
    void cliNetworkRadioDoesNotTrustWriterExitOrUnreadableReadback_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("readback");
        for (const auto &mode : {"denied", "output", "truncated", "changed-then-failed", "timeout", "no-op"})
            QTest::newRow(mode) << QString(mode) << false;
        for (const auto &mode : {"fail", "malformed", "truncated", "stderr-truncated", "timeout"})
            QTest::newRow((QString("readback-") + mode).toLatin1().constData()) << QString(mode) << true;
    }
    void cliNetworkRadioDoesNotTrustWriterExitOrUnreadableReadback() {
        QFETCH(QString, mode); QFETCH(bool, readback);
        NetworkDesk desk;
        writeFile(desk.s.env.home + (readback ? "/radio-readback-mode" : "/radio-write-mode"), mode.toUtf8());
        const auto result = CliApi::execute(desk.s.env, {"network", "radio", "false", "--yes"});
        QCOMPARE(result.exitCode, 1);
        QCOMPARE(result.body.value("outcome").toString(), QString("partial"));
        QVERIFY(!result.body.value("verified").toBool());
        QCOMPARE(readAll(desk.s.env.home + "/radio-calls"), QByteArray("false\n"));
        const auto observed = result.body.value("data").toObject().value("readback");
        if (readback) {
            QCOMPARE(observed.toObject().value("status").toString(), QString("read_unavailable"));
            QVERIFY(observed.toObject().value("enabled").isNull());
        } else if (mode == "no-op") QVERIFY(observed.toObject().value("enabled").toBool());
        else QVERIFY(observed.isNull());
    }
    void networkRadioRefreshesQueuedPreconditions_data() {
        QTest::addColumn<QString>("changed");
        for (const auto &value : {"switch", "hardware", "inventory"}) QTest::newRow(value) << QString(value);
    }
    void networkRadioRefreshesQueuedPreconditions() {
        QFETCH(QString, changed);
        NetworkDesk desk; ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        ProviderPlan plan; bool planned = false;
        provider.planWifiEnabled(false, [&](const auto &value) { plan = value; planned = true; });
        QTRY_VERIFY(planned); QVERIFY(plan.ok);
        ChangeCoordinator::Done release;
        ChangeCoordinator::Request prior; prior.settingId = "prior";
        prior.apply = [&](auto done) { release = done; };
        coordinator.submit(std::move(prior), [](const auto &) {}); QVERIFY(release);
        Transaction result; bool done = false;
        connect(&provider, &NetworkProvider::operationFinished, this, [&](const auto &tx) { result = tx; done = true; });
        provider.changeWifiEnabled(plan, true); QVERIFY(provider.radioBusy());
        if (changed == "switch") writeFile(desk.s.env.home + "/radio-enabled", "false");
        if (changed == "hardware") writeFile(desk.s.env.home + "/radio-hardware", "false");
        if (changed == "inventory") desk.set("devices", "wlan1:wifi:unavailable:\n");
        release(true, {}); QTRY_VERIFY(done);
        QCOMPARE(result.phase, QString("failed"));
        QVERIFY(!provider.radioBusy());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls"));
    }
    void networkRadioHonorsTheRestoreLeaseAndGlobalLock() {
        NetworkDesk desk; ChangeCoordinator lease(desk.s.env);
        QVERIFY(lease.acquireLease("fixture"));
        auto result = CliApi::execute(desk.s.env, {"network", "radio", "false", "--yes"});
        QCOMPARE(result.exitCode, 1);
        QCOMPARE(result.body.value("error").toObject().value("code").toString(), QString("busy"));
        QVERIFY(desk.s.calls().isEmpty()); lease.releaseLease();
        QLockFile lock(desk.s.env.appStateDir + "/change.lock"); QVERIFY(lock.tryLock(0));
        result = CliApi::execute(desk.s.env, {"network", "radio", "false", "--yes"}); lock.unlock();
        QCOMPARE(result.exitCode, 1);
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls"));
    }
    void networkGuiRadioUsesTheSharedPlanAndProtectsAnActiveWrite() {
        NetworkDesk desk; ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        writeFile(desk.s.env.home + "/radio-write-mode", "held");
        provider.setWifiEnabled(false); QVERIFY(provider.radioBusy());
        QTRY_VERIFY(QFileInfo::exists(desk.s.env.home + "/radio-started"));
        provider.setWifiEnabled(true); QVERIFY(provider.radioBusy());
        QVERIFY(provider.busy());
        provider.activate("11111111-1111-1111-1111-111111111111", "Home");
        provider.deactivate("wlan0");
        for (const auto &call : desk.s.calls()) {
            QVERIFY(!call.contains("connection up") && !call.contains("device disconnect"));
        }
        QVariantMap scan;
        provider.requestScan({}, [&](const auto &value) { scan = value; });
        QTRY_VERIFY(!scan.isEmpty());
        QVERIFY(!scan.value("ok").toBool());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
        writeFile(desk.s.env.home + "/release-radio", "1");
        QTRY_VERIFY(!provider.radioBusy());
        QTRY_COMPARE(provider.states().value("general").toMap().value("status").toString(), QString("ok"));
        QVERIFY(!provider.wifiEnabled());
        QCOMPARE(readAll(desk.s.env.home + "/radio-calls"), QByteArray("false\n"));
    }
    void networkRadioRefusesAnActiveDisconnect() {
        NetworkDesk desk; ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        provider.deactivate("wlan0");
        ProviderPlan plan; bool done = false;
        provider.planWifiEnabled(false, [&](const auto &value) { plan = value; done = true; });
        QVERIFY(done); QVERIFY(!plan.ok);
        QCOMPARE(plan.errorCode, QString("busy"));
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/radio-calls"));
        QTRY_VERIFY(provider.pending().isEmpty());
    }
    void cliNetworkScanPlansWithoutRequestsAndJournalsOnlyRequestAcceptance() {
        NetworkDesk desk;
        auto reply = CliApi::execute(desk.s.env, {"network", "rescan", "wlan0", "--dry-run"});
        QCOMPARE(reply.exitCode, 0);
        QCOMPARE(reply.body.value("outcome").toString(), QString("planned"));
        QVERIFY(!reply.body.value("verified").toBool());
        const auto details = reply.body.value("data").toObject().value("details").toObject();
        QCOMPARE(details.value("devices").toArray(), QJsonArray{"wlan0"});
        QVERIFY(!details.value("scanCompletionVerified").toBool());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
        QVERIFY(!QFileInfo::exists(desk.s.env.appStateDir + "/journal.jsonl"));
        QCOMPARE(desk.s.calls().size(), 2);
        reply = CliApi::execute(desk.s.env, {"network", "rescan", "wlan0"});
        QCOMPARE(reply.exitCode, 0);
        QCOMPARE(reply.body.value("outcome").toString(), QString("scan_requested"));
        QVERIFY(!reply.body.value("verified").toBool());
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("requestAccepted").toBool());
        QVERIFY(!data.value("scanCompletionVerified").toBool() && !data.value("accessPointAgeKnown").toBool());
        QCOMPARE(data.value("transaction").toObject().value("phase").toString(), QString("committed"));
        QCOMPARE(data.value("transaction").toObject().value("extra").toObject().value("verificationScope").toString(), QString("scan-request-acceptance-only"));
        QCOMPARE(readAll(desk.s.env.home + "/scan-calls"), QByteArray("wlan0\n"));
        ChangeCoordinator history(desk.s.env);
        QCOMPARE(history.history().size(), 1);
        QCOMPARE(history.history().first().kind, QString("action"));
        QVERIFY(!history.history().first().undoable());
        const auto cached = CliApi::execute(desk.s.env, {"network", "wifi"});
        QCOMPARE(cached.exitCode, 0);
        QVERIFY(!cached.body.value("data").toObject().value("scanRequested").toBool());
        QCOMPARE(readAll(desk.s.env.home + "/scan-calls"), QByteArray("wlan0\n"));
    }
    void cliNetworkScanSelectsEligibleAdaptersAndKeepsPartialResults() {
        NetworkDesk desk;
        desk.set("devices", "wlan0:wifi:disconnected:\nwlan1:wifi:connected:Home\nwlan2:wifi:unavailable:\nwlan3:wifi:unmanaged:\nwlan4:wifi:connected (externally):external\nfuture0:wifi:future-state:\n");
        auto plan = CliApi::execute(desk.s.env, {"network", "rescan", "--dry-run"});
        QCOMPARE(plan.exitCode, 0);
        QCOMPARE(plan.body.value("data").toObject().value("details").toObject().value("devices").toArray(), (QJsonArray{"wlan0","wlan1"}));
        QCOMPARE(plan.body.value("data").toObject().value("details").toObject().value("excluded").toArray().size(), 4);
        writeFile(desk.s.env.home + "/scan-mode-wlan1", "denied");
        const auto reply = CliApi::execute(desk.s.env, {"network", "rescan"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("outcome").toString(), QString("partial"));
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("requestAccepted").isNull());
        const auto rows = data.value("results").toArray();
        QCOMPARE(rows.size(), 2);
        QVERIFY(rows[0].toObject().value("requestAccepted").toBool());
        QVERIFY(rows[1].toObject().value("requestAccepted").isNull());
        QCOMPARE(readAll(desk.s.env.home + "/scan-calls"), QByteArray("wlan0\nwlan1\n"));
    }
    void cliNetworkScanResolvesEveryTargetBeforeSendingRequests() {
        NetworkDesk desk;
        desk.set("devices", "wlan0:wifi:disconnected:\nwlan1:wifi:disconnected:\n");
        writeFile(desk.s.env.home + "/lookup-fail-wlan1", "1");
        const auto reply = CliApi::execute(desk.s.env, {"network", "rescan"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("outcome").toString(), QString("blocked"));
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        QVERIFY(!reply.body.value("data").toObject().value("requestAttempted").toBool());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
    }
    void cliNetworkScanLimitsTheBatchAndRetainsUnsentTargets() {
        NetworkDesk desk;
        QStringList devices;
        for (int i = 0; i < 16; ++i) {
            const QString name = "wlan" + QString::number(i);
            devices << name + ":wifi:disconnected:";
            writeFile(desk.s.env.home + "/scan-mode-" + name, "timeout");
        }
        desk.set("devices", (devices.join('\n') + '\n').toUtf8());
        const auto reply = CliApi::execute(desk.s.env, {"network", "rescan"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("outcome").toString(), QString("unknown"));
        const auto rows = reply.body.value("data").toObject().value("results").toArray();
        QCOMPARE(rows.size(), 16);
        // Coarse timers may expire slightly early. Do not assume precisely two
        // processes start; require truthful evidence for attempts and unsent rows.
        int attempted = 0;
        for (const auto &value : rows) {
            const auto row = value.toObject();
            if (row.value("requestAttempted").toBool()) {
                ++attempted;
                QVERIFY(row.value("requestAccepted").isNull());
            } else {
                QCOMPARE(row.value("requestAccepted"), QJsonValue(false));
            }
        }
        QVERIFY(attempted > 0 && attempted < rows.size());
        QCOMPARE(readAll(desk.s.env.home + "/scan-calls").trimmed().split('\n').size(), attempted);
    }
    void cliNetworkScanRefusalsDoNotSendRequests_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("code");
        QTest::newRow("off") << QString("off") << QString("unavailable");
        QTest::newRow("unmanaged") << QString("unmanaged") << QString("unavailable");
        QTest::newRow("non-wifi") << QString("non-wifi") << QString("invalid_value");
        QTest::newRow("missing") << QString("missing") << QString("stale_target");
        QTest::newRow("failed-read") << QString("failed-read") << QString("read_unavailable");
        QTest::newRow("bad-path") << QString("bad-path") << QString("read_unavailable");
        QTest::newRow("missing-busctl") << QString("missing-busctl") << QString("unavailable");
    }
    void cliNetworkScanRefusalsDoNotSendRequests() {
        QFETCH(QString, mode);
        QFETCH(QString, code);
        NetworkDesk desk;
        if (mode == "off") desk.set("general", "disabled:none\n");
        if (mode == "unmanaged") desk.set("devices", "wlan0:wifi:unmanaged:\n");
        if (mode == "non-wifi") desk.set("devices", "wlan0:ethernet:connected:Wired\n");
        if (mode == "missing") desk.set("devices", {});
        if (mode == "failed-read") desk.mode("devices", "fail");
        if (mode == "bad-path") writeFile(desk.s.env.home + "/object-wlan0", "/org/freedesktop/NetworkManager/Other/1\n");
        if (mode == "missing-busctl") QFile::remove(desk.s.bin("busctl"));
        const auto reply = CliApi::execute(desk.s.env, {"network", "rescan", "wlan0"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), code);
        QVERIFY(!reply.body.value("data").toObject().value("requestAttempted").toBool());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
    }
    void cliNetworkScanFailedRepliesRemainUnconfirmed_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"denied", "output", "truncated", "timeout"}) QTest::newRow(mode) << QString(mode);
    }
    void cliNetworkScanFailedRepliesRemainUnconfirmed() {
        QFETCH(QString, mode);
        NetworkDesk desk;
        writeFile(desk.s.env.home + "/scan-mode-wlan0", mode.toUtf8());
        const auto reply = CliApi::execute(desk.s.env, {"network", "rescan", "wlan0"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("outcome").toString(), QString("unknown"));
        QVERIFY(!reply.body.value("verified").toBool());
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("requestAttempted").toBool() && data.value("requestAccepted").isNull());
        QVERIFY(!data.value("scanCompletionVerified").toBool());
        QCOMPARE(data.value("transaction").toObject().value("phase").toString(), QString("partial"));
        QCOMPARE(readAll(desk.s.env.home + "/scan-calls"), QByteArray("wlan0\n"));
    }
    void networkScanHonorsLeaseAndGlobalChangeLock() {
        NetworkDesk desk;
        ChangeCoordinator lease(desk.s.env);
        QVERIFY(lease.acquireLease("restore-fixture"));
        auto reply = CliApi::execute(desk.s.env, {"network", "rescan", "wlan0"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("busy"));
        QVERIFY(desk.s.calls().isEmpty());
        lease.releaseLease();
        QLockFile other(desk.s.env.appStateDir + "/change.lock");
        QVERIFY(other.tryLock(0));
        reply = CliApi::execute(desk.s.env, {"network", "rescan", "wlan0"});
        other.unlock();
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
    }
    void networkQueuedScanRefreshesPreconditionsBeforeRequests() {
        NetworkDesk desk;
        ChangeCoordinator coordinator(desk.s.env);
        CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        ChangeCoordinator::Done release;
        ChangeCoordinator::Request prior;
        prior.settingId = "fixture-prior";
        prior.apply = [&](auto done) { release = done; };
        coordinator.submit(std::move(prior), [](const auto &) {});
        QVERIFY(release);
        QVariantMap result;
        bool done = false;
        provider.requestScan("wlan0", [&](const auto &value) { result = value; done = true; });
        desk.set("general", "disabled:none\n");
        release(true, {});
        QTRY_VERIFY(done);
        QCOMPARE(result.value("outcome").toString(), QString("blocked"));
        QVERIFY(!result.value("requestAttempted").toBool());
        QVERIFY(!provider.scanning());
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/scan-calls"));
    }
    void networkGuiScanUsesTheSharedPathAndKeepsDuplicateRequestsSeparate() {
        NetworkDesk desk;
        ChangeCoordinator coordinator(desk.s.env);
        CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        writeFile(desk.s.env.home + "/scan-mode-wlan0", "held");
        provider.rescan();
        QTRY_VERIFY(QFileInfo::exists(desk.s.env.home + "/scan-started"));
        QVERIFY(provider.scanning());
        QVariantMap duplicate;
        provider.requestScan("wlan0", [&](const auto &value) { duplicate = value; });
        QCOMPARE(duplicate.value("errorCode").toString(), QString("busy"));
        QVERIFY(provider.scanning());
        writeFile(desk.s.env.home + "/release-scan", "1");
        QTRY_VERIFY(!provider.scanning());
        QCOMPARE(provider.scanResult().value("outcome").toString(), QString("scan_requested"));
        QTRY_COMPARE(provider.status(), QString("ok"));
        QCOMPARE(readAll(desk.s.env.home + "/scan-calls"), QByteArray("wlan0\n"));
    }
    void cliNetworkScopedReadsDoNotProbeUnrelatedSources_data() {
        QTest::addColumn<QString>("section");
        for (const auto &section : {"general", "devices", "saved", "wifi"}) QTest::newRow(section) << QString(section);
    }
    void cliNetworkScopedReadsDoNotProbeUnrelatedSources() {
        QFETCH(QString, section);
        NetworkDesk desk;
        const auto reply = CliApi::execute(desk.s.env, {"network", section});
        QCOMPARE(reply.exitCode, 0);
        const auto data = reply.body.value("data").toObject();
        QCOMPARE(desk.s.calls().size(), 1);
        QCOMPARE(data.value("requestedSections").toArray(), QJsonArray{section});
        const auto states = data.value("states").toObject();
        for (const auto &key : QStringList{"general", "devices", "saved", "wifi"})
            QCOMPARE(states.value(key).toObject().value("status").toString(), key == section ? QString("ok") : QString("not_requested"));
        if (section != QLatin1String("general")) QVERIFY(data.value("wifiEnabled").isNull());
        if (section != QLatin1String("saved")) QVERIFY(data.value("saved").isNull());
    }
    void cliNetworkFailureNeverBecomesAnOffRadioOrEmptyVerifiedList_data() {
        QTest::addColumn<QString>("section");
        QTest::addColumn<QString>("mode");
        for (const auto &section : {"general", "devices", "saved", "wifi"})
            for (const auto &mode : {"fail", "truncated", "stderr-truncated"})
                QTest::newRow(qPrintable(QString(section) + "-" + mode)) << QString(section) << QString(mode);
        QTest::newRow("timeout") << QString("general") << QString("timeout");
    }
    void cliNetworkFailureNeverBecomesAnOffRadioOrEmptyVerifiedList() {
        QFETCH(QString, section);
        QFETCH(QString, mode);
        NetworkDesk desk;
        desk.mode(section, mode);
        const auto reply = CliApi::execute(desk.s.env, {"network", "status"});
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(!reply.body.value("verified").toBool());
        const auto data = reply.body.value("data").toObject();
        QCOMPARE(data.value("status").toString(), QString("partial"));
        QVERIFY(!data.value("complete").toBool());
        QCOMPARE(data.value("states").toObject().value(section).toObject().value("status").toString(), QString("read_unavailable"));
        if (section == QLatin1String("general")) QVERIFY(data.value("wifiEnabled").isNull());
        else QVERIFY(data.value(section == QLatin1String("wifi") ? "accessPoints" : section).isNull());
    }
    void cliNetworkRejectsMalformedAndAmbiguousRecords_data() {
        QTest::addColumn<QString>("section");
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("general-empty") << QString("general") << QByteArray();
        QTest::newRow("general-unknown-radio") << QString("general") << QByteArray("maybe:full\n");
        QTest::newRow("general-extra-row") << QString("general") << QByteArray("enabled:full\nenabled:full\n");
        QTest::newRow("general-extra-field") << QString("general") << QByteArray("enabled:full:extra\n");
        QTest::newRow("device-short") << QString("devices") << QByteArray("wlan0:wifi:connected\n");
        QTest::newRow("device-empty-id") << QString("devices") << QByteArray(":wifi:disconnected:\n");
        QTest::newRow("device-duplicate") << QString("devices") << QByteArray("wlan0:wifi:connected:Home\nwlan0:wifi:disconnected:\n");
        QTest::newRow("bad-escape") << QString("devices") << QByteArray("wlan0:wifi:connected:bad\\qname\n");
        QTest::newRow("dangling-escape") << QString("devices") << QByteArray("wlan0:wifi:connected:bad\\\n");
        QTest::newRow("invalid-utf8") << QString("devices") << (QByteArray("wlan0:wifi:connected:") + char(0xff));
        QTest::newRow("saved-invalid-uuid") << QString("saved") << QByteArray("Home:11111111--111-1111-1111-111111111111:wifi:yes\n");
        QTest::newRow("saved-invalid-active") << QString("saved") << QByteArray("Home:11111111-1111-1111-1111-111111111111:wifi:unknown\n");
        QTest::newRow("saved-duplicate-uuid") << QString("saved") << QByteArray("Home:11111111-1111-1111-1111-111111111111:wifi:yes\nDifferent:11111111-1111-1111-1111-111111111111:ethernet:no\n");
        QTest::newRow("wifi-invalid-signal") << QString("wifi") << QByteArray("*:Home:oops:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan0\n");
        QTest::newRow("wifi-large-signal") << QString("wifi") << QByteArray("*:Home:101:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan0\n");
        QTest::newRow("wifi-bad-bssid") << QString("wifi") << QByteArray("*:Home:50:WPA2:bad:wlan0\n");
        QTest::newRow("wifi-missing-fields") << QString("wifi") << QByteArray("*:Home:50:WPA2\n");
        QTest::newRow("wifi-active-flag") << QString("wifi") << QByteArray("?:Home:50:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan0\n");
        QTest::newRow("wifi-duplicate-identity") << QString("wifi") << QByteArray("*:Home:50:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan0\n :Other:30::02\\:00\\:00\\:00\\:00\\:01:wlan0\n");
    }
    void cliNetworkRejectsMalformedAndAmbiguousRecords() {
        QFETCH(QString, section);
        QFETCH(QByteArray, bytes);
        NetworkDesk desk;
        desk.set(section, bytes);
        const auto reply = CliApi::execute(desk.s.env, {"network", section});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        QVERIFY(!reply.body.value("data").toObject().value("complete").toBool());
    }
    void cliNetworkEmptyInventoriesAndUnknownConnectivityAreValidReads() {
        NetworkDesk desk;
        desk.set("general", "disabled:unknown\n");
        for (const auto &section : {"devices", "saved", "wifi"}) desk.set(section, {});
        const auto reply = CliApi::execute(desk.s.env, {"network", "status"});
        QCOMPARE(reply.exitCode, 0);
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("complete").toBool());
        QCOMPARE(data.value("wifiEnabled"), QJsonValue(false));
        QVERIFY(!data.value("connectivityKnown").toBool());
        QVERIFY(data.value("devices").toArray().isEmpty());
        QVERIFY(data.value("saved").toArray().isEmpty());
        QVERIFY(data.value("accessPoints").toArray().isEmpty());
    }
    void cliNetworkMissingToolAndExcessiveRowsRemainUnavailable() {
        Sandbox s;
        const auto missing = CliApi::execute(s.env, {"network", "status"});
        QCOMPARE(missing.exitCode, 1);
        const auto data = missing.body.value("data").toObject();
        QCOMPARE(data.value("status").toString(), QString("unavailable"));
        QVERIFY(data.value("wifiEnabled").isNull() && data.value("devices").isNull());
        NetworkDesk desk;
        QByteArray rows;
        for (int i = 0; i < 4097; ++i) rows += "device" + QByteArray::number(i) + ":ethernet:disconnected:\n";
        desk.set("devices", rows);
        const auto excessive = CliApi::execute(desk.s.env, {"network", "devices"});
        QCOMPARE(excessive.exitCode, 1);
        QVERIFY(excessive.body.value("data").toObject().value("devices").isNull());
    }
    void networkRefreshWaitsForAllReadsAndIgnoresEveryStaleSection() {
        NetworkDesk desk;
        ChangeCoordinator coordinator(desk.s.env);
        CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        QSignalSpy refreshed(&provider, &NetworkProvider::refreshed);
        for (const auto &section : {"general", "devices", "saved", "wifi"}) writeFile(desk.s.env.home + "/delay-" + section, "1");
        provider.refresh();
        QTRY_VERIFY(QFile::exists(desk.s.env.home + "/captured-general") && QFile::exists(desk.s.env.home + "/captured-devices")
                    && QFile::exists(desk.s.env.home + "/captured-saved") && QFile::exists(desk.s.env.home + "/captured-wifi"));
        QCOMPARE(refreshed.size(), 0);
        for (const auto &section : {"general", "devices", "saved", "wifi"}) QFile::remove(desk.s.env.home + "/delay-" + section);
        desk.set("general", "disabled:none\n");
        desk.set("devices", "new0:ethernet:disconnected:\n");
        desk.set("saved", {}); desk.set("wifi", {});
        provider.refresh();
        QTRY_COMPARE(refreshed.size(), 1);
        writeFile(desk.s.env.home + "/release-reads", "1");
        QTRY_COMPARE(runner.activeCount(), 0);
        QCOMPARE(provider.snapshot().value("generation").toInt(), 2);
        QCOMPARE(provider.devices().first().toMap().value("device").toString(), QString("new0"));
        QVERIFY(provider.saved().isEmpty() && provider.networks().isEmpty());
        QVERIFY(!provider.wifiEnabled());
    }
    void networkGuiKeepsItsGroupingAndNeverKeepsAStaleFailedList() {
        NetworkDesk desk;
        ChangeCoordinator coordinator(desk.s.env);
        CommandRunner runner(desk.s.env);
        NetworkProvider provider(&runner, &coordinator);
        QSignalSpy refreshed(&provider, &NetworkProvider::refreshed);
        provider.refresh();
        QTRY_COMPARE(refreshed.size(), 1);
        QCOMPARE(provider.devices().size(), 1);
        QCOMPARE(provider.saved().size(), 2);
        QCOMPARE(provider.networks().size(), 1);
        QCOMPARE(provider.networks().first().toMap().value("signal").toInt(), 40);
        desk.mode("devices", "fail"); desk.mode("wifi", "fail");
        provider.refresh();
        QTRY_COMPARE(refreshed.size(), 2);
        QVERIFY(provider.devices().isEmpty() && provider.networks().isEmpty());
        QCOMPARE(provider.states().value("devices").toMap().value("status").toString(), QString("read_unavailable"));
    }
    void networkDeduplicatesAccessPoints() {
        const auto rows = NetworkProvider::parseNetworks(" :Cafe:30:WPA2\n :Cafe:70:WPA2\n*:Home:40:WPA3\n :Open:20:\n ::10:WPA2\n");
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].toMap().value("ssid").toString(), QString("Home"));
        QCOMPARE(rows[1].toMap().value("signal").toInt(), 70);
        QVERIFY(!rows[2].toMap().value("secured").toBool());
    }
    void networkDevicesIgnoreInternals() {
        const auto rows = NetworkProvider::parseDevices("wlan0:wifi:connected:Home\nlo:loopback:connected (externally):lo\np2p:wifi-p2p:disconnected:\ntailscale0:tun:connected (externally):ts\n");
        QCOMPARE(rows.size(), 2);
    }
    void networkSwitchUsesACheckpointAndRollsBack() {
        Sandbox s;
        s.command("busctl", kScript7);
        s.command("nmcli", kNmcliBringsUpCafe);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(network.awaitingKeep(), 5000);
        const QString calls = s.calls().join('\n');
        QVERIFY(calls.indexOf("CheckpointCreate") < calls.indexOf("connection up"));
        network.revertConnection();
        QTRY_VERIFY_WITH_TIMEOUT(s.calls().join('\n').contains("CheckpointRollback o /org/freedesktop/NetworkManager/Checkpoint/7"), 5000);
        QVERIFY(!network.awaitingKeep());
    }
    void networkKeepDestroysTheCheckpoint() {
        Sandbox s;
        s.command("busctl", kScript8);
        s.command("nmcli", kNmcliBringsUpCafe);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(network.awaitingKeep(), 5000);
        network.keepConnection();
        QTRY_VERIFY_WITH_TIMEOUT(s.calls().join('\n').contains("CheckpointDestroy"), 5000);
        QVERIFY(!s.calls().join('\n').contains("CheckpointRollback"));
    }
    void networkFailedSwitchRollsBackImmediately() {
        Sandbox s;
        s.command("busctl", kScript9);
        s.command("nmcli", kScript10);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(s.calls().join('\n').contains("CheckpointRollback"), 5000);
    }
    void networkWorksWithoutACheckpoint() {
        Sandbox s;
        s.command("busctl", "echo denied >&2; exit 1");
        s.command("nmcli", kNmcliBringsUpCafe);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        QSignalSpy messages(&network, &NetworkProvider::message);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(!network.awaitingKeep());
        QVERIFY(!messages.first().at(1).toBool());
    }
    void networkKnowsWhatItDoesNotManage() {
        const auto rows = NetworkProvider::parseDevices("wlp0s20f3:wifi:connected:Home\ntailscale0:tun:connected (externally):tailscale0\nenp1s0:ethernet:unavailable:\n");
        QCOMPARE(rows.size(), 3);
        QVERIFY(!rows[0].toMap().value("external").toBool());
        QVERIFY(rows[1].toMap().value("external").toBool());
        QVERIFY(rows[1].toMap().value("connected").toBool());
        QVERIFY(!rows[2].toMap().value("connected").toBool());
    }
    void aConnectionIsOnlyUpWhenNetworkManagerSaysSo() {
        Sandbox s;
        s.command("busctl", "echo denied >&2; exit 1");
        // nmcli says yes, and then the profile is not active.
        s.command("nmcli", "exit 0");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        QSignalSpy messages(&network, &NetworkProvider::message);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QCOMPARE(network.pending().value("Cafe").toString(), QString("connect"));
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(messages.first().at(0).toString().contains("did not become"));
        QVERIFY(network.pending().isEmpty());
        QVERIFY(network.failures().contains("Cafe"));
        QVERIFY(!network.busy());
    }
    void aDisconnectIsCheckedAfterwards() {
        Sandbox s;
        s.command("nmcli", R"SH(case "$*" in *"DEVICE,TYPE,STATE,CONNECTION device"*) echo 'wlan0:wifi:connected:Home';; esac; exit 0)SH");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        QSignalSpy messages(&network, &NetworkProvider::message);
        network.deactivate("wlan0");
        QCOMPARE(network.pending().value("wlan0").toString(), QString("disconnect"));
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(0).toString().contains("still connected"));
        QVERIFY(network.failures().contains("wlan0"));
        QVERIFY(network.pending().isEmpty());
    }
    void networkPanelsPlaceEveryEntryOnce() {
        Catalog catalog;
        QStringList placed, wifiRows;
        for (const auto &row : catalog.panelLayout("network"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    for (const auto &id : panel.value("rows").toArray()) {
                        placed << id.toString();
                        if (panel.value("block").toString() == "wifi")
                            wifiRows << id.toString();
                    }
                }
        QCOMPARE(placed.size(), 18);
        QCOMPARE(QSet<QString>(placed.begin(), placed.end()).size(), 18);
        QStringList expected;
        for (const auto *entry : catalog.entriesForPage("network"))
            if (entry->id != QLatin1String("native.wifi"))
                expected << entry->id;
        placed.sort();
        expected.sort();
        QCOMPARE(placed, expected);
        // The band lives in the Wi-Fi card, and two entries that run one command stay two.
        QCOMPARE(wifiRows, QStringList{"network.band"});
        QVERIFY(placed.contains("share.localsend") && placed.contains("trigger.share.file"));
        QVERIFY(placed.contains("setup.network.qr") && placed.contains("share.qr.note"));
        QCOMPARE(catalog.find("native.wifi")->launch.value("focus").toString(), QString("block.wifi"));
    }
    void powerPanelsPlaceEveryEntryOnce() {
        Catalog catalog;
        QStringList placed, blocks;
        QHash<QString, QStringList> byPanel;
        for (const auto &row : catalog.panelLayout("power"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    if (!panel.value("block").toString().isEmpty())
                        blocks << panel.value("block").toString();
                    for (const auto &id : panel.value("rows").toArray()) {
                        placed << id.toString();
                        byPanel[panel.value("title").toString()] << id.toString();
                    }
                }
        QStringList expected;
        for (const auto *entry : catalog.entriesForPage("power"))
            if (entry->id != QLatin1String("native.battery"))
                expected << entry->id;
        // Twenty entries: the battery summary stands for Battery status, the rest are placed once.
        QCOMPARE(expected.size() + 1, 20);
        QStringList sorted = placed, sortedExpected = expected;
        sorted.sort();
        sortedExpected.sort();
        QCOMPARE(sorted, sortedExpected);
        QCOMPARE(blocks, QStringList{"battery"});
        QCOMPARE(catalog.find("native.battery")->launch.value("focus").toString(), QString("block.battery"));
        QCOMPARE(byPanel.value("Power").size(), 4);
        QCOMPARE(byPanel.value("Lock and screensaver").size(), 4);
        QCOMPARE(byPanel.value("Session").size(), 8);
        QCOMPARE(byPanel.value("Hibernation"), QStringList({"power.hibernation", "hibernation.setup", "hibernation.remove"}));
        // The seven session actions are the panel's tiles.
        int tiles = 0;
        for (const auto &id : byPanel.value("Session"))
            if (catalog.find(id)->group == QLatin1String("Session actions"))
                ++tiles;
        QCOMPARE(tiles, 7);
    }
    void everyAppsEntryIsReachable() {
        Catalog catalog;
        QStringList placed, blocks;
        QHash<QString, int> library;
        int agents = 0;
        bool agentsCollapsible = false;
        for (const auto &row : catalog.panelLayout("apps"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    if (!panel.value("block").toString().isEmpty())
                        blocks << panel.value("block").toString();
                    for (const auto &id : panel.value("rows").toArray())
                        placed << id.toString();
                    if (panel.value("section").toString() == "AI agents") {
                        agents = panel.value("rows").toArray().size();
                        agentsCollapsible = panel.value("collapsible").toBool();
                    }
                    for (const auto &group : panel.value("library").toArray())
                        for (const auto &item : group.toObject().value("items").toArray())
                            library.insert(group.toObject().value("heading").toString() + "/" + item.toObject().value("section").toString(), item.toObject().value("rows").toArray().size());
                }
        // Two native destinations stand for their sections; everything else is a control,
        // a row in a closed panel, or a row in a library group, each exactly once.
        QStringList expected, natives;
        for (const auto *entry : catalog.entriesForPage("apps")) {
            if (entry->launch.value("kind").toString() == QLatin1String("page") && entry->launch.value("page").toString() == QLatin1String("apps"))
                natives << entry->launch.value("focus").toString();
            else
                expected << entry->id;
        }
        QCOMPARE(expected.size() + natives.size(), 195);
        QCOMPARE(placed.size(), QSet<QString>(placed.begin(), placed.end()).size());
        placed.sort();
        expected.sort();
        QCOMPARE(placed, expected);
        natives.sort();
        // Installed apps is reached both as the list and as where launchers are changed.
        QCOMPARE(natives, QStringList({"block.associations", "block.installedApps", "block.installedApps"}));
        QCOMPARE(blocks, QStringList({"associations", "installedApps"}));
        QCOMPARE(agents, 26);
        QVERIFY(agentsCollapsible);
        // Sixteen categories, counted from the catalog.
        QCOMPARE(library.size(), 16);
        QCOMPARE(library.value("/Packages"), 8);
        QCOMPARE(library.value("/Web apps and TUIs"), 5);
        QCOMPARE(library.value("Install/Install development tools"), 21);
        QCOMPARE(library.value("Remove/Remove dictation"), 2);
        // Two entries that run one command stay two.
        QVERIFY(placed.contains("install.package") && placed.contains("install.app.generic"));
    }
    void aNewSoftwareSectionJoinsTheLibrary() {
        Sandbox s;
        const QString dir = s.dir.path() + "/catalog";
        writeFile(dir + "/pages.json", R"([{"id":"p","label":"P","icon":"x","description":"","sections":["A"],"panels":[{"columns":[[{"section":"Library","library":[{"heading":"","items":[{"section":"A","label":"A","icon":"x"}]}]}]]}]}])");
        writeFile(dir + "/settings.json", "[]");
        writeFile(dir + "/workflows.json", R"([{"id":"a.one","page":"p","section":"A","label":"One","type":"action","launch":{"kind":"exec","argv":["true"]}},{"id":"b.new","page":"p","section":"Install robots","label":"Robots","type":"action","launch":{"kind":"exec","argv":["true"]}}])");
        Catalog catalog(dir);
        const QJsonArray layout = catalog.panelLayout("p");
        QCOMPARE(layout.size(), 1);
        const auto panel = layout[0].toObject().value("columns").toArray()[0].toArray()[0].toObject();
        const auto items = panel.value("library").toArray()[0].toObject().value("items").toArray();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[1].toObject().value("section").toString(), QString("Install robots"));
        QCOMPARE(panel.value("rows").toArray().size(), 2);
    }
    void everyAccountsEntryIsPlacedOnce() {
        Catalog catalog;
        QStringList placed;
        QHash<QString, QStringList> byPanel;
        for (const auto &row : catalog.panelLayout("accounts"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    for (const auto &id : panel.value("rows").toArray()) {
                        placed << id.toString();
                        byPanel[panel.value("title").toString()] << id.toString();
                    }
                }
        QStringList expected;
        for (const auto *entry : catalog.entriesForPage("accounts"))
            expected << entry->id;
        QCOMPARE(expected.size(), 20);
        placed.sort();
        expected.sort();
        QCOMPARE(placed, expected);
        QCOMPARE(byPanel.value("Security status"), QStringList({"accounts.sshd", "accounts.firewall"}));
        QCOMPARE(byPanel.value("Password and sign-in").size(), 6);
        QCOMPARE(byPanel.value("Remote login"), QStringList({"setup.security.sshd", "setup.security.ssh-agent", "remove.security.sshd"}));
        QCOMPARE(byPanel.value("Firewall"), QStringList({"firewall.status", "firewall.enable", "firewall.disable", "firewall.allow", "firewall.delete"}));
        QCOMPARE(byPanel.value("Sudo and Docker").size(), 3);
        QCOMPARE(byPanel.value("Git and GitHub"), QStringList{"setup.config.git"});
        // The firewall actions keep their section; only their panel is new.
        QCOMPARE(catalog.find("firewall.allow")->section, QString("Remote login"));
    }
    void everySystemEntryIsPlacedOnce() {
        Catalog catalog;
        QStringList placed, blocks;
        QHash<QString, int> library;
        QString cautionGroup;
        for (const auto &row : catalog.panelLayout("system"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    if (!panel.value("block").toString().isEmpty())
                        blocks << panel.value("block").toString();
                    for (const auto &id : panel.value("rows").toArray())
                        placed << id.toString();
                    for (const auto &group : panel.value("library").toArray())
                        for (const auto &itemValue : group.toObject().value("items").toArray()) {
                            const auto item = itemValue.toObject();
                            library.insert(item.value("section").toString(), item.value("rows").toArray().size());
                            if (item.value("tone").toString() == "caution")
                                cautionGroup = item.value("section").toString();
                        }
                }
        // Three native destinations stand for the live blocks; the rest are library rows.
        QStringList expected, natives;
        for (const auto *entry : catalog.entriesForPage("system")) {
            if (entry->launch.value("kind").toString() == QLatin1String("page") && entry->launch.value("page").toString() == QLatin1String("system"))
                natives << entry->launch.value("focus").toString();
            else
                expected << entry->id;
        }
        QCOMPARE(expected.size() + natives.size(), 68);
        QCOMPARE(expected.size(), 65);
        QCOMPARE(placed.size(), QSet<QString>(placed.begin(), placed.end()).size());
        placed.sort();
        expected.sort();
        QCOMPARE(placed, expected);
        natives.sort();
        QCOMPARE(natives, QStringList({"block.services", "block.startup", "block.storage"}));
        QCOMPARE(blocks, QStringList({"storage", "services", "startup"}));
        // Seven groups, counted from the catalog, the resets set apart.
        QCOMPARE(library.size(), 7);
        QCOMPARE(library.value("This computer"), 6);
        QCOMPARE(library.value("Updates"), 13);
        QCOMPARE(library.value("Language and time"), 8);
        QCOMPARE(library.value("Snapshots and recovery"), 6);
        QCOMPARE(library.value("Boot and firmware"), 6);
        QCOMPARE(library.value("Restart components"), 10);
        QCOMPARE(library.value("Reset to defaults"), 16);
        QCOMPARE(cautionGroup, QString("Reset to defaults"));
        // The jump links, in reading order.
        QCOMPARE(catalog.page("system")->jumps, QStringList({"Storage", "Startup apps", "Services", "Maintenance"}));
    }
    void everyAdvancedEntryIsPlacedOnce() {
        Catalog catalog;
        QStringList placed, blocks;
        QHash<QString, QStringList> bySection;
        QJsonObject files;
        for (const auto &row : catalog.panelLayout("advanced"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    if (!panel.value("block").toString().isEmpty())
                        blocks << panel.value("block").toString();
                    for (const auto &id : panel.value("rows").toArray()) {
                        placed << id.toString();
                        bySection[panel.value("section").toString()] << id.toString();
                    }
                    if (panel.value("files").toBool())
                        files = panel;
                }
        // Three native destinations stand for the blocks; every other entry is a row, once.
        QStringList expected, natives;
        for (const auto *entry : catalog.entriesForPage("advanced")) {
            if (entry->launch.value("kind").toString() == QLatin1String("page") && entry->launch.value("page").toString() == QLatin1String("advanced"))
                natives << entry->launch.value("focus").toString();
            else
                expected << entry->id;
        }
        QCOMPARE(expected.size() + natives.size(), 42);
        QCOMPARE(placed.size(), QSet<QString>(placed.begin(), placed.end()).size());
        placed.sort();
        expected.sort();
        QCOMPARE(placed, expected);
        natives.sort();
        QCOMPARE(natives, QStringList({"block.diagnostics", "block.history", "block.hooks"}));
        QCOMPARE(blocks, QStringList({"history", "diagnostics", "hooks"}));
        QCOMPARE(bySection.value("Config files").size(), 20);
        QCOMPARE(bySection.value("Developer environment").size(), 9);
        QCOMPARE(bySection.value("Learn").size(), 10);
        // Every config row opens a file in the editor; the first look is five of them.
        for (const auto &id : bySection.value("Config files"))
            QCOMPARE(catalog.find(id)->launch.value("kind").toString(), QString("config"));
        const QStringList preview = files.value("preview").toVariant().toStringList();
        QCOMPARE(preview, QStringList({"setup.config.hyprland", "setup.input", "setup.keybindings", "config.shelljson", "config.shelltoml"}));
        for (const auto &id : preview)
            QVERIFY(bySection.value("Config files").contains(id));
        QCOMPARE(catalog.page("advanced")->jumps, QStringList({"History", "Diagnostics", "Hooks", "Config files", "Developer tools", "Learn"}));
    }
    void networkRefusesBadIds() {
        Sandbox s;
        s.command("nmcli", "exit 0");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        QSignalSpy messages(&network, &NetworkProvider::message);
        network.activate("--help", "x");
        QCOMPARE(messages.count(), 1);
        QVERIFY(s.calls().isEmpty());
    }
    void serviceUnitsMergeAndSort() {
        const auto rows = ServicesProvider::mergeUnits(
            R"([{"unit":"b.service","active":"active","description":"B"},{"unit":"a.service","active":"failed","description":"A"},{"unit":"t.timer","active":"active"}])",
            R"([{"unit_file":"b.service","state":"enabled"},{"unit_file":"c.service","state":"disabled"},{"unit_file":"g@.service","state":"disabled"},{"unit_file":"s.service","state":"static"}])");
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].toMap().value("unit").toString(), QString("a.service"));
        QVERIFY(rows[0].toMap().value("failed").toBool());
        QCOMPARE(rows[1].toMap().value("enabled").toString(), QString("enabled"));
        QCOMPARE(rows[2].toMap().value("unit").toString(), QString("c.service"));
    }
    void serviceActionsAreConstrained() {
        QVERIFY(ServicesProvider::isSafeUnit("foo-bar@x.service"));
        QVERIFY(!ServicesProvider::isSafeUnit(""));
        QVERIFY(!ServicesProvider::isSafeUnit("foo.timer"));
        QVERIFY(!ServicesProvider::isSafeUnit("--now.service; reboot"));
        QVERIFY(!ServicesProvider::isSafeUnit("--bad.service"));
        QVERIFY(!ServicesProvider::isSafeUnit("foo.service\n"));
        Sandbox s;
        s.command("systemctl", "echo active");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ServicesProvider services(&runner, &coordinator);
        QSignalSpy messages(&services, &ServicesProvider::message);
        services.serviceAction("foo.service", "mask", false);
        QCOMPARE(messages.count(), 1);
        services.serviceAction("bad unit", "start", false);
        QCOMPARE(messages.count(), 2);
        QVERIFY(s.calls().isEmpty());
    }
    void serviceStartIsVerified() {
        ServiceDesk desk;
        desk.set("user", "ActiveState", "failed");
        desk.mode("noop");
        ChangeCoordinator coordinator(desk.s.env);
        CommandRunner runner(desk.s.env);
        ServicesProvider services(&runner, &coordinator);
        QSignalSpy messages(&services, &ServicesProvider::message);
        services.serviceAction("agent.service", "start", false);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(messages.first().at(0).toString().contains("did not start"));
        QVERIFY(desk.s.calls().join('\n').contains("systemctl --user start agent.service"));
    }
    void startupMasksWithoutTouchingTheSystemEntry() {
        Sandbox s;
        const QString system = s.env.sys("/etc/xdg/autostart/agent.desktop");
        writeFile(system, "[Desktop Entry]\nName=Agent\nExec=agent\n");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ServicesProvider services(&runner, &coordinator);
        auto rows = ServicesProvider::readStartup(s.env);
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.first().toMap().value("enabled").toBool());
        services.setStartupEnabled("agent.desktop", false);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.history().isEmpty() && coordinator.history().first().finished(), 5000);
        QCOMPARE(coordinator.history().first().phase, QString("committed"));
        rows = ServicesProvider::readStartup(s.env);
        QVERIFY(!rows.first().toMap().value("enabled").toBool());
        QCOMPARE(readAll(system), QByteArray("[Desktop Entry]\nName=Agent\nExec=agent\n"));
        QVERIFY(readAll(s.env.configHome + "/autostart/agent.desktop").contains("Hidden=true"));
    }
    void startupRefusesPathTraversal() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ServicesProvider services(&runner, &coordinator);
        QSignalSpy messages(&services, &ServicesProvider::message);
        services.setStartupEnabled("../../evil.desktop", false);
        QCOMPARE(messages.count(), 1);
        QVERIFY(coordinator.history().isEmpty());
    }
    void shortcutModifiersAndKeys() {
        QCOMPARE(ShortcutsProvider::modsToText(64 + 1), QString("SUPER + SHIFT"));
        QCOMPARE(ShortcutsProvider::modsToText(0), QString());
        QCOMPARE(ShortcutsProvider::canonicalKeys("shift + super + f"), QString("SUPER + SHIFT + F"));
        QVERIFY(ShortcutsProvider::isValidKeys("SUPER + F"));
        QVERIFY(!ShortcutsProvider::isValidKeys("F"));
        QVERIFY(!ShortcutsProvider::isValidKeys("SUPER + \"); os.exit()"));
        const auto rows = ShortcutsProvider::parseBinds(R"([{"modmask":64,"key":"space","description":"Launcher"},{"modmask":0,"key":"","catch_all":true}])");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("keys").toString(), QString("SUPER + SPACE"));
    }
    void shortcutCollisionsAreNeverSilent() {
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/hypr-binds", R"([{"modmask":64,"key":"SPACE","description":"Launcher"},{"modmask":64,"key":"E","description":"Files"}])");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ShortcutsProvider shortcuts(&runner, &coordinator);
        QSignalSpy changed(&shortcuts, &ShortcutsProvider::changed);
        shortcuts.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 5000);
        const auto check = shortcuts.checkKeys("super + e");
        QCOMPARE(check.value("conflicts").toStringList(), QStringList{"Files"});
        QVERIFY(!check.value("critical").toBool());
        QVERIFY(shortcuts.checkKeys("SUPER + SPACE").value("critical").toBool());
        QVERIFY(shortcuts.checkKeys("SUPER + Y").value("conflicts").toStringList().isEmpty());
        QSignalSpy messages(&shortcuts, &ShortcutsProvider::message);
        shortcuts.addBinding("SUPER + E", "Mine", "mine", false);
        QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }
    void shortcutAddIsVerifiedAndRemovable() {
        HyprDesk desk;
        Sandbox &s = desk.s;
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ShortcutsProvider shortcuts(&runner, &coordinator);
        QSignalSpy messages(&shortcuts, &ShortcutsProvider::message);
        shortcuts.addBinding("super + y", "Mine", "run \"quoted\" thing", false);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY2(!messages.first().at(1).toBool(), qPrintable(messages.first().at(0).toString()));
        const QByteArray lua = readAll(s.env.configHome + "/hypr/omarchy-control.lua");
        QVERIFY(lua.contains("o.bind(\"SUPER + Y\", \"Mine\", \"run \\\"quoted\\\" thing\")"));
        QCOMPARE(shortcuts.ownedBinds().size(), 1);
        shortcuts.removeBinding("SUPER + Y");
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 2, 5000);
        QCOMPARE(shortcuts.ownedBinds().size(), 0);
    }
    void shortcutFailureRestoresTheModule() {
        Sandbox s;
        s.minimalHyprland();
        s.command("hyprctl", "exit 0");
        const QByteArray stock = readAll(s.env.configHome + "/hypr/hyprland.lua");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ShortcutsProvider shortcuts(&runner, &coordinator);
        QSignalSpy messages(&shortcuts, &ShortcutsProvider::message);
        shortcuts.addBinding("SUPER + Y", "Mine", "mine", false);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
        QCOMPARE(readAll(s.env.configHome + "/hypr/hyprland.lua"), stock);
    }
    void pluginParsingAndManifestFallback() {
        Sandbox s;
        writeFile(s.env.omarchyPath + "/shell/plugins/a/manifest.json", R"({"id":"omarchy.a","name":"A","kinds":["panel"]})");
        writeFile(s.env.omarchyPath + "/shell/plugins/bar/x.manifest.json", R"({"id":"omarchy.x","name":"X","kinds":["bar-widget"]})");
        writeFile(s.env.configHome + "/omarchy/plugins/mine/manifest.json", R"({"id":"me.mine","name":"Mine"})");
        const auto rows = PluginsProvider::scanManifests(s.env);
        QCOMPARE(rows.size(), 3);
        for (const auto &row : rows)
            QVERIFY(row.toMap().value("unknownState").toBool());
        QVERIFY(PluginsProvider::isSafeId("omarchy.clock"));
        QVERIFY(!PluginsProvider::isSafeId("--force"));
        QVERIFY(!PluginsProvider::isSafeId("a b"));
    }
    void barLayoutPreservesInstanceData() {
        const auto layout = PluginsProvider::parseLayout(R"({"bar":{"layout":{"left":[{"id":"a"},{"id":"b","format":"x"}],"tray":[]}}})");
        QCOMPARE(layout.value("left").toList().size(), 2);
        QCOMPARE(layout.value("left").toList().at(1).toString(), QString("b"));
        QVERIFY(layout.value("center").toList().isEmpty());
    }
    void pluginToggleIsVerifiedThroughTheShell() {
        ShellDesk desk;
        Sandbox &s = desk.s;
        desk.mode("noop");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        PluginsProvider plugins(&runner, &coordinator);
        QSignalSpy messages(&plugins, &PluginsProvider::message);
        plugins.setEnabled("omarchy.service", false);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(s.calls().join('\n').contains("omarchy plugin disable omarchy.service"));
    }
    void storageParsingSkipsPseudoFilesystems() {
        const auto rows = SystemProvider::parseDf("Filesystem Type 1B Used Avail Use% Mounted\n/dev/a btrfs 100 42 58 42% /\ntmpfs tmpfs 10 1 9 10% /run\n/dev/b ext4 100 95 5 95% /my disk\n");
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[1].toMap().value("mount").toString(), QString("/my disk"));
        QCOMPARE(rows[1].toMap().value("percent").toInt(), 95);
    }
    void storageKnowsAppMountsSharedDisksAndMissingReadings() {
        const auto rows = SystemProvider::parseDf("Filesystem Type 1B-blocks Used Avail Use% Mounted on\n"
                                                  "/dev/a btrfs 1000 420 580 42% /\n"
                                                  "/dev/a btrfs 1000 420 580 42% /home\n"
                                                  "T3-Code.AppImage fuse.T3-Code.AppImage 181 181 0 100% /tmp/.mount_T3-CoAb12\n"
                                                  "//nas/share cifs - - - - /mnt/nas\n");
        QCOMPARE(rows.size(), 4);
        // The second subvolume says whose space it uses; the first does not.
        QVERIFY(!rows[0].toMap().contains("sharedWith"));
        QCOMPARE(rows[1].toMap().value("sharedWith").toString(), QString("/"));
        // An app running from an image is apart, and is not counted as a disk.
        QVERIFY(rows[2].toMap().value("temporary").toBool());
        QVERIFY(!rows[0].toMap().value("temporary").toBool());
        QVERIFY(!rows[2].toMap().contains("sharedWith"));
        // A reading that failed is unknown, not an empty disk.
        QVERIFY(rows[0].toMap().value("known").toBool());
        QVERIFY(!rows[3].toMap().value("known").toBool());
    }
    void diagnosticsAreRedacted() {
        const QString report = SystemProvider::buildDiagnostics("wifi password=hunter2\ntoken: abc", {{"shell", true}}, {{"omarchy", "4.5"}}, {});
        QVERIFY(!report.contains("hunter2"));
        QVERIFY(!report.contains("abc"));
        QVERIFY(report.contains("4.5"));
    }
    void desktopEntriesParse() {
        Sandbox s;
        writeFile(s.env.sys("/usr/share/applications/a.desktop"), "[Desktop Entry]\nType=Application\nName=Alpha\nName[da]=Alfa\nMimeType=text/plain;\n[Desktop Action x]\nName=Other\n");
        writeFile(s.env.sys("/usr/share/applications/hidden.desktop"), "[Desktop Entry]\nType=Application\nName=Hidden\nNoDisplay=true\n");
        writeFile(s.env.sys("/usr/share/applications/link.desktop"), "[Desktop Entry]\nType=Link\nName=Link\n");
        const auto rows = SystemProvider::readApps(s.env);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].toMap().value("name").toString(), QString("Alpha"));
        QVERIFY(rows[1].toMap().value("hidden").toBool());
    }

    // ---- Displays -------------------------------------------------------------
    void displayDiscoveryRejectsMalformedReadsAndAwaitsBothSources() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        auto reply = CliApi::execute(s.env, {"display", "status"});
        QVERIFY(reply.body.value("ok").toBool());
        const auto data = reply.body.value("data").toObject();
        QCOMPARE(data.value("monitors").toArray().size(), 3);
        QCOMPARE(data.value("hyprlandVersion").toString(), QString("0.56.2"));
        QVERIFY(!data.value("physicalAppearanceVerified").toBool());
        writeFile(s.dir.path() + "/hypr-monitors", "{broken");
        reply = CliApi::execute(s.env, {"display", "status"});
        QVERIFY(!reply.body.value("ok").toBool());
        QVERIFY(reply.body.value("data").toObject().value("monitors").isNull());
        QCOMPARE(reply.body.value("data").toObject().value("versionStatus").toString(), QString("ok"));
        QVariantList rows; QString error;
        QVERIFY(DisplayProvider::decodeMonitors("[]", &rows, &error));
        QVERIFY(rows.isEmpty());
        const auto valid = QJsonDocument::fromJson(twoMonitors()).array().first().toObject();
        for (const auto &field : {QString("width"), QString("x"), QString("scale"), QString("transform")}) {
            auto malformed = valid; malformed[field] = "invalid";
            QVERIFY2(!DisplayProvider::decodeMonitors(QJsonDocument(QJsonArray{malformed}).toJson(), &rows, &error), qPrintable(field));
        }
        QVERIFY(!DisplayProvider::decodeMonitors(QJsonDocument(QJsonArray{valid, valid}).toJson(), &rows, &error));
        QVERIFY(!DisplayProvider::isSafeOutputName("DP-1\n"));
    }
    void displayRecoveryResumesAnInterruptedPersistencePhase_data() {
        QTest::addColumn<QString>("stopAfter");
        QTest::newRow("module write") << "module";
        QTest::newRow("both file writes") << "both";
        QTest::newRow("later edit") << "edited";
    }
    void displayRecoveryResumesAnInterruptedPersistencePhase() {
        QFETCH(QString, stopAfter);
        DisplayDesk desk;
        const auto rows = desk.api({"display", "status"}).value("data").toObject().value("monitors").toArray();
        auto layout = rows; auto row = layout.first().toObject(); row["x"] = 40; layout[0] = row;
        HyprModule target(desk.env); QVERIFY(target.load());
        const auto original = readAll(target.hyprlandPath());
        const QString token = "dp-" + QString(32, 'a'); QString error;
        QJsonObject receipt{{"schema", 1}, {"token", token}, {"session", "sandbox"}, {"hyprland", "0.56.2"}, {"phase", "keeping"},
            {"deadline", double(QDateTime::currentMSecsSinceEpoch() + 15000)}, {"original", rows}, {"layout", layout},
            {"fingerprints", QJsonObject{{target.path(), "missing"}, {target.hyprlandPath(), FileOps::fingerprint(target.hyprlandPath())}}}};
        QVERIFY(DisplayPreview::prepare(desk.env, &error));
        QVERIFY(DisplayPreview::save(desk.env, receipt, &error));
        target.setMonitors(QJsonArray{QJsonObject{{"output", "eDP-1"}, {"mode", "1920x1200@60.00"}, {"position", "40x0"}, {"scale", 1}}});
        QByteArray import; QVERIFY(target.plannedImport(&import));
        QVERIFY2(DisplayPreview::capturePersistence(desk.env, &receipt, target.render(), import, &error), qPrintable(error));
        writeFile(target.path(), target.render());
        if (stopAfter != "module") writeFile(target.hyprlandPath(), import);
        if (stopAfter == "edited") writeFile(target.hyprlandPath(), import + "-- later edit\n");
        writeFile(desk.s.dir.path() + "/x", "40");
        // No owner remains: the separate revert process reconstructs the timer
        // plan and uses the durable before/after guards to recover partial files.
        const auto reply = desk.api({"display", "preview", "revert", token});
        QCOMPARE(reply.value("ok").toBool(), stopAfter != "edited");
        QCOMPARE(readAll(desk.s.dir.path() + "/x").trimmed(), QByteArray("0"));
        if (stopAfter == "edited") {
            QCOMPARE(readAll(target.hyprlandPath()), import + "-- later edit\n");
            QVERIFY(QFileInfo::exists(target.path()));
            QCOMPARE(DisplayPreview::load(desk.env, token).value("phase").toString(), QString("restore_failed"));
        } else {
            QCOMPARE(readAll(target.hyprlandPath()), original);
            QVERIFY(!QFileInfo::exists(target.path()));
            QCOMPARE(DisplayPreview::load(desk.env, token).value("phase").toString(), QString("reverted"));
        }
    }
    void displayLayoutPlansAreTypedAndDoNotStartRecovery() {
        DisplayDesk desk;
        const auto file = desk.layout(40);
        const auto plan = desk.api({"display", "layout", file, "--dry-run"});
        QVERIFY2(plan.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(plan).toJson())));
        QCOMPARE(plan.value("outcome").toString(), QString("planned"));
        QVERIFY(!QFile::exists(desk.s.env.appStateDir + "/history.jsonl"));
        QVERIFY(!QFileInfo::exists(desk.s.env.appStateDir + "/display-previews"));
        QVERIFY(!QFile::exists(desk.s.dir.path() + "/hypr-eval"));
        QVERIFY(!desk.api({"display", "layout", file}).value("ok").toBool());
        for (const auto &bytes : {QByteArray("[]"), QByteArray("[{\"output\":\"eDP-1\",\"enabled\":false}]"), QByteArray("[{\"output\":\"eDP-1\",\"x\":\"40\"}]"), QByteArray("[{\"output\":\"eDP-1\",\"x\":40.5}]"), QByteArray("[{\"output\":\"eDP-1\",\"unknown\":1}]"), QByteArray("[{\"output\":\"eDP-1\",\"mirrorOf\":\"bad\\\\\\\"name\"}]"), QByteArray("[{\"output\":\"eDP-1\"},{\"output\":\"eDP-1\"}]")}) {
            writeFile(file, bytes);
            QVERIFY(!desk.api({"display", "layout", file, "--dry-run"}).value("ok").toBool());
        }
        QVERIFY(!QFile::exists(desk.s.dir.path() + "/hypr-eval"));
    }
    void displayKeepAdoptsTheTokenAfterTheCallerHasExited() {
        DisplayDesk desk;
        const auto file = desk.layout(40);
        QProcess caller; caller.setProcessEnvironment(desk.env.process);
        caller.start(QCoreApplication::applicationFilePath(), {"--cli-sandbox", desk.s.dir.path(), "display", "layout", file, "--yes"});
        QVERIFY(caller.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(caller.state(), QProcess::NotRunning, 15000);
        const auto reply = QJsonDocument::fromJson(caller.readAllStandardOutput()).object();
        QVERIFY2(reply.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson()) + caller.readAllStandardError()));
        const QString token = reply.value("data").toObject().value("receipt").toObject().value("token").toString();
        QVERIFY(DisplayPreview::validToken(token));
        QCOMPARE(readAll(desk.s.dir.path() + "/x").trimmed(), QByteArray("40"));
        AppCore core(desk.env); DisplayProvider gui(&core.runner, &core.coordinator);
        gui.refresh(); QTRY_VERIFY_WITH_TIMEOUT(gui.previewing(), 5000);
        QCOMPARE(gui.previewToken(), token);
        QVERIFY(!desk.api({"display", "preview", "keep", token}).value("ok").toBool());
        const auto kept = desk.api({"display", "preview", "keep", token, "--yes"});
        QVERIFY2(kept.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(kept).toJson())));
        QCOMPARE(kept.value("outcome").toString(), QString("kept"));
        const auto receipt = DisplayPreview::load(desk.env, token);
        QCOMPARE(receipt.value("phase").toString(), QString("kept"));
        QVERIFY(!receipt.value("physicalAppearanceVerified").toBool());
        HyprModule module(desk.env); QVERIFY(module.load()); QVERIFY(module.importInstalled());
        QCOMPARE(module.monitors().first().toObject().value("position").toString(), QString("40x0"));
        QVERIFY(!desk.api({"display", "preview", "revert", token}).value("ok").toBool());
        gui.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!gui.busy(), 5000);
        QVERIFY(DisplayPreview::pendingReason(desk.env).isEmpty());
    }
    void displayPreviewExpiryAndExplicitRevertSurviveTheCaller_data() {
        QTest::addColumn<bool>("expire");
        QTest::newRow("explicit revert") << false;
        QTest::newRow("timeout after caller exits") << true;
    }
    void displayPreviewExpiryAndExplicitRevertSurviveTheCaller() {
        QFETCH(bool, expire);
        DisplayDesk desk;
        const auto reply = desk.api({"display", "layout", desk.layout(40), "--yes"});
        QVERIFY2(reply.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
        const QString token = reply.value("data").toObject().value("receipt").toObject().value("token").toString();
        AppCore other(desk.env); QString error;
        QVERIFY(!other.coordinator.acquireLease("run-aaaaaaaaaaaa", &error));
        QVERIFY(error.contains("preview"));
        const auto blocked = desk.api({"network", "disconnect", "wlan0", "--yes"});
        QCOMPARE(blocked.value("error").toObject().value("code").toString(), QString("display_preview_pending"));
        if (!expire) {
            const auto reverted = desk.api({"display", "preview", "revert", token});
            QVERIFY2(reverted.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(reverted).toJson())));
        }
        QTRY_COMPARE_WITH_TIMEOUT(DisplayPreview::load(desk.env, token).value("phase").toString(), QString("reverted"), 22000);
        QCOMPARE(readAll(desk.s.dir.path() + "/x").trimmed(), QByteArray("0"));
        QVERIFY(DisplayPreview::load(desk.env, token).value("restoreVerified").toBool());
        QVERIFY(!QFileInfo::exists(HyprModule(desk.env).path()));
        QVERIFY(!desk.api({"display", "preview", "keep", token, "--yes"}).value("ok").toBool());
        QVERIFY(DisplayPreview::pendingReason(desk.env).isEmpty());
    }
    void displayKeepPreservesHandEditsAndReportsTheRestoredPreview() {
        DisplayDesk desk;
        const auto reply = desk.api({"display", "layout", desk.layout(40), "--yes"});
        QVERIFY(reply.value("ok").toBool());
        const QString token = reply.value("data").toObject().value("receipt").toObject().value("token").toString();
        const QString config = HyprModule(desk.env).hyprlandPath();
        const auto edited = readAll(config) + "-- later hand edit\n"; writeFile(config, edited);
        const auto kept = desk.api({"display", "preview", "keep", token, "--yes"});
        QVERIFY(!kept.value("ok").toBool());
        QCOMPARE(DisplayPreview::load(desk.env, token).value("phase").toString(), QString("reverted"));
        QCOMPARE(readAll(config), edited);
        QCOMPARE(readAll(desk.s.dir.path() + "/x").trimmed(), QByteArray("0"));
        QVERIFY(!QFileInfo::exists(HyprModule(desk.env).path()));
    }
    void aDisplaysCurrentModeIsAlwaysValid() {
        // Found on a real nested output, which lists no modes at all.
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/hypr-monitors", R"([{"name":"WAYLAND-1","width":1280,"height":720,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"disabled":false,"availableModes":[]}])");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 1, 5000);
        QVERIFY(displays.validateLayout(displays.monitors()).isEmpty());
    }
    void textSizeFallsBackToTheStockDefault() {
        Sandbox s;
        AppCore core(s.env);
        const ReadResult result = readSetting(core.engine, "appearance.textsize");
        QCOMPARE(result.status, QString("ok"));
        QCOMPARE(result.value, QString("12"));
    }
    void cleanScalesMatchHyprlandRules() {
        QCOMPARE(DisplayProvider::cleanScale(1.25, 2880, 1800), 1.25);
        QCOMPARE(DisplayProvider::cleanScale(1, 1920, 1080), 1.0);
        const double rounded = DisplayProvider::cleanScale(1.3, 1920, 1080);
        QVERIFY(rounded >= 1.3);
        QCOMPARE(fmod(1920 * 120 / (rounded * 120), 1.0), 0.0);
    }
    void monitorLuaRefusesNothingUnsafeReaches() {
        QVERIFY(DisplayProvider::isSafeOutputName("DP-1"));
        QVERIFY(!DisplayProvider::isSafeOutputName("DP\"); os.execute(\"x"));
        QCOMPARE(DisplayProvider::monitorLua(QJsonObject{{"output", "DP-1"}, {"mode", "1920x1080@60.00"}, {"position", "0x0"}, {"scale", 1.5}, {"transform", 0}}),
                 QString("hl.monitor({ output = \"DP-1\", mode = \"1920x1080@60.00\", position = \"0x0\", scale = 1.5 })"));
        QVERIFY(DisplayProvider::monitorLua(QJsonObject{{"output", "DP-1"}, {"disabled", true}}).contains("disabled = true"));
    }
    void monitorParsing() {
        const auto rows = DisplayProvider::parseMonitors(R"([{"name":"eDP-1","width":2880,"height":1800,"refreshRate":120.001,"x":0,"y":0,"scale":1.25,"transform":0,"disabled":false,"availableModes":["2880x1800@120.00Hz"]}])");
        QCOMPARE(rows.size(), 1);
        const auto m = rows.first().toMap();
        QCOMPARE(m.value("mode").toString(), QString("2880x1800@120.00"));
        QCOMPARE(m.value("modes").toStringList(), QStringList{"2880x1800@120.00"});
        QVERIFY(m.value("internal").toBool());
    }
    void layoutValidationKeepsOneDisplayOn() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        auto layout = displays.monitors();
        QVERIFY(displays.validateLayout(layout).isEmpty());
        auto off = layout;
        for (auto &row : off) {
            auto m = row.toMap();
            m["enabled"] = false;
            row = m;
        }
        QVERIFY(displays.validateLayout(off).contains("At least one"));
        auto badMode = layout;
        auto first = badMode[0].toMap();
        first["mode"] = "9999x9999@1.00";
        badMode[0] = first;
        QVERIFY(displays.validateLayout(badMode).contains("does not offer"));
        auto gone = layout;
        auto ghost = gone[0].toMap();
        ghost["output"] = "HDMI-9";
        gone[0] = ghost;
        QVERIFY(displays.validateLayout(gone).contains("no longer connected"));
    }
    void editingStaysLockedWithoutARecoveryCheck() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        QVERIFY(!displays.editingUnlocked());
        QSignalSpy messages(&displays, &DisplayProvider::message);
        displays.preview(displays.monitors());
        QCOMPARE(messages.count(), 1);
        QVERIFY(!QFile::exists(s.dir.path() + "/hypr-eval"));
        // A pass recorded for another Hyprland version does not carry over.
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.1.0"})");
        QVERIFY(!displays.editingUnlocked());
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
    }
    void watchdogRestoresOnTimeout() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        const QString plan = watchdogPlan(s, 1);
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        QCOMPARE(readAll(s.dir.path() + "/result").trimmed(), QByteArray("restored"));
        const QByteArray evals = readAll(s.dir.path() + "/hypr-eval");
        QVERIFY(evals.contains("output = \"eDP-1\""));
        QVERIFY(evals.contains("output = \"DP-1\""));
        // Enabled outputs come back first, so there is never a moment with none.
        QVERIFY(evals.indexOf("eDP-1") < evals.indexOf("DP-2"));
        QVERIFY(evals.contains("DP-2\", disabled = true"));
    }
    void watchdogHonorsKeep() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        const QString plan = watchdogPlan(s, 30);
        writeFile(s.dir.path() + "/keep", "");
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        QCOMPARE(readAll(s.dir.path() + "/result").trimmed(), QByteArray("kept"));
        QVERIFY(!QFile::exists(s.dir.path() + "/hypr-eval"));
    }
    void watchdogRestoresOnRequest() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        const QString plan = watchdogPlan(s, 30);
        writeFile(s.dir.path() + "/revert", "");
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        QVERIFY(timer.elapsed() < 5000);
        QCOMPARE(readAll(s.dir.path() + "/result").trimmed(), QByteArray("restored"));
    }
    void watchdogSkipsUnpluggedOutputs() {
        Sandbox s;
        // DP-1 went away during the preview.
        monitorFixture(s, R"([{"name":"eDP-1","width":1,"height":1,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"availableModes":[]},{"name":"DP-2","disabled":true,"width":1,"height":1,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"availableModes":[]}])");
        const QString plan = watchdogPlan(s, 0);
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        const QByteArray evals = readAll(s.dir.path() + "/hypr-eval");
        QVERIFY(evals.contains("eDP-1"));
        QVERIFY(!evals.contains("\"DP-1\""));
    }
    void watchdogNeverLeavesEveryOutputOff() {
        Sandbox s;
        // Every output it remembered is gone; whatever is left must come on.
        monitorFixture(s, R"([{"name":"HDMI-7","width":1,"height":1,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"availableModes":[]}])");
        const QString plan = watchdogPlan(s, 0);
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        const QByteArray evals = readAll(s.dir.path() + "/hypr-eval");
        QVERIFY(evals.contains("HDMI-7"));
        QVERIFY(evals.contains("preferred"));
    }
    void watchdogReportsARefusedRestore() {
        Sandbox s;
        s.command("hyprctl", kScript14);
        const QString plan = watchdogPlan(s, 0);
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        QCOMPARE(readAll(s.dir.path() + "/result").trimmed(), QByteArray("restore-failed"));
    }
    void watchdogSurvivesTheWindowDying() {
        // The real scenario: start the watchdog as its own process, kill the parent
        // that would have kept it, and confirm the layout still comes back.
        Sandbox s;
        monitorFixture(s, twoMonitors());
        const QString plan = watchdogPlan(s, 1);
        QProcess parent;
        parent.setProcessEnvironment(s.env.process);
        // setsid gives the watchdog its own session, as startDetached does in the app.
        parent.start("bash", {"-c", QString("setsid '%1' --watchdog '%2' '%3' >/dev/null 2>&1 & sleep 30").arg(QCoreApplication::applicationFilePath(), s.dir.path(), plan)});
        QVERIFY(parent.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(readAll(s.dir.path() + "/result").contains("armed"), 8000);
        parent.kill();
        parent.waitForFinished(2000);
        QTRY_VERIFY_WITH_TIMEOUT(readAll(s.dir.path() + "/result").contains("restored"), 8000);
        QVERIFY(readAll(s.dir.path() + "/hypr-eval").contains("eDP-1"));
    }
    void previewArmsRecoveryBeforeTheFirstChange() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        // A self binary that cannot run means recovery cannot be armed.
        Env broken = s.env;
        broken.selfBinary = "/nonexistent/omarchy-control";
        CommandRunner brokenRunner(broken);
        DisplayProvider displays(&brokenRunner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!displays.status().isEmpty(), 1000);
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
        QSignalSpy messages(&displays, &DisplayProvider::message);
        displays.preview(displays.monitors());
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 8000);
        QVERIFY(messages.first().at(0).toString().contains("nothing was changed"));
        QVERIFY(!QFile::exists(s.dir.path() + "/hypr-eval"));
    }

    // ---- Displays: selection, staging, and what the page is told -------------
    void theSelectionIsKeptAndFallsBackWhenUnplugged() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        QSignalSpy messages(&displays, &DisplayProvider::message);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        // It starts on the display the desktop has focused, then stays wherever it is put.
        QCOMPARE(displays.selected(), QString("eDP-1"));
        displays.select("DP-1");
        QCOMPARE(displays.selected(), QString("DP-1"));
        displays.select("HDMI-9");
        QCOMPARE(displays.selected(), QString("DP-1"));
        // Off displays stay selectable, so they can be turned back on.
        displays.select("DP-2");
        QCOMPARE(displays.selected(), QString("DP-2"));
        displays.select("DP-1");
        QSignalSpy changed(&displays, &DisplayProvider::changed);
        displays.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 5000);
        QCOMPARE(displays.selected(), QString("DP-1"));
        writeFile(s.dir.path() + "/hypr-monitors", withoutMonitor(twoMonitors(), "DP-1"));
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 2, 5000);
        QCOMPARE(displays.selected(), QString("eDP-1"));
        QVERIFY(!messages.isEmpty());
        QVERIFY(messages.last().at(0).toString().contains("DP-1"));
    }
    void stagedEditsSurviveRefreshesAndLeaveOnlyWithTheirDisplay() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        // Locked: nothing can be staged before the recovery check has passed.
        displays.stageEdit("DP-1", "x", 2000);
        QVERIFY(!displays.dirty());
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
        displays.stageEdit("DP-1", "x", 2000);
        displays.stageEdit("eDP-1", "y", 50);
        QVERIFY(displays.dirty());
        QCOMPARE(stagedRow(displays, "DP-1").value("pending").toStringList(), QStringList{"x"});
        // Fresh state from Hyprland does not wipe what is being worked on.
        QSignalSpy changed(&displays, &DisplayProvider::changed);
        displays.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 5000);
        QVERIFY(displays.dirty());
        QCOMPARE(stagedRow(displays, "DP-1").value("x").toInt(), 2000);
        // Moving it back to where it is leaves nothing pending for it.
        displays.stagePosition("DP-1", 1920, 0);
        QVERIFY(stagedRow(displays, "DP-1").value("pending").toStringList().isEmpty());
        QVERIFY(displays.dirty());
        displays.stageEdit("DP-1", "x", 2000);
        // Unplugged: its edits go, the others stay, and the page is told.
        writeFile(s.dir.path() + "/hypr-monitors", withoutMonitor(twoMonitors(), "DP-1"));
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 2, 5000);
        QVERIFY(stagedRow(displays, "DP-1").isEmpty());
        QVERIFY(displays.dirty());
        QCOMPARE(stagedRow(displays, "eDP-1").value("y").toInt(), 50);
        QCOMPARE(displays.outcome().value("kind").toString(), QString("warning"));
        QVERIFY(displays.outcome().value("title").toString().contains("DP-1"));
        displays.discardStaged();
        QVERIFY(!displays.dirty());
        QCOMPARE(stagedRow(displays, "eDP-1").value("y").toInt(), 0);
    }
    void stagedScalesAreOnesHyprlandCanShow() {
        for (const auto &choice : DisplayProvider::scaleChoices(2560, 1440, 1.6)) {
            const int k = qRound(choice.toMap().value("value").toDouble() * 120);
            QCOMPARE((2560 * 120) % k, 0);
            QCOMPARE((1440 * 120) % k, 0);
        }
        Sandbox s;
        monitorFixture(s, R"([{"name":"DP-1","width":2560,"height":1440,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"disabled":false,"focused":true,"availableModes":["2560x1440@60.00Hz","1920x1080@60.00Hz"]}])");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 1, 5000);
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
        // 150% does not divide 2560x1440 into whole pixels; the nearest that does is staged.
        displays.stageEdit("DP-1", "scale", 1.5);
        QCOMPARE(stagedRow(displays, "DP-1").value("scale").toDouble(), 1.6);
        QCOMPARE(stagedRow(displays, "DP-1").value("logicalWidth").toInt(), 1600);
        QCOMPARE(stagedRow(displays, "DP-1").value("logicalHeight").toInt(), 900);
        // A new mode carries the scale along, and the workspace follows both.
        displays.stageEdit("DP-1", "mode", "1920x1080@60.00");
        QCOMPARE(stagedRow(displays, "DP-1").value("width").toInt(), 1920);
        QCOMPARE(stagedRow(displays, "DP-1").value("logicalWidth").toInt(), 1200);
        // Rotated, the workspace stands on its side.
        displays.stageEdit("DP-1", "transform", 1);
        QCOMPARE(stagedRow(displays, "DP-1").value("logicalWidth").toInt(), 675);
        QVERIFY(displays.stageProblem().isEmpty());
    }
    void oneAnswerSaysWhereTheSelectedScaleGoes() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!displays.selectedScale().isEmpty(), 3000);
        // The focused display changes right away, through Omarchy's own command.
        QCOMPARE(displays.selectedScale().value("route").toString(), QString("immediate"));
        // Another display only through the arrangement, which is locked for now.
        displays.select("DP-1");
        QCOMPARE(displays.selectedScale().value("route").toString(), QString("locked"));
        QVERIFY(displays.selectedScale().value("reason").toString().contains("DP-1"));
        displays.select("DP-2");
        QCOMPARE(displays.selectedScale().value("route").toString(), QString("off"));
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
        displays.select("DP-1");
        QCOMPARE(displays.selectedScale().value("route").toString(), QString("stage"));
        displays.stageEdit("DP-1", "scale", 2);
        QVERIFY(displays.selectedScale().value("pending").toBool());
        QCOMPARE(displays.selectedScale().value("applied").toDouble(), 1.0);
        QVERIFY(displays.selectedScale().value("reason").toString().contains("100%"));
        // With a layout staged, even the focused display's scale joins it.
        displays.select("eDP-1");
        QCOMPARE(displays.selectedScale().value("route").toString(), QString("stage"));
        displays.discardStaged();
        QCOMPARE(displays.selectedScale().value("route").toString(), QString("immediate"));
    }
    void aRefusedPreviewKeepsWhatWasStaged() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        ChangeCoordinator coordinator(s.env);
        Env broken = s.env;
        broken.selfBinary = "/nonexistent/omarchy-control";
        CommandRunner runner(broken);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().size(), 3, 5000);
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
        displays.stageEdit("DP-1", "x", 2400);
        displays.previewStaged();
        QTRY_COMPARE_WITH_TIMEOUT(displays.outcome().value("kind").toString(), QString("error"), 8000);
        QVERIFY(displays.outcome().value("detail").toString().contains("nothing was changed"));
        QVERIFY(displays.dirty());
        QCOMPARE(stagedRow(displays, "DP-1").value("x").toInt(), 2400);
        QVERIFY(!displays.busy());
        QVERIFY(!QFile::exists(s.dir.path() + "/hypr-eval"));
    }
    void aPreviewThatOutlivedTheWindowIsReported() {
        Sandbox s;
        monitorFixture(s, twoMonitors());
        const QString run = s.env.runtimeDir + "/omarchy-control";
        writeFile(run + "/display-0ld0ne00.result", "restored\n");
        // Still owned by a watchdog that is running: not this window's to report.
        writeFile(run + "/display-l1ve0000.result", "armed\n");
        writeFile(run + "/display-l1ve0000.json", "{}");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        DisplayProvider displays(&runner, &coordinator);
        QCOMPARE(displays.outcome().value("kind").toString(), QString("info"));
        QVERIFY(displays.outcome().value("detail").toString().contains("previous layout"));
        QVERIFY(!QFile::exists(run + "/display-0ld0ne00.result"));
        QVERIFY(QFile::exists(run + "/display-l1ve0000.result"));
        displays.dismissOutcome();
        QVERIFY(displays.outcome().isEmpty());
    }
    void brightnessCanTargetOneDisplay() {
        Sandbox s;
        s.minimalHyprland();
        s.omarchy(R"SH(
root="$(dirname "$HOME")"
case "$*" in
  "brightness display --monitor DP-2"*) exit 1;;
  "brightness display --monitor DP-1 --no-osd "*) v="$6"; echo "${v%\%}" > "$root/b";;
  "brightness display --monitor DP-1") cat "$root/b" 2>/dev/null || echo 40;;
  "brightness display") echo 70;;
esac
)SH");
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "displays.brightness").value, QString("70"));
        QCOMPARE(readSetting(core.engine, "displays.brightness@DP-1").value, QString("40"));
        // A display with neither a backlight nor DDC/CI has no such control, and says so.
        const ReadResult none = readSetting(core.engine, "displays.brightness@DP-2");
        QCOMPARE(none.status, QString("unavailable"));
        QVERIFY(none.reason.contains("DP-2"));
        // Only a plain connector name can be a target, and only of a setting that has one.
        QVERIFY(!core.catalog.find("displays.brightness@DP-1;reboot"));
        QVERIFY(!core.catalog.find("displays.brightness@"));
        QVERIFY(!core.catalog.find("displays.nightlight@DP-1"));
        const Transaction tx = applySetting(core.engine, "displays.brightness@DP-1", "55");
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(s.calls().contains("omarchy brightness display --monitor DP-1 --no-osd 55%"));
        QCOMPARE(readSetting(core.engine, "displays.brightness@DP-1").value, QString("55"));
        // Undo goes back to the same display, not whichever one has focus.
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "displays.brightness@DP-1").value, QString("40"));
    }
    void displayToolsFollowTheHardware() {
        Sandbox s;
        AppCore core(s.env);
        auto reason = [&](const QString &id) {
            core.capabilities.scanFiles();
            const auto a = core.capabilities.availability(*core.catalog.find(id));
            return a.available ? QString() : a.reason;
        };
        QVERIFY(reason("monitor.recover").contains("built-in"));
        writeFile(s.env.sys("/sys/class/drm/card1-eDP-1/status"), "connected\n");
        // Built in and enabled: there is nothing to recover, and nothing to switch off to.
        QVERIFY(reason("monitor.recover").contains("already enabled"));
        QVERIFY(reason("trigger.hardware.laptop-display").contains("external"));
        writeFile(s.env.stateHome + "/omarchy/toggles/hypr/internal-monitor-disable.lua", "");
        QVERIFY(reason("monitor.recover").isEmpty());
        writeFile(s.env.sys("/sys/class/drm/card1-DP-1/status"), "connected\n");
        // With an external display connected, Omarchy's recovery does nothing.
        QVERIFY(reason("monitor.recover").contains("external display is connected"));
        QVERIFY(reason("trigger.hardware.laptop-display").isEmpty());
        QVERIFY(reason("trigger.hardware.mirror-display").isEmpty());
        QCOMPARE(core.capabilities.values().value("internaldisplay.name").toString(), QString("eDP-1"));
    }

    // ---- Regressions from the adversarial review ------------------------------
    void unreadableFilesStopTheChangeAndSurvive() {
        if (geteuid() == 0)
            QSKIP("root can read everything");
        Sandbox s;
        const QString file = s.env.configHome + "/locked.json";
        writeFile(file, "precious");
        QFile::setPermissions(file, QFile::Permissions());
        QCOMPARE(FileOps::fingerprint(file), QString("unreadable"));
        ChangeCoordinator coordinator(s.env);
        bool applied = false;
        ChangeCoordinator::Request request;
        request.files << file;
        request.apply = [&](ChangeCoordinator::Done done) { applied = true; done(false, "x"); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("failed"));
        QVERIFY(!applied);
        QVERIFY(QFile::exists(file));
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
        QCOMPARE(readAll(file), QByteArray("precious"));
    }
    void noBackupMeansNoChange() {
        if (geteuid() == 0)
            QSKIP("root ignores directory permissions");
        Sandbox s;
        const QString file = s.env.configHome + "/c.json";
        writeFile(file, "original");
        ChangeCoordinator coordinator(s.env);
        QFile::setPermissions(coordinator.backupDirectory(), QFile::ReadOwner | QFile::ExeOwner);
        bool applied = false;
        ChangeCoordinator::Request request;
        request.files << file;
        request.apply = [&](ChangeCoordinator::Done done) { applied = true; done(true, {}); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("failed"));
        QFile::setPermissions(coordinator.backupDirectory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(!applied);
        QVERIFY(result.message.contains("backup"));
    }
    void failedVerifyKeepsAnEditMadeMeanwhile() {
        Sandbox s;
        const QString file = s.env.configHome + "/c.json";
        writeFile(file, "original");
        ChangeCoordinator coordinator(s.env);
        ChangeCoordinator::Request request;
        request.files << file;
        request.apply = [file](ChangeCoordinator::Done done) { done(FileOps::atomicWrite(file, "ours"), {}); };
        request.verify = [file](ChangeCoordinator::Done done) {
            FileOps::atomicWrite(file, "someone else's edit");
            done(false, "did not take");
        };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("partial"));
        QCOMPARE(readAll(file), QByteArray("someone else's edit"));
        QVERIFY(result.message.contains("edited"));
    }
    void appendingAfterATruncatedLineKeepsTheRecord() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "stuck";
        tx.label = "X";
        tx.phase = "applying";
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n{\"id\":\"bad\",\"pha");
        QCOMPARE(coordinator.recoverInterrupted().size(), 1);
        QCOMPARE(coordinator.transaction("stuck").phase, QString("interrupted"));
        // Resolved means resolved: a later launch must not recover it a second time.
        QVERIFY(coordinator.recoverInterrupted().isEmpty());
    }
    void pruningKeepsAnUnfinishedChangesBackup() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        QByteArray journal;
        Transaction old;
        old.id = "stuck";
        old.phase = "applying";
        journal += QJsonDocument(old.toJson()).toJson(QJsonDocument::Compact) + "\n";
        for (int i = 0; i < 40; ++i) {
            Transaction tx;
            tx.id = QString("done%1").arg(i);
            tx.phase = "committed";
            journal += QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n";
        }
        writeFile(coordinator.journalPath(), journal);
        writeFile(coordinator.backupDirectory() + "/stuck/0", "x");
        writeFile(coordinator.backupDirectory() + "/done0/0", "x");
        ChangeCoordinator::Request request;
        request.apply = [](ChangeCoordinator::Done done) { done(true, {}); };
        request.verify = [](ChangeCoordinator::Done done) { done(true, {}); };
        bool finished = false;
        coordinator.submit(request, [&](const Transaction &) { finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(QFile::exists(coordinator.backupDirectory() + "/stuck/0"));
        QVERIFY(!QFile::exists(coordinator.backupDirectory() + "/done0/0"));
    }
    void lockRefusalDoesNotPrune() {
        Sandbox s;
        ChangeCoordinator coordinator(s.env);
        writeFile(coordinator.backupDirectory() + "/in-progress-elsewhere/0", "x");
        QLockFile other(s.env.appStateDir + "/change.lock");
        QVERIFY(other.tryLock(0));
        ChangeCoordinator::Request request;
        request.apply = [](ChangeCoordinator::Done done) { done(true, {}); };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE_WITH_TIMEOUT(result.phase, QString("failed"), 5000);
        QVERIFY(QFile::exists(coordinator.backupDirectory() + "/in-progress-elsewhere/0"));
    }
    void wifiBandReadsTheSelectedField() {
        Sandbox s;
        writeFile(s.env.sys("/sys/class/net/wlan0/wireless/x"), "");
        s.omarchy("printf 'band\\t5\\navailable\\t5\\nselected\\tauto\\n'");
        AppCore core(s.env);
        const ReadResult result = readSetting(core.engine, "network.band");
        QCOMPARE(result.status, QString("ok"));
        QCOMPARE(result.value, QString("auto"));
    }
    void toggleRollbackLooksBeforeItFlips() {
        // The command fails before toggling. Rollback must not then toggle it, which
        // would apply the very change that was just reported as failed.
        Sandbox s;
        s.omarchy("exit 1");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "power.screensaver", "false");
        QVERIFY(tx.phase != QLatin1String("committed"));
        QCOMPARE(s.calls().filter("omarchy toggle screensaver").size(), 1);
        QVERIFY(!QFile::exists(s.env.stateHome + "/omarchy/toggles/screensaver-off"));
    }
    void rollbackThatDoesNotWorkIsNotCalledRestored() {
        // Setter claims success both ways and changes nothing back.
        Sandbox s;
        s.omarchy("f=\"$HOME/val\"; case \"$*\" in 'hyprland corners') cat \"$f\" 2>/dev/null || echo theme;; 'hyprland corners sharp') echo rounded > \"$f\";; esac");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.corners", "sharp");
        QCOMPARE(tx.phase, QString("partial"));
        QVERIFY(!tx.message.contains("was restored"));
    }
    void uninstalledAppsAreRefusedEverywhere() {
        // Includes the CLI and undo, which never see the filtered list.
        Sandbox s;
        s.omarchy("exit 0");
        AppCore core(s.env);
        const ChangePlan plan = core.engine.plan("apps.default.browser", "firefox");
        QVERIFY(!plan.ok);
        QVERIFY(plan.error.contains("not installed"));
        QVERIFY(s.calls().isEmpty());
    }
    void undoOfAResetRespectsLaterEdits() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{\"idle\":{\"lock\":600}}");
        writeFile(s.env.omarchyPath + "/config/omarchy/shell.json", "{\"idle\":{\"lock\":300}}");
        AppCore core(s.env);
        Transaction reset;
        bool done = false;
        core.engine.reset("power.idle.lock", [&](const Transaction &t) { reset = t; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QCOMPARE(reset.phase, QString("committed"));
        QCOMPARE(reset.after, QString("300"));
        writeFile(config, "{\"idle\":{\"lock\":900}}");
        const Transaction undone = undoTx(core.engine, reset.id);
        QCOMPARE(undone.phase, QString("failed"));
        QVERIFY(readAll(config).contains("900"));
    }
    void aResetThatCannotBeConfirmedIsNotSuccess() {
        // The override comes out, but Hyprland then cannot say what is in effect.
        Sandbox s;
        s.minimalHyprland();
        HyprModule module(s.env);
        module.setOption({"input", "repeat_rate"}, 31);
        QVERIFY(module.save());
        QVERIFY(module.installImport());
        const QByteArray before = readAll(module.path());
        s.command("hyprctl", kReloadsButCannotAnswer);
        AppCore core(s.env);
        Transaction reset;
        bool done = false;
        core.engine.reset("input.repeat_rate", [&](const Transaction &t) { reset = t; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
        QCOMPARE(reset.phase, QString("rolled-back"));
        QCOMPARE(readAll(module.path()), before);
    }
    void weatherDoesNotPromiseAnUndoItCannotKeep() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString file = s.env.stateHome + "/omarchy/settings/weather.json";
        writeFile(file, "{\"name\":\"Malibu\",\"latitude\":34,\"longitude\":-118}");
        s.omarchy("printf '{\"name\":\"%s\"}' \"$4\" > \"$HOME/.local/state/omarchy/settings/weather.json\"");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "system.weather", "Oslo");
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(!tx.undoable());
    }
    void anUnrestorableValueIsNotOfferedForUndo() {
        Sandbox s;
        s.omarchy("f=\"$HOME/dns\"; case \"$*\" in dns) cat \"$f\" 2>/dev/null || echo Custom;; 'dns '*) echo \"$2\" > \"$f\";; esac");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "network.dns", "Cloudflare");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(tx.before, QString("Custom"));
        QVERIFY(!tx.undoable());
    }
    void scaleRecordsWhatHyprlandSettledOn() {
        Sandbox s;
        s.command("hyprctl", "echo '{}'");
        s.omarchy("f=\"$HOME/scale\"; case \"$*\" in 'hyprland monitor scaling') cat \"$f\" 2>/dev/null || echo 1;; 'hyprland monitor scaling 1') echo 1 > \"$f\";; 'hyprland monitor scaling '*) echo 1.33333 > \"$f\";; esac");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "displays.scale", "1.25");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(tx.after, QString("1.33333"));
        QVERIFY(tx.undoable());
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
    }
    void aScaleThatDoesNotChangeIsNotSuccess() {
        Sandbox s;
        s.command("hyprctl", "echo '{}'");
        s.omarchy("case \"$*\" in 'hyprland monitor scaling') echo 1;; esac");
        AppCore core(s.env);
        QVERIFY(applySetting(core.engine, "displays.scale", "2").phase != QLatin1String("committed"));
    }
    void aSavedLayoutBlocksTheScaleSetting() {
        Sandbox s;
        s.minimalHyprland();
        HyprModule module(s.env);
        module.setMonitors(QJsonArray{QJsonObject{{"output", "DP-1"}, {"mode", "1x1@60.00"}, {"position", "0x0"}, {"scale", 1}}});
        QVERIFY(module.save());
        AppCore core(s.env);
        const ChangePlan plan = core.engine.plan("displays.scale", "2");
        QVERIFY(!plan.ok);
        QVERIFY(plan.error.contains("Arrangement"));
    }
    void resettingAnUnsetOptionLeavesTheConfigAlone() {
        Sandbox s;
        s.minimalHyprland();
        const QByteArray stock = readAll(s.env.configHome + "/hypr/hyprland.lua");
        AppCore core(s.env);
        core.capabilities.scanServicesBlocking();
        Transaction reset;
        bool done = false;
        core.engine.reset("input.repeat_rate", [&](const Transaction &t) { reset = t; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QCOMPARE(reset.phase, QString("failed"));
        QCOMPARE(readAll(s.env.configHome + "/hypr/hyprland.lua"), stock);
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }
    void typedTextIsNotExpandedAsAPath() {
        Sandbox s;
        s.omarchy("echo \"$*\" >> \"$HOME/args\"");
        AppCore core(s.env);
        applySetting(core.engine, "system.weather", "~/{config}");
        QVERIFY(readAll(s.env.home + "/args").contains("~/{config}"));
    }
    void jsonSetKeepsACommentOnTheBraceLine() {
        QByteArray doc = "{ /* keep */ }";
        QVERIFY(FileOps::jsonSet(&doc, {"a"}, 1));
        QVERIFY(doc.contains("/* keep */"));
        QCOMPARE(FileOps::jsonGet(doc, {"a"}).toInt(), 1);
    }
    void trailingCommaRemovalLeavesStringsAlone() {
        const QByteArray doc = "{\"a\": \"x,}\", \"b\": [1, 2, ], }";
        QCOMPARE(FileOps::jsonGet(doc, {"a"}).toString(), QString("x,}"));
        QCOMPARE(FileOps::jsonGet(doc, {"b"}).toArray().size(), 2);
    }
    void barEntriesCanBeBareIds() {
        const auto layout = PluginsProvider::parseLayout(R"({"bar":{"layout":{"left":["omarchy.menu",{"id":"omarchy.clock"}]}}})");
        QCOMPARE(layout.value("left").toList().at(0).toString(), QString("omarchy.menu"));
        QCOMPARE(layout.value("left").toList().at(1).toString(), QString("omarchy.clock"));
    }
    void dockPinsFallBackToTheDefaults() {
        Sandbox s;
        s.omarchy("exit 1");
        writeFile(s.env.configHome + "/omarchy/shell.json", "{\"version\":1}");
        writeFile(s.env.omarchyPath + "/config/omarchy/shell.json", "{\"dock\":{\"pinned\":[\"firefox\"]}}");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        PluginsProvider plugins(&runner, &coordinator);
        plugins.refresh();
        QCOMPARE(plugins.dockPins(), QStringList{"firefox"});
    }
    void aWidgetMoveToleratesASlowWrite() {
        Sandbox s;
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{\"version\":1,\"bar\":{\"layout\":{\"left\":[{\"id\":\"w\"}],\"right\":[]}}}");
        s.omarchy("case \"$1\" in bar) (sleep 0.4; printf '{\"version\":1,\"bar\":{\"layout\":{\"left\":[],\"right\":[{\"id\":\"w\"}]}}}' > \"$HOME/.config/omarchy/shell.json\") & ;; plugin) exit 1;; esac");
        s.command("omarchy-shell", "case \"$2\" in listShellConfig) cat \"$HOME/.config/omarchy/shell.json\";; esac");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        PluginsProvider plugins(&runner, &coordinator);
        QSignalSpy messages(&plugins, &PluginsProvider::message);
        plugins.moveWidget("w", "right", 0, "left", 0);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 8000);
        QVERIFY2(!messages.first().at(1).toBool(), qPrintable(messages.first().at(0).toString()));
    }
    void autostartCanComeBackOn() {
        const QByteArray entry = ServicesProvider::withHidden("[Desktop Entry]\nName=A\nX-GNOME-Autostart-enabled=false\nHidden=true\n", false);
        QVERIFY(entry.contains("Hidden=false"));
        QVERIFY(entry.contains("X-GNOME-Autostart-enabled=true"));
        QVERIFY(ServicesProvider::withHidden("[Desktop Entry]\nName=A\n", true).contains("Hidden=true"));
    }
    void aOneShotServiceThatRanIsAStart() {
        ServiceDesk desk;
        desk.set("user", "Type", "oneshot");
        ChangeCoordinator coordinator(desk.s.env);
        CommandRunner runner(desk.s.env);
        ServicesProvider services(&runner, &coordinator);
        QSignalSpy messages(&services, &ServicesProvider::message);
        services.serviceAction("agent.service", "start", false);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(!messages.first().at(1).toBool());
    }
    void aFailedStatusQueryIsNotProofOfStopped() {
        Sandbox s;
        s.command("systemctl", "case \"$*\" in *show*) exit 1;; *list-*) echo '[]';; esac");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ServicesProvider services(&runner, &coordinator);
        QSignalSpy messages(&services, &ServicesProvider::message);
        services.serviceAction("foo.service", "stop", false);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
    }
    void networkRefusesAnOverlappingSwitch() {
        Sandbox s;
        s.command("busctl", "case \"$*\" in *CheckpointCreate*) echo 'o \"/org/freedesktop/NetworkManager/Checkpoint/7\"';; esac");
        s.command("nmcli", kNmcliBringsUpCafe);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(network.awaitingKeep(), 5000);
        QSignalSpy messages(&network, &NetworkProvider::message);
        network.activate("22222222-2222-2222-2222-222222222222", "Other");
        QCOMPARE(messages.count(), 1);
        QVERIFY(messages.first().at(1).toBool());
        QCOMPARE(s.calls().filter("CheckpointCreate").size(), 1);
    }
    void keepCountdownMatchesTheRealRollbackTimer() {
        // The timer starts when the checkpoint does, not when the connection comes up.
        Sandbox s;
        s.command("busctl", "case \"$*\" in *CheckpointCreate*) echo 'o \"/org/freedesktop/NetworkManager/Checkpoint/7\"';; esac");
        s.command("nmcli", kNmcliBringsUpCafe);
        writeFile(s.dir.path() + "/slow", "");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(network.awaitingKeep(), 8000);
        QVERIFY2(network.keepSeconds() <= 58, qPrintable(QString::number(network.keepSeconds())));
    }
    void aFailedKeepIsNotReportedAsKept() {
        Sandbox s;
        s.command("busctl", "case \"$*\" in *CheckpointCreate*) echo 'o \"/org/freedesktop/NetworkManager/Checkpoint/7\"';; *CheckpointDestroy*) exit 1;; esac");
        s.command("nmcli", kNmcliBringsUpCafe);
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        NetworkProvider network(&runner, &coordinator);
        network.activate("11111111-1111-1111-1111-111111111111", "Cafe");
        QTRY_VERIFY_WITH_TIMEOUT(network.awaitingKeep(), 5000);
        QSignalSpy messages(&network, &NetworkProvider::message);
        network.keepConnection();
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
    }
    void mirroredDisplaysComeBackMirrored() {
        const auto rows = DisplayProvider::parseMonitors(R"([{"name":"DP-1","width":1,"height":1,"refreshRate":60,"mirrorOf":"eDP-1","availableModes":[]}])");
        QCOMPARE(rows.first().toMap().value("mirrorOf").toString(), QString("eDP-1"));
        QVERIFY(DisplayProvider::monitorLua(QJsonObject{{"output", "DP-1"}, {"mode", "m"}, {"position", "0x0"}, {"scale", 1}, {"mirror", "eDP-1"}}).contains("mirror = \"eDP-1\""));
    }
    void recoveryCheckEndToEnd_data() {
        QTest::addColumn<bool>("compositorObeys");
        QTest::addColumn<int>("frameLag");
        QTest::addColumn<bool>("expectPass");
        QTest::newRow("working compositor") << true << 0 << true;
        // The eval returns before the rule lands, so the check's first look is stale.
        QTest::newRow("compositor applies next frame") << true << 1 << true;
        QTest::newRow("compositor ignores changes") << false << 0 << false;
    }
    void recoveryCheckEndToEnd() {
        // The whole flow with the real detached watchdog process and a stateful fake
        // compositor. The check passes only if a change really took and really came back.
        QFETCH(bool, compositorObeys);
        QFETCH(int, frameLag);
        QFETCH(bool, expectPass);
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/obeys", compositorObeys ? "1" : "0");
        writeFile(s.dir.path() + "/x", "0");
        if (frameLag > 0)
            writeFile(s.dir.path() + "/lag", QByteArray::number(frameLag));
        s.command("hyprctl", kStatefulCompositor);
        Env env = s.env;
        env.selfBinary = QCoreApplication::applicationFilePath();
        env.process.insert("OC_TEST_ROOT", s.dir.path());
        CommandRunner runner(env);
        ChangeCoordinator coordinator(env);
        DisplayProvider displays(&runner, &coordinator);
        QSignalSpy finished(&displays, &DisplayProvider::checkFinished);
        displays.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(displays.monitors().size() == 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!displays.status().isEmpty(), 1000);
        displays.runRecoveryCheck();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 20000);
        QCOMPARE(finished.first().at(0).toBool(), expectPass);
        QCOMPARE(displays.checkState(), QString(expectPass ? "passed" : "failed"));
        // Whatever the verdict, the display ended up where it started.
        QCOMPARE(readAll(s.dir.path() + "/x").trimmed(), QByteArray("0"));
        QTRY_COMPARE_WITH_TIMEOUT(displays.editingUnlocked(), expectPass, 3000);
        // A failure says which step failed and what Hyprland reports now; a pass clears it.
        if (expectPass) {
            QVERIFY(displays.outcome().isEmpty());
        } else {
            QCOMPARE(displays.outcome().value("scope").toString(), QString("check"));
            QVERIFY(displays.outcome().value("detail").toString().contains("did not report the test move"));
            QVERIFY(displays.outcome().value("detail").toString().contains("original layout"));
            QVERIFY(displays.lastCheck().value("reason").toString().contains("test move"));
        }
        QCOMPARE(displays.lastCheck().value("passed").toBool(), expectPass);
        // Nothing of the run is left behind to be mistaken later for an interrupted one.
        QVERIFY(QDir(env.runtimeDir + "/omarchy-control").entryList({"display-*"}, QDir::Files).isEmpty());
    }
    void previewWaitsForHyprlandToShowTheLayout_data() {
        QTest::addColumn<int>("frameLag");
        QTest::newRow("compositor applies at once") << 0;
        // The eval returns before the rule lands, so a read in the same turn is stale.
        QTest::newRow("compositor applies next frame") << 1;
    }
    void previewWaitsForHyprlandToShowTheLayout() {
        // A preview clears the staged edits, so the page draws what Hyprland reports:
        // it has to be the previewed layout, not the one from before. After Restore,
        // the check against the old layout must not mistake a late frame for a failure.
        QFETCH(int, frameLag);
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/obeys", "1");
        writeFile(s.dir.path() + "/x", "0");
        if (frameLag > 0)
            writeFile(s.dir.path() + "/lag", QByteArray::number(frameLag));
        s.command("hyprctl", kStatefulCompositor);
        Env env = s.env;
        env.selfBinary = QCoreApplication::applicationFilePath();
        env.process.insert("OC_TEST_ROOT", s.dir.path());
        CommandRunner runner(env);
        ChangeCoordinator coordinator(env);
        DisplayProvider displays(&runner, &coordinator);
        displays.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(displays.monitors().size() == 1, 5000);
        writeFile(displays.recoveryCheckPath(), R"({"passed":true,"hyprland":"0.56.2"})");
        QTRY_VERIFY_WITH_TIMEOUT(displays.editingUnlocked(), 3000);
        displays.stageEdit("eDP-1", "x", 40);
        displays.previewStaged();
        QTRY_VERIFY_WITH_TIMEOUT(displays.previewing(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().first().toMap().value("x").toInt(), 40, 5000);
        QVERIFY(!displays.dirty());
        QCOMPARE(stagedRow(displays, "eDP-1").value("x").toInt(), 40);
        displays.revert();
        QTRY_VERIFY_WITH_TIMEOUT(!displays.previewing(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(displays.monitors().first().toMap().value("x").toInt(), 0, 5000);
        QCOMPARE(readAll(s.dir.path() + "/x").trimmed(), QByteArray("0"));
        // Let the post-restore read and refresh finish before judging the outcome.
        QTest::qWait(500);
        QVERIFY2(displays.outcome().isEmpty(), qPrintable(displays.outcome().value("detail").toString()));
        const auto leftovers = QDir(env.runtimeDir + "/omarchy-control").entryList({"display-*"}, QDir::Files);
        for (const auto &path : leftovers) QVERIFY(path.endsWith(".result"));
        QCOMPARE(displays.previewReceipt().value("phase").toString(), QString("reverted"));
    }
    void recoveryCheckFailsWhenTheWatchdogCannotStart() {
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/hypr-monitors", twoMonitors());
        Env env = s.env;
        env.selfBinary = "/nonexistent/omarchy-control";
        CommandRunner runner(env);
        ChangeCoordinator coordinator(env);
        DisplayProvider displays(&runner, &coordinator);
        QSignalSpy finished(&displays, &DisplayProvider::checkFinished);
        displays.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(displays.monitors().size() == 3, 5000);
        displays.runRecoveryCheck();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 8000);
        QVERIFY(!finished.first().at(0).toBool());
        QCOMPARE(displays.checkState(), QString("failed"));
        QVERIFY(!QFile::exists(s.dir.path() + "/hypr-eval"));
    }
    void unsafeOutputNamesNeverReachLua() {
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/hypr-monitors", kHostileMonitor);
        CommandRunner runner(s.env);
        ChangeCoordinator coordinator(s.env);
        DisplayProvider displays(&runner, &coordinator);
        QSignalSpy finished(&displays, &DisplayProvider::checkFinished);
        displays.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(displays.status(), QString("unavailable"), 5000);
        QVERIFY(displays.monitors().isEmpty());
        QVERIFY(!displays.editingUnlocked());
        displays.runRecoveryCheck();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(!QFile::exists(s.dir.path() + "/hypr-eval"));
    }
    void watchdogLastResortRefusesUnsafeNames() {
        Sandbox s;
        s.minimalHyprland();
        writeFile(s.dir.path() + "/hypr-monitors", kHostileMonitorNoModes);
        const QString plan = watchdogPlan(s, 0);
        QCOMPARE(DisplayProvider::runWatchdog(s.env, plan), 0);
        QVERIFY(!readAll(s.dir.path() + "/hypr-eval").contains("os.execute"));
        QCOMPARE(readAll(s.dir.path() + "/result").trimmed(), QByteArray("restore-failed"));
    }

    // ---- Compatibility, package lock, rules, and accessibility ----------------
    void aChangedCommandPausesOnlyItsSettings() {
        Sandbox s;
        s.env.enforceCompat = true;
        s.command("omarchy-hyprland-corners", "echo new version");
        s.minimalHyprland();
        s.omarchy("exit 0");
        AppCore core(s.env);
        // The fake's hash cannot match the reviewed one, which is exactly a changed script.
        const ChangePlan paused = core.engine.plan("appearance.corners", "sharp");
        QVERIFY(!paused.ok);
        QVERIFY(paused.error.contains("omarchy-hyprland-corners"));
        // A setting whose command is not on this machine is not reported as changed.
        QVERIFY(core.engine.plan("power.stayawake", "true").ok);
        QVERIFY(core.compat.changedCommands({}).contains("omarchy-hyprland-corners"));
        // Reading never pauses.
        QVERIFY(core.compat.changedFor(*core.catalog.find("system.version"), {}).isEmpty());
        core.compat.acceptAll({});
        QVERIFY(core.engine.plan("appearance.corners", "sharp").ok);
        QVERIFY(core.compat.changedCommands({}).isEmpty());
        // Accepting one version does not accept the next.
        s.command("omarchy-hyprland-corners", "echo another version");
        AppCore later(s.env);
        QVERIFY(!later.engine.plan("appearance.corners", "sharp").ok);
    }
    void aNewHyprlandPausesOverridesUntilAccepted() {
        Sandbox s;
        s.env.enforceCompat = true;
        s.minimalHyprland();
        Compat compat(s.env);
        Catalog catalog;
        const auto *entry = catalog.find("input.repeat_rate");
        QVERIFY(compat.changedFor(*entry, "0.56.2").isEmpty());
        QVERIFY(!compat.changedFor(*entry, "0.57.0").isEmpty());
        compat.acceptAll("0.57.0");
        QVERIFY(compat.changedFor(*entry, "0.57.0").isEmpty());
        QVERIFY(!compat.changedFor(*entry, "0.58.0").isEmpty());
    }
    void everyPinnedCommandMatchesTheInventoriedOnes() {
        QFile file(":/data/compat.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        Catalog catalog;
        QVERIFY(root.value("settings").toObject().size() > 25);
        for (const auto &key : root.value("settings").toObject().keys())
            QVERIFY2(catalog.find(key), qPrintable(key));
        // Every declared provider/profile dependency has a reviewed fingerprint;
        // extending adapters must not invalidate an otherwise legitimate pin map.
        const QJsonObject adapters = root.value("adapters").toObject();
        QStringList required{"profile.bar", "profile.plugins", "profile.themes"};
        const auto operations = QJsonDocument::fromJson(readAll(PROJECT_ROOT "/src/data/cli-operations.json")).array();
        for (const auto &row : operations)
            for (const auto &key : row.toObject().value("compatibilityKeys").toArray())
                required << key.toString();
        for (const QString &key : required)
            QVERIFY2(adapters.contains(key), qPrintable(key));
        const auto inventory = QJsonDocument::fromJson(readAll(PROJECT_ROOT "/docs/inventory-2026-11-05.json")).object();
        QSet<QString> binaries;
        for (const auto &row : inventory.value("commands").toArray())
            binaries.insert(row.toObject().value("binary").toString());
        for (const QJsonValue &binaries : adapters)
            for (const QJsonValue &binary : binaries.toArray())
                QVERIFY2(root.value("commands").toObject().contains(binary.toString()), qPrintable(binary.toString()));
        for (const QString &name : root.value("commands").toObject().keys()) {
            QVERIFY2(binaries.contains(name), qPrintable(name));
            QVERIFY(QRegularExpression("^[a-f0-9]{64}$").match(root.value("commands").toObject().value(name).toString()).hasMatch());
        }
    }
    void packageWorkWaitsForPacman() {
        Sandbox s;
        s.command("omarchy-launch-floating-terminal-with-presentation", "touch \"$HOME/launched\"");
        writeFile(s.env.sys("/var/lib/pacman/db.lck"), "");
        AppCore core(s.env);
        HelperLauncher launcher(&core.runner, &core.capabilities);
        HelperLauncher::Outcome outcome;
        bool done = false;
        launcher.launch(*core.catalog.find("update.omarchy"), [&](const HelperLauncher::Outcome &o) { outcome = o; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(!outcome.opened);
        QVERIFY(outcome.message.contains("package operation"));
        // Anything that does not touch packages is unaffected.
        done = false;
        launcher.launch(*core.catalog.find("snapshot.create"), [&](const HelperLauncher::Outcome &o) { outcome = o; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(!outcome.message.contains("package operation"));
    }
    void aMissingKernelModuleTreeMeansRestart() {
        Sandbox s;
        QDir().mkpath(s.env.sys("/usr/lib/modules/some-other-kernel"));
        AppCore core(s.env);
        QVERIFY(core.capabilities.has("reboot.needed"));
        QDir().mkpath(s.env.sys("/usr/lib/modules/") + QSysInfo::kernelVersion());
        writeFile(s.env.sys("/usr/lib/modules/") + QSysInfo::kernelVersion() + "/modules.builtin", "");
        core.capabilities.scanFiles();
        QVERIFY(!core.capabilities.has("reboot.needed"));
    }
    void theClockFormatsNeedTheClockOnTheBar() {
        Sandbox s;
        // Omarchy's `bar get` answers for a widget that is on the bar and fails for one that is not.
        s.omarchy("case \"$*\" in\n"
                  "  'bar get omarchy.clock format') echo 'dddd HH:mm';;\n"
                  "  'bar get omarchy.clock verticalFormat') echo HH;;\n"
                  "  *) exit 1;;\n"
                  "esac");
        const QString shellJson = s.env.configHome + "/omarchy/shell.json";
        writeFile(shellJson, R"({"bar":{"layout":{"left":["omarchy.menu"],"center":[{"id":"omarchy.clock","format":"dddd HH:mm"}]}}})");
        AppCore core(s.env);
        const QStringList rows{"desktop.clock.format", "desktop.clock.vertical"};
        QVERIFY(core.capabilities.has("barclock"));
        for (const QString &id : rows)
            QCOMPARE(readSetting(core.engine, id).status, QString("ok"));

        // A bare id is the clock too, and a longer id that starts the same is not.
        writeFile(shellJson, R"({"bar":{"layout":{"right":["omarchy.clock"]}}})");
        core.capabilities.scanFiles();
        QVERIFY(core.capabilities.has("barclock"));
        writeFile(shellJson, R"({"bar":{"layout":{"right":["omarchy.clockwork"]}}})");
        core.capabilities.scanFiles();
        QVERIFY(!core.capabilities.has("barclock"));

        // A custom bar that swapped the clock for its own leaves these with nothing to read or change.
        writeFile(shellJson, R"({"bar":{"id":"local.neon-bar","layout":{"left":["omarchy.menu"],"center":[{"id":"nixfred.chronos"}],"tray":[]}}})");
        core.capabilities.scanFiles();
        QVERIFY(!core.capabilities.has("barclock"));
        for (const QString &id : rows) {
            const auto availability = core.capabilities.availability(*core.catalog.find(id));
            QVERIFY(!availability.available);
            QVERIFY(availability.reason.contains("no Omarchy clock"));
            const auto result = readSetting(core.engine, id);
            QCOMPARE(result.status, QString("unavailable"));
            QVERIFY(result.reason.contains("no Omarchy clock"));
        }

        // A shell.json that says nothing about the layout is the stock bar, so the rows stay as they were.
        for (const QByteArray &contents : {QByteArray("{}"), QByteArray(R"({"bar":{"id":"local.neon-bar"}})"), QByteArray("not json")}) {
            writeFile(shellJson, contents);
            core.capabilities.scanFiles();
            QVERIFY(core.capabilities.has("barclock"));
            QVERIFY(core.capabilities.availability(*core.catalog.find("desktop.clock.format")).available);
        }
    }
    void aRestoreKeepsWhatItReplaced() {
        Sandbox s;
        const QString file = s.env.configHome + "/c.json";
        writeFile(file, "original");
        ChangeCoordinator coordinator(s.env);
        ChangeCoordinator::Request request;
        request.files << file;
        request.apply = [file](ChangeCoordinator::Done done) {
            FileOps::atomicWrite(file, "possibly someone's edit");
            done(false, "broke");
        };
        Transaction result;
        coordinator.submit(request, [&](const Transaction &tx) { result = tx; });
        QTRY_COMPARE(result.phase, QString("rolled-back"));
        QCOMPARE(readAll(file), QByteArray("original"));
        QCOMPARE(readAll(coordinator.backupDirectory() + "/" + result.id + "/replaced-c.json"), QByteArray("possibly someone's edit"));
    }
    void vrrSurvivesARule() {
        QVERIFY(DisplayProvider::monitorLua(QJsonObject{{"output", "DP-1"}, {"mode", "m"}, {"position", "0x0"}, {"scale", 1}, {"vrr", true}}).contains("vrr = 1"));
        Sandbox s;
        HyprModule module(s.env);
        module.setMonitors(QJsonArray{QJsonObject{{"output", "DP-1"}, {"mode", "m"}, {"position", "0x0"}, {"scale", 1}, {"vrr", true}}});
        QVERIFY(module.render().contains("vrr = 1"));
    }
    void nestedJsonFieldsParse() {
        CommandResult result;
        result.exitCode = 0;
        result.out = R"({"config":{"language":"en","autoPasteEnabled":false}})";
        bool ok = false;
        QCOMPARE(SettingsEngine::parseOutput(result, QJsonObject{{"parse", "json:config.language"}}, &ok), QString("en"));
        QVERIFY(ok);
        QCOMPARE(SettingsEngine::parseOutput(result, QJsonObject{{"parse", "json:config.autoPasteEnabled"}}, &ok), QString("false"));
        SettingsEngine::parseOutput(result, QJsonObject{{"parse", "json:config.nope"}}, &ok);
        QVERIFY(!ok);
    }
    void superwhisperSettingsSendJson() {
        Sandbox s;
        s.command("superwhisper", "f=\"$HOME/tone\"; case \"$*\" in 'general show') printf '{\"config\":{\"tone\":%s}}' \"$(cat \"$f\" 2>/dev/null || echo '\"casual\"')\";; 'general set tone '*) printf '%s' \"$4\" > \"$f\";; esac");
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "input.dictation.tone").value, QString("casual"));
        QCOMPARE(applySetting(core.engine, "input.dictation.tone", "formal").phase, QString("committed"));
        QVERIFY(s.calls().join('\n').contains("general set tone \"formal\""));
        QVERIFY(!core.engine.plan("input.dictation.language", "x\"y").ok);
    }
    void windowRulesRenderSafely() {
        Sandbox s;
        HyprModule module(s.env);
        module.setRules(QJsonArray{
            QJsonObject{{"match", "firefox"}, {"action", "float"}},
            QJsonObject{{"match", "x\")\nos.execute(\"y"}, {"action", "workspace"}, {"value", "3"}},
            QJsonObject{{"match", "term"}, {"action", "opacity"}, {"value", "0.9 0.85"}},
            QJsonObject{{"match", "steam"}, {"action", "noblur"}},
        });
        const QString lua = QString::fromUtf8(module.render());
        QVERIFY(lua.contains("o.window(\"firefox\", { float = true })"));
        QVERIFY(lua.contains("workspace = \"3\""));
        QVERIFY(lua.contains("opacity = \"0.9 0.85\""));
        QVERIFY(lua.contains("o.window(\"steam\", { no_blur = true })"));
        QVERIFY(!lua.contains("\nos.execute"));
        QVERIFY(module.save());
        QCOMPARE(module.state(), HyprModule::Ours);
    }
    void windowRuleValidation_data() {
        QTest::addColumn<QString>("match");
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("ok");
        QTest::newRow("float") << "firefox" << "float" << "" << true;
        QTest::newRow("pattern") << "^(foo|bar)$" << "tile" << "" << true;
        QTest::newRow("bad pattern") << "(unclosed" << "tile" << "" << false;
        QTest::newRow("empty") << "  " << "float" << "" << false;
        QTest::newRow("workspace") << "x" << "workspace" << "3" << true;
        QTest::newRow("workspace junk") << "x" << "workspace" << "3\"); boom" << false;
        QTest::newRow("opacity pair") << "x" << "opacity" << "0.9 0.85" << true;
        QTest::newRow("opacity high") << "x" << "opacity" << "7" << false;
        QTest::newRow("no blur") << "steam" << "noblur" << "" << true;
        QTest::newRow("unknown action") << "x" << "exec" << "rm" << false;
        QTest::newRow("ignored float value") << "x" << "float" << "unexpected" << false;
        QTest::newRow("ignored tile value") << "x" << "tile" << "0.9" << false;
        QTest::newRow("ignored noblur value") << "x" << "noblur" << "true" << false;
        QTest::newRow("workspace trailing newline") << "x" << "workspace" << "3\n" << false;
        QTest::newRow("opacity trailing newline") << "x" << "opacity" << "0.9\n" << false;
        QTest::newRow("match control") << "fire\nfox" << "float" << "" << false;
        QTest::newRow("special workspace") << "x" << "workspace" << "special:notes" << true;
        QTest::newRow("long match") << QString(121, 'x') << "float" << "" << false;
    }
    void windowRuleValidation() {
        QFETCH(QString, match);
        QFETCH(QString, action);
        QFETCH(QString, value);
        QFETCH(bool, ok);
        Sandbox s;
        CommandRunner runner(s.env);
        ChangeCoordinator coordinator(s.env);
        WindowRulesProvider provider(&runner, &coordinator);
        QCOMPARE(provider.validate(match, action, value).isEmpty(), ok);
    }
    void windowRuleAddsAndRemoves() {
        Sandbox s;
        s.minimalHyprland();
        CommandRunner runner(s.env);
        ChangeCoordinator coordinator(s.env);
        WindowRulesProvider provider(&runner, &coordinator);
        QSignalSpy messages(&provider, &WindowRulesProvider::message);
        provider.addRule("firefox", "float", "");
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() == 1, 5000);
        QVERIFY2(!messages.first().at(1).toBool(), qPrintable(messages.first().at(0).toString()));
        QCOMPARE(provider.rules().size(), 1);
        provider.removeRule(0);
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() == 2, 5000);
        QCOMPARE(provider.rules().size(), 0);
    }
    void redoReappliesAnUndo() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{\"idle\":{\"lock\":300}}");
        AppCore core(s.env);
        const Transaction change = applySetting(core.engine, "power.idle.lock", "600");
        const Transaction undone = undoTx(core.engine, change.id);
        QCOMPARE(undone.kind, QString("undo"));
        QVERIFY(undone.undoable());
        QVERIFY(readAll(config).contains("300"));
        QCOMPARE(undoTx(core.engine, undone.id).phase, QString("committed"));
        QVERIFY(readAll(config).contains("600"));
    }
    void everyFocusableControlIsNamedAndReachable() {
        // Screen-reader evidence that needs no screen reader: Tab through each page and
        // ask Qt's accessibility layer what it would announce.
        QAccessible::setActive(true);
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QStringList unnamed;
        for (const auto &value : backend.pages()) {
            const QString page = value.toMap().value("id").toString();
            QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, page), Q_ARG(QVariant, QString()));
            QTest::qWait(200);
            QSet<QQuickItem *> visited;
            for (int i = 0; i < 400; ++i) {
                QTest::keyClick(window, Qt::Key_Tab);
                QQuickItem *item = window->activeFocusItem();
                if (!item || visited.contains(item))
                    break;
                visited << item;
                QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(item);
                const QString name = accessible ? accessible->text(QAccessible::Name) : QString();
                const QString description = accessible ? accessible->text(QAccessible::Description) : QString();
                if (name.trimmed().isEmpty() && description.trimmed().isEmpty())
                    unnamed << page + ": " + QString::fromLatin1(item->metaObject()->className());
            }
            QVERIFY2(visited.size() >= 2, qPrintable(page));
        }
        QVERIFY2(unnamed.isEmpty(), qPrintable(unnamed.join('\n')));
    }
    void hoveringASettingRowMovesNothing() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "appearance"), Q_ARG(QVariant, QString()));
        QTest::qWait(400);

        // Every full row in view, found by walking the real scene.
        QList<QQuickItem *> rows;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            if (!item->isVisible())
                return;
            if (!item->property("entryId").toString().isEmpty() && !item->property("compact").toBool()) {
                const QRectF box = item->mapRectToScene(item->boundingRect());
                if (box.top() >= 60 && box.bottom() <= window->height())
                    rows << item;
                return;
            }
            for (QQuickItem *child : item->childItems())
                walk(child);
        };
        walk(window->contentItem());
        QVERIFY2(rows.size() >= 3, qPrintable(QString::number(rows.size())));

        const auto named = [](QQuickItem *row, const QString &name) {
            QQuickItem *found = nullptr;
            std::function<void(QQuickItem *)> search = [&](QQuickItem *item) {
                if (item->objectName() == name)
                    found = item;
                for (QQuickItem *child : item->childItems())
                    if (!found)
                        search(child);
            };
            search(row);
            return found;
        };
        const auto layoutOf = [&](QQuickItem *row) {
            QList<QRectF> boxes{row->mapRectToScene(row->boundingRect())};
            for (const QString &name : {QStringLiteral("settingControl"), QStringLiteral("pinButton"), QStringLiteral("detailsButton")})
                boxes << named(row, name)->mapRectToScene(named(row, name)->boundingRect());
            return boxes;
        };

        // Every control ends on the same edge, down the whole page.
        QSet<int> rightEdges;
        for (QQuickItem *row : rows) {
            for (const QString &name : {QStringLiteral("settingControl"), QStringLiteral("actionButton")}) {
                QQuickItem *control = named(row, name);
                if (control && control->isVisible())
                    rightEdges << qRound(control->mapRectToScene(control->boundingRect()).right());
            }
        }
        QVERIFY(!rightEdges.isEmpty());
        QCOMPARE(rightEdges.size(), 1);

        QTest::mouseMove(window, QPoint(1, 1));
        QTest::qWait(250);
        QQuickItem *firstPin = named(rows.first(), "pinButton");
        QCOMPARE(firstPin->opacity(), 0.0);
        const QString firstId = rows.first()->property("entryId").toString();

        // At rest a tap on the hidden button changes nothing.
        QMetaObject::invokeMethod(firstPin, "clicked");
        QVERIFY(!backend.pins().contains(firstId));

        QList<QList<QRectF>> before;
        for (QQuickItem *row : rows)
            before << layoutOf(row);
        for (int i = 0; i < rows.size(); ++i) {
            const QPointF center = rows[i]->mapToScene(QPointF(40, rows[i]->height() / 2));
            QTest::mouseMove(window, center.toPoint());
            // The button showing is what proves the hover reached the row.
            QTRY_VERIFY2(named(rows[i], "pinButton")->opacity() > 0.4, qPrintable(rows[i]->property("entryId").toString()));
            QTest::qWait(250);
            for (int j = 0; j < rows.size(); ++j)
                QCOMPARE(layoutOf(rows[j]), before[j]);
        }
    }
    void aTabAlwaysOpensAtTheTopAndSearchStillLandsOnItsRow() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        // Short enough that every page scrolls and the first tabs are still on screen.
        window->resize(1000, 400);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QTest::qWait(200);

        const auto find = [&](const QString &name) {
            QQuickItem *found = nullptr;
            std::function<void(QQuickItem *)> search = [&](QQuickItem *item) {
                if (item->objectName() == name)
                    found = item;
                for (QQuickItem *child : item->childItems())
                    if (!found)
                        search(child);
            };
            search(window->contentItem());
            return found;
        };
        const auto view = [&] {
            return find("pageLoader")->property("item").value<QObject *>()->property("contentItem").value<QObject *>();
        };
        const auto offset = [&] { return view()->property("contentY").toReal(); };
        const auto scrollDown = [&] {
            view()->setProperty("contentY", 120);
            QCOMPARE(offset(), 120.0);
        };
        const auto clickTab = [&](const QString &page) {
            QQuickItem *tab = find("nav_" + page);
            QVERIFY2(tab, qPrintable(page));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, tab->mapToScene(QPointF(tab->width() / 2, tab->height() / 2)).toPoint());
            QTRY_COMPARE(window->property("currentPage").toString(), page);
            QTest::qWait(100);
        };

        clickTab("appearance");
        QCOMPARE(offset(), 0.0);
        scrollDown();
        // A different tab, sharing the same scrolling view.
        clickTab("desktop");
        QCOMPARE(offset(), 0.0);
        scrollDown();
        // The tab already open.
        clickTab("desktop");
        QCOMPARE(offset(), 0.0);
        scrollDown();
        QMetaObject::invokeMethod(window, "back");
        QTRY_COMPARE(window->property("currentPage").toString(), QString("appearance"));
        QCOMPARE(offset(), 0.0);
        // Overview has its own view.
        clickTab("overview");
        scrollDown();
        clickTab("overview");
        QCOMPARE(offset(), 0.0);

        // A search result lands on its row, and keeps doing so after the highlight clears.
        const QString far = "appearance.plymouth";
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "appearance"), Q_ARG(QVariant, far));
        QTRY_VERIFY(offset() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(window->property("focusRequest").toString().isEmpty(), 4000);
        QTest::qWait(200);
        QVERIFY(offset() > 0);
        clickTab("appearance");
        QCOMPARE(offset(), 0.0);
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "appearance"), Q_ARG(QVariant, far));
        QTRY_VERIFY(offset() > 0);
    }

    // ---- Look ------------------------------------------------------------------
    void textColorsStayReadableOnAnyTheme() {
        // Catppuccin Latte's own secondary text, on its own background.
        const QStringList latte{"#eff1f5", "#dce0e8"};
        QVERIFY(Theme::contrast("#9ca0b0", "#eff1f5") < 3);
        const QString fixed = Theme::readable("#9ca0b0", "#4c4f69", latte);
        QVERIFY2(Theme::contrast(fixed, "#eff1f5") >= 4.5, qPrintable(fixed));
        QVERIFY2(Theme::contrast(fixed, "#dce0e8") >= 4.5, qPrintable(fixed));
        // A color that already reads is left exactly as the theme wrote it.
        QCOMPARE(Theme::readable("#ff9f0a", "#f5f5f7", {"#1c1c1e", "#2c2c2e"}), QString("#ff9f0a"));
        // A warning that needs help stays a warning rather than turning gray.
        const QColor amber(Theme::readable("#df8e1d", "#4c4f69", latte));
        QVERIFY2(Theme::contrast(amber.name(), "#eff1f5") >= 4.5, qPrintable(amber.name()));
        QVERIFY2(amber.hslSaturationF() > 0.4, qPrintable(amber.name()));
        QVERIFY2(qAbs(amber.hslHueF() - QColor("#df8e1d").hslHueF()) < 0.02, qPrintable(amber.name()));
        // Nothing can be done past the foreground itself, and it does not hang trying.
        QCOMPARE(Theme::readable("#888888", "#888888", {"#888888"}), QString("#888888"));
    }
    void themePreviewsComeFromTheThemesThemselves() {
        Sandbox s;
        fakeDesktop(s);
        writeFile(s.env.omarchyPath + "/themes/tokyo-night/colors.toml", "accent = \"#7aa2f7\"\nbackground = \"#1a1b26\"\n");
        writeFile(s.env.omarchyPath + "/themes/tokyo-night/preview.png", "x");
        writeFile(s.env.omarchyPath + "/themes/mine/colors.toml", "accent = \"#000001\"\n");
        writeFile(s.env.configHome + "/omarchy/themes/mine/colors.toml", "accent = \"#000002\"\n");
        Theme theme(s.env);
        const QVariantMap tokyo = theme.preview("Tokyo Night");
        QCOMPARE(tokyo.value("accent").toString(), QString("#7aa2f7"));
        QVERIFY(tokyo.value("image").toString().endsWith("/themes/tokyo-night/preview.png"));
        // The user's own copy of a theme is the one that will be used.
        QCOMPARE(theme.preview("Mine").value("accent").toString(), QString("#000002"));
        QVERIFY(!theme.preview("Mine").contains("image"));
        // No preview, but wallpapers: the first still stands in. A video cannot.
        writeFile(s.env.configHome + "/omarchy/themes/mine/backgrounds/1-motion.mp4", "x");
        writeFile(s.env.configHome + "/omarchy/themes/mine/backgrounds/2-still.webp", "x");
        QVERIFY(theme.preview("Mine").value("image").toString().endsWith("/backgrounds/2-still.webp"));
        QVERIFY(theme.preview("Not Installed").isEmpty());
        // The name comes from a command's output, so it cannot walk out of the folder.
        QVERIFY(theme.preview("../../etc").isEmpty());
        QVERIFY(theme.preview(".hidden").isEmpty());
    }
    void picturesAreCutToTheDesktopsCorners() {
        Sandbox s;
        fakeDesktop(s);
        const QString path = s.dir.path() + "/a picture %.png";
        QImage source(800, 200, QImage::Format_RGB32);
        source.fill(Qt::red);
        QVERIFY(source.save(path));
        Theme theme(s.env);
        IconProvider provider;
        const auto request = [&](int radius) {
            const QString id = theme.thumbnail(path, radius).mid(QString("image://icons/").size());
            QSize size;
            const QImage image = provider.requestImage(id, &size, QSize(160, 100));
            return std::make_pair(image, size);
        };
        const auto [rounded, roundedSize] = request(12);
        QCOMPARE(roundedSize, QSize(160, 100));
        QCOMPARE(rounded.size(), QSize(160, 100));
        QCOMPARE(qAlpha(rounded.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(rounded.pixel(80, 50)), 255);
        // A square desktop gets square pictures, with no corner cut at all.
        const auto [square, squareSize] = request(0);
        Q_UNUSED(squareSize);
        QCOMPARE(qAlpha(square.pixel(0, 0)), 255);
        // A file URL means the same picture, and a missing one is simply empty.
        QCOMPARE(theme.thumbnail(QUrl::fromLocalFile(path).toString(), 0), theme.thumbnail(path, 0));
        QVERIFY(theme.thumbnail("", 4).isEmpty());
        QSize size;
        const QImage missing = provider.requestImage("thumb/4/0/" + QString::fromUtf8(QUrl::toPercentEncoding("/no/such.png")), &size, QSize(32, 32));
        QCOMPARE(qAlpha(missing.pixel(16, 16)), 0);
    }
    void aThemesBackgroundsFollowTheTheme() {
        Sandbox s;
        fakeDesktop(s);
        const QString current = s.env.stateHome + "/omarchy/current";
        const QString state = s.dir.path() + "/picked";
        const auto swapTo = [&](const QString &name, const QStringList &pictures) {
            // What Omarchy does: the theme's folder is replaced outright.
            QDir(current + "/theme").removeRecursively();
            for (const auto &picture : pictures)
                writeFile(current + "/theme/backgrounds/" + picture, "x");
            QFile::remove(current + "/background");
            QFile::link(current + "/theme/backgrounds/" + pictures.first(), current + "/background");
            writeFile(current + "/theme.name", name.toUtf8());
            writeFile(state, name.toUtf8());
        };
        swapTo("One", {"one-a.png", "one-b.png"});
        s.omarchy("state='" + state.toUtf8() + "'\ncurrent='" + current.toUtf8() + "'\ncase \"$*\" in\n"
                  "  'theme list') printf 'One\\nTwo\\n';;\n"
                  "  'theme current') cat \"$state\";;\n"
                  "  'theme set Two') rm -rf \"$current/theme\"; mkdir -p \"$current/theme/backgrounds\"; : > \"$current/theme/backgrounds/two-a.png\"; : > \"$current/theme/backgrounds/two-b.png\"; : > \"$current/theme/backgrounds/two-c.png\"; ln -sf \"$current/theme/backgrounds/two-b.png\" \"$current/background\"; printf Two > \"$current/theme.name\"; printf Two > \"$state\";;\n"
                  "  version) echo 4.5.0-3;;\nesac\n");
        Backend backend(s.env);
        backend.pageShown("appearance");
        const auto background = [&] { return backend.states()->value(Backend::stateKey("appearance.background")).toMap(); };
        const auto labels = [&] {
            QStringList names;
            for (const auto &choice : background().value("choices").toList())
                names << choice.toMap().value("label").toString();
            return names;
        };
        QTRY_COMPARE(labels(), QStringList({"one-a", "one-b"}));
        QVERIFY(background().value("value").toString().endsWith("/one-a.png"));

        // Changed in this window.
        backend.commit("appearance.theme", "Two");
        QTRY_COMPARE_WITH_TIMEOUT(labels(), QStringList({"two-a", "two-b", "two-c"}), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(background().value("value").toString().endsWith("/two-b.png"), 5000);

        // Changed from outside it, such as from the Omarchy menu.
        swapTo("One", {"one-a.png", "one-b.png"});
        QTRY_COMPARE_WITH_TIMEOUT(labels(), QStringList({"one-a", "one-b"}), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(background().value("value").toString().endsWith("/one-a.png"), 5000);
    }
    void aSameNamedPictureFromAnotherThemeIsNotTheCachedOne() {
        Sandbox s;
        fakeDesktop(s);
        const QString current = s.env.stateHome + "/omarchy/current";
        const QString picture = current + "/theme/backgrounds/1-main.png";
        writeFile(picture, "first");
        QFile::remove(current + "/background");
        QFile::link(picture, current + "/background");
        QFile first(picture);
        QVERIFY(first.open(QIODevice::ReadWrite));
        QVERIFY(first.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000), QFileDevice::FileModificationTime));
        first.close();
        Theme theme(s.env);
        const QString before = theme.thumbnail(picture, 8);
        QTRY_VERIFY(!theme.wallpaper().isEmpty());
        const QString wallpaperBefore = theme.wallpaper();
        // Same path, new theme: Omarchy replaces the folder outright.
        QVERIFY(QDir(current + "/theme").removeRecursively());
        writeFile(picture, "second");
        QVERIFY(theme.thumbnail(picture, 8) != before);
        QTRY_VERIFY_WITH_TIMEOUT(theme.wallpaper() != wallpaperBefore, 3000);
        // Still the same file underneath.
        QCOMPARE(QUrl(theme.wallpaper()).toLocalFile(), QFileInfo(picture).canonicalFilePath());
    }
    void cornersFollowTheDesktop() {
        Sandbox s;
        fakeDesktop(s);
        Theme theme(s.env);
        QVERIFY(theme.radius() >= 0);
        s.command("hyprctl", "echo '{\"int\":0}'");
        theme.refreshRounding();
        QTRY_COMPARE(theme.radius(), 0);
        QCOMPARE(theme.panelRadius(), 0);
        QCOMPARE(theme.controlRadius(), 0);
        QVERIFY(!theme.rounded());
        s.command("hyprctl", "echo '{\"int\":10}'");
        theme.refreshRounding();
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(theme.panelRadius(), 10);
        QVERIFY(theme.controlRadius() > 0 && theme.controlRadius() <= 10);
        QVERIFY(theme.rounded());
    }
    void overviewReadsEverySettingItShows() {
        // A row Overview draws but never reads sits on its loading state forever.
        Catalog catalog;
        const QString page = QString::fromUtf8(readAll(PROJECT_ROOT "/src/OverviewPage.qml"));
        static const QRegularExpression row("SettingRow \\{[^}]*backend\\.entry\\(\"([^\"]+)\"\\)");
        auto matches = row.globalMatch(page);
        int rows = 0;
        while (matches.hasNext()) {
            const QString id = matches.next().captured(1);
            ++rows;
            QVERIFY2(catalog.find(id) && catalog.find(id)->quick, qPrintable(id));
        }
        QVERIFY(rows >= 6);
    }
    void buttonsSayWhatTheyDo() {
        Catalog catalog;
        const auto verb = [&](const char *id) { return catalog.find(id)->verb(); };
        QCOMPARE(verb("system.reboot"), QString("Reboot"));
        QCOMPARE(verb("system.shutdown"), QString("Shut down"));
        QCOMPARE(verb("system.logout"), QString("Log out"));
        QCOMPARE(verb("system.lock"), QString("Lock"));
        QCOMPARE(verb("hibernation.remove"), QString("Remove"));
        QCOMPARE(verb("hibernation.setup"), QString("Set up"));
        QCOMPARE(verb("update.check"), QString("Check"));
        QCOMPARE(verb("panel.power"), QString("Open"));
        // Every verb a row can show is translated wherever the app claims to be.
        for (const auto &language : shippedLanguages()) {
            if (language == QLatin1String("en"))
                continue;
            const auto strings = QJsonDocument::fromJson(readAll(QString(PROJECT_ROOT "/src/i18n/") + language + ".json")).object();
            for (const auto &entry : catalog.entries())
                if (entry.isAction())
                    QVERIFY2(strings.contains(entry.verb()), qPrintable(QString(language) + ": " + entry.verb()));
        }
    }
    void aDestructiveDialogStartsOnCancel() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        auto *dialog = window->findChild<QObject *>("confirmDialog");
        QVERIFY(dialog);
        const auto open = [&](bool danger) {
            QMetaObject::invokeMethod(dialog, "open_", Q_ARG(QVariant, "Remove"), Q_ARG(QVariant, "Gone for good."), Q_ARG(QVariant, QVariantList()),
                                      Q_ARG(QVariant, "Remove"), Q_ARG(QVariant, danger), Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant()));
        };
        open(true);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_COMPARE(window->activeFocusItem()->property("text").toString(), QString("Cancel"));
        QMetaObject::invokeMethod(dialog, "close");
        QTRY_VERIFY(!dialog->property("visible").toBool());
        open(false);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_COMPARE(window->activeFocusItem()->property("text").toString(), QString("Remove"));
        QMetaObject::invokeMethod(dialog, "close");
    }
    void tabNeverLeavesTheKeyboardOffScreen() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(1000, 420);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "input"), Q_ARG(QVariant, QString()));
        QTest::qWait(300);
        int inPage = 0;
        for (int i = 0; i < 90; ++i) {
            QTest::keyClick(window, Qt::Key_Tab);
            QTest::qWait(5);
            QQuickItem *item = window->activeFocusItem();
            QVERIFY(item);
            const QRectF box = item->mapRectToScene(item->boundingRect());
            if (box.left() < 250)
                continue;   // The sidebar and toolbar do not scroll.
            ++inPage;
            QVERIFY2(box.top() >= 0 && box.bottom() <= window->height(), qPrintable(QString("Tab %1 at %2..%3").arg(i).arg(box.top()).arg(box.bottom())));
        }
        QVERIFY(inPage > 20);
    }
    void pickingAThemeTileChangesTheTheme() {
        Sandbox s;
        fakeDesktop(s);
        writeFile(s.env.stateHome + "/picked", "One");
        s.omarchy("state='" + s.env.stateHome.toUtf8() + "/picked'\ncase \"$*\" in\n  'theme list') printf 'One\\nTwo\\n';;\n  'theme current') cat \"$state\";;\n  'theme set '*) shift 2; printf '%s' \"$*\" > \"$state\";;\n  version) echo 4.5.0-3;;\nesac\n");
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "appearance"), Q_ARG(QVariant, QString()));
        const auto tile = [&](const QString &name) {
            QQuickItem *found = nullptr;
            std::function<void(QQuickItem *)> search = [&](QQuickItem *item) {
                if (item->property("look").isValid() && item->property("text").toString().isEmpty()) {
                    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(item);
                    if (accessible && accessible->text(QAccessible::Name) == name)
                        found = item;
                }
                for (QQuickItem *child : item->childItems())
                    if (!found)
                        search(child);
            };
            search(window->contentItem());
            return found;
        };
        QAccessible::setActive(true);
        QTRY_VERIFY(tile("Two"));
        QQuickItem *two = tile("Two");
        QTRY_VERIFY(tile("One") && tile("One")->property("selected").toBool());
        QVERIFY(!two->property("selected").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, two->mapToScene(QPointF(two->width() / 2, 30)).toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(!s.calls().filter("omarchy theme set Two").isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(tile("Two") && tile("Two")->property("selected").toBool(), 5000);
    }

    // ---- Updates --------------------------------------------------------------
    static QByteArray releaseJson(const QString &version) {
        const QByteArray v = version.toUtf8();
        const QByteArray base = "https://github.com/last-refuge/omarchy-control/releases/download/v" + v + "/";
        return "{\"tag_name\":\"v" + v + "\",\"body\":\"Better things\",\"html_url\":\"https://github.com/last-refuge/omarchy-control/releases/tag/v" + v + "\",\"assets\":["
               "{\"name\":\"SHA256SUMS\",\"browser_download_url\":\"" + base + "SHA256SUMS\"},"
               "{\"name\":\"omarchy-control-" + v + "-1-x86_64.pkg.tar.zst\",\"browser_download_url\":\"" + base + "omarchy-control-" + v + "-1-x86_64.pkg.tar.zst\"}]}";
    }
    // A fake curl that answers the API, then serves the two release files.
    static void fakeGitHub(const Sandbox &s, const QString &version, const QByteArray &packageBytes, const QByteArray &listedBytes) {
        const QByteArray architecture = QSysInfo::currentCpuArchitecture() == "arm64" ? QByteArray("aarch64") : QSysInfo::currentCpuArchitecture().toUtf8();
        const QByteArray name = "omarchy-control-" + version.toUtf8() + "-1-" + architecture + ".pkg.tar.zst";
        const QByteArray sum = QCryptographicHash::hash(listedBytes, QCryptographicHash::Sha256).toHex();
        writeFile(s.dir.path() + "/api.json", releaseJson(version).replace("x86_64", architecture));
        writeFile(s.dir.path() + "/package", packageBytes);
        s.command("curl", "for last; do :; done\n"
                          "while [ $# -gt 0 ]; do [ \"$1\" = -o ] && dest=$2; shift; done\n"
                          "case \"$last\" in\n"
                          "  *releases/latest) cat '" + s.dir.path().toUtf8() + "/api.json'; printf '\\n200';;\n"
                          "  *SHA256SUMS) printf '%s  %s\\n' " + sum + " " + name + ";;\n"
                          "  *.pkg.tar.zst) cp '" + s.dir.path().toUtf8() + "/package' \"$dest\";;\n"
                          "esac\n");
    }
    void versionsCompareAsNumbers() {
        QVERIFY(Updater::isNewer("0.2.0", "0.1.0"));
        QVERIFY(Updater::isNewer("0.10.0", "0.9.0"));
        QVERIFY(Updater::isNewer("1.0.0", "0.99.99"));
        QVERIFY(!Updater::isNewer("0.1.0", "0.1.0"));
        QVERIFY(!Updater::isNewer("0.1.0", "0.2.0"));
        // A tag this cannot read never counts as an update.
        QVERIFY(!Updater::isNewer("1.0.0-beta", "0.1.0"));
        QVERIFY(!Updater::isNewer("latest", "0.1.0"));
        QVERIFY(!Updater::isNewer("", "0.1.0"));
        QCOMPARE(Updater::versionFromPacman("omarchy-control 0.2.0-1\n"), QString("0.2.0"));
        QCOMPARE(Updater::versionFromPacman("error: package 'omarchy-control' was not found\n"), QString());
    }
    // Each copy updates the way it came. Only one a repository installed, signed by it,
    // updates with the system; a GitHub copy keeps GitHub, whatever a repository offers.
    void eachCopyUpdatesTheWayItCame() {
        const QByteArray offered = "Repository      : omarchy\nName            : omarchy-control\nVersion         : 1:0.4.1-2\n"
                                   "Description     : One searchable place : to understand\n\nRepository      : extra\nName            : omarchy-control\n";
        const QByteArray signedCopy = "Name            : omarchy-control\nVersion         : 0.4.0-1\nPackager        : Omarchy <pkgs@omarchy.org>\n"
                                      "Validated By    : SHA-256 Sum  Signature\n";
        const QByteArray githubCopy = "Name            : omarchy-control\nVersion         : 0.4.0-1\nPackager        : Unknown Packager\nValidated By    : None\n";
        const Updater::Source fromRepo = Updater::sourceFrom(offered, signedCopy);
        QVERIFY(fromRepo.fromRepository());
        QCOMPARE(fromRepo.repository, QString("omarchy"));
        QCOMPARE(fromRepo.version, QString("0.4.1"));
        // The case that matters: offered by a repository, but installed from GitHub.
        const Updater::Source fromGithub = Updater::sourceFrom(offered, githubCopy);
        QCOMPARE(fromGithub.repository, QString("omarchy"));
        QVERIFY(!fromGithub.fromRepository());
        QVERIFY(!Updater::sourceFrom("error: package 'omarchy-control' was not found\n", githubCopy).fromRepository());
        QVERIFY(!Updater::sourceFrom(offered, "error: package 'omarchy-control' was not found\n").fromRepository());
        QVERIFY(!Updater::sourceFrom("Repository : omarchy\nName : something-else\n", signedCopy).fromRepository());
    }
    void releaseNotesReadAsPlainText() {
        QCOMPARE(Updater::plainNotes("## What's Changed\n* Fix a jump by @someone in https://github.com/x/y/pull/1\n- Add About\n\n\n**Full Changelog**: https://github.com/x/y/compare/a...b\n"),
                 QString("What's Changed\n• Fix a jump\n• Add About"));
        QCOMPARE(Updater::plainNotes(""), QString());
    }
    void checksumsMatchTheExactFile() {
        const QByteArray a(64, 'a'), b(64, 'b');
        const QByteArray sums = a + "  one.pkg.tar.zst\n" + b + " *two.pkg.tar.zst\nnot a checksum line\n";
        QCOMPARE(Updater::checksumFor(sums, "one.pkg.tar.zst"), QString::fromLatin1(a));
        QCOMPARE(Updater::checksumFor(sums, "two.pkg.tar.zst"), QString::fromLatin1(b));
        QCOMPARE(Updater::checksumFor(sums, "pkg.tar.zst"), QString());
    }
    void releaseResponsesAreReadCarefully() {
        const auto respond = [](const QByteArray &body, int status) {
            CommandResult result;
            result.exitCode = 0;
            result.out = body + "\n" + QByteArray::number(status);
            return result;
        };
        Updater::Check found = Updater::interpret(respond(releaseJson("0.2.0"), 200), "x86_64");
        QCOMPARE(found.outcome, Updater::Check::Found);
        QCOMPARE(found.release.version, QString("0.2.0"));
        QCOMPARE(found.release.packageName, QString("omarchy-control-0.2.0-1-x86_64.pkg.tar.zst"));
        QCOMPARE(found.release.notes, QString("Better things"));
        QVERIFY(found.release.installable());

        // No package for this architecture: known about, but not installable.
        QVERIFY(!Updater::interpret(respond(releaseJson("0.2.0"), 200), "aarch64").release.installable());
        const QByteArray armRelease = releaseJson("0.2.0").replace("x86_64", "aarch64");
        QVERIFY(Updater::interpret(respond(armRelease, 200), "aarch64").release.installable());
        QVERIFY(Updater::interpret(respond(armRelease, 200), "arm64").release.installable());

        // A response cannot point the download anywhere else.
        QByteArray elsewhere = releaseJson("0.2.0");
        elsewhere.replace("https://github.com/last-refuge/omarchy-control/releases/download/", "https://evil.example/");
        QVERIFY(!Updater::interpret(respond(elsewhere, 200), "x86_64").release.installable());
        // Nor name a different file, even from the right place.
        QByteArray renamed = releaseJson("0.2.0");
        renamed.replace("\"name\":\"omarchy-control-0.2.0-1-x86_64.pkg.tar.zst\"", "\"name\":\"../../evil.pkg.tar.zst\"");
        QVERIFY(!Updater::interpret(respond(renamed, 200), "x86_64").release.installable());

        QCOMPARE(Updater::interpret(respond("{\"message\":\"Not Found\"}", 404), "x86_64").outcome, Updater::Check::NoReleases);
        QVERIFY(Updater::interpret(respond("{}", 403), "x86_64").error.contains("limiting"));
        QCOMPARE(Updater::interpret(respond("{}", 500), "x86_64").outcome, Updater::Check::Failed);
        QCOMPARE(Updater::interpret(respond("not json", 200), "x86_64").outcome, Updater::Check::Failed);
        CommandResult offline;
        offline.exitCode = 6;
        QVERIFY(Updater::interpret(offline, "x86_64").error.contains("connection"));
    }
    void updateCheckNeverInstalls() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        fakeGitHub(s, "0.2.0", "bytes", "bytes");
        s.command("sudo", "exit 0");
        QCOMPARE(Cli::run(s.env, {"update"}), 0);
        QCOMPARE(Cli::run(s.env, {"update", "check", "--json"}), 0);
        QVERIFY(s.calls().filter("sudo").isEmpty());
        QCOMPARE(s.calls().filter("curl").size(), 2);
        QCOMPARE(Cli::run(s.env, {"update", "nonsense"}), 1);
    }
    void updateInstallHandsAVerifiedPackageToPacman() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        fakeGitHub(s, "0.2.0", "real package", "real package");
        // Copies what it was given, because the download is cleaned up afterwards.
        s.command("sudo", "for last; do :; done\ncp \"$last\" '" + s.dir.path().toUtf8() + "/installed'\n");
        QCOMPARE(Cli::run(s.env, {"update", "install"}), 0);
        const QStringList sudo = s.calls().filter(QRegularExpression("^sudo "));
        QCOMPARE(sudo.size(), 1);
        const QString architecture = QSysInfo::currentCpuArchitecture() == "arm64" ? QStringLiteral("aarch64") : QSysInfo::currentCpuArchitecture();
        QVERIFY2(sudo.first().startsWith("sudo pacman -U " + s.env.appCacheDir + "/updates/omarchy-control-0.2.0-1-" + architecture + ".pkg.tar.zst"), qPrintable(sudo.first()));
        QVERIFY(!sudo.first().contains("--noconfirm"));
        QCOMPARE(readAll(s.dir.path() + "/installed"), QByteArray("real package"));
        QVERIFY(!QFile::exists(s.env.appCacheDir + "/updates"));
    }
    void updateInstallRefusesATamperedPackage() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        fakeGitHub(s, "0.2.0", "tampered", "real package");
        s.command("sudo", "exit 0");
        QCOMPARE(Cli::run(s.env, {"update", "install"}), 1);
        QVERIFY(s.calls().filter(QRegularExpression("^sudo ")).isEmpty());
        QVERIFY(!QFile::exists(s.env.appCacheDir + "/updates"));
    }
    void updateInstallLeavesTheCurrentVersionAlone() {
        QCoreApplication::setApplicationVersion("0.2.0");
        Sandbox s;
        fakeGitHub(s, "0.2.0", "bytes", "bytes");
        s.command("sudo", "exit 0");
        QCOMPARE(Cli::run(s.env, {"update", "install"}), 0);
        QVERIFY(s.calls().filter(QRegularExpression("^sudo ")).isEmpty());
        QVERIFY(s.calls().filter(".pkg.tar.zst").isEmpty());
        QCoreApplication::setApplicationVersion("0.1.0");
    }
    void updateInstallWaitsForPacman() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        fakeGitHub(s, "0.2.0", "bytes", "bytes");
        s.command("sudo", "exit 0");
        writeFile(s.env.sys("/var/lib/pacman/db.lck"), "");
        QCOMPARE(Cli::run(s.env, {"update", "install"}), 1);
        QVERIFY(s.calls().filter(QRegularExpression("^sudo ")).isEmpty());
        QVERIFY(s.calls().filter(".pkg.tar.zst").isEmpty());
    }
    void aFailedPacmanIsReportedAsAFailedUpdate() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        fakeGitHub(s, "0.2.0", "bytes", "bytes");
        s.command("sudo", "exit 1");
        QCOMPARE(Cli::run(s.env, {"update", "install"}), 1);
        QCOMPARE(s.calls().filter(QRegularExpression("^sudo ")).size(), 1);
    }
    void theAboutPageChecksInstallsAndRestarts() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        fakeDesktop(s);
        fakeGitHub(s, "0.2.0", "bytes", "bytes");
        s.command("omarchy-control", "exit 0");
        s.command("omarchy-launch-floating-terminal-with-presentation", "exit 0");
        s.command("bash", "exit 0");
        s.command("pacman", "echo 'omarchy-control 0.1.0-1'");
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));

        // About is the last tab, at the bottom of the list.
        QCOMPARE(backend.pages().last().toMap().value("id").toString(), QString("about"));

        // Every wider gap in the list has a line in it, so grouping never reads as a mistake.
        int separators = 0;
        std::function<void(QQuickItem *)> count = [&](QQuickItem *item) {
            if (item->objectName() == "navSeparator" && item->isVisible())
                ++separators;
            for (QQuickItem *child : item->childItems())
                count(child);
        };
        count(window->contentItem());
        QCOMPARE(separators, 3);

        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "about"), Q_ARG(QVariant, QString()));
        QTest::qWait(300);
        const auto find = [&](const QString &name) {
            QQuickItem *found = nullptr;
            std::function<void(QQuickItem *)> search = [&](QQuickItem *item) {
                if (item->objectName() == name)
                    found = item;
                for (QQuickItem *child : item->childItems())
                    if (!found)
                        search(child);
            };
            search(window->contentItem());
            return found;
        };
        const auto click = [&](const QString &name) {
            QQuickItem *button = find(name);
            QVERIFY2(button && button->isVisible(), qPrintable(name));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        };
        QVERIFY(find("aboutVersion")->property("text").toString().contains("0.1.0"));
        // Opening the page asks nothing of the network.
        QVERIFY(s.calls().filter("curl").isEmpty());
        QVERIFY(!find("installUpdate")->isVisible());

        click("checkUpdates");
        QTRY_COMPARE(backend.updates()->state(), QString("available"));
        QCOMPARE(backend.updates()->latestVersion(), QString("0.2.0"));
        QTRY_VERIFY(find("updateStatus")->property("text").toString().contains("0.2.0"));
        QTest::qWait(100);

        // Installing opens the terminal route and goes no further on its own.
        click("installUpdate");
        QTRY_VERIFY(backend.updates()->installStarted());
        QTRY_VERIFY(!s.calls().filter("omarchy-launch-floating-terminal-with-presentation omarchy-control update install").isEmpty());
        QVERIFY(s.calls().filter(QRegularExpression("^sudo ")).isEmpty());
        QCOMPARE(backend.updates()->state(), QString("available"));

        // Coming back with the same version still installed changes nothing.
        backend.windowActivated();
        QTest::qWait(300);
        QCOMPARE(backend.updates()->state(), QString("available"));

        // The window notices the finished update when it gets focus back.
        s.command("pacman", "echo 'omarchy-control 0.2.0-1'");
        backend.windowActivated();
        QTRY_COMPARE(backend.updates()->state(), QString("restart"));
        QTRY_VERIFY(find("restartApp")->isVisible());
        QVERIFY(!find("checkUpdates")->isVisible());
        QTest::qWait(100);
        QSignalSpy quit(&engine, &QQmlEngine::quit);
        click("restartApp");
        QTRY_COMPARE(quit.size(), 1);
        QTRY_VERIFY(!s.calls().filter("omarchy-control open --page about").isEmpty());
    }
    void aCheckReportsWhenThereIsNothingToFind() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        s.command("curl", "printf '{}\\n404'");
        CommandRunner runner(s.env);
        UpdateProvider updates(&runner);
        updates.check();
        QTRY_COMPARE(updates.state(), QString("none"));
        s.command("curl", "exit 6");
        updates.check();
        QTRY_COMPARE(updates.state(), QString("error"));
        QVERIFY(updates.error().contains("connection"));
        s.command("curl", "printf '{\"tag_name\":\"v0.1.0\"}\\n200'");
        updates.check();
        QTRY_COMPARE(updates.state(), QString("current"));
    }

    // ---- Command line ---------------------------------------------------------
    void cliRefusesConsequentialChangesWithoutYes() {
        Sandbox s;
        s.omarchy("case \"$*\" in dns) echo DHCP;; 'dns '*) touch \"$HOME/changed\";; esac");
        QCOMPARE(Cli::run(s.env, {"apply", "network.dns", "Cloudflare"}), 1);
        QVERIFY(!QFile::exists(s.env.home + "/changed"));
        QVERIFY(s.calls().filter("dns Cloudflare").isEmpty());
    }
    void cliExitCodesAreMeaningful() {
        Sandbox s;
        s.minimalHyprland();
        s.omarchy("case \"$*\" in version) echo 4.5.0-3;; 'hyprland corners') echo theme;; esac");
        QCOMPARE(Cli::run(s.env, {"get", "appearance.corners"}), 0);
        QCOMPARE(Cli::run(s.env, {"get", "no.such.setting"}), 1);
        QCOMPARE(Cli::run(s.env, {"plan", "appearance.corners", "sharp"}), 0);
        QCOMPARE(Cli::run(s.env, {"plan", "appearance.corners", "wavy"}), 1);
        QCOMPARE(Cli::run(s.env, {"not-a-command"}), 1);
        QCOMPARE(Cli::run(s.env, {"--help"}), 0);
        QCOMPARE(Cli::run(s.env, {"catalog", "--json"}), 0);
        QCOMPARE(Cli::run(s.env, {"search", "theme"}), 0);
        QCOMPARE(Cli::run(s.env, {"history"}), 0);
    }
    void cliApiDiscoveryDoesNotProbeOrChangeTheDesktop() {
        Sandbox s;
        s.minimalHyprland();
        s.omarchy("exit 0");
        const QString file = s.env.configHome + "/omarchy/shell.json";
        writeFile(file, "{\"idle\":{\"lock\":300}}\n");
        for (const QStringList &args : QList<QStringList>{{"commands"}, {"catalog"}, {"describe", "appearance.blur.size"},
                                                         {"search", "microphone"}, {"coverage", "--cli"}}) {
            const auto reply = CliApi::execute(s.env, QStringList{"--api-version", "1"} + args);
            QCOMPARE(reply.exitCode, 0);
            QCOMPARE(reply.body.value("apiVersion").toInt(), 1);
            QVERIFY(reply.body.value("error").isNull());
        }
        QVERIFY(s.calls().isEmpty());
        const auto literal = CliApi::execute(s.env, {"--api-version", "1", "search", "--", "--yes"});
        QCOMPARE(literal.exitCode, 0);
        QVERIFY(s.calls().isEmpty());
        QCOMPARE(readAll(file), QByteArray("{\"idle\":{\"lock\":300}}\n"));
        QVERIFY(!QFile::exists(s.env.appStateDir + "/history.jsonl"));
        QVERIFY(!QFileInfo::exists(s.env.home + "/.claude/skills"));
        const auto description = CliApi::execute(s.env, {"describe", "appearance.blur.size"}).body.value("data").toObject();
        QVERIFY(description.value("writable").toBool());
        QCOMPARE(description.value("schema").toObject().value("minimum").toInt(), 1);
        QCOMPARE(description.value("schema").toObject().value("maximum").toInt(), 20);
        QVERIFY(description.value("files").toArray().contains(s.env.configHome + "/hypr/omarchy-control-blur.lua"));
        QCOMPARE(description.value("availability").toObject().value("status").toString(), QString("not_checked"));
        const auto gap = CliApi::execute(s.env, {"describe", "display.layout"}).body.value("data").toObject();
        QCOMPARE(gap.value("classification").toString(), QString("api-cli"));
        QVERIFY(!gap.value("invocations").toArray().isEmpty());
        const auto ledger = CliApi::coverage(Catalog());
        QVERIFY(!ledger.value("complete").toBool());
        QCOMPARE(ledger.value("counts").toObject().value("cli-gap").toInt(), 0);
        QVERIFY(ledger.value("counts").toObject().value("legacy-cli").toInt() > 0);
        QSet<QString> operationIds;
        for (const auto &row : ledger.value("entries").toArray())
            if (row.toObject().value("surface") == "gui-operation") operationIds.insert(row.toObject().value("id").toString());
        for (const auto &row : ledger.value("entries").toArray())
            for (const auto &operation : row.toObject().value("operationIds").toArray())
                QVERIFY2(operationIds.contains(operation.toString()), qPrintable(operation.toString()));
        const auto native = CliApi::execute(s.env, {"describe", "native.displays"}).body.value("data").toObject();
        QVERIFY(native.value("operationIds").toArray().contains("display.layout"));
        const auto target = CliApi::execute(s.env, {"describe", "displays.brightness"}).body.value("data").toObject();
        QCOMPARE(target.value("target").toObject().value("enumeration").toArray(), QJsonArray({"display", "status"}));
    }
    void crashMutingHasExplicitPlansAndGuardedReadback() {
        Sandbox s;
        for (const auto &helper : {"omarchy-crash-mute", "omarchy-toggle", "omarchy-toggle-enabled"}) s.command(helper, "exit 0");
        s.omarchy("[[ \"$1 $2 $3\" == 'crash mute --' ]] || exit 91\n"
                  "dir=\"$HOME/.local/state/omarchy/toggles/crash-ignore\"\n"
                  "if [[ $5 == on ]]; then mkdir -p \"$dir\"; touch \"$dir/$4\"; else rm -f \"$dir/$4\"; fi\n");
        auto api = [&](const QStringList &args) { return CliApi::execute(s.env, QStringList{"preferences"} + args).body; };
        const QString path = s.env.stateHome + "/omarchy/toggles/crash-ignore/hyprland";
        QVERIFY(api({"crash-mutes"}).value("data").toObject().value("items").toArray().isEmpty());
        QVERIFY(!QFileInfo::exists(s.env.stateHome + "/omarchy/toggles/crash-ignore"));
        QCOMPARE(api({"crash-mute", "hyprland"}).value("data").toObject().value("muted"), QJsonValue(false));
        QCOMPARE(api({"crash-mute", "hyprland", "true", "--dry-run"}).value("outcome").toString(), QString("planned"));
        QVERIFY(s.calls().isEmpty());
        QVERIFY(api({"crash-mute", "hyprland", "true"}).value("verified").toBool());
        QCOMPARE(readAll(path), QByteArray());
        QCOMPARE(api({"crash-mutes"}).value("data").toObject().value("items").toArray().first().toObject().value("program").toString(), QString("hyprland"));
        const int calls = s.calls().size();
        QVERIFY(api({"crash-mute", "hyprland", "true"}).value("verified").toBool());
        QCOMPARE(s.calls().size(), calls);
        QVERIFY(api({"crash-mute", "hyprland", "false"}).value("verified").toBool());
        QVERIFY(!QFileInfo::exists(path));
        for (const auto &tx : ChangeCoordinator(s.env).history()) QVERIFY(!tx.undoable());
        for (const auto &value : {"TRUE", "1", "yes"}) QVERIFY(!api({"crash-mute", "hyprland", value}).value("ok").toBool());
        for (const auto &program : {"../outside", ".", "..", "two words"}) QVERIFY(!api({"crash-mute", program, "true"}).value("ok").toBool());
        QVERIFY(!api({"crash-mute", "hyprland", "--dry-run"}).value("ok").toBool());
        const auto emptyName = api({"crash-mute", ""});
        QCOMPARE(emptyName.value("error").toObject().value("code").toString(), QString("invalid_arguments"));
        QVERIFY(!api({"crash-mute", "hyprland", ""}).value("ok").toBool());
        QCOMPARE(api({"crash-mute", "hyprland", "yes"}).value("outcome").toString(), QString("blocked"));
        Env strict = s.env; strict.enforceCompat = true;
        QCOMPARE(CliApi::execute(strict, {"preferences", "crash-mute", "hyprland", "true", "--dry-run"}).body.value("error").toObject().value("code").toString(), QString("compatibility_blocked"));
        QCOMPARE(CliApi::execute(s.env, {"describe", "crash.mute"}).body.value("data").toObject().value("nativeOperations").toArray(), QJsonArray{"preferences.crash-mutes"});
        const auto matches = CliApi::execute(s.env, {"search", "mute crash"}).body.value("data").toArray();
        QVERIFY(std::any_of(matches.begin(), matches.end(), [](const auto &row) { return row.toObject().value("id") == "crash.mute"; }));
    }
    void crashMutingPreservesUnsafeMarkers_data() {
        backgroundIntroductionsPreserveUnsafeAndEditedMarkers_data();
    }
    void crashMutingPreservesUnsafeMarkers() {
        QFETCH(QString, mode);
        const QString kind = mode;
        Sandbox s;
        const QString directory = s.env.stateHome + "/omarchy/toggles/crash-ignore";
        const QString path = directory + "/app";
        QDir().mkpath(directory);
        const QString outside = s.env.home + "/outside";
        writeFile(outside, "custom");
        if (kind == "nonempty") writeFile(path, "custom");
        else if (kind == "symlink") QVERIFY(QFile::link(outside, path));
        else if (kind == "dangling") QVERIFY(QFile::link(outside + "-missing", path));
        else if (kind == "hardlink") QVERIFY(::link(QFile::encodeName(outside).constData(), QFile::encodeName(path).constData()) == 0);
        else if (kind == "directory") QVERIFY(QDir().mkpath(path));
        else if (kind == "shared-parent") QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther));
        const auto reply = CliApi::execute(s.env, {"preferences", "crash-mute", "app", "false"}).body;
        QCOMPARE(reply.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QCOMPARE(readAll(outside), QByteArray("custom")); QVERIFY(s.calls().isEmpty());
    }
    void crashMutingRetainsBackupsForAnUnconfirmedHelper() {
        Sandbox s;
        for (const auto &helper : {"omarchy-crash-mute", "omarchy-toggle", "omarchy-toggle-enabled"}) s.command(helper, "exit 0");
        s.omarchy("mkdir -p \"$HOME/.local/state/omarchy/toggles/crash-ignore\"\n"
                  "echo custom > \"$HOME/.local/state/omarchy/toggles/crash-ignore/app\"\nexit 0\n");
        const auto reply = CliApi::execute(s.env, {"preferences", "crash-mute", "app", "true"}).body;
        QCOMPARE(reply.value("outcome").toString(), QString("partial")); QVERIFY(!reply.value("verified").toBool());
        QCOMPARE(readAll(s.env.stateHome + "/omarchy/toggles/crash-ignore/app"), QByteArray("custom\n"));
        const auto tx = ChangeCoordinator(s.env).history().first(); QVERIFY(!tx.undoable()); QVERIFY(!tx.files.isEmpty());
    }
    void backgroundIntroductionsHaveExplicitVerifiedChangesAndUndo() {
        Sandbox s;
        s.command("omarchy-theme-bg-intro", "exit 0");
        s.omarchy("case \"$*\" in\n"
                  " 'theme bg intro disable') mkdir -p \"$HOME/.local/state/omarchy/toggles\"; touch \"$HOME/.local/state/omarchy/toggles/background-intros-off\";;\n"
                  " 'theme bg intro enable') rm -f \"$HOME/.local/state/omarchy/toggles/background-intros-off\";;\n"
                  "esac\nexit 1 # a stopped shell may reject the notification after the write\n");
        AppCore core(s.env);
        const QString id = "appearance.background.intros";
        QCOMPARE(readSetting(core.engine, id).value, QString("true"));
        const auto plan = core.engine.plan(id, "false");
        QVERIFY(plan.ok);
        QVERIFY(plan.files.contains(s.env.stateHome + "/omarchy/toggles/background-intros-off"));
        QVERIFY(s.calls().isEmpty());
        const auto off = applySetting(core.engine, id, "false");
        QCOMPARE(off.phase, QString("committed"));
        QVERIFY(off.undoable());
        QCOMPARE(readSetting(core.engine, id).value, QString("false"));
        const int count = s.calls().size();
        QCOMPARE(applySetting(core.engine, id, "false").phase, QString("committed"));
        QCOMPARE(s.calls().size(), count);
        const auto on = applySetting(core.engine, id, "true");
        QCOMPARE(on.phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, id).value, QString("true"));
        bool finished = false;
        core.engine.undo(on.id, [&](const auto &tx) { QCOMPARE(tx.phase, QString("committed")); finished = true; });
        QTRY_VERIFY(finished);
        QCOMPARE(readSetting(core.engine, id).value, QString("false"));
        for (const auto &call : s.calls()) QVERIFY(!call.contains("intro toggle"));
        const auto route = CliApi::execute(s.env, {"describe", "appearance.background.intro"}).body.value("data").toObject();
        QCOMPARE(route.value("nativeSetting").toString(), id);
        Env strict = s.env; strict.enforceCompat = true;
        s.command("omarchy-theme-bg-intro", "exit 0 # changed helper");
        AppCore guarded(strict);
        QCOMPARE(guarded.engine.plan(id, "true").errorCode, QString("compatibility_blocked"));
    }
    void backgroundIntroductionsPreserveUnsafeAndEditedMarkers_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"nonempty", "symlink", "dangling", "hardlink", "directory", "shared-parent"}) QTest::newRow(mode) << QString(mode);
    }
    void backgroundIntroductionsPreserveUnsafeAndEditedMarkers() {
        QFETCH(QString, mode);
        Sandbox s;
        s.command("omarchy-theme-bg-intro", "exit 0");
        const QString flag = s.env.stateHome + "/omarchy/toggles/background-intros-off";
        const QString other = s.env.home + "/other";
        writeFile(flag, {}); writeFile(other, {});
        if (mode == "nonempty") writeFile(flag, "personal content\n");
        if (mode == "directory") { QFile::remove(flag); QVERIFY(QDir().mkdir(flag)); }
        if (mode == "symlink" || mode == "dangling") { QFile::remove(flag); QVERIFY(QFile::link(mode == "symlink" ? other : other + "-missing", flag)); }
        if (mode == "hardlink") { QFile::remove(flag); QCOMPARE(::link(QFile::encodeName(other).constData(), QFile::encodeName(flag).constData()), 0); }
        if (mode == "shared-parent") QVERIFY(QFile::setPermissions(QFileInfo(flag).absolutePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther));
        AppCore core(s.env);
        QCOMPARE(core.engine.plan("appearance.background.intros", "true").errorCode, QString("ownership_conflict"));
        QCOMPARE(readSetting(core.engine, "appearance.background.intros").status, QString("unavailable"));
        const auto tx = applySetting(core.engine, "appearance.background.intros", "true");
        QCOMPARE(tx.phase, QString("failed"));
        QVERIFY(s.calls().isEmpty());
        if (mode == "nonempty") QCOMPARE(readAll(flag), QByteArray("personal content\n"));
        QVERIFY(QFileInfo::exists(other));
        QVERIFY(!QFileInfo::exists(other + "-missing"));
    }
    void queuedBackgroundIntroductionWriteRefusesAnInterveningEdit() {
        Sandbox s; s.command("omarchy-theme-bg-intro", "exit 0"); s.omarchy("exit 0"); AppCore core(s.env);
        ChangeCoordinator::Done release;
        ChangeCoordinator::Request blocker; blocker.settingId = "fixture";
        blocker.apply = [&](auto done) { release = done; };
        core.coordinator.submit(std::move(blocker), [](const auto &) {});
        QVERIFY(bool(release));
        bool finished = false;
        core.engine.apply("appearance.background.intros", "false", [&](const auto &tx) { QCOMPARE(tx.phase, QString("failed")); finished = true; });
        QVERIFY(!finished);
        const QString flag = s.env.stateHome + "/omarchy/toggles/background-intros-off";
        writeFile(flag, "later edit\n");
        release(true, {});
        QTRY_VERIFY(finished);
        QCOMPARE(readAll(flag), QByteArray("later edit\n"));
        QVERIFY(s.calls().isEmpty());
    }
    void backgroundIntroductionFailurePreservesAnEditDuringTheHelper() {
        Sandbox s; s.command("omarchy-theme-bg-intro", "exit 0");
        s.omarchy("mkdir -p \"$HOME/.local/state/omarchy/toggles\"\nprintf 'later content\\n' > \"$HOME/.local/state/omarchy/toggles/background-intros-off\"\nexit 1\n");
        AppCore core(s.env);
        const auto tx = applySetting(core.engine, "appearance.background.intros", "false");
        QCOMPARE(tx.phase, QString("partial"));
        QVERIFY(!tx.undoable());
        QCOMPARE(readAll(s.env.stateHome + "/omarchy/toggles/background-intros-off"), QByteArray("later content\n"));
        QCOMPARE(s.calls().size(), 1);
    }
    void powerPercentageOptionPlansAndVerifiesAnExactInstance() {
        ShellDesk desk; desk.powerWidget();
        const QString selector = desk.selector("omarchy.power"); QVERIFY(!selector.isEmpty());
        auto snapshot = desk.api({"bar", "status"}).value("data").toObject().value("bar").toObject();
        auto widget = snapshot.value("widgets").toArray().last().toObject();
        QCOMPARE(widget.value("options").toObject().value("showPercentage").toObject().value("value"), QJsonValue(false));
        QVERIFY(widget.value("options").toObject().value("showPercentage").toObject().value("inherited").toBool());
        const QByteArray before = readAll(desk.config());
        const auto plan = desk.api({"widget", "option", selector, "showPercentage", "true", "--dry-run"});
        QVERIFY(plan.value("ok").toBool()); QCOMPARE(plan.value("outcome").toString(), QString("planned"));
        QVERIFY(plan.value("data").toObject().value("details").toObject().value("valueBefore").isNull());
        QCOMPARE(readAll(desk.config()), before);
        const auto applied = desk.api({"widget", "option", selector, "showPercentage", "true"});
        QVERIFY(applied.value("ok").toBool()); QVERIFY(applied.value("verified").toBool());
        const auto entry = desk.object().value("bar").toObject().value("layout").toObject().value("right").toArray().last().toObject();
        QCOMPARE(entry.value("showPercentage"), QJsonValue(true)); QCOMPARE(entry.value("customOption").toString(), QString("preserve"));
        QCOMPARE(desk.object().value("custom").toObject().value("keep"), QJsonValue(true));
        const int calls = desk.s.calls().filter("omarchy bar set").size();
        const auto stale = desk.api({"widget", "option", selector, "showPercentage", "false"});
        QCOMPARE(stale.value("error").toObject().value("code").toString(), QString("stale_target"));
        QCOMPARE(desk.s.calls().filter("omarchy bar set").size(), calls);
        const auto operation = CliApi::execute(desk.s.env, {"describe", "battery.percentage"}).body.value("data").toObject();
        QVERIFY(operation.value("nativeOperations").toArray().contains("widgets.option"));
    }
    void powerPercentageOptionRefusesCustomAndUnavailableTargets_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("code");
        for (const auto &mode : {"bare", "custom", "shadow-power"}) QTest::newRow(mode) << QString(mode) << QString("unsupported_configuration");
        QTest::newRow("plugin-down") << QString("plugin-down") << QString("read_unavailable");
    }
    void powerPercentageOptionRefusesCustomAndUnavailableTargets() {
        QFETCH(QString, mode); QFETCH(QString, code);
        ShellDesk desk;
        desk.powerWidget(mode == "bare" ? QJsonValue("omarchy.power") : mode == "custom" ? QJsonValue(QJsonObject{{"id", "omarchy.power"}, {"showPercentage", "yes"}}) : QJsonValue(QJsonObject{{"id", "omarchy.power"}}));
        const QString selector = desk.selector("omarchy.power");
        if (mode == "shadow-power" || mode == "plugin-down") desk.mode(mode);
        const QByteArray before = readAll(desk.config());
        const auto result = desk.api({"widget", "option", selector, "showPercentage", "true"});
        QVERIFY(!result.value("ok").toBool()); QCOMPARE(result.value("error").toObject().value("code").toString(), code);
        QCOMPARE(readAll(desk.config()), before); QVERIFY(desk.s.calls().filter("omarchy bar set").isEmpty());
    }
    void powerPercentageOptionRequiresReadbackOfThePreservedLayout_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"noop", "lose-options", "change-bar", "memory-only", "disk-only"}) QTest::newRow(mode) << QString(mode);
    }
    void powerPercentageOptionRequiresReadbackOfThePreservedLayout() {
        QFETCH(QString, mode);
        ShellDesk desk; desk.powerWidget(); const QString selector = desk.selector("omarchy.power"); desk.mode(mode);
        const auto result = desk.api({"widget", "option", selector, "showPercentage", "true"});
        QVERIFY(!result.value("ok").toBool()); QVERIFY(!result.value("verified").toBool());
        QCOMPARE(result.value("outcome").toString(), QString("partial"));
        ChangeCoordinator history(desk.s.env); QVERIFY(!history.history().first().undoable());
        QVERIFY(!history.history().first().files.isEmpty());
    }
    void powerPercentageOptionRejectsInvalidValuesBeforeProbing() {
        ShellDesk desk;
        const QString selector = desk.selector("omarchy.clock"); const auto count = desk.s.calls().size();
        for (const auto &value : {"1", "TRUE", "yes", "{\"enabled\":true}"}) {
            const auto result = desk.api({"widget", "option", selector, "showPercentage", value});
            QCOMPARE(result.value("error").toObject().value("code").toString(), QString("invalid_value"));
        }
        QCOMPARE(desk.s.calls().size(), count);
    }
    void cliApiResetUndoAndHistoryShareGuardedEngine() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString file = s.env.configHome + "/omarchy/shell.json";
        writeFile(file, "{\"idle\":{\"lock\":300}}\n");
        auto api = [&](QStringList args) { return CliApi::execute(s.env, args + QStringList{"--api-version", "1"}); };
        auto applied = api({"apply", "power.idle.lock", "600"});
        QCOMPARE(applied.exitCode, 0);
        const QString id = applied.body.value("data").toObject().value("id").toString();
        QVERIFY(!id.isEmpty());
        const auto history = api({"history"});
        QCOMPARE(history.exitCode, 0);
        QVERIFY(!history.body.value("data").toArray().isEmpty());
        QCOMPARE(api({"undo", id}).exitCode, 0);
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("300"));
        QCOMPARE(api({"undo", id}).body.value("error").toObject().value("code").toString(), QString("undo_unavailable"));
        QCOMPARE(api({"apply", "power.idle.lock", "600"}).exitCode, 0);
        QCOMPARE(api({"reset", "power.idle.lock"}).exitCode, 0);
        QVERIFY(!QJsonDocument::fromJson(readAll(file)).object().value("idle").toObject().contains("lock"));
        QCOMPARE(api({"reset", "native.displays"}).exitCode, 1);
        QCOMPARE(api({"reset", "power.idle.lock", "--dry-run"}).body.value("error").toObject().value("code").toString(), QString("invalid_arguments"));
        Env strict = s.env; strict.enforceCompat = true;
        s.minimalHyprland();
        s.command("hyprctl", "echo '{\"version\":\"9.99\"}'");
        const auto refused = CliApi::execute(strict, {"reset", "appearance.border", "--api-version", "1"});
        QCOMPARE(refused.exitCode, 1);
        QVERIFY(refused.body.value("data").toObject().value("message").toString().contains("changed since"));
        QVERIFY(!QFileInfo::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }
    void cliApiReadsLiveChoicesAndTheRequestedDisplay() {
        Sandbox s;
        s.minimalHyprland();
        s.omarchy("case \"$*\" in 'font current') echo 'Font A';; 'font list') printf 'Font A\\nFont B\\n';; 'brightness display --monitor DP-1') echo 44;; esac");
        const auto font = CliApi::execute(s.env, {"--api-version", "1", "get", "appearance.font"});
        QCOMPARE(font.exitCode, 0);
        QCOMPARE(font.body.value("data").toObject().value("value").toString(), QString("Font A"));
        QCOMPARE(font.body.value("data").toObject().value("choices").toArray().size(), 2);
        const auto description = CliApi::execute(s.env, {"describe", "appearance.font", "--live"});
        QCOMPARE(description.exitCode, 0);
        QCOMPARE(description.body.value("data").toObject().value("liveChoices").toArray().size(), 2);
        const auto display = CliApi::execute(s.env, {"--api-version", "1", "get", "displays.brightness@DP-1"});
        QCOMPARE(display.exitCode, 0);
        QCOMPARE(display.body.value("data").toObject().value("id").toString(), QString("displays.brightness@DP-1"));
        QCOMPARE(display.body.value("data").toObject().value("value").toString(), QString("44"));
        QVERIFY(s.calls().contains("omarchy brightness display --monitor DP-1"));
        const auto target = CliApi::execute(s.env, {"describe", "displays.brightness@DP-1"}).body.value("data").toObject();
        QCOMPARE(target.value("target").toObject().value("id").toString(), QString("DP-1"));
        const auto numeric = CliApi::execute(s.env, {"--api-version", "1", "plan", "input.sensitivity", "-1e-2"});
        QCOMPARE(numeric.exitCode, 0);
    }
    void cliApiPlanApplyAndReadUseTheSharedEngine() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString file = s.env.configHome + "/omarchy/shell.json";
        writeFile(file, "{\"idle\":{\"lock\":300}}\n");
        const auto plan = CliApi::execute(s.env, {"--api-version", "1", "plan", "power.idle.lock", "600"});
        QCOMPARE(plan.exitCode, 0);
        QCOMPARE(plan.body.value("outcome").toString(), QString("planned"));
        QVERIFY(!plan.body.value("verified").toBool());
        QCOMPARE(readAll(file), QByteArray("{\"idle\":{\"lock\":300}}\n"));
        ChangeCoordinator coordinator(s.env);
        QVERIFY(coordinator.history().isEmpty());
        const auto applied = CliApi::execute(s.env, {"--api-version", "1", "apply", "power.idle.lock", "600"});
        QCOMPARE(applied.exitCode, 0);
        QVERIFY(applied.body.value("verified").toBool());
        QCOMPARE(applied.body.value("outcome").toString(), QString("committed"));
        QVERIFY(!applied.body.value("data").toObject().value("id").toString().isEmpty());
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("600"));
        QCOMPARE(coordinator.history().size(), 1);
        const auto bad = CliApi::execute(s.env, {"--api-version", "1", "apply", "power.idle.lock", "nonsense"});
        QCOMPARE(bad.exitCode, 1);
        QCOMPARE(bad.body.value("error").toObject().value("code").toString(), QString("invalid_value"));
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("600"));
        QCOMPARE(coordinator.history().size(), 1);
    }
    void cliApiRejectsBadSyntaxBeforeAnyProbe_data() {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("code");
        QTest::newRow("missing-version") << QStringList{"apply", "power.idle.lock", "600", "--api-version"} << QString("invalid_arguments");
        QTest::newRow("new-version") << QStringList{"apply", "power.idle.lock", "600", "--api-version", "2"} << QString("unsupported_api_version");
        QTest::newRow("typo") << QStringList{"apply", "power.idle.lock", "600", "--yess"} << QString("invalid_arguments");
        QTest::newRow("extra-value") << QStringList{"apply", "power.idle.lock", "600", "700"} << QString("invalid_arguments");
        QTest::newRow("missing-value") << QStringList{"apply", "power.idle.lock"} << QString("invalid_arguments");
        QTest::newRow("repeated-flag") << QStringList{"apply", "power.idle.lock", "600", "--yes", "--yes"} << QString("invalid_arguments");
        QTest::newRow("wrong-flag") << QStringList{"catalog", "--yes"} << QString("invalid_arguments");
        QTest::newRow("unsupported-family") << QStringList{"profile", "apply", "some-plan"} << QString("unsupported_api_operation");
        QTest::newRow("unknown") << QStringList{"typo"} << QString("unknown_command");
        QTest::newRow("unknown-id") << QStringList{"apply", "no.such.id", "1"} << QString("unknown_id");
        QTest::newRow("page") << QStringList{"apply", "native.audio.output", "x"} << QString("not_a_setting");
        QTest::newRow("wrong-target") << QStringList{"get", "displays.brightness@../../bad"} << QString("unknown_id");
        QTest::newRow("audio-status-dry-run") << QStringList{"audio", "status", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("audio-extra") << QStringList{"audio", "output", "default", "x", "extra"} << QString("invalid_arguments");
        QTest::newRow("audio-number") << QStringList{"audio", "input", "volume", "mic", "1.5"} << QString("invalid_value");
        QTest::newRow("audio-yes") << QStringList{"audio", "input", "mute", "mic", "true", "--yes"} << QString("invalid_arguments");
        QTest::newRow("bt-address") << QStringList{"bluetooth", "connect", "AA:BB;exit"} << QString("invalid_value");
        QTest::newRow("bt-power") << QStringList{"bluetooth", "power", "on"} << QString("invalid_value");
        QTest::newRow("service-scope") << QStringList{"services", "global", "status"} << QString("invalid_arguments");
        QTest::newRow("service-status-dry") << QStringList{"services", "user", "status", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("service-action") << QStringList{"services", "user", "mask", "agent.service"} << QString("invalid_arguments");
        QTest::newRow("service-option-target") << QStringList{"services", "user", "start", "--", "--bad.service"} << QString("invalid_value");
        QTest::newRow("service-template") << QStringList{"services", "user", "start", "agent@.service"} << QString("invalid_value");
        QTest::newRow("startup-extra") << QStringList{"startup", "list", "extra"} << QString("invalid_arguments");
        QTest::newRow("startup-path") << QStringList{"startup", "enable", "../bad.desktop"} << QString("invalid_value");
        QTest::newRow("startup-yes") << QStringList{"startup", "enable", "agent.desktop", "--yes"} << QString("invalid_arguments");
        QTest::newRow("apps-list-dry") << QStringList{"apps", "list", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("apps-id") << QStringList{"apps", "launch", "../evil.desktop"} << QString("invalid_value");
        QTest::newRow("apps-yes") << QStringList{"apps", "launch", "a.desktop", "--yes"} << QString("invalid_arguments");
        QTest::newRow("mime-number") << QStringList{"mime", "set", "text/plain", "../evil.desktop"} << QString("invalid_value");
        QTest::newRow("mime-key") << QStringList{"mime", "set", "text/plain\nx=x", "a.desktop"} << QString("invalid_value");
        QTest::newRow("mime-read-dry") << QStringList{"mime", "get", "text/plain", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("system-scope") << QStringList{"system", "unknown"} << QString("invalid_arguments");
        QTest::newRow("system-dry") << QStringList{"system", "status", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("system-yes") << QStringList{"system", "status", "--yes"} << QString("invalid_arguments");
        QTest::newRow("diagnostics-action") << QStringList{"diagnostics", "upload"} << QString("invalid_arguments");
        QTest::newRow("diagnostics-extra") << QStringList{"diagnostics", "export", "/tmp/report"} << QString("invalid_arguments");
        QTest::newRow("diagnostics-yes") << QStringList{"diagnostics", "export", "--yes"} << QString("invalid_arguments");
        QTest::newRow("shortcut-list-extra") << QStringList{"shortcut", "list", "extra"} << QString("invalid_arguments");
        QTest::newRow("shortcut-check-missing") << QStringList{"shortcut", "check"} << QString("invalid_arguments");
        QTest::newRow("shortcut-read-dry") << QStringList{"shortcut", "list", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("shortcut-read-yes") << QStringList{"shortcut", "check", "SUPER + F", "--yes"} << QString("invalid_arguments");
        QTest::newRow("shortcut-invalid-add") << QStringList{"shortcut", "add", "F", "Files", "files"} << QString("invalid_value");
        QTest::newRow("rule-types-extra") << QStringList{"rule", "types", "extra"} << QString("invalid_arguments");
        QTest::newRow("rule-validate-missing") << QStringList{"rule", "validate", "firefox"} << QString("invalid_arguments");
        QTest::newRow("rule-validate-dry") << QStringList{"rule", "validate", "firefox", "float", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("rule-unguarded-remove") << QStringList{"rule", "remove", "0"} << QString("invalid_value");
        QTest::newRow("shortcut-replace-read") << QStringList{"shortcut", "list", "--replace"} << QString("invalid_arguments");
        QTest::newRow("rule-replace") << QStringList{"rule", "add", "x", "float", "--replace"} << QString("invalid_arguments");
        QTest::newRow("shortcut-command-control") << QStringList{"shortcut", "add", "SUPER + F", "Files", "files\nother"} << QString("invalid_value");
        QTest::newRow("shortcut-numeric-code") << QStringList{"shortcut", "add", "SUPER + 42", "Files", "files"} << QString("invalid_value");
        QTest::newRow("plugin-read-dry") << QStringList{"plugin", "list", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("plugin-path") << QStringList{"plugin", "enable", "../evil"} << QString("invalid_value");
        QTest::newRow("plugin-yes") << QStringList{"plugin", "disable", "omarchy.clock", "--yes"} << QString("invalid_arguments");
        QTest::newRow("plugin-clone-id") << QStringList{"plugin", "clone", "../clock"} << QString("invalid_value");
        QTest::newRow("bar-read-dry") << QStringList{"bar", "status", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("dock-path") << QStringList{"dock", "pin", "../evil.desktop"} << QString("invalid_value");
        QTest::newRow("widget-fraction") << QStringList{"widget", "move", "left:0@" + QString(64,'a'), "right", "1.5"} << QString("invalid_value");
        QTest::newRow("widget-unguarded") << QStringList{"widget", "move", "omarchy.clock", "right", "1"} << QString("invalid_value");
        QTest::newRow("widget-negative") << QStringList{"widget", "move", "left:0@" + QString(64,'a'), "right", "-1"} << QString("invalid_value");
        QTest::newRow("icons-missing-id") << QStringList{"icons", "samples"} << QString("invalid_arguments");
        QTest::newRow("icons-extra") << QStringList{"icons", "samples", "Pack", "extra"} << QString("invalid_arguments");
        QTest::newRow("icons-path") << QStringList{"icons", "search", "../Pack"} << QString("invalid_value");
        QTest::newRow("icons-query-control") << QStringList{"icons", "search", "Pack", "bad\nquery"} << QString("invalid_value");
        QTest::newRow("icons-query-bound") << QStringList{"icons", "search", "Pack", QString(513, 'x')} << QString("invalid_value");
        QTest::newRow("icons-dry") << QStringList{"icons", "samples", "Pack", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("icons-yes") << QStringList{"icons", "search", "Pack", "--yes"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-zero") << QStringList{"icons", "search", "Pack", "--limit", "0"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-overflow") << QStringList{"icons", "search", "Pack", "--limit", "9999999999999999999999"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-large") << QStringList{"icons", "search", "Pack", "--limit", "301"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-fraction") << QStringList{"icons", "search", "Pack", "--limit", "1.5"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-negative") << QStringList{"icons", "search", "Pack", "--limit", "-1"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-missing") << QStringList{"icons", "search", "Pack", "--limit"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-repeated") << QStringList{"icons", "search", "Pack", "--limit", "2", "--limit", "3"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-wrong-action") << QStringList{"icons", "samples", "Pack", "--limit", "2"} << QString("invalid_arguments");
        QTest::newRow("icons-limit-wrong-family") << QStringList{"catalog", "--limit", "2"} << QString("invalid_arguments");
        QTest::newRow("icons-legacy-mutation") << QStringList{"icons", "import", "/tmp/pack", "--yes"} << QString("unsupported_api_operation");
        QTest::newRow("network-missing") << QStringList{"network"} << QString("invalid_arguments");
        QTest::newRow("network-action") << QStringList{"network", "activate"} << QString("invalid_arguments");
        QTest::newRow("network-extra") << QStringList{"network", "devices", "wlan0"} << QString("invalid_arguments");
        QTest::newRow("network-dry") << QStringList{"network", "status", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("network-yes") << QStringList{"network", "status", "--yes"} << QString("invalid_arguments");
        QTest::newRow("network-limit") << QStringList{"network", "wifi", "--limit", "3"} << QString("invalid_arguments");
        QTest::newRow("scan-extra") << QStringList{"network", "rescan", "wlan0", "wlan1"} << QString("invalid_arguments");
        QTest::newRow("scan-path") << QStringList{"network", "rescan", "../x"} << QString("invalid_value");
        QTest::newRow("scan-option-device") << QStringList{"network", "rescan", "--", "--help"} << QString("invalid_value");
        QTest::newRow("scan-control") << QStringList{"network", "rescan", "wlan\n0"} << QString("invalid_value");
        QTest::newRow("scan-long-name") << QStringList{"network", "rescan", QString(16,'a')} << QString("invalid_value");
        QTest::newRow("scan-yes") << QStringList{"network", "rescan", "--yes"} << QString("invalid_arguments");
        QTest::newRow("radio-missing") << QStringList{"network", "radio"} << QString("invalid_arguments");
        QTest::newRow("radio-extra") << QStringList{"network", "radio", "true", "false"} << QString("invalid_arguments");
        QTest::newRow("radio-not-bool") << QStringList{"network", "radio", "on"} << QString("invalid_value");
        QTest::newRow("radio-empty") << QStringList{"network", "radio", ""} << QString("invalid_value");
        QTest::newRow("radio-upper") << QStringList{"network", "radio", "TRUE"} << QString("invalid_value");
        QTest::newRow("activate-uuid") << QStringList{"network", "activate", "bad", "wlan0"} << QString("invalid_value");
        QTest::newRow("activate-interface") << QStringList{"network", "activate", "11111111-1111-1111-1111-111111111111", "../dev"} << QString("invalid_value");
        QTest::newRow("disconnect-missing") << QStringList{"network", "disconnect"} << QString("invalid_arguments");
        QTest::newRow("disconnect-dash") << QStringList{"network", "disconnect", "--", "-bad"} << QString("invalid_value");
        QTest::newRow("preview-missing") << QStringList{"network", "preview"} << QString("invalid_arguments");
        QTest::newRow("preview-keep-missing") << QStringList{"network", "preview", "keep"} << QString("invalid_arguments");
        QTest::newRow("preview-bad-token") << QStringList{"network", "preview", "revert", "../x"} << QString("invalid_value");
        QTest::newRow("preview-status-dry") << QStringList{"network", "preview", "status", "--dry-run"} << QString("invalid_arguments");
        QTest::newRow("preview-status-yes") << QStringList{"network", "preview", "status", "--yes"} << QString("invalid_arguments");
    }
    void cliApiRejectsBadSyntaxBeforeAnyProbe() {
        QFETCH(QStringList, args);
        QFETCH(QString, code);
        Sandbox s;
        s.omarchy("exit 0");
        const auto reply = CliApi::execute(s.env, args);
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), code);
        QVERIFY(!reply.body.value("verified").toBool());
        QVERIFY(s.calls().isEmpty());
    }
    void cliApiHonorsOwnershipConfirmationAndCompatibility() {
        Sandbox s;
        s.minimalHyprland();
        s.omarchy("case \"$*\" in 'font list') echo Test;; 'font current') echo Test;; esac");
        const QString module = s.env.configHome + "/hypr/omarchy-control.lua";
        const auto confirm = CliApi::execute(s.env, {"--api-version", "1", "apply", "input.repeat_rate", "33"});
        QCOMPARE(confirm.exitCode, 1);
        QCOMPARE(confirm.body.value("error").toObject().value("code").toString(), QString("confirmation_required"));
        QVERIFY(!QFileInfo::exists(module));
        writeFile(module, "-- personal Lua\n");
        const auto owned = CliApi::execute(s.env, {"--api-version", "1", "apply", "input.repeat_rate", "33", "--yes"});
        QCOMPARE(owned.exitCode, 1);
        QCOMPARE(owned.body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QCOMPARE(readAll(module), QByteArray("-- personal Lua\n"));
        Env strict = s.env;
        strict.enforceCompat = true;
        s.command("omarchy-font-set", "exit 0 # changed since review");
        const auto compatibility = CliApi::execute(strict, {"--api-version", "1", "apply", "appearance.font", "Test", "--yes"});
        QCOMPARE(compatibility.exitCode, 1);
        QCOMPARE(compatibility.body.value("error").toObject().value("code").toString(), QString("compatibility_blocked"));
        QVERIFY(s.calls().filter("omarchy font set").isEmpty());
    }
    void cliApiHelpAndFailedVerificationNeverClaimAChange() {
        Sandbox s;
        s.minimalHyprland();
        s.omarchy("case \"$*\" in 'hyprland corners') echo sharp;; esac");
        const auto help = CliApi::execute(s.env, {"--api-version", "1", "apply", "appearance.corners", "rounded", "--help"});
        QCOMPARE(help.exitCode, 0);
        QVERIFY(s.calls().isEmpty());
        const auto reply = CliApi::execute(s.env, {"--api-version", "1", "apply", "appearance.corners", "rounded"});
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(!reply.body.value("verified").toBool());
        // The fake setter returned successfully but kept reporting the old value.
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("change_not_confirmed"));
    }
    void cliApiProcessOutputAndLegacyJsonStaySeparate() {
        Sandbox s;
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--cli-sandbox", s.dir.path(), "apply", "power.idle.lock", "600", "--api-version", "1", "--yess"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 1);
        QJsonParseError error;
        const QJsonDocument response = QJsonDocument::fromJson(child.readAllStandardOutput(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(response.object().value("error").toObject().value("code").toString(), QString("invalid_arguments"));
        QVERIFY(child.readAllStandardError().isEmpty());
        child.start(QCoreApplication::applicationFilePath(), {"--cli-sandbox", s.dir.path(), "catalog", "--json"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 0);
        const QJsonDocument legacy = QJsonDocument::fromJson(child.readAllStandardOutput(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(legacy.isArray());
        QCOMPARE(legacy.array().size(), Catalog().entries().size());
    }
    void cliShortcutInputDoesNotDiscardTokens_data() {
        QTest::addColumn<QString>("keys");
        for (const QString &keys : QStringList{"HYPER + SUPER + F", "SUPER + SUPER + F", "SUPER ++ F", "+ SUPER + F", "SUPER + F +", "SUPER + CTRL", "SUPER + F\n", "F"})
            QTest::newRow(qPrintable(keys)) << keys;
    }
    void cliShortcutInputDoesNotDiscardTokens() {
        QFETCH(QString, keys);
        Sandbox s;
        const auto reply = CliApi::execute(s.env, {"shortcut", "check", keys});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("invalid_value"));
        QVERIFY(s.calls().isEmpty());
    }
    void cliShortcutsExposeEveryReportedBindingAndOwnership() {
        Sandbox s;
        s.minimalHyprland();
        const QByteArray bindings = R"([{"modmask":64,"key":"F","description":"Files","dispatcher":"exec","arg":"files","submap":""},{"modmask":64,"key":"F","description":"","dispatcher":"resizeactive","arg":"20 0","submap":"resize","release":true},{"modmask":66,"key":"F","description":"Caps variant"},{"modmask":64,"key":"","keycode":41,"description":"By code"},{"modmask":4,"key":"","catch_all":true,"description":"Catch all"}])";
        writeFile(s.dir.path() + "/hypr-binds", bindings);
        HyprModule module(s.env);
        module.setBinds(QJsonArray{QJsonObject{{"keys", "SUPER + Y"}, {"description", "Mine"}, {"command", "app"}}});
        QVERIFY(module.save());
        const auto before = readAll(module.path());
        const auto reply = CliApi::execute(s.env, {"shortcut", "check", "super+f"});
        QCOMPARE(reply.exitCode, 0);
        const auto data = reply.body.value("data").toObject();
        QCOMPARE(data.value("effective").toObject().value("binds").toArray(), QJsonDocument::fromJson(bindings).array());
        QCOMPARE(data.value("owned").toObject().value("binds").toArray().first().toObject().value("command").toString(), QString("app"));
        const auto check = data.value("check").toObject();
        QCOMPARE(check.value("keys").toString(), QString("SUPER + F"));
        QCOMPARE(check.value("collisionState").toString(), QString("conflict"));
        QCOMPARE(check.value("matches").toArray().size(), 2);
        QVERIFY(check.value("matches").toArray().at(1).toObject().value("release").toBool());
        QCOMPARE(check.value("matches").toArray().at(1).toObject().value("submap").toString(), QString("resize"));
        QCOMPARE(check.value("conflicts").toArray().at(1).toString(), QString("resizeactive 20 0"));
        QCOMPARE(check.value("unresolved").toArray().size(), 1);
        QCOMPARE(CliApi::execute(s.env, {"shortcut", "check", "SUPER + Y"}).body.value("data").toObject().value("check").toObject().value("collisionState").toString(), QString("unknown"));
        const auto catchAll = CliApi::execute(s.env, {"shortcut", "check", "CTRL + X"});
        QCOMPARE(catchAll.body.value("data").toObject().value("check").toObject().value("matches").toArray().first().toObject().value("matchKind").toString(), QString("catch-all"));
        QCOMPARE(readAll(module.path()), before);
        QVERIFY(!QFile::exists(s.env.appStateDir + "/history.jsonl"));
        for (const auto &call : s.calls()) QCOMPARE(call, QString("hyprctl -j binds"));
    }
    void cliShortcutsDoNotTreatUnreadableSourcesAsEmpty_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("failed") << QByteArray("echo '[]'; exit 1");
        QTest::newRow("malformed") << QByteArray("echo '{}'");
        QTest::newRow("bad-row") << QByteArray("echo '[{}]'");
        QTest::newRow("bad-mask") << QByteArray("echo '[{\"modmask\":64.5,\"key\":\"F\"}]'");
        QTest::newRow("bad-flag") << QByteArray("echo '[{\"modmask\":64,\"key\":\"F\",\"catch_all\":\"false\"}]'");
        QTest::newRow("truncated") << QByteArray("printf '[]'; head -c 4200000 /dev/zero");
        QTest::newRow("timeout") << QByteArray("sleep 10");
    }
    void cliShortcutsDoNotTreatUnreadableSourcesAsEmpty() {
        QFETCH(QByteArray, body);
        Sandbox s;
        s.command("hyprctl", body);
        const auto reply = CliApi::execute(s.env, {"shortcut", "check", "SUPER + F"});
        QCOMPARE(reply.exitCode, 1);
        const auto data = reply.body.value("data").toObject();
        QVERIFY(!data.value("complete").toBool());
        QVERIFY(data.value("effective").toObject().value("binds").isNull());
        QCOMPARE(data.value("check").toObject().value("collisionState").toString(), QString("unavailable"));
        QVERIFY(!QFile::exists(s.env.appStateDir + "/history.jsonl"));
    }
    void cliOwnedShortcutAndRuleReadsReportConflictsAndInaccessibleParents() {
        Sandbox s;
        s.minimalHyprland();
        HyprModule module(s.env);
        const auto absent = CliApi::execute(s.env, {"shortcut", "list"});
        QCOMPARE(absent.exitCode, 0);
        QCOMPARE(absent.body.value("data").toObject().value("owned").toObject().value("status").toString(), QString("absent"));
        writeFile(module.path(), "-- hand-written Lua; never execute\nos.execute('unsafe')\n");
        auto read = CliApi::execute(s.env, {"shortcut", "list"});
        QCOMPARE(read.exitCode, 1);
        QCOMPARE(read.body.value("data").toObject().value("owned").toObject().value("status").toString(), QString("ownership_conflict"));
        QVERIFY(read.body.value("data").toObject().value("owned").toObject().value("binds").isNull());
        read = CliApi::execute(s.env, {"rule", "list", "--api-version", "1"});
        QCOMPARE(read.exitCode, 1);
        QCOMPARE(read.body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QVERIFY(QFile::remove(module.path()));
        QVERIFY(QFile::setPermissions(QFileInfo(module.path()).absolutePath(), {}));
        const auto unavailable = CliApi::execute(s.env, {"rule", "list", "--api-version", "1"});
        QVERIFY(QFile::setPermissions(QFileInfo(module.path()).absolutePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(unavailable.exitCode, 1);
        QCOMPARE(unavailable.body.value("error").toObject().value("code").toString(), QString("unavailable"));
        QVERIFY(unavailable.body.value("data").toObject().value("rules").isNull());
    }
    void cliRuleValidationIsPureAndUsesDiscoverableConstraints() {
        Sandbox s;
        const auto types = CliApi::execute(s.env, {"rule", "types"});
        QCOMPARE(types.exitCode, 0);
        const auto actions = types.body.value("data").toObject().value("actions").toArray();
        QCOMPARE(actions.size(), 5);
        for (const auto &item : actions) {
            const auto schema = item.toObject();
            const auto examples = schema.value("examples").toArray();
            const QString value = examples.isEmpty() ? QString() : examples.first().toString();
            const auto reply = CliApi::execute(s.env, {"rule", "validate", " firefox ", schema.value("action").toString(), value});
            QCOMPARE(reply.exitCode, 0);
            QCOMPARE(reply.body.value("outcome").toString(), QString("validated"));
            QVERIFY(!reply.body.value("verified").toBool());
            QCOMPARE(reply.body.value("data").toObject().value("rule").toObject().value("match").toString(), QString("firefox"));
        }
        const auto invalid = CliApi::execute(s.env, {"rule", "validate", "x", "float", "ignored"});
        QCOMPARE(invalid.exitCode, 1);
        QVERIFY(!invalid.body.value("data").toObject().value("valid").toBool());
        QCOMPARE(CliApi::execute(s.env, {"rule", "validate", "(", "float"}).exitCode, 1);
        QVERIFY(s.calls().isEmpty());
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
        QVERIFY(!QFile::exists(s.env.appStateDir + "/history.jsonl"));
    }
    void cliOwnedModuleReadsAreBoundedAndRejectDanglingLinks() {
        Sandbox s;
        HyprModule module(s.env);
        const QStringList args{"rule", "list", "--api-version", "1"};
        const auto absent = CliApi::execute(s.env, args);
        QCOMPARE(absent.exitCode, 0);
        QVERIFY(absent.body.value("data").toObject().value("rules").toArray().isEmpty());
        writeFile(module.path(), QByteArray(4 * 1024 * 1024 + 1, 'x'));
        auto reply = CliApi::execute(s.env, args);
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("unavailable"));
        QCOMPARE(QFileInfo(module.path()).size(), qint64(4 * 1024 * 1024 + 1));
        QVERIFY(QFile::remove(module.path()));
        QVERIFY(::symlink("missing-module.lua", qPrintable(module.path())) == 0);
        reply = CliApi::execute(s.env, args);
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(reply.body.value("data").toObject().value("rules").isNull());
        QVERIFY(QFile::remove(module.path()));
        QVERIFY(module.save());
        QVERIFY(QFile::setPermissions(module.path(), {}));
        reply = CliApi::execute(s.env, args);
        QVERIFY(QFile::setPermissions(module.path(), QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(s.calls().isEmpty());
        QVERIFY(!QFile::exists(s.env.appStateDir + "/history.jsonl"));
    }
    void cliShortcutAndRuleRoutingPreservesLegacyInterfaces() {
        QVERIFY(CliApi::requested({"shortcut", "check", "SUPER + F"}));
        QVERIFY(CliApi::requested({"--bad", "shortcut", "list"}));
        QVERIFY(CliApi::requested({"rule", "types"}));
        QVERIFY(CliApi::requested({"rule", "validate", "x", "float"}));
        QVERIFY(!CliApi::requested({"shortcut", "add", "SUPER + F", "Files", "files"}));
        QVERIFY(!CliApi::requested({"rule", "list", "--json"}));
        QVERIFY(CliApi::requested({"rule", "list", "--api-version", "1"}));
        Sandbox s;
        s.minimalHyprland();
        HyprModule module(s.env);
        module.setRules(QJsonArray{QJsonObject{{"match", "firefox"}, {"action", "float"}, {"value", ""}}});
        QVERIFY(module.save());
        const auto before = readAll(module.path());
        for (const QString &index : QStringList{"garbage", "-1", "1.5", "2147483648", " 0", "+0"})
            QCOMPARE(Cli::run(s.env, {"rule", "remove", index, "--json"}), 1);
        QCOMPARE(Cli::run(s.env, {"rule", "remove", "0", "extra"}), 1);
        QCOMPARE(readAll(module.path()), before);
        QVERIFY(s.calls().isEmpty());
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--cli-sandbox", s.dir.path(), "rule", "list", "--json"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 0);
        const auto legacy = QJsonDocument::fromJson(child.readAllStandardOutput());
        QVERIFY(legacy.isArray());
        QCOMPARE(legacy.array(), module.rules());
        const auto versioned = CliApi::execute(s.env, {"rule", "list", "--api-version", "1"});
        QCOMPARE(versioned.body.value("operation").toString(), QString("rules.inspect"));
        QCOMPARE(versioned.body.value("data").toObject().value("rules").toArray().first().toObject().value("index").toInt(-1), 0);
    }
    void shortcutAddRefreshesCollisionsAndRefusesFailedDiscovery() {
        Sandbox s;
        s.minimalHyprland();
        CommandRunner runner(s.env);
        ChangeCoordinator coordinator(s.env);
        ShortcutsProvider provider(&runner, &coordinator);
        QSignalSpy messages(&provider, &ShortcutsProvider::message);
        writeFile(s.dir.path() + "/hypr-binds", R"([{"modmask":64,"key":"F","description":"Fresh conflict"}])");
        provider.addBinding("SUPER + F", "Mine", "files", false);
        QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 1, 5000);
        QVERIFY(messages.first().at(1).toBool());
        s.command("hyprctl", "echo '[]'; exit 1");
        provider.addBinding("SUPER + F", "Mine", "files", true);
        QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 2, 5000);
        QVERIFY(messages.last().at(1).toBool());
        QVERIFY(coordinator.history().isEmpty());
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }
    void cliRuleActionsPlanCommitAndRemoveAnExactDuplicate() {
        HyprDesk desk;
        const QString module = desk.s.env.configHome + "/hypr/omarchy-control.lua";
        auto reply = CliApi::execute(desk.s.env, {"rule", "add", "firefox", "workspace", "3", "--dry-run"});
        QCOMPARE(reply.exitCode, 0);
        QCOMPARE(reply.body.value("outcome").toString(), QString("planned"));
        QCOMPARE(reply.body.value("data").toObject().value("details").toObject().value("verificationScope").toString(), QString("owned-file-and-reload"));
        QVERIFY(!QFile::exists(module));
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        for (int i = 0; i < 2; ++i) {
            reply = CliApi::execute(desk.s.env, {"rule", "add", "firefox", "workspace", "3"});
            QCOMPARE(reply.exitCode, 0);
            QVERIFY(reply.body.value("verified").toBool());
            QCOMPARE(reply.body.value("data").toObject().value("transaction").toObject().value("recovery").toString(), QString("no-undo"));
        }
        const QString selector = desk.selector("rule");
        QVERIFY(selector.contains('@'));
        reply = CliApi::execute(desk.s.env, {"rule", "remove", selector});
        QCOMPARE(reply.exitCode, 0);
        HyprModule owned(desk.s.env); QVERIFY(owned.load()); QCOMPARE(owned.rules().size(), 1);
        const auto before = readAll(module);
        reply = CliApi::execute(desk.s.env, {"rule", "remove", selector});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("stale_target"));
        QCOMPARE(readAll(module), before);
        QCOMPARE(ChangeCoordinator(desk.s.env).history().size(), 3);
    }
    void cliShortcutActionsRequireExplicitReplacementAndAcknowledgeConsequences() {
        HyprDesk desk;
        desk.stock(QJsonArray{QJsonObject{{"modmask", 64}, {"key", "SPACE"}, {"description", "Launcher"}, {"dispatcher", "__lua"}, {"arg", "22"}, {"submap", ""}}});
        QStringList args{"shortcut", "add", "SUPER + SPACE", "Mine", "my-app"};
        QCOMPARE(CliApi::execute(desk.s.env, args).body.value("error").toObject().value("code").toString(), QString("conflict"));
        args << "--replace";
        auto reply = CliApi::execute(desk.s.env, args + QStringList{"--dry-run"});
        QCOMPARE(reply.exitCode, 0);
        QVERIFY(!reply.body.value("data").toObject().value("confirmText").toString().isEmpty());
        QCOMPARE(CliApi::execute(desk.s.env, args).body.value("error").toObject().value("code").toString(), QString("confirmation_required"));
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        reply = CliApi::execute(desk.s.env, args + QStringList{"--yes"});
        QCOMPARE(reply.exitCode, 0);
        const auto plan = reply.body.value("data").toObject().value("plan").toObject();
        QVERIFY(!plan.value("details").toObject().value("liveCommandKnown").toBool());
        QCOMPARE(plan.value("details").toObject().value("record").toObject().value("command").toString(), QString("my-app"));
        const QString selector = desk.selector("shortcut");
        QCOMPARE(CliApi::execute(desk.s.env, {"shortcut", "remove", selector}).body.value("error").toObject().value("code").toString(), QString("confirmation_required"));
        reply = CliApi::execute(desk.s.env, {"shortcut", "remove", selector, "--yes"});
        QCOMPARE(reply.exitCode, 0);
        const auto live = QJsonDocument::fromJson(readAll(desk.s.env.home + "/live.json")).array();
        QCOMPARE(live.first().toObject().value("description").toString(), QString("Launcher"));
    }
    void cliShortcutMutationRefusesUnresolvableReplacement_data() {
        QTest::addColumn<QJsonObject>("binding");
        QTest::newRow("keycode") << QJsonObject{{"modmask", 64}, {"key", ""}, {"keycode", 41}};
        QTest::newRow("submap") << QJsonObject{{"modmask", 64}, {"key", "F"}, {"submap", "resize"}};
        QTest::newRow("catch-all") << QJsonObject{{"modmask", 64}, {"key", ""}, {"catch_all", true}};
    }
    void cliShortcutMutationRefusesUnresolvableReplacement() {
        QFETCH(QJsonObject, binding);
        HyprDesk desk; desk.stock(QJsonArray{binding});
        const auto reply = CliApi::execute(desk.s.env, {"shortcut", "add", "SUPER + F", "Mine", "app", "--replace", "--yes"});
        QCOMPARE(reply.exitCode, 1);
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("unsupported_configuration"));
        QVERIFY(!QFile::exists(desk.s.env.home + "/reloads"));
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void cliHyprMutationsDoNotTurnUnverifiedChangesIntoSuccess_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("family"); QTest::addColumn<QString>("phase");
        QTest::newRow("no-effect") << "no-effect" << "shortcut" << "rolled-back";
        QTest::newRow("wrong-dispatcher") << "wrong-command" << "shortcut" << "rolled-back";
        QTest::newRow("new-errors") << "new-error" << "rule" << "rolled-back";
        QTest::newRow("unreadable-post-errors") << "post-bad-errors" << "rule" << "partial";
        QTest::newRow("failed-recovery") << "recovery-fail" << "shortcut" << "partial";
        QTest::newRow("concurrent-edit") << "edit-after-reload" << "rule" << "partial";
    }
    void cliHyprMutationsDoNotTurnUnverifiedChangesIntoSuccess() {
        QFETCH(QString, mode); QFETCH(QString, family); QFETCH(QString, phase);
        HyprDesk desk; desk.mode(mode);
        if (mode == QLatin1String("recovery-fail")) desk.mode("no-effect");
        const QString main = desk.s.env.configHome + "/hypr/hyprland.lua";
        const QByteArray before = readAll(main);
        const auto reply = CliApi::execute(desk.s.env, family == QLatin1String("rule") ? QStringList{"rule", "add", "firefox", "float"}
            : QStringList{"shortcut", "add", "SUPER + Y", "Mine", "app"});
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(!reply.body.value("verified").toBool());
        QCOMPARE(reply.body.value("outcome").toString(), phase);
        QCOMPARE(readAll(main), before);
        const QString path = desk.s.env.configHome + "/hypr/omarchy-control.lua";
        if (mode == QLatin1String("edit-after-reload")) QVERIFY(readAll(path).contains("concurrent external edit"));
        else QVERIFY(!QFile::exists(path));
        QCOMPARE(ChangeCoordinator(desk.s.env).history().size(), 1);
    }
    void cliHyprPlansRejectUnavailableCompatibilityAndOwnership() {
        HyprDesk desk;
        desk.mode("bad-errors");
        auto reply = CliApi::execute(desk.s.env, {"rule", "add", "firefox", "float", "--dry-run"});
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        QVERIFY(!QFile::exists(desk.s.env.home + "/reloads"));
        QFile::remove(desk.s.env.home + "/bad-errors");
        desk.mode("new-version"); desk.s.env.enforceCompat = true;
        reply = CliApi::execute(desk.s.env, {"rule", "add", "firefox", "float", "--dry-run"});
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("compatibility_blocked"));
        QFile::remove(desk.s.env.home + "/new-version");
        const QString module = desk.s.env.configHome + "/hypr/omarchy-control.lua";
        const QString elsewhere = desk.s.env.home + "/elsewhere.lua"; writeFile(elsewhere, "keep me");
        QVERIFY(QFile::link(elsewhere, module));
        reply = CliApi::execute(desk.s.env, {"rule", "add", "firefox", "float"});
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QCOMPARE(readAll(elsewhere), QByteArray("keep me"));
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void hyprQueuedPlansRecheckFilesAndCollisionsBeforeCapture() {
        for (bool collision : {false, true}) {
            HyprDesk desk;
            ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
            ShortcutsProvider provider(&runner, &coordinator);
            bool planned = false; ProviderPlan plan;
            provider.planAdd("SUPER + F", "Mine", "app", false, [&](const ProviderPlan &value) { plan = value; planned = true; });
            QTRY_VERIFY_WITH_TIMEOUT(planned, 5000); QVERIFY(plan.ok);
            ChangeCoordinator::Done release;
            ChangeCoordinator::Request hold;
            hold.settingId = "test-hold"; hold.apply = [&](auto done) { release = done; };
            coordinator.submit(hold, {}); QVERIFY(release);
            QSignalSpy finished(&provider, &ShortcutsProvider::operationFinished);
            provider.applyPlan(plan);
            QString path = desk.s.env.configHome + "/hypr/omarchy-control.lua";
            if (collision) desk.stock(QJsonArray{QJsonObject{{"modmask", 64}, {"key", "F"}, {"description", "New collision"}}});
            else writeFile(path, "-- a concurrent owner\n");
            release(true, {});
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
            QVERIFY(coordinator.history().first().phase == QLatin1String("failed"));
            QVERIFY(!QFile::exists(desk.s.env.home + "/reloads"));
            if (collision) QVERIFY(!QFile::exists(path));
            else QCOMPARE(readAll(path), QByteArray("-- a concurrent owner\n"));
        }
    }
    void hyprPreflightFailureAndRestoreLeaseStartNoMutation() {
        HyprDesk desk;
        ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
        WindowRulesProvider provider(&runner, &coordinator);
        ProviderPlan plan; bool ready = false;
        provider.planAdd("firefox", "float", "", [&](const ProviderPlan &value) { plan = value; ready = true; });
        QTRY_VERIFY_WITH_TIMEOUT(ready, 5000); QVERIFY(plan.ok);
        desk.mode("bad-errors");
        QSignalSpy finished(&provider, &WindowRulesProvider::operationFinished);
        provider.applyPlan(plan); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(!QFile::exists(desk.s.env.home + "/reloads"));
        QVERIFY(!QFile::exists(desk.s.env.configHome + "/hypr/omarchy-control.lua"));
        QFile::remove(desk.s.env.home + "/bad-errors");
        ChangeCoordinator restore(desk.s.env); QVERIFY(restore.acquireLease("test-restore"));
        const auto blocked = CliApi::execute(desk.s.env, {"rule", "add", "firefox", "float"});
        QCOMPARE(blocked.exitCode, 1);
        QVERIFY(!QFile::exists(desk.s.env.home + "/reloads"));
    }
    void guiRemovalSelectorsCannotFollowAReplacedRow() {
        HyprDesk desk;
        ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env);
        WindowRulesProvider rules(&runner, &coordinator); ShortcutsProvider shortcuts(&runner, &coordinator);
        HyprModule module(desk.s.env);
        module.setRules(QJsonArray{QJsonObject{{"match", "firefox"}, {"action", "float"}}});
        module.setBinds(QJsonArray{QJsonObject{{"keys", "SUPER + F"}, {"description", "Old"}, {"command", "old"}}});
        QVERIFY(module.save());
        const QString selectedRule = rules.selectableRules().first().toMap().value("selector").toString();
        const QString selectedBind = shortcuts.ownedBinds().first().toMap().value("selector").toString();
        module.setRules(QJsonArray{QJsonObject{{"match", "calculator"}, {"action", "tile"}}});
        module.setBinds(QJsonArray{QJsonObject{{"keys", "SUPER + F"}, {"description", "New"}, {"command", "new"}}});
        QVERIFY(module.save()); const auto edited = readAll(module.path());
        QSignalSpy ruleMessages(&rules, &WindowRulesProvider::message), bindMessages(&shortcuts, &ShortcutsProvider::message);
        rules.removeRule(selectedRule); shortcuts.removeBinding(selectedBind);
        QCOMPARE(ruleMessages.count(), 1); QVERIFY(ruleMessages.first().at(1).toBool());
        QCOMPARE(bindMessages.count(), 1); QVERIFY(bindMessages.first().at(1).toBool());
        QCOMPARE(readAll(module.path()), edited);
        QVERIFY(coordinator.history().isEmpty()); QVERIFY(desk.s.calls().isEmpty());
    }
    void cliShellInspectionKeepsLivePersistedAndHintStatesSeparate() {
        ShellDesk desk;
        auto reply = desk.api({"plugin", "status"});
        QVERIFY(reply.value("ok").toBool());
        auto data = reply.value("data").toObject();
        QCOMPARE(data.value("dock").toObject().value("pins").toArray(), (QJsonArray{"a", "gone"}));
        const auto widgets = data.value("bar").toObject().value("widgets").toArray();
        QCOMPARE(widgets.at(1).toObject().value("entry").toObject().value("format").toString(), QString("first"));
        QCOMPARE(widgets.at(2).toObject().value("entry").toObject().value("format").toString(), QString("second"));
        QVERIFY(widgets.at(1).toObject().value("selector") != widgets.at(2).toObject().value("selector"));
        desk.mode("live-down");
        QVERIFY(desk.api({"plugin", "list"}).value("ok").toBool());
        reply = desk.api({"bar", "status"}); QVERIFY(!reply.value("ok").toBool());
        auto bar = reply.value("data").toObject().value("bar").toObject();
        QVERIFY(bar.value("layout").isNull());
        QCOMPARE(bar.value("persisted").toObject().value("status").toString(), QString("ok"));
        desk.mode("plugin-down");
        writeFile(desk.s.env.configHome + "/omarchy/plugins/hint/manifest.json", R"({"id":"local.hint","name":"Hint","kinds":["panel"]})");
        const auto plugins = desk.api({"plugin", "list"}).value("data").toObject().value("plugins").toObject();
        QVERIFY(plugins.value("items").isNull());
        QVERIFY(plugins.value("manifestHints").toArray().first().toObject().value("unknownState").toBool());
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void cliShellRegistryRejectsUnavailableOrAmbiguousData_data() {
        QTest::addColumn<QString>("mode");
        for (const QString &mode : QStringList{"plugin-down", "bad-registry", "duplicate-registry", "truncated-registry"}) QTest::newRow(qPrintable(mode)) << mode;
    }
    void cliShellRegistryRejectsUnavailableOrAmbiguousData() {
        QFETCH(QString, mode); ShellDesk desk; desk.mode(mode);
        const auto reply = desk.api({"plugin", "list"});
        QVERIFY(!reply.value("ok").toBool());
        QVERIFY(reply.value("data").toObject().value("plugins").toObject().value("items").isNull());
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
    }
    void cliShellWritesPlanAndVerify_data() {
        QTest::addColumn<QString>("action");
        for (const QString &action : QStringList{"disable", "bar-enable", "move", "pin", "unpin"}) QTest::newRow(qPrintable(action)) << action;
    }
    void cliShellWritesPlanAndVerify() {
        QFETCH(QString, action); ShellDesk desk;
        QStringList args;
        if (action == QLatin1String("disable")) args = {"plugin", "disable", "omarchy.service"};
        else if (action == QLatin1String("bar-enable")) args = {"plugin", "enable", "local.alt-bar"};
        else if (action == QLatin1String("move")) args = {"widget", "move", desk.selector("omarchy.clock", 1), "right", "0"};
        else args = {"dock", action, action == QLatin1String("pin") ? "b.desktop" : "gone"};
        const auto before = readAll(desk.config());
        auto reply = desk.api(args + QStringList{"--dry-run"});
        QVERIFY2(reply.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))));
        QCOMPARE(reply.value("outcome").toString(), QString("planned"));
        QCOMPARE(readAll(desk.config()), before); QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        reply = desk.api(args); QVERIFY2(reply.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))));
        QVERIFY(reply.value("verified").toBool()); QCOMPARE(reply.value("outcome").toString(), QString("committed"));
        const auto after = desk.object(); QVERIFY(after.value("custom").toObject().value("keep").toBool());
        if (action == QLatin1String("move")) {
            const auto layout = after.value("bar").toObject().value("layout").toObject();
            QCOMPARE(layout.value("right").toArray().first().toObject().value("format").toString(), QString("second"));
            QCOMPARE(layout.value("left").toArray().at(1).toObject().value("format").toString(), QString("first"));
        }
        if (action == QLatin1String("pin")) QCOMPARE(after.value("dock").toObject().value("pinned").toArray(), (QJsonArray{"a", "gone", "b"}));
        if (action == QLatin1String("unpin")) QCOMPARE(after.value("dock").toObject().value("pinned").toArray(), (QJsonArray{"a"}));
        const auto history = ChangeCoordinator(desk.s.env).history(); QCOMPARE(history.size(), 1); QVERIFY(!history.first().undoable());
        QVERIFY(history.first().files.contains(desk.config()));
    }
    void cliShellUnverifiedWritersReportPartialAndPreserveBackups_data() {
        QTest::addColumn<QString>("mode");
        for (const QString &mode : QStringList{"noop", "false-registry", "memory-only", "disk-only", "lose-options", "concurrent-edit", "fail-after-write"}) QTest::newRow(qPrintable(mode)) << mode;
    }
    void cliShellUnverifiedWritersReportPartialAndPreserveBackups() {
        QFETCH(QString, mode); ShellDesk desk;
        QStringList args = mode == QLatin1String("lose-options") ? QStringList{"widget", "move", desk.selector("omarchy.clock", 1), "right", "0"}
            : QStringList{"plugin", "disable", "omarchy.service"};
        desk.mode(mode); const auto before = readAll(desk.config());
        const auto reply = desk.api(args);
        QVERIFY(!reply.value("ok").toBool()); QVERIFY(!reply.value("verified").toBool());
        QCOMPARE(reply.value("outcome").toString(), QString("partial"));
        const auto tx = ChangeCoordinator(desk.s.env).history().first();
        const QString backup = tx.files.value(desk.config()).toObject().value("backup").toString();
        QCOMPARE(readAll(backup), before);
        if (mode == QLatin1String("concurrent-edit")) QCOMPARE(desk.object().value("external-choice").toString(), QString("keep"));
        if (mode == QLatin1String("disk-only") || mode == QLatin1String("fail-after-write")) QVERIFY(desk.object().value("disabledPlugins").toArray().contains("omarchy.service"));
    }
    void cliShellDiscoveryAndPlansKeepBadFilesAndDefaultPinsExplicit() {
        ShellDesk desk;
        const auto original = readAll(desk.config());
        writeFile(desk.config(), "{ not JSON }");
        auto reply = desk.api({"dock", "status"}); QVERIFY(!reply.value("ok").toBool());
        QVERIFY(reply.value("data").toObject().value("dock").toObject().value("pins").isNull());
        QVERIFY(!desk.api({"plugin", "disable", "omarchy.service"}).value("ok").toBool());
        QCOMPARE(readAll(desk.config()), QByteArray("{ not JSON }"));
        writeFile(desk.config(), "");
        reply = desk.api({"dock", "status"}); QVERIFY(reply.value("ok").toBool());
        QCOMPARE(reply.value("data").toObject().value("dock").toObject().value("pins").toArray(), (QJsonArray{"a", "gone"}));
        writeFile(desk.config(), "{\"version\":1}");
        QCOMPARE(desk.api({"dock", "pin", "b"}).value("error").toObject().value("code").toString(), QString("unsupported_configuration"));
        writeFile(desk.config(), original); desk.mode("plugin-down"); desk.mode("live-down");
        QVERIFY(desk.api({"dock", "status"}).value("ok").toBool());
        QVERIFY(desk.api({"dock", "unpin", "gone"}).value("ok").toBool());
    }
    void cliShellMovesRefuseStaleSelectorsAndTrayClones() {
        ShellDesk desk;
        const QString stale = desk.selector("omarchy.clock");
        auto changed = desk.object(); auto bar = changed.value("bar").toObject(); bar["id"] = "local.alt-bar"; changed["bar"] = bar;
        const auto edited = QJsonDocument(changed).toJson(); writeFile(desk.config(), edited); writeFile(desk.s.env.home + "/live-shell.json", edited);
        QCOMPARE(desk.api({"widget", "move", stale, "right", "0"}).value("error").toObject().value("code").toString(), QString("stale_target"));
        QVERIFY(!desk.api({"widget", "move", desk.selector("local.tray"), "tray", "0"}).value("ok").toBool());
        QCOMPARE(desk.api({"plugin", "disable", "omarchy.bar"}).value("error").toObject().value("code").toString(), QString("unsupported_operation"));
        QCOMPARE(desk.api({"widget", "move", desk.selector("omarchy.clock"), "right", "999"}).value("error").toObject().value("code").toString(), QString("invalid_value"));
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed")); QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void shellQueuedPlansRefuseChangedStateAndRestoreLeases() {
        ShellDesk desk; ChangeCoordinator coordinator(desk.s.env); CommandRunner runner(desk.s.env); PluginsProvider provider(&runner, &coordinator);
        ProviderPlan plan; bool ready = false;
        provider.planEnabled("omarchy.service", false, [&](const ProviderPlan &value) { plan = value; ready = true; });
        QTRY_VERIFY_WITH_TIMEOUT(ready, 5000); QVERIFY(plan.ok);
        ChangeCoordinator::Done release; ChangeCoordinator::Request hold; hold.settingId = "hold"; hold.apply = [&](auto done) { release = done; };
        coordinator.submit(hold, {}); QVERIFY(release);
        QSignalSpy finished(&provider, &PluginsProvider::operationFinished); provider.applyPlan(plan);
        auto updated = desk.object(); updated["changed-outside"] = true; const auto edited = QJsonDocument(updated).toJson();
        writeFile(desk.config(), edited); writeFile(desk.s.env.home + "/live-shell.json", edited); release(true, {});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(coordinator.history().first().phase, QString("failed")); QCOMPARE(readAll(desk.config()), edited);
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
        ChangeCoordinator restore(desk.s.env); QVERIFY(restore.acquireLease("test-restore"));
        QVERIFY(!desk.api({"dock", "pin", "b"}).value("ok").toBool());
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
    }
    void cliShellPreservesLegacyRoutingAndRejectsIndexCoercion() {
        QVERIFY(!CliApi::requested({"plugin", "disable", "omarchy.service"}));
        QVERIFY(!CliApi::requested({"widget", "move", "a", "right", "0", "left", "0"}));
        QVERIFY(CliApi::requested({"widget", "move", "a", "right", "0", "--dry-run"}));
        QVERIFY(CliApi::requested({"plugin", "list"})); QVERIFY(CliApi::requested({"bar", "status"})); QVERIFY(CliApi::requested({"dock", "status"}));
        ShellDesk desk;
        QCOMPARE(Cli::run(desk.s.env, {"widget", "move", "omarchy.clock", "right", "bad", "left", "1", "--json"}), 1);
        QVERIFY(desk.s.calls().isEmpty());
    }
    void cliShellDiscoveryRetainsReportedIdsOutsideTheWriterGrammar() {
        ShellDesk desk; desk.mode("unicode-registry");
        const auto reply = desk.api({"plugin", "list"});
        QVERIFY(reply.value("ok").toBool());
        bool found = false;
        for (const auto &row : reply.value("data").toObject().value("plugins").toObject().value("items").toArray())
            found |= row.toObject().value("id").toString() == QString::fromUtf8("local.функция");
        QVERIFY(found);
        QCOMPARE(desk.api({"plugin", "enable", QString::fromUtf8("local.функция")}).value("error").toObject().value("code").toString(), QString("invalid_value"));
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
    }
    void cliShellMutationsRespectAppMaskingOwnershipAndCompatibility() {
        ShellDesk desk;
        writeFile(desk.s.env.dataHome + "/applications/b.desktop", "[Desktop Entry]\nHidden=true\n");
        QCOMPARE(desk.api({"dock", "pin", "b"}).value("error").toObject().value("code").toString(), QString("unknown_target"));
        const auto before = readAll(desk.config());
        const QString link = desk.s.env.home + "/linked-config";
        QVERIFY(::link(QFile::encodeName(desk.config()).constData(), QFile::encodeName(link).constData()) == 0);
        QCOMPARE(desk.api({"dock", "unpin", "gone"}).value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QVERIFY(QFile::remove(link));
        desk.s.command("omarchy-dock", "# deliberately changed fixture\nexit 0"); desk.s.env.enforceCompat = true;
        QCOMPARE(desk.api({"dock", "unpin", "gone"}).value("error").toObject().value("code").toString(), QString("compatibility_blocked"));
        QCOMPARE(readAll(desk.config()), before); QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void cliDevicesDiscoverActualTargetsAndCapabilities() {
        DeviceDesk desk;
        desk.mode("slow-route");
        const auto audio = CliApi::execute(desk.s.env, {"audio", "status"});
        QCOMPARE(audio.exitCode, 0);
        const auto state = audio.body.value("data").toObject();
        QCOMPARE(state.value("outputs").toArray().size(), 3);
        QCOMPARE(state.value("inputs").toArray().size(), 2);
        QCOMPARE(state.value("streams").toArray().size(), 1);
        QCOMPARE(state.value("effectiveOutput").toString(), QString("speakers"));
        QVERIFY(state.value("outputRoutingKnown").toBool());
        const auto bluetooth = CliApi::execute(desk.s.env, {"bluetooth", "status"});
        QCOMPARE(bluetooth.exitCode, 0);
        QCOMPARE(bluetooth.body.value("data").toObject().value("devices").toArray().size(), 1);
        QVERIFY(bluetooth.body.value("data").toObject().value("powered").toBool());
        QVERIFY(!bluetooth.body.value("data").toObject().value("allRadiosPowered").toBool());
        desk.set("radio", false);
        desk.set("otherRadio", true);
        QVERIFY(CliApi::execute(desk.s.env, {"bluetooth", "status"}).body.value("data").toObject().value("powered").toBool());
        const auto schema = CliApi::execute(desk.s.env, {"describe", "audio.setVolume"}).body.value("data").toObject();
        QCOMPARE(schema.value("classification").toString(), QString("api-cli"));
        QCOMPARE(schema.value("parameters").toArray().last().toObject().value("maximum").toInt(), 150);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
    }
    void cliAudioChangesArePlannedJournaledAndVerified_data() {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("output") << QStringList{"audio", "output", "default", "phones"} << QString("defaultOut") << QJsonValue("phones");
        QTest::newRow("input") << QStringList{"audio", "input", "default", "mic2"} << QString("defaultIn") << QJsonValue("mic2");
        QTest::newRow("dsp-volume") << QStringList{"audio", "output", "volume", "dsp", "55"} << QString("speakersVolume") << QJsonValue(55);
        QTest::newRow("mic-volume") << QStringList{"audio", "input", "volume", "mic", "25"} << QString("micVolume") << QJsonValue(25);
        QTest::newRow("dsp-mute") << QStringList{"audio", "output", "mute", "dsp", "true"} << QString("speakersMute") << QJsonValue(true);
        QTest::newRow("mic-mute") << QStringList{"audio", "input", "mute", "mic", "true"} << QString("micMute") << QJsonValue(true);
        QTest::newRow("stream") << QStringList{"audio", "stream", "move", "7@700", "phones"} << QString("streamSink") << QJsonValue(12);
    }
    void cliAudioChangesArePlannedJournaledAndVerified() {
        QFETCH(QStringList, args);
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        DeviceDesk desk;
        const auto before = desk.data();
        auto planned = CliApi::execute(desk.s.env, args + QStringList{"--dry-run"});
        QCOMPARE(planned.exitCode, 0);
        QCOMPARE(planned.body.value("outcome").toString(), QString("planned"));
        QVERIFY(!planned.body.value("verified").toBool());
        QCOMPARE(desk.data(), before);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        const auto changed = CliApi::execute(desk.s.env, args);
        QVERIFY2(changed.exitCode == 0, qPrintable(QJsonDocument(changed.body).toJson()));
        QVERIFY(changed.body.value("verified").toBool());
        QCOMPARE(desk.data().value(key), value);
        QCOMPARE(desk.data().value("dspVolume").toInt(), 100);
        const auto history = ChangeCoordinator(desk.s.env).history();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().phase, QString("committed"));
        QCOMPARE(history.first().recovery, QString("no-undo"));
    }
    void cliAudioRejectsFalseSuccessAndStaleTargets_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("code");
        QTest::newRow("ignored-write") << QString("noop") << QStringList{"audio", "input", "volume", "mic", "20"} << QString("change_not_confirmed");
        QTest::newRow("readback-fails") << QString("fail-after") << QStringList{"audio", "input", "mute", "mic", "true"} << QString("change_not_confirmed");
        QTest::newRow("recycled-stream") << QString("recycle-stream") << QStringList{"audio", "stream", "move", "7@700", "phones"} << QString("change_not_confirmed");
        QTest::newRow("recycled-device") << QString("recycle-device") << QStringList{"audio", "output", "volume", "dsp", "20"} << QString("change_not_confirmed");
        QTest::newRow("malformed") << QString("malformed") << QStringList{"audio", "status"} << QString("read_unavailable");
        QTest::newRow("gone") << QString() << QStringList{"audio", "input", "mute", "gone", "true"} << QString("stale_target");
        QTest::newRow("bad-range") << QString() << QStringList{"audio", "input", "volume", "mic", "151"} << QString("invalid_value");
        QTest::newRow("bad-number") << QString() << QStringList{"audio", "input", "volume", "mic", "1.5"} << QString("invalid_value");
    }
    void cliAudioRejectsFalseSuccessAndStaleTargets() {
        QFETCH(QString, mode);
        QFETCH(QStringList, args);
        QFETCH(QString, code);
        DeviceDesk desk;
        if (!mode.isEmpty()) desk.mode(mode);
        const auto response = CliApi::execute(desk.s.env, args);
        QCOMPARE(response.exitCode, 1);
        QCOMPARE(response.body.value("error").toObject().value("code").toString(), code);
        QVERIFY(!response.body.value("verified").toBool());
        if (mode.startsWith("recycle"))
            QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
        if (mode == QLatin1String("fail-after"))
            QVERIFY(desk.data().value("micMute").toBool()); // failure is not proof of no change
    }
    void cliBluetoothChangesRequireReadableState_data() {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("key");
        QTest::addColumn<bool>("value");
        QTest::newRow("on") << QString("power") << QString("true") << QString("radio") << true;
        QTest::newRow("off") << QString("power") << QString("false") << QString("radio") << false;
        QTest::newRow("connect") << QString("connect") << QString("aa:bb:cc:dd:ee:01") << QString("connected") << true;
        QTest::newRow("disconnect") << QString("disconnect") << QString("AA:BB:CC:DD:EE:01") << QString("connected") << false;
        QTest::newRow("forget") << QString("forget") << QString("AA:BB:CC:DD:EE:01") << QString("known") << false;
    }
    void cliBluetoothChangesRequireReadableState() {
        QFETCH(QString, action);
        QFETCH(QString, target);
        QFETCH(QString, key);
        QFETCH(bool, value);
        DeviceDesk desk;
        desk.set("connected", action == QLatin1String("disconnect") || action == QLatin1String("forget"));
        desk.set("radio", action != QLatin1String("power") || target != QLatin1String("true"));
        const QStringList args{"bluetooth", action, target};
        const auto before = desk.data();
        const auto plan = CliApi::execute(desk.s.env, args + QStringList{"--dry-run"});
        QCOMPARE(plan.exitCode, 0);
        QCOMPARE(desk.data(), before);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        if (!plan.body.value("data").toObject().value("confirmText").toString().isEmpty()) {
            const auto refusal = CliApi::execute(desk.s.env, args);
            QCOMPARE(refusal.body.value("error").toObject().value("code").toString(), QString("confirmation_required"));
            QCOMPARE(desk.data(), before);
        }
        const auto response = CliApi::execute(desk.s.env, args + QStringList{"--yes"});
        QVERIFY2(response.exitCode == 0, qPrintable(QJsonDocument(response.body).toJson()));
        QVERIFY(response.body.value("verified").toBool());
        QCOMPARE(desk.data().value(key).toBool(), value);
        if (action == QLatin1String("power"))
            QCOMPARE(desk.data().value("otherRadio").toBool(), value);
        QCOMPARE(ChangeCoordinator(desk.s.env).history().size(), 1);
    }
    void cliBluetoothDoesNotConfuseFailedReadsWithOffOrRemoved_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("target");
        QTest::newRow("info-error") << QString("info-after-fails") << QString("disconnect") << QString("AA:BB:CC:DD:EE:01");
        QTest::newRow("empty-info") << QString("empty-info-after") << QString("disconnect") << QString("AA:BB:CC:DD:EE:01");
        QTest::newRow("list-error") << QString("list-after-fails") << QString("forget") << QString("AA:BB:CC:DD:EE:01");
        QTest::newRow("radio-error") << QString("radio-after-fails") << QString("power") << QString("false");
        QTest::newRow("only-one-radio-on") << QString("partial-power-on") << QString("power") << QString("true");
    }
    void cliBluetoothDoesNotConfuseFailedReadsWithOffOrRemoved() {
        QFETCH(QString, mode);
        QFETCH(QString, action);
        QFETCH(QString, target);
        DeviceDesk desk;
        desk.set("connected", true);
        desk.mode(mode);
        const auto response = CliApi::execute(desk.s.env, {"bluetooth", action, target, "--yes"});
        QCOMPARE(response.exitCode, 1);
        QVERIFY(!response.body.value("verified").toBool());
        QCOMPARE(response.body.value("error").toObject().value("code").toString(), QString("change_not_confirmed"));
        QCOMPARE(ChangeCoordinator(desk.s.env).history().first().phase, QString("partial"));
    }
    void cliDevicesPreservePairingCompatibilityAndRestoreBoundaries() {
        DeviceDesk desk;
        const auto pair = CliApi::execute(desk.s.env, {"bluetooth", "pair", "AA:BB:CC:DD:EE:01"});
        QCOMPARE(pair.body.value("error").toObject().value("code").toString(), QString("interaction_required"));
        QVERIFY(desk.s.calls().isEmpty());
        desk.set("paired", false);
        QCOMPARE(CliApi::execute(desk.s.env, {"bluetooth", "connect", "AA:BB:CC:DD:EE:01"}).body.value("error").toObject().value("code").toString(), QString("interaction_required"));
        desk.mode("radio-read-fails");
        QCOMPARE(CliApi::execute(desk.s.env, {"bluetooth", "status"}).exitCode, 1);
        desk.s.command("omarchy-audio-output-sink", "exit 0 # modified helper");
        Env strict = desk.s.env;
        strict.enforceCompat = true;
        QCOMPARE(CliApi::execute(strict, {"audio", "output", "volume", "dsp", "20"}).body.value("error").toObject().value("code").toString(), QString("compatibility_blocked"));
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
        ChangeCoordinator restore(desk.s.env);
        QVERIFY(restore.acquireLease("test-restore"));
        const auto blocked = CliApi::execute(desk.s.env, {"audio", "input", "volume", "mic", "20"});
        QCOMPARE(blocked.exitCode, 1);
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
    }
    void cliServicesScopesFailIndependently() {
        ServiceDesk desk;
        desk.mode("system-unavailable");
        const auto user = CliApi::execute(desk.s.env, {"services", "user", "status"});
        QCOMPARE(user.exitCode, 0);
        QCOMPARE(user.body.value("data").toObject().value("services").toArray().size(), 1);
        QVERIFY(!desk.s.calls().join('\n').contains("--system"));
        QCOMPARE(CliApi::execute(desk.s.env, {"services", "system", "status"}).exitCode, 1);
        desk.mode("files-error");
        QCOMPARE(CliApi::execute(desk.s.env, {"services", "user", "status"}).exitCode, 1);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        QFile::remove(desk.s.env.home + "/files-error");
        desk.mode("malformed-list");
        QCOMPARE(CliApi::execute(desk.s.env, {"services", "user", "status"}).exitCode, 1);
        CommandRunner runner(desk.s.env);
        ChangeCoordinator coordinator(desk.s.env);
        ServicesProvider provider(&runner, &coordinator);
        QSignalSpy refreshed(&provider, &ServicesProvider::refreshed);
        provider.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(refreshed.size(), 1, 5000);
        QCOMPARE(provider.status(), QString("unavailable"));
    }
    void cliServicesActionsAreScopedPlannedAndVerified_data() {
        QTest::addColumn<QString>("scope");
        QTest::addColumn<QString>("action");
        for (const QString &scope : {QStringLiteral("user"), QStringLiteral("system")})
            for (const QString &action : {QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("restart"), QStringLiteral("enable"), QStringLiteral("disable")})
                QTest::newRow(qPrintable(scope + "-" + action)) << scope << action;
    }
    void cliServicesActionsAreScopedPlannedAndVerified() {
        QFETCH(QString, scope);
        QFETCH(QString, action);
        ServiceDesk desk;
        if (action == QLatin1String("stop") || action == QLatin1String("restart"))
            desk.set(scope, "ActiveState", "active");
        if (action == QLatin1String("disable"))
            desk.set(scope, "UnitFileState", "enabled");
        const auto before = desk.data();
        const QStringList args{"services", scope, action, "agent.service"};
        const auto planned = CliApi::execute(desk.s.env, args + QStringList{"--dry-run"});
        QVERIFY2(planned.exitCode == 0, qPrintable(QJsonDocument(planned.body).toJson()));
        QCOMPARE(desk.data(), before);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        if (!planned.body.value("data").toObject().value("confirmText").toString().isEmpty()) {
            QCOMPARE(CliApi::execute(desk.s.env, args).body.value("error").toObject().value("code").toString(), QString("confirmation_required"));
            QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
        }
        const auto changed = CliApi::execute(desk.s.env, args + QStringList{"--yes"});
        QVERIFY2(changed.exitCode == 0, qPrintable(QJsonDocument(changed.body).toJson()));
        QVERIFY(changed.body.value("verified").toBool());
        const auto history = ChangeCoordinator(desk.s.env).history();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().settingId, "services." + scope + ".agent.service");
        QVERIFY(!history.first().undoable());
        const bool enabled = action == QLatin1String("enable") || action == QLatin1String("disable");
        const QString expected = enabled ? (action == QLatin1String("enable") ? "enabled" : "disabled")
            : action == QLatin1String("stop") ? "inactive" : "active";
        QCOMPARE(history.first().after, expected);
        QCOMPARE(desk.data().value(scope).toObject().value(enabled ? "UnitFileState" : "ActiveState").toString(), expected);
        const QString other = scope == QLatin1String("user") ? "system" : "user";
        QCOMPARE(desk.data().value(other), before.value(other));
        QVERIFY(desk.s.calls().join('\n').contains("systemctl --" + scope + " " + action + " agent.service --no-ask-password"));
    }
    void cliServicesRejectOldSuccessAndUnverifiedChanges_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("active");
        QTest::newRow("old-oneshot-success") << QString("noop") << QString("start") << QString("oneshot") << QString("inactive");
        QTest::newRow("ignored-restart") << QString("noop") << QString("restart") << QString("simple") << QString("active");
        QTest::newRow("ignored-enable") << QString("noop") << QString("enable") << QString("simple") << QString("inactive");
        QTest::newRow("ignored-stop") << QString("noop") << QString("stop") << QString("simple") << QString("active");
        QTest::newRow("unreadable-after") << QString("read-after-fails") << QString("start") << QString("simple") << QString("inactive");
        QTest::newRow("empty-after") << QString("empty-after") << QString("stop") << QString("simple") << QString("active");
        QTest::newRow("still-activating") << QString("activating-after") << QString("start") << QString("simple") << QString("inactive");
        QTest::newRow("replaced-unit") << QString("identity-after") << QString("start") << QString("simple") << QString("inactive");
    }
    void cliServicesRejectOldSuccessAndUnverifiedChanges() {
        QFETCH(QString, mode); QFETCH(QString, action); QFETCH(QString, type); QFETCH(QString, active);
        ServiceDesk desk;
        desk.set("user", "Type", type);
        desk.set("user", "ActiveState", active);
        desk.mode(mode);
        const auto changed = CliApi::execute(desk.s.env, {"services", "user", action, "agent.service"});
        QCOMPARE(changed.exitCode, 1);
        QVERIFY(!changed.body.value("verified").toBool());
        QCOMPARE(changed.body.value("outcome").toString(), QString("partial"));
        QCOMPARE(changed.body.value("error").toObject().value("code").toString(), QString("change_not_confirmed"));
    }
    void cliServicesHandleOneShotsAuthorizationAndPreconditions() {
        ServiceDesk desk;
        desk.set("user", "Type", "oneshot");
        const auto oneShot = CliApi::execute(desk.s.env, {"services", "user", "start", "agent.service"});
        QCOMPARE(oneShot.exitCode, 0);
        QCOMPARE(oneShot.body.value("data").toObject().value("transaction").toObject().value("after").toString(), QString("completed-oneshot"));
        desk.mode("authorization-denied");
        const auto denied = CliApi::execute(desk.s.env, {"services", "system", "start", "agent.service"});
        QCOMPARE(denied.exitCode, 1);
        QVERIFY(!denied.body.value("verified").toBool());
        QVERIFY(desk.s.calls().join('\n').contains("--no-ask-password"));
        QFile::remove(desk.s.env.home + "/authorization-denied");
        desk.set("user", "UnitFileState", "static");
        QCOMPARE(CliApi::execute(desk.s.env, {"services", "user", "enable", "agent.service"}).body.value("error").toObject().value("code").toString(), QString("unsupported_configuration"));
        QCOMPARE(CliApi::execute(desk.s.env, {"services", "user", "start", "missing.service"}).body.value("error").toObject().value("code").toString(), QString("stale_target"));
        ServiceDesk stale;
        stale.mode("stale-before-write");
        QCOMPARE(CliApi::execute(stale.s.env, {"services", "user", "start", "agent.service"}).exitCode, 1);
        QVERIFY(!QFile::exists(stale.s.env.home + "/changed"));
        ServiceDesk broken;
        broken.mode("missing-field");
        QCOMPARE(CliApi::execute(broken.s.env, {"services", "user", "start", "agent.service"}).body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        ChangeCoordinator holder(broken.s.env);
        QVERIFY(holder.acquireLease("profile-restore"));
        QFile::remove(broken.s.env.home + "/missing-field");
        QCOMPARE(CliApi::execute(broken.s.env, {"services", "user", "start", "agent.service"}).exitCode, 1);
        QVERIFY(!QFile::exists(broken.s.env.home + "/changed"));
    }
    void cliStartupEditsOnlyThePreferenceAndUserOverride() {
        Sandbox s;
        const QString system = s.env.sys("/etc/xdg/autostart/agent.desktop");
        const QByteArray contents = "[Desktop Entry]\r\nName=Agent\r\nExec=agent\r\nOnlyShowIn=GNOME;\r\nTryExec=agent\r\nX-GNOME-Autostart-enabled=false\r\nHidden=true\r\n[Desktop Action Debug]\r\nHidden=true\r\nX-GNOME-Autostart-enabled=false\r\nExec=agent --debug\r\n";
        writeFile(system, contents);
        const auto listed = CliApi::execute(s.env, {"startup", "list"});
        QCOMPARE(listed.exitCode, 0);
        QCOMPARE(listed.body.value("data").toObject().value("apps").toArray().size(), 1);
        QVERIFY(s.calls().isEmpty()); // Startup discovery does not require systemd.
        const auto planned = CliApi::execute(s.env, {"startup", "enable", "agent.desktop", "--dry-run"});
        QCOMPARE(planned.exitCode, 0);
        QCOMPARE(planned.body.value("data").toObject().value("details").toObject().value("verificationScope").toString(), QString("startup-preference-only"));
        const QString user = s.env.configHome + "/autostart/agent.desktop";
        QVERIFY(!QFile::exists(user));
        QVERIFY(ChangeCoordinator(s.env).history().isEmpty());
        const auto enabled = CliApi::execute(s.env, {"startup", "enable", "agent.desktop"});
        QCOMPARE(enabled.exitCode, 0);
        QVERIFY(enabled.body.value("verified").toBool());
        QCOMPARE(readAll(system), contents);
        const auto edited = readAll(user);
        QVERIFY(edited.contains("Hidden=false\r\n"));
        QVERIFY(edited.contains("X-GNOME-Autostart-enabled=true\r\n"));
        QVERIFY(edited.contains("OnlyShowIn=GNOME;\r\nTryExec=agent\r\n"));
        QVERIFY(edited.contains("[Desktop Action Debug]\r\nHidden=true\r\nX-GNOME-Autostart-enabled=false\r\nExec=agent --debug\r\n"));
        QCOMPARE(CliApi::execute(s.env, {"startup", "disable", "agent.desktop"}).exitCode, 0);
        QVERIFY(!ServicesProvider::readStartup(s.env).first().toMap().value("enabled").toBool());
        QVERIFY(!ChangeCoordinator(s.env).history().first().undoable());
    }
    void cliStartupHonorsXdgPrecedenceWithoutInheritingMissingKeys() {
        Sandbox s;
        const QString low = s.env.home + "/low", high = s.env.home + "/high", custom = s.env.home + "/custom";
        s.env.process.insert("XDG_CONFIG_HOME", custom);
        s.env.process.insert("XDG_CONFIG_DIRS", high + ":relative:" + low);
        writeFile(low + "/autostart/agent.desktop", "[Desktop Entry]\nName=Low\nExec=low\n");
        writeFile(high + "/autostart/agent.desktop", "[Desktop Entry]\nName=High\nExec=high\n");
        writeFile(custom + "/autostart/agent.desktop", "[Desktop Entry]\nHidden=true\n");
        const auto list = CliApi::execute(s.env, {"startup", "list"});
        const auto row = list.body.value("data").toObject().value("apps").toArray().first().toObject();
        QCOMPARE(row.value("path").toString(), custom + "/autostart/agent.desktop");
        QVERIFY(row.value("exec").toString().isEmpty());
        QCOMPARE(row.value("source").toString(), QString("override"));
        QCOMPARE(CliApi::execute(s.env, {"startup", "enable", "agent.desktop"}).exitCode, 0);
        QVERIFY(readAll(custom + "/autostart/agent.desktop").contains("Hidden=false"));
        QVERIFY(!QFile::exists(s.env.configHome + "/autostart/agent.desktop"));
        QCOMPARE(readAll(high + "/autostart/agent.desktop"), QByteArray("[Desktop Entry]\nName=High\nExec=high\n"));
    }
    void cliStartupRefusesUnreadableAmbiguousAndLinkedOverrides() {
        Sandbox s;
        const QString system = s.env.sys("/etc/xdg/autostart/agent.desktop"), user = s.env.configHome + "/autostart/agent.desktop";
        writeFile(system, "[Desktop Entry]\nName=Agent\nExec=agent\n");
        writeFile(user, "[Desktop Entry]\nHidden=true\n");
        QVERIFY(QFile::setPermissions(user, {}));
        QCOMPARE(CliApi::execute(s.env, {"startup", "enable", "agent.desktop"}).body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        QVERIFY(QFile::setPermissions(user, QFile::ReadOwner | QFile::WriteOwner));
        writeFile(user, "[Desktop Entry]\nHidden=true\nHidden=false\n");
        QCOMPARE(CliApi::execute(s.env, {"startup", "enable", "agent.desktop"}).body.value("error").toObject().value("code").toString(), QString("unsupported_configuration"));
        QFile::remove(user);
        QVERIFY(QFile::link(system, user));
        QCOMPARE(CliApi::execute(s.env, {"startup", "disable", "agent.desktop"}).body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QVERIFY(!readAll(system).contains("Hidden="));
        QVERIFY(ChangeCoordinator(s.env).history().isEmpty());
        QFile::remove(user);
        QVERIFY(QDir(QFileInfo(user).absolutePath()).removeRecursively());
        QVERIFY(QFile::link(QFileInfo(system).absolutePath(), QFileInfo(user).absolutePath()));
        QCOMPARE(CliApi::execute(s.env, {"startup", "disable", "agent.desktop"}).body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        const QString configLink = s.env.home + "/system-config-link";
        QVERIFY(QFile::link(s.env.sys("/etc/xdg"), configLink));
        s.env.process.insert("XDG_CONFIG_HOME", configLink);
        QCOMPARE(CliApi::execute(s.env, {"startup", "disable", "agent.desktop"}).body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QVERIFY(!readAll(system).contains("Hidden="));
    }
    void startupPreservesAnEditQueuedBeforeApply() {
        Sandbox s;
        const QString user = s.env.configHome + "/autostart/agent.desktop";
        writeFile(user, "[Desktop Entry]\nName=Agent\nExec=agent\n");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        ServicesProvider provider(&runner, &coordinator);
        ChangeCoordinator::Done release;
        ChangeCoordinator::Request blocking;
        blocking.settingId = "test.block";
        blocking.apply = [&](ChangeCoordinator::Done done) { release = done; };
        coordinator.submit(std::move(blocking), [](const Transaction &) {});
        QVERIFY(bool(release));
        QSignalSpy finished(&provider, &ServicesProvider::operationFinished);
        provider.setStartupEnabled("agent.desktop", false);
        const QByteArray edit = "[Desktop Entry]\nName=Changed elsewhere\nExec=agent\n";
        writeFile(user, edit);
        release(true, {});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(readAll(user), edit);
        QCOMPARE(coordinator.history().first().phase, QString("failed"));
    }
    void cliAppsDiscoveryUsesXdgPrecedenceAndDesktopIds() {
        MimeDesk desk;
        desk.app("tools/viewer.desktop");
        desk.app("masked.desktop");
        writeFile(desk.s.env.dataHome + "/applications/masked.desktop", "[Desktop Entry]\nHidden=true\n");
        desk.app("a.desktop", "Comment=User version\n", true);
        const auto listing = CliApi::execute(desk.s.env, {"apps", "list"});
        QCOMPARE(listing.exitCode, 0);
        const auto apps = listing.body.value("data").toObject().value("apps").toArray();
        auto find = [&](const QString &id) { for (const auto &row : apps) if (row.toObject().value("id").toString() == id) return row.toObject(); return QJsonObject{}; };
        QCOMPARE(find("a.desktop").value("comment").toString(), QString("User version"));
        QCOMPARE(find("tools-viewer.desktop").value("path").toString(), desk.s.env.dataDirs.last() + "/applications/tools/viewer.desktop");
        QVERIFY(find("masked.desktop").value("deleted").toBool());
        QVERIFY(!find("masked.desktop").value("available").toBool());
        QVERIFY(desk.s.calls().isEmpty());
        desk.app("tools-viewer.desktop"); // ambiguous IDs in one data root
        const auto launch = CliApi::execute(desk.s.env, {"apps", "launch", "tools-viewer.desktop"});
        QCOMPARE(launch.body.value("error").toObject().value("code").toString(), QString("unavailable"));
        const QString userFile = desk.s.env.dataHome + "/applications/a.desktop";
        QVERIFY(QFile::setPermissions(userFile, {}));
        QCOMPARE(CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "a.desktop"}).exitCode, 1);
        QVERIFY(QFile::setPermissions(userFile, QFile::ReadOwner | QFile::WriteOwner));
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void cliAppLaunchReportsOnlyAHandoffAndPreservesLegacyRouting() {
        MimeDesk desk;
        QVERIFY(!CliApi::requested({"apps", "launcher", "list", "--json"}));
        QVERIFY(CliApi::requested({"apps", "list"}));
        QVERIFY(CliApi::requested({"apps", "launcher", "list", "--api-version", "1"}));
        QCOMPARE(CliApi::execute(desk.s.env, {"apps", "launcher", "set", "a.desktop", "--api-version", "1"}).body.value("error").toObject().value("code").toString(), QString("unsupported_api_operation"));
        QCOMPARE(CliApi::execute(desk.s.env, {"apps", "launch", "missing.desktop"}).exitCode, 1);
        const auto plan = CliApi::execute(desk.s.env, {"apps", "launch", "b.desktop", "--dry-run"});
        QCOMPARE(plan.exitCode, 0);
        QVERIFY(desk.s.calls().isEmpty());
        const auto launch = CliApi::execute(desk.s.env, {"apps", "launch", "b.desktop"});
        QCOMPARE(launch.exitCode, 0);
        QCOMPARE(launch.body.value("outcome").toString(), QString("launched"));
        QVERIFY(!launch.body.value("verified").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(desk.s.calls().join('\n').contains("gtk-launch b"), 3000);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
    }
    void cliMimeDiscoverySeparatesFailedAndUnresolvedReads() {
        MimeDesk desk;
        const auto list = CliApi::execute(desk.s.env, {"mime", "list"});
        QCOMPARE(list.exitCode, 0);
        QCOMPARE(list.body.value("data").toObject().value("associations").toArray().size(), 7);
        const auto read = CliApi::execute(desk.s.env, {"mime", "get", "application/x-custom"});
        QCOMPARE(read.exitCode, 0);
        QCOMPARE(read.body.value("data").toObject().value("candidates").toArray().size(), 2);
        QVERIFY(!read.body.value("data").toObject().value("candidatesComplete").toBool());
        desk.mode("unknown-handler");
        const auto unknown = CliApi::execute(desk.s.env, {"mime", "get", "text/plain"});
        QCOMPARE(unknown.body.value("data").toObject().value("current").toString(), QString("missing.desktop"));
        QVERIFY(!unknown.body.value("data").toObject().value("currentInstalled").toBool());
        QFile::remove(desk.s.env.home + "/unknown-handler");
        desk.mode("empty-query");
        const auto empty = CliApi::execute(desk.s.env, {"mime", "get", "text/plain"});
        QCOMPARE(empty.exitCode, 0);
        QCOMPARE(empty.body.value("data").toObject().value("status").toString(), QString("unresolved"));
        QVERIFY(!empty.body.value("verified").toBool());
        QFile::remove(desk.s.env.home + "/empty-query");
        desk.mode("query-fails");
        const auto failed = CliApi::execute(desk.s.env, {"mime", "get", "text/plain"});
        QCOMPARE(failed.exitCode, 1);
        QVERIFY(failed.body.value("data").toObject().value("current").toString().isEmpty());
        QCOMPARE(CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "b.desktop"}).exitCode, 1);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        QFile::remove(desk.s.env.home + "/query-fails"); desk.mode("multiple-results");
        QCOMPARE(CliApi::execute(desk.s.env, {"mime", "get", "text/plain"}).exitCode, 1);
    }
    void cliMimePlansAndChangesOneKeyInTheEffectiveUserFile_data() {
        QTest::addColumn<bool>("desktopSpecific");
        QTest::newRow("generic") << false;
        QTest::newRow("desktop-specific") << true;
    }
    void cliMimePlansAndChangesOneKeyInTheEffectiveUserFile() {
        QFETCH(bool, desktopSpecific);
        MimeDesk desk;
        const QString file = desktopSpecific ? desk.s.env.configHome + "/hyprland-mimeapps.list" : desk.generic();
        const QByteArray before = "# preserve\r\n[Default Applications]\r\ntext/plain=a.desktop;\r\napplication/pdf=a.desktop;\r\n[Added Associations]\r\ntext/plain=a.desktop;b.desktop;\r\n";
        writeFile(file, before);
        const auto genericBefore = readAll(desk.generic());
        const auto plan = CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "b.desktop", "--dry-run"});
        QCOMPARE(plan.exitCode, 0);
        QCOMPARE(plan.body.value("data").toObject().value("details").toObject().value("file").toString(), file);
        QCOMPARE(readAll(file), before);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        const auto changed = CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "b.desktop"});
        QVERIFY2(changed.exitCode == 0, qPrintable(QJsonDocument(changed.body).toJson()));
        QVERIFY(changed.body.value("verified").toBool());
        auto expected = before; expected.replace("text/plain=a.desktop;\r\n", "text/plain=b.desktop;a.desktop;\r\n");
        QCOMPARE(readAll(file), expected);
        if (desktopSpecific) QCOMPARE(readAll(desk.generic()), genericBefore);
        QCOMPARE(ChangeCoordinator(desk.s.env).history().size(), 1);
        QVERIFY(!ChangeCoordinator(desk.s.env).history().first().undoable());
        const auto custom = CliApi::execute(desk.s.env, {"mime", "set", "application/x-custom", "b.desktop"});
        QCOMPARE(custom.exitCode, 0);
    }
    void cliMimeRefusesFalseSuccessAndRestoresTheUserFile_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("failed-read-with-matching-output") << QString("fail-after-write");
        QTest::newRow("ignored-default") << QString("ignore-write");
        QTest::newRow("empty-read") << QString("empty-query");
    }
    void cliMimeRefusesFalseSuccessAndRestoresTheUserFile() {
        QFETCH(QString, mode);
        MimeDesk desk;
        const auto before = readAll(desk.generic());
        desk.mode(mode);
        const auto changed = CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "b.desktop"});
        QCOMPARE(changed.exitCode, 1);
        QVERIFY(!changed.body.value("verified").toBool());
        QCOMPARE(changed.body.value("outcome").toString(), QString("rolled-back"));
        QCOMPARE(readAll(desk.generic()), before);
    }
    void cliMimeRefusesUnavailableAppsLinksConflictsAndRestoreLeases() {
        MimeDesk desk;
        const auto before = readAll(desk.generic());
        desk.mode("app-replaced");
        QCOMPARE(CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "b.desktop"}).exitCode, 1);
        QCOMPARE(readAll(desk.generic()), before);
        MimeDesk linked;
        const QString target = linked.s.env.home + "/original.list";
        writeFile(target, readAll(linked.generic())); QFile::remove(linked.generic());
        QVERIFY(QFile::link(target, linked.generic()));
        QCOMPARE(CliApi::execute(linked.s.env, {"mime", "set", "text/plain", "b.desktop"}).body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        MimeDesk ambiguous;
        writeFile(ambiguous.generic(), "[Default Applications]\ntext/plain=a.desktop;\ntext/plain=b.desktop;\n");
        QCOMPARE(CliApi::execute(ambiguous.s.env, {"mime", "set", "text/plain", "b.desktop"}).body.value("error").toObject().value("code").toString(), QString("unsupported_configuration"));
        MimeDesk restore;
        ChangeCoordinator holder(restore.s.env);
        QVERIFY(holder.acquireLease("restore"));
        const auto refused = CliApi::execute(restore.s.env, {"mime", "set", "text/plain", "b.desktop"});
        QCOMPARE(refused.exitCode, 1);
        QCOMPARE(readAll(restore.generic()), before);
    }
    void cliMimeChangesAreRecognizedByTheRealXdgQuery_data() {
        QTest::addColumn<bool>("desktopSpecific");
        QTest::newRow("generic") << false;
        QTest::newRow("desktop-specific") << true;
    }
    void cliMimeChangesAreRecognizedByTheRealXdgQuery() {
        QFETCH(bool, desktopSpecific);
        MimeDesk desk;
        const QString real = QStandardPaths::findExecutable("xdg-mime");
        QVERIFY2(!real.isEmpty(), "The integration check needs xdg-utils installed.");
        QFile::remove(desk.s.bin("xdg-mime"));
        QVERIFY(QFile::link(real, desk.s.bin("xdg-mime")));
        QVERIFY(QFile::link(QStandardPaths::findExecutable("realpath"), desk.s.bin("realpath")));
        for (const QString &id : {QStringLiteral("a.desktop"), QStringLiteral("b.desktop")})
            writeFile(desk.s.env.dataDirs.last() + "/applications/" + id,
                      ("[Desktop Entry]\nType=Application\nName=" + id + "\nExec=" + QStandardPaths::findExecutable("true") + "\nMimeType=text/plain;\n").toUtf8());
        desk.s.env.process.insert("DE", "generic");
        if (desktopSpecific)
            writeFile(desk.s.env.configHome + "/hyprland-mimeapps.list", "[Default Applications]\ntext/plain=a.desktop;\n");
        const auto before = CliApi::execute(desk.s.env, {"mime", "get", "text/plain"});
        QVERIFY2(before.exitCode == 0, qPrintable(QJsonDocument(before.body).toJson()));
        QCOMPARE(before.body.value("data").toObject().value("current").toString(), QString("a.desktop"));
        const auto changed = CliApi::execute(desk.s.env, {"mime", "set", "text/plain", "b.desktop"});
        QVERIFY2(changed.exitCode == 0, qPrintable(QJsonDocument(changed.body).toJson()));
        QVERIFY(changed.body.value("verified").toBool());
    }
    void cliSystemFactsCompleteAndScopedReads() {
        FactsDesk desk;
        desk.battery("BAT0", "75", "Charging", "0"); desk.battery("BAT1", "25", "Discharging", "12");
        const QString hooks = desk.s.env.configHome + "/omarchy/hooks";
        writeFile(hooks + "/theme-set.d/10-test", "#!/bin/sh\ntouch \"$HOME/hook-ran\"\n");
        QVERIFY(QFile::setPermissions(hooks + "/theme-set.d/10-test", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        writeFile(hooks + "/post-update.sample", "# inert\n");
        writeFile(hooks + "/notes", "not executable\n");
        const auto read = CliApi::execute(desk.s.env, {"system", "status"});
        QVERIFY2(read.exitCode == 0, qPrintable(QJsonDocument(read.body).toJson()));
        QVERIFY(read.body.value("verified").toBool());
        const auto data = read.body.value("data").toObject();
        QCOMPARE(data.value("machine").toObject().value("omarchy").toString(), QString("4.5.0"));
        QCOMPARE(data.value("battery").toObject().value("percent").toInt(), 75);
        QCOMPARE(data.value("battery").toObject().value("devices").toArray().size(), 2);
        QCOMPARE(data.value("battery").toObject().value("cycles").toInt(), 0);
        QCOMPARE(data.value("storage").toArray().size(), 2);
        QCOMPARE(data.value("hooks").toArray().size(), 3);
        QVERIFY(!QFile::exists(desk.s.env.home + "/hook-ran"));
        FactsDesk scoped;
        scoped.s.command("df", "exit 2");
        const auto battery = CliApi::execute(scoped.s.env, {"system", "battery"});
        QCOMPARE(battery.exitCode, 0);
        QVERIFY(battery.body.value("data").toObject().value("battery").toObject().value("presenceKnown").toBool());
        QVERIFY(!battery.body.value("data").toObject().value("battery").toObject().value("present").toBool());
        QVERIFY(scoped.s.calls().isEmpty());
        QCOMPARE(CliApi::execute(scoped.s.env, {"system", "storage"}).exitCode, 1);
        QVERIFY(ChangeCoordinator(scoped.s.env).history().isEmpty());
    }
    void cliSystemFactsDistinguishUnknownFromAbsence() {
        FactsDesk desk;
        desk.battery("BAT0", "101", "", "bad");
        auto read = CliApi::execute(desk.s.env, {"system", "battery"});
        QCOMPARE(read.exitCode, 0);
        QVERIFY(!read.body.value("verified").toBool());
        QCOMPARE(read.body.value("data").toObject().value("battery").toObject().value("percent").toInt(), -1);
        const QString batteryRoot = desk.s.env.sys("/sys/class/power_supply");
        QVERIFY(QDir(batteryRoot).removeRecursively());
        read = CliApi::execute(desk.s.env, {"system", "battery"});
        QCOMPARE(read.exitCode, 1);
        QVERIFY(!read.body.value("data").toObject().value("battery").toObject().value("presenceKnown").toBool());
        QVERIFY(!read.body.value("data").toObject().value("battery").toObject().contains("present"));
        desk.s.omarchy("echo 9.9.9; exit 2");
        desk.s.command("hyprctl", "case \"$*\" in *devices*) echo '{\"mice\":[{\"name\":\"touchpad\"}],\"touch\":[{}]}';; *) echo '{\"version\":\"9.9.9\"}';; esac; exit 2");
        read = CliApi::execute(desk.s.env, {"system", "machine"});
        QCOMPARE(read.exitCode, 0);
        QVERIFY(!read.body.value("verified").toBool());
        QVERIFY(read.body.value("data").toObject().value("machine").toObject().value("omarchy").toString().isEmpty());
        QVERIFY(read.body.value("data").toObject().value("machine").toObject().value("hyprland").toString().isEmpty());
        AppCore core(desk.s.env);
        core.capabilities.scanServicesBlocking();
        QVERIFY(core.capabilities.versions().value("hyprland").toString().isEmpty());
        QVERIFY(!core.capabilities.has("touchpad"));
        QVERIFY(!core.capabilities.has("touchscreen"));
        const QString hooks = desk.s.env.configHome + "/omarchy/hooks";
        writeFile(hooks + "/entry", "# none\n");
        QVERIFY(QFile::setPermissions(hooks, {}));
        QCOMPARE(CliApi::execute(desk.s.env, {"system", "hooks"}).exitCode, 1);
        QVERIFY(QFile::setPermissions(hooks, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::setPermissions(desk.s.env.configHome, {}));
        QCOMPARE(CliApi::execute(desk.s.env, {"system", "hooks"}).exitCode, 1);
        QVERIFY(QFile::setPermissions(desk.s.env.configHome, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    void cliSystemStoragePreservesUnknownAndPartialRows() {
        FactsDesk desk;
        desk.s.command("df", "printf 'Filesystem Type 1B-blocks Used Avail Use%% Mounted on\\n/dev/a ext4 100 95 -5 106%% /my  disk\\n//nas/share cifs - - - - /mnt/nas\\nmalformed-row\\n'");
        const auto read = CliApi::execute(desk.s.env, {"system", "storage"});
        QCOMPARE(read.exitCode, 0);
        QVERIFY(!read.body.value("verified").toBool());
        const auto rows = read.body.value("data").toObject().value("storage").toArray();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].toObject().value("mount").toString(), QString("/my  disk"));
        QCOMPARE(rows[0].toObject().value("available").toInt(), -5);
        QVERIFY(rows[1].toObject().value("size").isNull());
        QVERIFY(!rows[1].toObject().value("known").toBool());
        desk.s.command("df", "echo not-a-table");
        QCOMPARE(CliApi::execute(desk.s.env, {"system", "storage"}).exitCode, 1);
        desk.s.command("df", "printf 'Filesystem Type 1B-blocks Used Avail Use%% Mounted on\\n/dev/a ext4 100 1 99 1%% /\\n'; exit 2");
        const auto failed = CliApi::execute(desk.s.env, {"system", "storage"});
        QCOMPARE(failed.exitCode, 1);
        QVERIFY(failed.body.value("data").toObject().value("storage").toArray().isEmpty());
    }
    void diagnosticsRedactsQuotedCredentialsAndPrivateKeys() {
        const QString report = SystemProvider::buildDiagnostics(
            "{\"api_key\": \"two secret words\"}\nsecret='single quoted secret'\nAuthorization: Bearer bearer-content\nwifi-sec.psk \"wifi secret words\"\n-----BEGIN PRIVATE KEY-----\nFAKE-PRIVATE-CONTENT\n-----END PRIVATE KEY-----", {}, {}, {});
        for (const QString &secret : {QStringLiteral("two secret words"), QStringLiteral("single quoted secret"), QStringLiteral("bearer-content"), QStringLiteral("wifi secret words"), QStringLiteral("FAKE-PRIVATE-CONTENT")})
            QVERIFY2(!report.contains(secret), qPrintable(report));
    }
    void cliDiagnosticsExportIsAwaitedPrivateAndUnique() {
        FactsDesk desk;
        desk.mode("slow");
        const QString helperHash = FileOps::fingerprint(desk.s.bin("omarchy-debug"));
        const auto plan = CliApi::execute(desk.s.env, {"diagnostics", "export", "--dry-run"});
        QCOMPARE(plan.exitCode, 0);
        QVERIFY(!QFileInfo::exists(desk.exports()));
        QVERIFY(desk.s.calls().isEmpty());
        const auto exported = CliApi::execute(desk.s.env, {"diagnostics", "export"});
        QVERIFY2(exported.exitCode == 0, qPrintable(QJsonDocument(exported.body).toJson()));
        QCOMPARE(exported.body.value("outcome").toString(), QString("exported"));
        QVERIFY(exported.body.value("verified").toBool());
        const auto data = exported.body.value("data").toObject();
        const QString path = data.value("path").toString();
        const auto report = readAll(path);
        QVERIFY(report.contains("4.5.0"));
        QVERIFY(report.contains("0.56.2"));
        QVERIFY(!report.contains("two secret words"));
        QVERIFY(!report.contains("hidden-token"));
        QCOMPARE(FileOps::fingerprint(path), data.value("sha256").toString());
        QVERIFY(!(QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
        QVERIFY(!(QFileInfo(desk.exports()).permissions() & (QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther)));
        const auto again = CliApi::execute(desk.s.env, {"diagnostics", "export"});
        QCOMPARE(again.exitCode, 0);
        QVERIFY(again.body.value("data").toObject().value("path").toString() != path);
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        QVERIFY(desk.s.calls().join('\n').contains("omarchy debug --no-sudo --print"));
        QVERIFY(!QFileInfo::exists(desk.s.env.home + "/public-log-used"));
        QCOMPARE(FileOps::fingerprint(desk.s.bin("omarchy-debug")), helperHash);
        QVERIFY(QDir(desk.exports()).entryList({"collect-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
    void cliDiagnosticsIncompleteCollectionIsNotSuccess_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("failed") << QString("fail");
        QTest::newRow("empty") << QString("empty");
        QTest::newRow("truncated") << QString("truncated");
        QTest::newRow("stderr-truncated") << QString("stderr-truncated");
        QTest::newRow("raw-limit") << QString("raw-limit");
    }
    void cliDiagnosticsIncompleteCollectionIsNotSuccess() {
        QFETCH(QString, mode);
        FactsDesk desk; desk.mode(mode);
        const auto exported = CliApi::execute(desk.s.env, {"diagnostics", "export"});
        QCOMPARE(exported.exitCode, 1);
        QCOMPARE(exported.body.value("outcome").toString(), QString("partial"));
        QCOMPARE(exported.body.value("error").toObject().value("code").toString(), QString("diagnostics_incomplete"));
        const auto data = exported.body.value("data").toObject();
        QVERIFY(data.value("reportSaved").toBool());
        QVERIFY(!data.value("complete").toBool());
        QVERIFY(readAll(data.value("path").toString()).contains("Collection status: incomplete"));
        QVERIFY(!readAll(data.value("path").toString()).contains("hidden-token"));
    }
    void cliDiagnosticsTimeoutProducesAnIncompleteReport() {
        FactsDesk desk; desk.mode("timeout");
        const auto exported = CliApi::execute(desk.s.env, {"diagnostics", "export"});
        QCOMPARE(exported.exitCode, 1);
        QCOMPARE(exported.body.value("outcome").toString(), QString("partial"));
        QCOMPARE(exported.body.value("data").toObject().value("backend").toObject().value("failure").toInt(), int(CommandResult::Timeout));
        QVERIFY(exported.body.value("data").toObject().value("reportSaved").toBool());
    }
    void diagnosticsRefusesOwnershipConflictsAndCleansCancelledWork() {
        FactsDesk changed;
        changed.s.env.enforceCompat = true;
        QCOMPARE(CliApi::execute(changed.s.env, {"diagnostics", "export"}).body.value("error").toObject().value("code").toString(), QString("compatibility_blocked"));
        QVERIFY(!QFileInfo::exists(changed.exports()));
        FactsDesk unknown;
        unknown.s.command("omarchy-debug", "echo unsupported");
        QCOMPARE(CliApi::execute(unknown.s.env, {"diagnostics", "export"}).body.value("error").toObject().value("code").toString(), QString("unsupported_configuration"));
        QVERIFY(!QFileInfo::exists(unknown.exports()));
        FactsDesk desk;
        writeFile(desk.exports(), "not a directory");
        const auto failed = CliApi::execute(desk.s.env, {"diagnostics", "export"});
        QCOMPARE(failed.exitCode, 1);
        QCOMPARE(failed.body.value("error").toObject().value("code").toString(), QString("ownership_conflict"));
        QCOMPARE(readAll(desk.exports()), QByteArray("not a directory"));
        QFile::remove(desk.exports());
        const QString elsewhere = desk.s.env.home + "/elsewhere";
        QDir().mkpath(elsewhere); QVERIFY(QFile::link(elsewhere, desk.exports()));
        QCOMPARE(CliApi::execute(desk.s.env, {"diagnostics", "export"}).exitCode, 1);
        QVERIFY(QDir(elsewhere).entryList(QDir::Files).isEmpty());
        QFile::remove(desk.exports());
        desk.mode("slow");
        AppCore core(desk.s.env);
        auto provider = std::make_unique<SystemProvider>(&core.catalog, &core.runner, &core.capabilities, &core.coordinator);
        int completed = 0;
        const QString pending = provider->collectDiagnostics([&](const QJsonObject &) { ++completed; });
        QVERIFY(!pending.isEmpty());
        QJsonObject busy;
        provider->collectDiagnostics([&](const QJsonObject &result) { busy = result; });
        QCOMPARE(busy.value("code").toString(), QString("configuration_conflict"));
        provider.reset();
        QTRY_VERIFY_WITH_TIMEOUT(core.runner.activeCount() == 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(pending), 5000);
        QCOMPARE(completed, 0);
    }
    void cliAudioCannotGuessAnUnreadableDspRoute() {
        DeviceDesk desk;
        desk.s.omarchy("exit 1");
        const auto snapshot = CliApi::execute(desk.s.env, {"audio", "status"});
        QCOMPARE(snapshot.exitCode, 0);
        QVERIFY(!snapshot.body.value("data").toObject().value("outputRoutingKnown").toBool());
        const auto output = CliApi::execute(desk.s.env, {"audio", "output", "volume", "dsp", "20"});
        QCOMPARE(output.body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
        // Input volume does not depend on resolving an output processing chain.
        const auto input = CliApi::execute(desk.s.env, {"audio", "input", "volume", "mic", "20"});
        QCOMPARE(input.exitCode, 0);
        QCOMPARE(desk.data().value("micVolume").toInt(), 20);
    }
    void cliAudioStreamSelectorsDoNotFollowAReusedIndex() {
        DeviceDesk desk;
        desk.mode("recycle-stream");
        const auto snapshot = CliApi::execute(desk.s.env, {"audio", "status"});
        const QString selector = snapshot.body.value("data").toObject().value("streams").toArray().first().toObject().value("selector").toString();
        QCOMPARE(selector, QString("7@700"));
        const auto moved = CliApi::execute(desk.s.env, {"audio", "stream", "move", selector, "phones"});
        QCOMPARE(moved.body.value("error").toObject().value("code").toString(), QString("stale_target"));
        QVERIFY(ChangeCoordinator(desk.s.env).history().isEmpty());
        QVERIFY(!QFile::exists(desk.s.env.home + "/changed"));
    }
    void cliApplyThenUndoRoundTrips() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{\"idle\":{\"lock\":300}}");
        QCOMPARE(Cli::run(s.env, {"apply", "power.idle.lock", "600"}), 0);
        QVERIFY(readAll(config).contains("600"));
        ChangeCoordinator coordinator(s.env);
        QCOMPARE(Cli::run(s.env, {"undo", coordinator.history().first().id}), 0);
        QVERIFY(readAll(config).contains("300"));
    }
    void cliSettlesAnInterruptedChangeBeforeANewOne() {
        Sandbox s;
        s.omarchy("exit 0");
        writeFile(s.env.configHome + "/omarchy/shell.json", "{\"idle\":{\"lock\":300}}");
        ChangeCoordinator coordinator(s.env);
        Transaction tx;
        tx.id = "old";
        tx.label = "X";
        tx.phase = "applying";
        writeFile(coordinator.journalPath(), QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n");
        QCOMPARE(Cli::run(s.env, {"apply", "power.idle.lock", "600"}), 0);
        QCOMPARE(coordinator.transaction("old").phase, QString("interrupted"));
    }
    void cliLaunchAsksFirstForConsequentialRoutes() {
        Sandbox s;
        s.omarchy("exit 0");
        s.command("omarchy-launch-floating-terminal-with-presentation", "touch \"$HOME/launched\"");
        QCOMPARE(Cli::run(s.env, {"launch", "setup.reset"}), 1);
        QTest::qWait(100);
        QVERIFY(!QFile::exists(s.env.home + "/launched"));
        QCOMPARE(Cli::run(s.env, {"launch", "not.a.route"}), 1);
        QCOMPARE(Cli::run(s.env, {"launch", "appearance.theme"}), 1);
    }
    void menuIntegrationKeepsExistingEntries() {
        Sandbox s;
        s.omarchy("exit 0");
        const QString menu = s.env.configHome + "/omarchy/extensions/omarchy-menu.jsonc";
        writeFile(menu, "{\n  // my comment\n  \"personal\": {\"label\": \"Personal\"},\n}\n");
        QCOMPARE(Cli::run(s.env, {"integrate", "menu"}), 0);
        QByteArray text = readAll(menu);
        QVERIFY(text.contains("// my comment"));
        QVERIFY(text.contains("\"personal\""));
        QCOMPARE(FileOps::jsonGet(text, {"omarchy-control", "action"}).toString(), QString("omarchy-control open"));
        // Running it twice does not stack up a second entry.
        QCOMPARE(Cli::run(s.env, {"integrate", "menu"}), 0);
        QCOMPARE(readAll(menu).count("\"omarchy-control\""), qsizetype(1));
        QCOMPARE(Cli::run(s.env, {"integrate", "menu", "--remove"}), 0);
        text = readAll(menu);
        QVERIFY(!text.contains("omarchy-control"));
        QVERIFY(text.contains("// my comment"));
        QVERIFY(text.contains("\"personal\""));
    }
    void menuIntegrationStartsFromTheStockTemplate() {
        Sandbox s;
        s.omarchy("exit 0");
        writeFile(s.env.omarchyPath + "/config/omarchy/extensions/omarchy-menu.jsonc", "{\n  // Extend the menu.\n}\n");
        QCOMPARE(Cli::run(s.env, {"integrate", "menu"}), 0);
        const QByteArray text = readAll(s.env.configHome + "/omarchy/extensions/omarchy-menu.jsonc");
        QVERIFY(text.contains("// Extend the menu."));
        QVERIFY(text.contains("Control Center"));
    }
    void skillInstallWritesTheBundledSkill() {
        Sandbox s;
        QCOMPARE(Cli::run(s.env, {"skill", "install"}), 0);
        const QByteArray skill = readAll(s.env.home + "/.claude/skills/omarchy-control/SKILL.md");
        QVERIFY(skill.startsWith("---\nname: omarchy-control"));
    }
    void skillReadsAndPlansDoNotCreateRegistrationFiles() {
        Sandbox s;
        for (const QString &action : {QStringLiteral("status"), QStringLiteral("plan")}) {
            const auto response = CliApi::execute(s.env, {"skill", action});
            QCOMPARE(response.exitCode, 0);
            QVERIFY(!response.body.value("verified").toBool());
            QVERIFY(!response.body.value("data").toObject().value("harnessDiscoveryVerified").toBool());
        }
        QCOMPARE(CliApi::execute(s.env, {"skill", "install", "--dry-run"}).exitCode, 0);
        QVERIFY(!QFileInfo::exists(s.env.appStateDir));
        QVERIFY(!QFileInfo::exists(s.env.dataHome));
        QVERIFY(!QFileInfo::exists(s.env.home + "/.claude"));
        QVERIFY(s.calls().isEmpty());
    }
    void skillBridgeKeepsStockGuidesAndRestoresTheOriginalRelativeLink() {
        Sandbox s;
        const QString stock = s.env.omarchyPath + "/default/agents/skills/omarchy";
        const QByteArray text = "---\nname: omarchy\ndescription: Stock fixture\n---\nRead [guide](plugins.md).\n";
        writeFile(stock + "/SKILL.md", text);
        writeFile(stock + "/plugins.md", "stock guide");
        const QString root = s.env.home + "/.claude/skills";
        QDir().mkpath(root);
        const QString relative = QDir(root).relativeFilePath(stock);
        QVERIFY(::symlink(QFile::encodeName(relative).constData(), QFile::encodeName(root + "/omarchy").constData()) == 0);
        const auto installed = CliApi::execute(s.env, {"skill", "install"});
        QCOMPARE(installed.exitCode, 0);
        QVERIFY(installed.body.value("verified").toBool());
        QVERIFY(installed.body.value("data").toObject().value("registrationComplete").toBool());
        const QByteArray bridge = readAll(root + "/omarchy/SKILL.md");
        QVERIFY(bridge.contains("omarchy-control commands"));
        QVERIFY(bridge.contains(".upstream/SKILL.md"));
        QCOMPARE(readAll(root + "/omarchy/.upstream/plugins.md"), QByteArray("stock guide"));
        QCOMPARE(readAll(stock + "/SKILL.md"), text);
        const QByteArray manifest = readAll(s.env.appStateDir + "/skill-integration/manifest.json");
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 0);
        QCOMPARE(readAll(s.env.appStateDir + "/skill-integration/manifest.json"), manifest);
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 0);
        QByteArray target(4096, '\0');
        const ssize_t size = ::readlink(QFile::encodeName(root + "/omarchy").constData(), target.data(), target.size());
        QVERIFY(size > 0);
        target.resize(size);
        QCOMPARE(target, QFile::encodeName(relative));
        QVERIFY(!QFileInfo(root + "/omarchy-control").isSymbolicLink());
        QCOMPARE(readAll(root + "/omarchy/SKILL.md"), text);
        QVERIFY(s.calls().isEmpty());
    }
    void skillCustomDirectoriesAndUnrelatedLinksArePreserved() {
        Sandbox s;
        writeFile(s.env.omarchyPath + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nstock");
        const QString root = s.env.home + "/.claude/skills";
        writeFile(root + "/omarchy/SKILL.md", "custom stock skill");
        const QString other = s.env.home + "/personal";
        writeFile(other + "/SKILL.md", "custom companion");
        QVERIFY(QFile::link(other, root + "/omarchy-control"));
        const auto response = CliApi::execute(s.env, {"skill", "install"});
        QCOMPARE(response.exitCode, 1);
        QCOMPARE(response.body.value("error").toObject().value("code").toString(), QString("registration_conflict"));
        QCOMPARE(readAll(root + "/omarchy/SKILL.md"), QByteArray("custom stock skill"));
        QCOMPARE(QFileInfo(root + "/omarchy-control").symLinkTarget(), other);
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 0);
        QCOMPARE(readAll(other + "/SKILL.md"), QByteArray("custom companion"));
    }
    void skillEditedGeneratedFilesBlockRepairAndSurviveRemovalWithReferences() {
        Sandbox s;
        const QString stock = s.env.omarchyPath + "/default/agents/skills/omarchy";
        writeFile(stock + "/SKILL.md", "---\nname: omarchy\n---\nstock");
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString data = s.env.dataHome + "/omarchy-control/agent-skills";
        writeFile(data + "/omarchy/SKILL.md", "user edited wrapper");
        const auto repaired = CliApi::execute(s.env, {"skill", "reconcile"});
        QCOMPARE(repaired.exitCode, 1);
        QVERIFY(!repaired.body.value("data").toObject().value("registrationComplete").toBool());
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 1);
        QCOMPARE(readAll(data + "/omarchy/SKILL.md"), QByteArray("user edited wrapper"));
        QCOMPARE(readAll(data + "/omarchy/.upstream/SKILL.md"), readAll(stock + "/SKILL.md"));
        QVERIFY(QFileInfo::exists(data + "/omarchy-control/SKILL.md"));
    }
    void skillEditedRegistrationRetainsFallbackPayload() {
        Sandbox s;
        writeFile(s.env.omarchyPath + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nstock");
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString link = s.env.home + "/.claude/skills/omarchy";
        QVERIFY(QFile::remove(link));
        const QString custom = s.env.home + "/custom";
        writeFile(custom + "/SKILL.md", "personal");
        QVERIFY(QFile::link(custom, link));
        const auto removed = CliApi::execute(s.env, {"skill", "remove"});
        QCOMPARE(removed.exitCode, 1);
        QCOMPARE(removed.body.value("outcome").toString(), QString("partial"));
        QCOMPARE(QFileInfo(link).symLinkTarget(), custom);
        QVERIFY(QFileInfo::exists(s.env.dataHome + "/omarchy-control/agent-skills/omarchy/.upstream/SKILL.md"));
        QCOMPARE(readAll(custom + "/SKILL.md"), QByteArray("personal"));
    }
    void skillAliasesAreDeduplicatedAndHermesProfilesAreDiscovered() {
        Sandbox s;
        const QString root = s.env.home + "/.agents/skills";
        QDir().mkpath(root);
        QDir().mkpath(s.env.home + "/.claude");
        QVERIFY(QFile::link(root, s.env.home + "/.claude/skills"));
        QDir().mkpath(s.env.home + "/.hermes/profiles/work");
        const auto response = CliApi::execute(s.env, {"skill", "install"});
        QCOMPARE(response.exitCode, 0);
        QCOMPARE(response.body.value("data").toObject().value("registrations").toArray().size(), 4);
        QVERIFY(QFileInfo::exists(s.env.home + "/.hermes/profiles/work/skills/omarchy-control/SKILL.md"));
        QCOMPARE(QFileInfo(s.env.home + "/.claude/skills").symLinkTarget(), root);
    }
    void skillDevelopmentRelinksReadCurrentUpstreamAndCanBeReconciled() {
        Sandbox s;
        const QString dev1 = s.env.home + "/dev/first";
        const QString dev2 = s.env.home + "/dev/second";
        writeFile(dev1 + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nfirst");
        writeFile(dev2 + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nsecond");
        const QString active = s.env.home + "/active-omarchy";
        QVERIFY(QFile::link(dev1, active));
        s.env.omarchyPath = active;
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString wrapper = s.env.home + "/.claude/skills/omarchy";
        QVERIFY(readAll(wrapper + "/.upstream/SKILL.md").endsWith("first"));
        QVERIFY(QFile::remove(active));
        QVERIFY(QFile::link(dev2, active));
        QVERIFY(readAll(wrapper + "/.upstream/SKILL.md").endsWith("second"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 0);
        // Omarchy provisioning may restore the stock link between calls.
        QVERIFY(QFile::remove(wrapper));
        QVERIFY(QFile::link(active + "/default/agents/skills/omarchy", wrapper));
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 0);
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 0);
        QVERIFY(readAll(wrapper + "/SKILL.md").endsWith("second"));
    }
    void skillOptOutSurvivesReconcileAndOptInIsExplicit() {
        Sandbox s;
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        QCOMPARE(CliApi::execute(s.env, {"skill", "opt-out"}).exitCode, 0);
        const QString skill = s.env.home + "/.claude/skills/omarchy-control/SKILL.md";
        QVERIFY(!QFileInfo::exists(skill));
        for (const QString &action : {QStringLiteral("install"), QStringLiteral("reconcile")}) {
            const auto response = CliApi::execute(s.env, {"skill", action});
            QCOMPARE(response.exitCode, 0);
            QCOMPARE(response.body.value("outcome").toString(), QString("opted_out"));
            QVERIFY(!QFileInfo::exists(skill));
        }
        QCOMPARE(CliApi::execute(s.env, {"skill", "opt-in"}).exitCode, 0);
        QVERIFY(QFileInfo::exists(skill));
    }
    void skillInterruptedRegistrationAndRemovalHaveRecoverableIntent() {
        Sandbox s;
        const QString stock = s.env.omarchyPath + "/default/agents/skills/omarchy";
        writeFile(stock + "/SKILL.md", "---\nname: omarchy\n---\nstock");
        const QString root = s.env.home + "/.claude/skills";
        QDir().mkpath(root);
        QVERIFY(QFile::link(stock, root + "/omarchy"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString path = s.env.appStateDir + "/skill-integration/manifest.json";
        auto doc = QJsonDocument::fromJson(readAll(path)).object();
        auto entries = doc.value("entries").toObject();
        auto entry = entries.value(root + "/omarchy").toObject();
        entry["pending"] = entry.value("current");
        entry["current"] = QJsonObject{{"type", "missing"}};
        entries[root + "/omarchy"] = entry;
        doc["entries"] = entries;
        writeFile(path, QJsonDocument(doc).toJson());
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 0);
        doc = QJsonDocument::fromJson(readAll(path)).object();
        entries = doc.value("entries").toObject();
        entry = entries.value(root + "/omarchy").toObject();
        entry["pending"] = entry.value("original");
        entries[root + "/omarchy"] = entry;
        doc["entries"] = entries;
        writeFile(path, QJsonDocument(doc).toJson());
        QVERIFY(QFile::remove(root + "/omarchy"));
        QVERIFY(QFile::link(stock, root + "/omarchy"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 0);
        QCOMPARE(QFileInfo(root + "/omarchy").symLinkTarget(), stock);
    }
    void skillUpstreamIntegrationIsNotWrappedAgain() {
        Sandbox s;
        const QString stock = s.env.omarchyPath + "/default/agents/skills/omarchy";
        writeFile(stock + "/SKILL.md", "---\nname: omarchy\n---\nConsult omarchy-control if installed.");
        const QString root = s.env.home + "/.claude/skills";
        QDir().mkpath(root);
        QVERIFY(QFile::link(stock, root + "/omarchy"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        QCOMPARE(QFileInfo(root + "/omarchy").symLinkTarget(), stock);
        const auto status = CliApi::execute(s.env, {"skill", "status"}).body.value("data").toObject();
        QVERIFY(status.value("upstreamReferenceDetected").toBool());
        QVERIFY(!status.value("registrationComplete").toBool());
    }
    void skillLockAndMalformedManifestNeverOverwriteRegistration() {
        Sandbox s;
        const QString state = s.env.appStateDir + "/skill-integration";
        QDir().mkpath(state);
        QLockFile lock(state + "/registration.lock");
        QVERIFY(lock.tryLock());
        auto response = CliApi::execute(s.env, {"skill", "install"});
        QCOMPARE(response.body.value("error").toObject().value("code").toString(), QString("integration_busy"));
        QVERIFY(!QFileInfo::exists(s.env.home + "/.claude"));
        lock.unlock();
        for (const QByteArray &bytes : {QByteArray("garbage"), QByteArray("{\"version\":2}")}) {
            writeFile(state + "/manifest.json", bytes);
            response = CliApi::execute(s.env, {"skill", "install"});
            QCOMPARE(response.exitCode, 1);
            QCOMPARE(readAll(state + "/manifest.json"), bytes);
            QVERIFY(!QFileInfo::exists(s.env.home + "/.claude"));
        }
    }
    void skillExplicitCustomHomeTargetsAndUnsafePaths() {
        Sandbox s;
        s.env.process.insert("CLAUDE_CONFIG_DIR", s.env.home + "/agent-home");
        const QString root = s.env.home + "/agent-home/skills";
        QCOMPARE(CliApi::execute(s.env, {"skill", "install", "--target", root}).exitCode, 0);
        QVERIFY(QFileInfo::exists(root + "/omarchy-control/SKILL.md"));
        QVERIFY(!QFileInfo::exists(s.env.home + "/.claude"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "opt-out"}).exitCode, 0);
        QVERIFY(!QFileInfo::exists(root + "/omarchy-control/SKILL.md"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "opt-in"}).exitCode, 0);
        QVERIFY(QFileInfo::exists(root + "/omarchy-control/SKILL.md"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove", "--target", root}).exitCode, 0);
        const QString outside = s.dir.path() + "/outside";
        QDir().mkpath(outside);
        QVERIFY(QFile::link(outside, s.env.home + "/outside-link"));
        for (const QString &target : {outside, s.env.home + "/outside-link", s.env.home + "/../bad", QString("relative")}) {
            const auto response = CliApi::execute(s.env, {"skill", "install", "--target", target});
            QCOMPARE(response.exitCode, 1);
            QVERIFY(!QFileInfo::exists(outside + "/omarchy-control"));
        }
    }
    void skillLinkedDataAndHardlinkedPayloadAreRefused() {
        Sandbox s;
        const QString real = s.env.home + "/real-data";
        QDir().mkpath(real);
        QDir().mkpath(QFileInfo(s.env.dataHome).absolutePath());
        QVERIFY(QFile::link(real, s.env.dataHome));
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 1);
        QVERIFY(!QFileInfo::exists(real + "/omarchy-control"));
        QVERIFY(QFile::remove(s.env.dataHome));
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString payload = s.env.dataHome + "/omarchy-control/agent-skills/omarchy-control/SKILL.md";
        const QString other = s.env.home + "/other-copy";
        QVERIFY(::link(QFile::encodeName(payload).constData(), QFile::encodeName(other).constData()) == 0);
        const QByteArray bytes = readAll(other);
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 1);
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 1);
        QCOMPARE(readAll(other), bytes);
    }
    void skillTargetRemovalIsScopedAndGeneratedUpgradesUseRecordedOwnership() {
        Sandbox s;
        const QString stock = s.env.omarchyPath + "/default/agents/skills/omarchy";
        writeFile(stock + "/SKILL.md", "---\nname: omarchy\n---\nstock");
        const QString first = s.env.home + "/first/skills", second = s.env.home + "/second/skills";
        QCOMPARE(CliApi::execute(s.env, {"skill", "install", "--target", first, "--target", second}).exitCode, 0);
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove", "--target", first}).exitCode, 0);
        QVERIFY(!QFileInfo::exists(first + "/omarchy-control/SKILL.md"));
        QVERIFY(QFileInfo::exists(second + "/omarchy-control/SKILL.md"));
        const QString payload = s.env.dataHome + "/omarchy-control/agent-skills/omarchy-control/SKILL.md";
        const QString manifest = s.env.appStateDir + "/skill-integration/manifest.json";
        const QByteArray old = "---\nname: omarchy-control\n---\nrecorded old bundle";
        writeFile(payload, old);
        auto doc = QJsonDocument::fromJson(readAll(manifest)).object();
        auto entries = doc.value("entries").toObject();
        auto entry = entries.value(payload).toObject();
        entry["current"] = QJsonObject{{"type", "file"}, {"sha256", FileOps::fingerprintBytes(old)}};
        entries[payload] = entry;
        doc["entries"] = entries;
        writeFile(manifest, QJsonDocument(doc).toJson());
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile", "--target", second}).exitCode, 0);
        QCOMPARE(readAll(payload), readAll(":/data/skill.md"));
        QVERIFY(!QFileInfo::exists(first + "/omarchy-control/SKILL.md"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove", "--target", second}).exitCode, 0);
        QVERIFY(!QFileInfo::exists(payload));
    }
    void skillStateLinksAndDanglingCustomSkillsAreNeverFollowed() {
        Sandbox s;
        const QString external = s.env.home + "/elsewhere";
        QDir().mkpath(external);
        QDir().mkpath(s.env.appStateDir);
        QVERIFY(QFile::link(external, s.env.appStateDir + "/skill-integration"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 1);
        QVERIFY(!QFileInfo::exists(external + "/manifest.json"));
        QVERIFY(QFile::remove(s.env.appStateDir + "/skill-integration"));
        const QString root = s.env.home + "/.claude/skills";
        QDir().mkpath(root);
        const QString absent = s.env.home + "/missing";
        QVERIFY(QFile::link(absent, root + "/omarchy-control"));
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 1);
        QCOMPARE(QFileInfo(root + "/omarchy-control").symLinkTarget(), absent);
        QVERIFY(!QFileInfo::exists(absent));
    }
    void skillStatusExposesConflictingUpstreamReferences() {
        Sandbox s;
        writeFile(s.env.omarchyPath + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nstock");
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString path = s.env.dataHome + "/omarchy-control/agent-skills/omarchy/.upstream";
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::link(s.env.home + "/personal-guides", path));
        const auto status = CliApi::execute(s.env, {"skill", "status"});
        QCOMPARE(status.exitCode, 0);
        const auto data = status.body.value("data").toObject();
        QVERIFY(data.value("hasConflicts").toBool());
        QVERIFY(!data.value("registrationComplete").toBool());
        bool reported = false;
        for (const auto &value : data.value("artifacts").toArray()) {
            const auto row = value.toObject();
            if (row.value("path") == path) {
                QVERIFY(!row.value("writable").toBool());
                QCOMPARE(row.value("state").toObject().value("target").toString(), s.env.home + "/personal-guides");
                reported = true;
            }
        }
        QVERIFY(reported);
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 1);
        QCOMPARE(QFileInfo(path).symLinkTarget(), s.env.home + "/personal-guides");
    }
    void skillReconcileMigratesOwnedVisibleGuidesWithoutDuplicatingStockSkills() {
        Sandbox s;
        const QString stock = s.env.omarchyPath + "/default/agents/skills/omarchy";
        writeFile(stock + "/SKILL.md", "---\nname: omarchy\n---\nstock");
        writeFile(stock + "/plugins.md", "current guide");
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString data = s.env.dataHome + "/omarchy-control/agent-skills/omarchy";
        const QString oldPath = data + "/upstream", newPath = data + "/.upstream";
        QVERIFY(QFile::rename(newPath, oldPath));
        const QByteArray oldBody = readAll(data + "/SKILL.md").replace(".upstream/", "upstream/").replace("`.upstream`", "`upstream`");
        writeFile(data + "/SKILL.md", oldBody);
        const QString path = s.env.appStateDir + "/skill-integration/manifest.json";
        auto manifest = QJsonDocument::fromJson(readAll(path)).object();
        auto entries = manifest.value("entries").toObject();
        entries[oldPath] = entries.take(newPath);
        auto bodyEntry = entries.value(data + "/SKILL.md").toObject();
        bodyEntry["current"] = QJsonObject{{"type", "file"}, {"sha256", FileOps::fingerprintBytes(oldBody)}};
        entries[data + "/SKILL.md"] = bodyEntry;
        manifest["entries"] = entries;
        writeFile(path, QJsonDocument(manifest).toJson());
        const QByteArray recorded = readAll(path);
        const auto plan = CliApi::execute(s.env, {"skill", "plan"});
        QCOMPARE(plan.exitCode, 0);
        QVERIFY(!plan.body.value("data").toObject().value("registrationComplete").toBool());
        QCOMPARE(readAll(path), recorded);
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 0);
        QVERIFY(!QFileInfo(oldPath).isSymbolicLink());
        QCOMPARE(readAll(data + "/SKILL.md"), readAll(":/data/skill-bridge.md"));
        QCOMPARE(readAll(newPath + "/plugins.md"), QByteArray("current guide"));
        const auto migrated = QJsonDocument::fromJson(readAll(path)).object().value("entries").toObject();
        QVERIFY(!migrated.contains(oldPath));
        QVERIFY(migrated.contains(newPath));
        QCOMPARE(CliApi::execute(s.env, {"skill", "remove"}).exitCode, 0);
        QVERIFY(!QFileInfo(newPath).isSymbolicLink());
    }
    void skillVisibleGuideMigrationPreservesCustomReferences() {
        Sandbox s;
        writeFile(s.env.omarchyPath + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nstock");
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString data = s.env.dataHome + "/omarchy-control/agent-skills/omarchy";
        const QString custom = s.env.home + "/my-guides";
        QVERIFY(QFile::link(custom, data + "/upstream"));
        const QByteArray body = readAll(data + "/SKILL.md");
        const auto status = CliApi::execute(s.env, {"skill", "status"});
        QVERIFY(status.body.value("data").toObject().value("hasConflicts").toBool());
        QCOMPARE(CliApi::execute(s.env, {"skill", "reconcile"}).exitCode, 1);
        QCOMPARE(QFileInfo(data + "/upstream").symLinkTarget(), custom);
        QCOMPARE(readAll(data + "/SKILL.md"), body);
    }
    void skillGuideMigrationIntentRemainsReadableAtTheTargetLimit() {
        Sandbox s;
        writeFile(s.env.omarchyPath + "/default/agents/skills/omarchy/SKILL.md", "---\nname: omarchy\n---\nstock");
        QCOMPARE(CliApi::execute(s.env, {"skill", "install"}).exitCode, 0);
        const QString data = s.env.dataHome + "/omarchy-control/agent-skills";
        const QString path = s.env.appStateDir + "/skill-integration/manifest.json";
        auto manifest = QJsonDocument::fromJson(readAll(path)).object();
        auto entries = manifest.value("entries").toObject();
        auto legacy = entries.value(data + "/omarchy/.upstream").toObject();
        legacy["pending"] = QJsonObject{{"type", "missing"}};
        entries[data + "/omarchy/upstream"] = legacy;
        for (int i = 1; i < 32; ++i) {
            const QString root = s.env.home + "/pending-target-" + QString::number(i);
            for (const QString &name : {QStringLiteral("omarchy"), QStringLiteral("omarchy-control")})
                entries[root + '/' + name] = QJsonObject{{"role", "registration"}, {"original", QJsonObject{{"type", "missing"}}},
                    {"current", QJsonObject{{"type", "link"}, {"target", data + '/' + name}}}};
        }
        manifest["entries"] = entries;
        const QByteArray recorded = QJsonDocument(manifest).toJson();
        writeFile(path, recorded);
        QCOMPARE(CliApi::execute(s.env, {"skill", "status"}).exitCode, 0);
        QCOMPARE(readAll(path), recorded);
    }
    void skillGrammarRejectsUnsupportedOptions_data() {
        QTest::addColumn<QStringList>("arguments");
        for (const QStringList &args : {QStringList{"skill"}, {"skill", "nope"}, {"skill", "install", "extra"},
             {"skill", "install", "--yes"}, {"skill", "status", "--dry-run"}, {"skill", "plan", "--dry-run"},
             {"skill", "install", "--target"}, {"skill", "opt-out", "--target", "/tmp/skills"},
             {"skill", "opt-in", "--dry-run"}, {"skill", "status", "--live"}, {"commands", "--target", "/tmp/skills"}})
            QTest::newRow(args.join(' ').toUtf8().constData()) << args;
    }
    void skillGrammarRejectsUnsupportedOptions() {
        QFETCH(QStringList, arguments);
        Sandbox s;
        const auto response = CliApi::execute(s.env, arguments);
        QCOMPARE(response.exitCode, 1);
        QCOMPARE(response.body.value("error").toObject().value("code").toString(), QString("invalid_arguments"));
        QVERIFY(!QFileInfo::exists(s.env.appStateDir));
    }
    void cliRulesAndShortcutsRespectValidation() {
        Sandbox s;
        s.minimalHyprland();
        QCOMPARE(Cli::run(s.env, {"rule", "add", "firefox", "exec", "rm"}), 1);
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
        QCOMPARE(Cli::run(s.env, {"shortcut", "add", "F", "x", "y"}), 1);
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }

    // ---- Interface behavior ---------------------------------------------------
    void editsMadeOutsideTheAppAppearOnTheirOwn() {
        Sandbox s;
        fakeDesktop(s);
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, "{\"idle\":{\"lock\":300}}");
        Backend backend(s.env);
        backend.pageShown("power");
        const auto value = [&] { return backend.states()->value(Backend::stateKey("power.idle.lock")).toMap().value("value").toString(); };
        QTRY_COMPARE_WITH_TIMEOUT(value(), QString("300"), 5000);
        // Replaced atomically, as an editor or Omarchy's own commands do.
        QVERIFY(FileOps::atomicWrite(config, "{\"idle\":{\"lock\":900}}"));
        QTRY_COMPARE_WITH_TIMEOUT(value(), QString("900"), 5000);
    }
    void theOverviewReadsWhetherBluetoothIsOn() {
        Sandbox s;
        fakeDesktop(s);
        s.omarchy(R"SH(case "$*" in "bluetooth power is-on") exit 0;; esac; exit 0)SH");
        s.bluetooth("exit 0");
        Backend backend(s.env);
        QCOMPARE(backend.bluetooth()->status(), QString("loading"));
        QVERIFY(!backend.bluetooth()->powered());
        // Without ever visiting the Sound page.
        backend.pageShown("overview");
        QTRY_COMPARE_WITH_TIMEOUT(backend.bluetooth()->status(), QString("ok"), 5000);
        QVERIFY(backend.bluetooth()->powered());
    }
    void theOwnershipPromptGuardsTheFirstWrite() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        QObject *window = engine.rootObjects().first();
        auto *dialog = window->findChild<QObject *>("confirmDialog");
        QVERIFY(dialog);
        const QString lua = s.env.configHome + "/hypr/hyprland.lua";
        const QByteArray stock = readAll(lua);
        QMetaObject::invokeMethod(window, "requestChange", Q_ARG(QVariant, "input.repeat_rate"), Q_ARG(QVariant, "31"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(dialog->property("body").toString().contains("hyprland.lua"));
        QTest::qWait(150);
        // Nothing moves while the question is open, and declining leaves it all alone.
        QCOMPARE(readAll(lua), stock);
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
        QMetaObject::invokeMethod(dialog, "close");
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::qWait(150);
        QCOMPARE(readAll(lua), stock);
        QVERIFY(!QFile::exists(s.env.configHome + "/hypr/omarchy-control.lua"));
    }
    void aFailedChangeShowsOnItsRow() {
        Sandbox s;
        fakeDesktop(s);
        s.omarchy(R"(case "$*" in version) echo 4.5.0-3;; 'hyprland corners') echo theme;; 'hyprland corners '*) echo denied >&2; exit 4;; esac)");
        Backend backend(s.env);
        QSignalSpy toasts(&backend, &Backend::toast);
        backend.commit("appearance.corners", "sharp");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.states()->value(Backend::stateKey("appearance.corners")).toMap().value("error").toString().isEmpty(), 8000);
        const auto state = backend.states()->value(Backend::stateKey("appearance.corners")).toMap();
        QVERIFY(!state.value("busy").toBool());
        QVERIFY(state.value("error").toString().contains("denied"));
        QTRY_COMPARE_WITH_TIMEOUT(backend.states()->value(Backend::stateKey("appearance.corners")).toMap().value("value").toString(), QString("theme"), 5000);
    }
    void restartNoticesAreCollected() {
        Sandbox s;
        fakeDesktop(s);
        s.omarchy(R"(f="$HOME/size"; case "$*" in version) echo 4.5.0-3;; 'display text size '*) printf '[font]\nbase-size = %s\n' "$4" > "$HOME/.config/omarchy/shell.toml";; esac)");
        Backend backend(s.env);
        backend.commit("appearance.textsize", "16");
        QTRY_COMPARE_WITH_TIMEOUT(backend.pendingRestarts().size(), 1, 8000);
        backend.clearRestartNotice();
        QVERIFY(backend.pendingRestarts().isEmpty());
    }

    // ---- Interface ------------------------------------------------------------
    void everyPageLoadsWithoutQmlWarnings() {
        // Loads the real QML against a fake machine and visits every page, so a
        // broken binding or a missing component fails here rather than on screen.
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlApplicationEngine::warnings, this, [&](const QList<QQmlError> &list) {
            for (const auto &warning : list)
                warnings << warning.toString();
        });
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join("\n")));
        QObject *window = engine.rootObjects().first();
        for (const auto &value : backend.pages()) {
            const QString page = value.toMap().value("id").toString();
            QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, page), Q_ARG(QVariant, QString()));
            QTest::qWait(250);
            QCOMPARE(window->property("currentPage").toString(), page);
        }
        // Search shows results; arriving by search scrolls to and marks the row.
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "power"), Q_ARG(QVariant, "power.stayawake"));
        QTest::qWait(200);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }
    // Tests and the screenshot harness run offscreen on the real session bus, so they must
    // never put an icon in the real bar. And with no tray to wait in, closing still quits.
    void withoutATrayClosingTheWindowQuits() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QVERIFY(!trayHost.available());
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window, 3000));
        QVERIFY(!window->property("inTray").toBool());
        QSignalSpy quit(&engine, &QQmlEngine::quit);
        window->close();
        QTRY_COMPARE(quit.count(), 1);
    }
    void keyboardDrivesSearchAndRespectsTyping() {
        Sandbox s;
        fakeDesktop(s);
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QSignalSpy quit(&engine, &QQmlEngine::quit);

        // A letter that means Quit elsewhere is only a letter while typing.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(window->property("typing").toBool());
        for (const char c : QByteArray("quiet"))
            QTest::keyClick(window, c);
        QTest::qWait(30);
        QCOMPARE(quit.count(), 0);
        QVERIFY(window->activeFocusItem()->property("text").toString().contains("quiet"));
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(window->activeFocusItem()->property("text").toString().isEmpty());

        // Search then Enter lands on the destination without running anything.
        for (const char c : QByteArray("stay awake"))
            QTest::keyClick(window, c);
        QTest::qWait(30);
        const int commandsBefore = s.calls().size();
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(window->property("currentPage").toString(), QString("power"));
        QVERIFY(!s.calls().mid(commandsBefore).join('\n').contains("toggle idle stay-awake"));
        QVERIFY(!window->property("typing").toBool());

        // Sent without Shift: the offscreen platform has no keymap to resolve Shift+?
        // the way a real keyboard layout does. That path is on the live checklist.
        QTest::keyClick(window, Qt::Key_Question);
        QTRY_VERIFY(window->property("helpVisible").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("helpVisible").toBool());

        // Alt+Left returns to where search started from.
        QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(window->property("currentPage").toString(), QString("overview"));

        QTest::keyClick(window, Qt::Key_Q);
        QTRY_COMPARE(quit.count(), 1);
    }
    void stateMapReflectsReads() {
        Sandbox s;
        fakeDesktop(s);
        Backend backend(s.env);
        backend.refresh("appearance.corners");
        QTRY_COMPARE_WITH_TIMEOUT(backend.states()->value(Backend::stateKey("appearance.corners")).toMap().value("status").toString(), QString("ok"), 5000);
        const auto state = backend.states()->value(Backend::stateKey("appearance.corners")).toMap();
        QCOMPARE(state.value("value").toString(), QString("theme"));
        QCOMPARE(state.value("display").toString(), QString("Follow theme"));
    }
    void pluginLifecyclePlansVerifiesAndRecoversAcrossCliCalls() {
        ShellDesk desk; desk.mode("lifecycle");
        desk.s.env.process.insert("USER", "testuser");
        const QString source = desk.s.env.omarchyPath + "/shell/plugins/clock";
        writeFile(source + "/manifest.json", R"({"id":"omarchy.clock","name":"Clock","kinds":["bar-widget"],"entryPoints":{"barWidget":"Widget.qml"}})");
        writeFile(source + "/Widget.qml", "import QtQuick\nItem {}\n");
        desk.s.command("omarchy-plugin-catalog", "printf '%s' '" + QJsonDocument(QJsonArray{QJsonObject{{"id", "omarchy.clock"}, {"firstParty", true}, {"sourceDir", source}, {"manifestPath", source + "/manifest.json"}}}).toJson(QJsonDocument::Compact) + "'");
        auto call = [&](QStringList args) { args << "--api-version" << "1"; return desk.api(args); };
        auto plan = call({"plugin", "clone", "omarchy.clock", "--dry-run"});
        QVERIFY2(plan.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(plan).toJson())));
        const QString path = desk.s.env.configHome + "/omarchy/plugins/testuser.clock";
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!call({"plugin", "clone", "omarchy.clock"}).value("ok").toBool());
        auto changed = call({"plugin", "clone", "omarchy.clock", "--yes"});
        QVERIFY2(changed.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(changed).toJson())));
        QVERIFY(changed.value("verified").toBool());
        const QString cloneToken = changed.value("data").toObject().value("transaction").toObject().value("extra").toObject().value("recoveryToken").toString();
        QVERIFY(!cloneToken.isEmpty());
        QVERIFY(!call({"plugin", "clone", "omarchy.clock", "--dry-run"}).value("ok").toBool());
        auto removed = call({"plugin", "remove", "testuser.clock", "--yes"});
        QVERIFY2(removed.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(removed).toJson())));
        QVERIFY(!QFileInfo::exists(path));
        const QString removeToken = removed.value("data").toObject().value("transaction").toObject().value("extra").toObject().value("recoveryToken").toString();
        const auto recovered = call({"plugin", "recover", removeToken, "--yes"});
        QVERIFY2(recovered.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(recovered).toJson())));
        QVERIFY(QFileInfo::exists(path + "/Widget.qml"));
        const auto original = call({"plugin", "recover", cloneToken, "--yes"});
        QVERIFY2(original.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(original).toJson())));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(desk.s.calls().contains("omarchy plugin remove testuser.clock --yes"));
    }
    void pluginTreesRecoverDirectoriesGitAndLinks_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("directory") << "directory";
        QTest::newRow("git") << "git";
        QTest::newRow("link") << "link";
        QTest::newRow("clone") << "clone";
    }
    void pluginTreesRecoverDirectoriesGitAndLinks() {
        QFETCH(QString, kind);
        Sandbox s;
        const QString path = s.env.configHome + "/omarchy/plugins/local.test", config = s.env.configHome + "/omarchy/shell.json";
        const QString outside = s.dir.path() + "/outside";
        writeFile(outside + "/untouched", "preserve");
        writeFile(config, "original config");
        if (kind == "link") { QDir().mkpath(QFileInfo(path).absolutePath()); QVERIFY(QFile::link(outside, path)); }
        else if (kind != "clone") {
            writeFile(path + "/manifest.json", "original manifest");
            writeFile(path + "/nested/file", "executable contents");
            QFile::setPermissions(path + "/nested/file", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            QVERIFY(QFile::link("nested/file", path + "/relative-link"));
            if (kind == "git") writeFile(path + "/.git/objects/object", "git object");
        }
        const QString token = "pr-" + QString(32, 'a'); QString error; QJsonObject receipt;
        const auto before = PluginFiles::inspect(path);
        QVERIFY2(PluginFiles::capture(s.env, token, "local.test", &receipt, &error), qPrintable(error));
        if (kind == "link") QVERIFY(QFile::remove(path));
        else if (kind != "clone") QVERIFY(QDir(path).removeRecursively());
        else writeFile(path + "/manifest.json", "new clone");
        writeFile(config, "changed config");
        QVERIFY2(PluginFiles::finish(s.env, &receipt, true, &error), qPrintable(error));
        receipt = PluginFiles::load(s.env, token, &error);
        QVERIFY2(!receipt.isEmpty(), qPrintable(error));
        writeFile(config, "later edit");
        QVERIFY(!PluginFiles::canRecover(s.env, receipt, &error));
        QCOMPARE(readAll(config), QByteArray("later edit"));
        writeFile(config, "changed config");
        QVERIFY2(PluginFiles::recover(s.env, &receipt, &error), qPrintable(error));
        QCOMPARE(PluginFiles::inspect(path), before);
        QCOMPARE(readAll(config), QByteArray("original config"));
        QCOMPARE(readAll(outside + "/untouched"), QByteArray("preserve"));
        QVERIFY2(PluginFiles::recover(s.env, &receipt, &error), qPrintable(error));
        if (kind == "clone") QCOMPARE(readAll(QFileInfo(path).absolutePath() + "/.local.test.control-displaced-" + token + "/manifest.json"), QByteArray("new clone"));
    }
    void pluginRecoveryRejectsCorruptedSnapshotsAndSpecialFiles() {
        Sandbox s; QString error; QJsonObject receipt;
        const QString path = s.env.configHome + "/omarchy/plugins/local.test";
        writeFile(path + "/manifest.json", "original");
        const QString token = "pr-" + QString(32, 'b');
        QVERIFY(PluginFiles::capture(s.env, token, "local.test", &receipt, &error));
        writeFile(path + "/manifest.json", "changed");
        QVERIFY(PluginFiles::finish(s.env, &receipt, true, &error));
        auto snapshot = receipt.value("before").toObject(); auto entries = snapshot.value("entries").toArray();
        auto malicious = entries.last().toObject(); malicious["path"] = "../outside"; entries.append(malicious);
        snapshot["entries"] = entries; snapshot["fingerprint"] = FileOps::fingerprintBytes(QJsonDocument(entries).toJson(QJsonDocument::Compact)); receipt["before"] = snapshot;
        QVERIFY(!PluginFiles::recover(s.env, &receipt, &error));
        QCOMPARE(readAll(path + "/manifest.json"), QByteArray("changed"));
        QVERIFY(!PluginFiles::load(s.env, "../../other", &error).contains("token"));
        const QString fifo = path + "/fifo";
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QVERIFY(PluginFiles::inspect(path, true, &error).isEmpty());
    }
    void cliSearchPinsPreserveOtherPreferencesAndShareTheGuiWriter() {
        Sandbox s;
        fakeDesktop(s);
        const QString path = s.env.appConfigDir + "/preferences.json";
        auto reply = CliApi::execute(s.env, {"preferences", "pins"});
        QVERIFY(reply.body.value("ok").toBool());
        QVERIFY(!QFileInfo::exists(s.env.appConfigDir));
        reply = CliApi::execute(s.env, {"preferences", "pin", "appearance.theme", "--dry-run"});
        QVERIFY(reply.body.value("ok").toBool());
        QVERIFY(!QFileInfo::exists(s.env.appConfigDir));
        writeFile(path, R"({"extra":{"keep":true},"pins":[]})");
        Backend stale(s.env);
        reply = CliApi::execute(s.env, {"preferences", "pin", "appearance.theme"});
        QVERIFY(reply.body.value("ok").toBool());
        stale.togglePin("appearance.icons");
        QCOMPARE(stale.pins(), (QStringList{"appearance.theme", "appearance.icons"}));
        QVERIFY(QJsonDocument::fromJson(readAll(path)).object().value("extra").toObject().value("keep").toBool());
        reply = CliApi::execute(s.env, {"preferences", "pin", "appearance.theme"});
        QCOMPARE(reply.body.value("outcome").toString(), QString("unchanged"));
        QVERIFY(!CliApi::execute(s.env, {"preferences", "pin", "unknown.id"}).body.value("ok").toBool());
        writeFile(path, R"({"pins":["old.removed.id"]})");
        QVERIFY(CliApi::execute(s.env, {"preferences", "unpin", "old.removed.id"}).body.value("ok").toBool());
        writeFile(path, "invalid");
        const auto malformed = CliApi::execute(s.env, {"preferences", "pin", "appearance.theme"}).body;
        QVERIFY(!malformed.value("ok").toBool());
        QCOMPARE(malformed.value("outcome").toString(), QString("blocked"));
        QCOMPARE(readAll(path), QByteArray("invalid"));
        QFile::remove(path);
        const QString outside = s.dir.path() + "/outside";
        writeFile(outside, "{}");
        QVERIFY(QFile::link(outside, path));
        QVERIFY(!CliApi::execute(s.env, {"preferences", "pin", "appearance.theme"}).body.value("ok").toBool());
        QCOMPARE(readAll(outside), QByteArray("{}"));
    }
    void pinsPersist() {
        Sandbox s;
        fakeDesktop(s);
        {
            Backend backend(s.env);
            backend.togglePin("appearance.theme");
            QCOMPARE(backend.pins(), QStringList{"appearance.theme"});
        }
        Backend again(s.env);
        QCOMPARE(again.pins(), QStringList{"appearance.theme"});
        again.togglePin("appearance.theme");
        QVERIFY(again.pins().isEmpty());
        again.togglePin("not.a.real.id");
        QVERIFY(again.pins().isEmpty());
    }
    void problemsAreSurfaced() {
        Sandbox s;
        s.omarchy(kScript15);
        s.command("hyprctl", "exit 1");
        Backend backend(s.env);
        QTRY_VERIFY_WITH_TIMEOUT(backend.problems().size() >= 2, 5000);
        const QString text = QVariant(backend.problems()).toStringList().join(' ');
        QStringList messages;
        for (const auto &row : backend.problems())
            messages << row.toMap().value("text").toString();
        QVERIFY(messages.join('\n').contains("shell"));
        QVERIFY(!backend.compatible());
    }

private:
    static QByteArray withoutMonitor(const QByteArray &json, const QString &name) {
        QJsonArray kept;
        for (const auto &monitor : QJsonDocument::fromJson(json).array())
            if (monitor.toObject().value("name").toString() != name)
                kept << monitor;
        return QJsonDocument(kept).toJson(QJsonDocument::Compact);
    }
    static QVariantMap stagedRow(const DisplayProvider &displays, const QString &output) {
        for (const auto &row : displays.staged())
            if (row.toMap().value("output").toString() == output)
                return row.toMap();
        return {};
    }
    static QByteArray twoMonitors() {
        return R"([{"name":"eDP-1","width":1920,"height":1080,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"disabled":false,"focused":true,"availableModes":["1920x1080@60.00Hz"]},{"name":"DP-1","width":2560,"height":1440,"refreshRate":60,"x":1920,"y":0,"scale":1,"transform":0,"disabled":false,"availableModes":["2560x1440@60.00Hz"]},{"name":"DP-2","width":1920,"height":1080,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"disabled":true,"availableModes":["1920x1080@60.00Hz"]}])";
    }
    static void monitorFixture(const Sandbox &s, const QByteArray &monitors) {
        s.minimalHyprland();
        writeFile(s.dir.path() + "/hypr-monitors", monitors);
    }
    static QString watchdogPlan(const Sandbox &s, int seconds) {
        const QJsonArray previous{
            QJsonObject{{"output", "DP-2"}, {"disabled", true}},
            QJsonObject{{"output", "eDP-1"}, {"mode", "1920x1080@60.00"}, {"position", "0x0"}, {"scale", 1}, {"transform", 0}},
            QJsonObject{{"output", "DP-1"}, {"mode", "2560x1440@60.00"}, {"position", "1920x0"}, {"scale", 1}, {"transform", 0}},
        };
        const QJsonObject plan{{"previous", previous}, {"seconds", seconds}, {"keepFile", s.dir.path() + "/keep"}, {"revertFile", s.dir.path() + "/revert"}, {"resultFile", s.dir.path() + "/result"}};
        const QString path = s.dir.path() + "/plan.json";
        writeFile(path, QJsonDocument(plan).toJson());
        return path;
    }
    static void fakeDesktop(const Sandbox &s) {
        s.minimalHyprland();
        s.omarchy(kScript16);
        s.command("pactl", kScript17);
        s.command("nmcli", "exit 0");
        s.bluetooth("exit 0");
        s.command("systemctl", "echo '[]'");
        s.command("xdg-mime", "exit 0");
        s.command("df", "exit 0");
        writeFile(s.env.configHome + "/omarchy/shell.json", "{}");
    }

private slots:
    // --- The Desktop page ---

    void aDockPinShowsWhatItsDesktopEntrySays() {
        Sandbox s;
        IconDesk desk(s);
        const QString shared = s.env.dataDirs.last() + "/applications/";
        addPack(desk.system, "Yaru-blue", {"folder", "named-app"});
        addPack(desk.system, "Other", {"named-app"});
        addPack(desk.system, "hicolor", {"only-in-hicolor"});
        writeFile(shared + "named.desktop", "[Desktop Entry]\nName=Named App\nIcon=named-app\n[Desktop Action x]\nName=Not this\nIcon=not-this\n");
        writeFile(shared + "hicolor.desktop", "[Desktop Entry]\nName=H\nIcon=only-in-hicolor\n");
        writeFile(shared + "absolute.desktop", "[Desktop Entry]\nName=A\nIcon=" + s.dir.path().toUtf8() + "/pic.png\n");
        writeFile(s.dir.path() + "/pic.png", "x");
        writeFile(shared + "gone.desktop", "[Desktop Entry]\nName=G\nIcon=/nowhere/pic.png\n");
        writeFile(shared + "noicon.desktop", "[Desktop Entry]\nName=Plain\n");
        writeFile(shared + "pixmap.desktop", "[Desktop Entry]\nName=P\nIcon=oldapp.png\n");
        writeFile(s.env.dataDirs.last() + "/pixmaps/oldapp.png", "x");
        writeFile(shared + "mine.desktop", "[Desktop Entry]\nName=System copy\n");
        writeFile(s.env.dataHome + "/applications/mine.desktop", "[Desktop Entry]\nName=My copy\n");

        const auto app = [&](const QString &id) { return PluginsProvider::resolveDockApp(s.env, id, "Yaru-blue"); };
        QCOMPARE(app("named").value("name").toString(), QString("Named App"));
        QVERIFY(app("named").value("icon").toString().contains("/Yaru-blue/"));
        // The pack in use decides, as it does for every other app.
        QVERIFY(PluginsProvider::resolveDockApp(s.env, "named", "Other").value("icon").toString().contains("/Other/"));
        QVERIFY(app("hicolor").value("icon").toString().contains("/hicolor/"));
        QVERIFY(app("absolute").value("icon").toString().endsWith("/pic.png"));
        QVERIFY(app("pixmap").value("icon").toString().endsWith("/pixmaps/oldapp.png"));
        QCOMPARE(app("mine").value("name").toString(), QString("My copy"));
        // Each of these falls through to the same drawn fallback, with its entry's word on it.
        QCOMPARE(app("gone").value("icon").toString(), QString());
        QCOMPARE(app("noicon").value("icon").toString(), QString());
        QCOMPARE(app("noicon").value("name").toString(), QString("Plain"));
        QVERIFY(!app("not-installed").value("installed").toBool());
        QCOMPARE(app("../../etc/passwd").value("name").toString(), QString());
    }

    void dockAppsKeepTheirOrderAndUseThePackInUse() {
        Sandbox s;
        IconDesk desk(s);
        const QString shared = s.env.dataDirs.last() + "/applications/";
        writeFile(shared + "b.desktop", "[Desktop Entry]\nName=Bee\nIcon=folder\n");
        writeFile(shared + "a.desktop", "[Desktop Entry]\nName=Ay\nIcon=folder\n");
        writeFile(s.env.configHome + "/omarchy/shell.json", "{\"dock\":{\"pinned\":[\"b\",\"missing\",\"a\"]}}");
        desk.setEffective("Numix");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        PluginsProvider plugins(&runner, &coordinator);
        plugins.refresh();
        QTRY_VERIFY(plugins.dockApps().value(0).toMap().value("icon").toString().contains("/Numix/"));
        QStringList names;
        for (const auto &row : plugins.dockApps())
            names << row.toMap().value("name").toString();
        QCOMPARE(names, QStringList({"Bee", "", "Ay"}));
        QCOMPARE(plugins.dockPins(), QStringList({"b", "missing", "a"}));
    }

    void aReorderThatDidNotHappenIsNotReportedAsDone() {
        Sandbox s;
        const QByteArray before = "{\"version\":1,\"bar\":{\"layout\":{\"left\":[\"a\",\"b\",\"c\"]}}}\n";
        const QString config = s.env.configHome + "/omarchy/shell.json";
        writeFile(config, before);
        // Exits cleanly and changes nothing. The widget is still in its section, which is
        // all the old check looked at.
        s.omarchy("exit 0");
        s.command("omarchy-shell", "case \"$2\" in listShellConfig) cat \"$HOME/.config/omarchy/shell.json\";; esac");
        ChangeCoordinator coordinator(s.env);
        CommandRunner runner(s.env);
        PluginsProvider plugins(&runner, &coordinator);
        QSignalSpy messages(&plugins, &PluginsProvider::message);
        plugins.moveWidget("a", "left", 2, "left", 0);
        QVERIFY(plugins.changing());
        QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 8000);
        QVERIFY(messages.first().at(1).toBool());
        QVERIFY(messages.first().at(0).toString().contains("did not move"));
        QVERIFY(!plugins.changing());
        QCOMPARE(readAll(config), before);
    }

private:
    struct DesktopPage {
        Sandbox &s;
        Translator translator;
        Theme theme;
        Backend backend;
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;
        explicit DesktopPage(Sandbox &sandbox)
            : s(sandbox), translator(sandbox.env.configHome + "/omarchy/language", ":/i18n", sandbox.env.omarchyPath + "/default/i18n/locales.json"), theme(sandbox.env), backend(sandbox.env) {
            QQuickStyle::setStyle("Material");
            engine.addImageProvider("icons", new IconProvider);
            engine.addImageProvider("iconpacks", new IconPackImageProvider(sandbox.env));
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("backend", &backend);
            engine.rootContext()->setContextProperty("initialPage", QString());
            engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
            engine.load(QUrl("qrc:/Main.qml"));
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            window->resize(1427, 1102);
            window->requestActivate();
        }
        void show(const QString &focus = {}) {
            open("desktop", focus);
        }
        void open(const QString &pageId, const QString &focus = {}) {
            QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, pageId), Q_ARG(QVariant, focus));
        }
        // Every setting row on screen by its id, as often as it is drawn.
        QStringList rowIds() const {
            QStringList ids;
            for (QQuickItem *row : named("settingRow"))
                ids << row->property("entryId").toString();
            return ids;
        }
        QQuickItem *row(const QString &id) const {
            for (QQuickItem *item : named("settingRow"))
                if (item->property("entryId").toString() == id)
                    return item;
            return nullptr;
        }
        QObject *backend_() { return &backend; }
        QQuickItem *pageView() const {
            const auto pages = named("pageLoader");
            return pages.isEmpty() ? nullptr : qvariant_cast<QQuickItem *>(pages.first()->property("item"));
        }
        qreal scrolled() const {
            QQuickItem *view = pageView();
            QObject *flick = view ? qvariant_cast<QObject *>(view->property("contentItem")) : nullptr;
            return flick ? flick->property("contentY").toReal() : -1;
        }
        bool focusWithin(QQuickItem *item) const {
            for (QQuickItem *f = window->activeFocusItem(); f; f = f->parentItem())
                if (f == item)
                    return true;
            return false;
        }
        QList<QQuickItem *> named(const QString &objectName) const {
            QList<QQuickItem *> found;
            std::function<void(QQuickItem *)> search = [&](QQuickItem *item) {
                if (item->objectName() == objectName && item->isVisible())
                    found << item;
                for (QQuickItem *child : item->childItems())
                    search(child);
            };
            search(window->contentItem());
            return found;
        }
        QQuickItem *chip(const QString &id) const {
            for (QQuickItem *item : named("barWidgetChip"))
                if (item->property("widgetId").toString() == id)
                    return item;
            return nullptr;
        }
        QQuickItem *handle(const QString &id) const {
            QQuickItem *c = chip(id);
            if (!c)
                return nullptr;
            for (QQuickItem *child : c->childItems())
                if (child->objectName() == "barWidgetHandle")
                    return child;
            return nullptr;
        }
        QStringList section(const QString &id) {
            QStringList ids;
            for (const auto &value : backend.plugins()->barLayout().value(id).toList())
                ids << value.toString();
            return ids;
        }
        QStringList moves() const {
            QStringList out;
            for (const QString &call : s.calls())
                if (call.startsWith("omarchy bar move"))
                    out << call.mid(17);
            return out;
        }
    };

    static void desktopFixture(Sandbox &s) {
        fakeDesktop(s);
        s.omarchy(kFakeBar);
        s.command("omarchy-shell", "case \"$2\" in listShellConfig) cat \"$HOME/.config/omarchy/shell.json\";; reloadConfig) echo ok;; esac");
        const QString state = s.env.home + "/bar-state/";
        writeFile(state + "left", "omarchy.menu omarchy.workspaces\n");
        writeFile(state + "centerBegin", "\n");
        writeFile(state + "center", "omarchy.clock\n");
        writeFile(state + "centerEnd", "omarchy.weather omarchy.system-update\n");
        writeFile(state + "right", "omarchy.tray omarchy.network omarchy.audio omarchy.power mystery.thing\n");
        writeFile(state + "tray", "\n");
        writeFile(state + "pins", "firefox gone org.gnome.Nautilus\n");
        writeFile(s.env.configHome + "/omarchy/shell.json", "{\"version\":1,\"bar\":{\"id\":\"local.test-bar\",\"layout\":{\"left\":[\"omarchy.menu\",\"omarchy.workspaces\"],\"centerBegin\":[],\"center\":[{\"id\":\"omarchy.clock\"}],\"centerEnd\":[\"omarchy.weather\",\"omarchy.system-update\"],\"right\":[\"omarchy.tray\",\"omarchy.network\",\"omarchy.audio\",\"omarchy.power\",\"mystery.thing\"],\"tray\":[]}},\"dock\":{\"pinned\":[\"firefox\",\"gone\",\"org.gnome.Nautilus\"]}}\n");
        const QString shared = s.env.dataDirs.last();
        addPack(shared + "/icons", "Yaru-blue", {"web-browser", "folder"});
        writeFile(shared + "/applications/firefox.desktop", "[Desktop Entry]\nName=Firefox\nIcon=web-browser\n");
        writeFile(shared + "/applications/org.gnome.Nautilus.desktop", "[Desktop Entry]\nName=Files\nIcon=folder\n");
    }

private slots:
    void inputPanelsPlaceEveryEntryOnce() {
        Catalog catalog;
        QStringList placed;
        QHash<QString, QStringList> byPanel;
        QString header;
        for (const auto &row : catalog.panelLayout("input"))
            for (const auto &column : row.toObject().value("columns").toArray())
                for (const auto &panelValue : column.toArray()) {
                    const auto panel = panelValue.toObject();
                    const QString name = panel.value("title").toString();
                    if (!panel.value("header").toString().isEmpty()) {
                        header = panel.value("header").toString();
                        placed << header;
                        byPanel[name] << header;
                    }
                    for (const auto &id : panel.value("rows").toArray()) {
                        placed << id.toString();
                        byPanel[name] << id.toString();
                    }
                }
        QStringList expected;
        for (const auto *entry : catalog.entriesForPage("input"))
            expected << entry->id;
        QCOMPARE(expected.size(), 36);
        QStringList sortedPlaced = placed, sortedExpected = expected;
        sortedPlaced.sort();
        sortedExpected.sort();
        QCOMPARE(sortedPlaced, sortedExpected);
        // The touchpad switch is the Touchpad panel's heading, and counts once there.
        QCOMPARE(header, QString("input.touchpad"));
        QCOMPARE(byPanel.value("Keyboard").size(), 4);
        QCOMPARE(byPanel.value("Mouse").size(), 4);
        QCOMPARE(byPanel.value("Touchpad").size(), 8);
        QCOMPARE(byPanel.value("Touchscreen").size(), 1);
        QCOMPARE(byPanel.value("Typing").size(), 3);
        QCOMPARE(byPanel.value("Dictation").size(), 8);
        QCOMPARE(byPanel.value("Voxtype").size(), 4);
        QCOMPARE(byPanel.value("Accessibility").size(), 4);
        QVERIFY(byPanel.value("Dictation").contains("dictation.install.superwhisper"));
        QVERIFY(byPanel.value("Voxtype").contains("setup.default.dictation.voxtype"));
        // Separating the driver reset: settings first, the action last.
        QCOMPARE(byPanel.value("Touchpad").last(), QString("restart.trackpad.input"));
    }
    void anEntryNoPanelClaimsStillGetsOne() {
        Sandbox s;
        const QString dir = s.dir.path() + "/catalog";
        writeFile(dir + "/pages.json", R"([{"id":"p","label":"P","icon":"x","description":"","sections":["A","B"],"panels":[{"columns":[[{"section":"A","icon":"x"}]]}]}])");
        writeFile(dir + "/settings.json", R"([{"id":"a.one","page":"p","section":"A","label":"One","type":"toggle"},{"id":"b.two","page":"p","section":"B","label":"Two","type":"toggle"},{"id":"a.three","page":"p","section":"A","label":"Three","type":"toggle","group":"Elsewhere"}])");
        writeFile(dir + "/workflows.json", "[]");
        Catalog catalog(dir);
        const QJsonArray layout = catalog.panelLayout("p");
        QCOMPARE(layout.size(), 2);
        const auto first = layout[0].toObject().value("columns").toArray()[0].toArray()[0].toObject();
        // A group with no panel of its own stays with its section.
        QCOMPARE(first.value("rows").toArray().size(), 2);
        const auto added = layout[1].toObject().value("columns").toArray()[0].toArray()[0].toObject();
        QCOMPARE(added.value("section").toString(), QString("B"));
        QCOMPARE(added.value("rows").toArray()[0].toString(), QString("b.two"));
        QVERIFY(catalog.panelLayout("nothing").isEmpty());
    }
    void theInputPageDrawsEveryEntryOnceAndLandsOnEach() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("input");
        QTRY_COMPARE(page.named("sectionPanel").size(), 8);
        QStringList drawn = page.rowIds();
        QStringList expected;
        const Catalog catalog;
        for (const auto *entry : catalog.entriesForPage("input"))
            expected << entry->id;
        drawn.sort();
        expected.sort();
        QCOMPARE(drawn, expected);
        // Seven jump links, in the page's order.
        QStringList links;
        for (QQuickItem *flow : page.named("sectionLinks"))
            for (QQuickItem *child : flow->childItems())
                if (child->objectName().startsWith("sectionLink_"))
                    links << child->objectName().mid(12);
        QCOMPARE(links, QStringList({"Keyboard", "Mouse", "Touchpad", "Touchscreen", "Typing", "Dictation", "Accessibility"}));
        QCOMPARE(page.pageView()->property("activeTab").toString(), QString("Keyboard"));
        // A link scrolls to its section and leaves the keyboard there.
        QQuickItem *link = page.named("sectionLink_Accessibility").first();
        QMetaObject::invokeMethod(link, "clicked");
        QTRY_VERIFY(page.scrolled() > 100);
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Accessibility"));
        QQuickItem *accessibility = nullptr;
        for (QQuickItem *panel : page.named("sectionPanel"))
            if (panel->property("section").toString() == "Accessibility")
                accessibility = panel;
        QVERIFY(accessibility);
        QTRY_VERIFY(page.focusWithin(accessibility));
        // Search lands on a row far down the page, from the top.
        page.open("input", "input.tp_drag");
        page.open("input", "input.dictation.paste");
        QTRY_VERIFY(page.focusWithin(page.row("input.dictation.paste")) || page.row("input.dictation.paste")->property("highlighted").toBool());
        QVERIFY(page.scrolled() > 100);
        // And on the touchpad switch, which is a panel's heading.
        page.open("input", "input.touchpad");
        QTRY_VERIFY(page.row("input.touchpad")->property("highlighted").toBool());
    }

    void theSoundPageShowsEveryDeviceAndLandsOnEachSection() {
        Sandbox s;
        desktopFixture(s);
        writeFile(s.env.sys("/sys/class/bluetooth/hci0/address"), "x");
        s.command("pactl", R"SH(
case "$*" in
  "-f json list sinks") echo '[{"index":151,"name":"spk","description":"Maker Chip (Series 3) HD Audio Speaker","mute":false,"volume":{"l":{"value_percent":"25%"}},"properties":{"object.id":"84","device.description":"Maker Chip (Series 3) HD Audio","device.profile.description":"Speaker"}},{"index":153,"name":"hdmi1","description":"Maker Chip (Series 3) HD Audio HDMI / DisplayPort 1 Output (Stereo)","mute":true,"volume":{"l":{"value_percent":"100%"}},"properties":{"object.id":"75","device.description":"Maker Chip (Series 3) HD Audio","device.profile.description":"HDMI / DisplayPort 1 Output"}}]';;
  "-f json list sources") echo '[]';;
  "-f json list sink-inputs") echo '[{"index":1,"sink":151,"volume":{},"properties":{"application.name":"Browser","media.name":"Playback"}},{"index":2,"sink":999,"volume":{},"properties":{"application.name":"Browser","media.name":"Playback"}}]';;
  get-default-sink) echo spk;;
esac)SH");
        s.bluetooth(R"SH(
case "$1" in
  devices) printf 'Device AA:BB:CC:DD:EE:03 Office Mouse\nDevice AA:BB:CC:DD:EE:04 Office Mouse\n';;
  info) printf 'Name: Office Mouse\nConnected: no\nPaired: yes\nIcon: input-mouse\n';;
esac)SH");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("sound");
        QTRY_COMPARE(page.named("deviceTitle").size(), 2);
        QStringList titles;
        for (QQuickItem *title : page.named("deviceTitle"))
            titles << title->property("text").toString();
        QCOMPARE(titles, QStringList({"HD Audio Speaker", "HDMI / DisplayPort 1"}));
        // Two streams from one app, one of them on an output that cannot be told.
        QTRY_COMPARE(page.named("streamOutput").size(), 2);
        // The selection is put back after the list fills, a moment later.
        QTRY_COMPARE(page.named("streamOutput").at(0)->property("currentIndex").toInt(), 0);
        QTRY_COMPARE(page.named("streamOutput").at(1)->property("currentIndex").toInt(), -1);
        QTRY_COMPARE(page.named("streamOutput").at(1)->property("displayText").toString(), QString());
        // Two devices with one name, both shown.
        QTRY_COMPARE(page.named("bluetoothDevice").size(), 2);
        // Muted says so in what is read aloud as well.
        QTRY_COMPARE(page.named("deviceMute").size(), 2);
        QVERIFY(page.named("deviceMute").at(1)->property("iconName").toString() == "soundoff");
        // All eight tools, and the jump links in the page's order.
        QStringList tools = page.rowIds();
        tools.sort();
        QCOMPARE(tools, QStringList({"audio.tuning", "helper.printer", "install.camera", "install.video.decode", "panel.audio", "panel.bluetooth", "restart.audio", "restart.bluetooth"}));
        QStringList links;
        for (QQuickItem *flow : page.named("sectionLinks"))
            for (QQuickItem *child : flow->childItems())
                if (child->objectName().startsWith("sectionLink_"))
                    links << child->objectName().mid(12);
        QCOMPARE(links, QStringList({"Output", "Microphone", "App audio", "Bluetooth", "Tools"}));
        // Search for each native section lands on its panel.
        auto holder = [&](const QString &block) -> QQuickItem * {
            for (QQuickItem *panel : page.named("sectionPanel"))
                if (panel->property("block").toString() == block)
                    return panel;
            return nullptr;
        };
        page.open("sound", "block.bluetooth");
        QTRY_VERIFY(page.focusWithin(holder("bluetooth")));
        page.open("sound", "block.output");
        QTRY_VERIFY(page.focusWithin(holder("output")));
        // The Tools link leads to the sound tools.
        QMetaObject::invokeMethod(page.named("sectionLink_Tools").first(), "clicked");
        QTRY_VERIFY(page.focusWithin(page.row("panel.audio")));
    }

    void theNetworkPageShowsEveryConnectionAndLandsOnEachPart() {
        Sandbox s;
        desktopFixture(s);
        writeFile(s.env.sys("/sys/class/net/wlan0/wireless/x"), "");
        s.command("nmcli", R"SH(
case "$*" in
  *WIFI,CONNECTIVITY*general) echo "enabled:full";;
  *DEVICE,TYPE,STATE,CONNECTION*device) printf 'wlan0:wifi:connected:Home\ntailscale0:tun:connected (externally):tailscale0\nenp1s0:ethernet:unavailable:\n';;
  *NAME,UUID,TYPE,ACTIVE*"connection show") printf 'Home:11111111-1111-1111-1111-111111111111:802-11-wireless:yes\nCafe:22222222-2222-2222-2222-222222222222:802-11-wireless:no\n';;
  *"wifi list"*) printf '*:Home:57:WPA2:02\\:00\\:00\\:00\\:00\\:01:wlan0\n :Cafe:40:WPA2:02\\:00\\:00\\:00\\:00\\:02:wlan0\n :Airport:20::02\\:00\\:00\\:00\\:00\\:03:wlan0\n';;
esac)SH");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("network");
        QTRY_COMPARE(page.named("connectionRow").size(), 3);
        QTRY_COMPARE(page.named("wifiNetwork").size(), 3);
        QPointer<QQuickItem> originalNetwork = page.named("wifiNetwork").first();
        auto *network = page.backend_()->property("network").value<NetworkProvider *>();
        QVERIFY(network);
        QSignalSpy refreshed(network, &NetworkProvider::refreshed);
        network->refresh();
        QTRY_COMPARE(refreshed.size(), 1);
        QVERIFY(originalNetwork);
        QCOMPARE(page.named("wifiNetwork").first(), originalNetwork.data());
        // Every catalog entry once, the band among them.
        QStringList rows = page.rowIds();
        QCOMPARE(rows.size(), 18);
        QCOMPARE(QSet<QString>(rows.begin(), rows.end()).size(), 18);
        QStringList links;
        for (QQuickItem *flow : page.named("sectionLinks"))
            for (QQuickItem *child : flow->childItems())
                if (child->objectName().startsWith("sectionLink_"))
                    links << child->objectName().mid(12);
        QCOMPARE(links, QStringList({"Connections", "Wi-Fi", "DNS", "Remote access", "Sharing", "Tools"}));
        // Search lands on the band inside the Wi-Fi card, and on a tool far down.
        page.open("network", "network.band");
        QTRY_VERIFY(page.row("network.band")->property("highlighted").toBool());
        page.open("network", "remove.sunshine");
        QTRY_VERIFY(page.focusWithin(page.row("remove.sunshine")));
        QMetaObject::invokeMethod(page.named("sectionLink_Tools").first(), "clicked");
        QTRY_VERIFY(page.focusWithin(page.row("trigger.tests.network-speedtest")));
    }

    void theNetworkPageDoesNotPresentFailedReadsAsOffOrEmpty() {
        Sandbox s;
        desktopFixture(s);
        writeFile(s.env.sys("/sys/class/net/wlan0/wireless/x"), "");
        s.command("nmcli", R"SH(
case "$*" in
 *WIFI,CONNECTIVITY*) if [ -f "$HOME/general-broken" ]; then exit 8; fi; echo enabled:full;;
 *) exit 8;;
esac)SH");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("network");
        QTRY_VERIFY(!page.named("wifiReadMessage").isEmpty());
        QTRY_COMPARE(page.named("wifiReadMessage").first()->property("text").toString(), QString("Wi-Fi networks could not be read. Try again."));
        QTRY_COMPARE(page.named("connectionReadMessage").first()->property("text").toString(), QString("NetworkManager is not responding, so connections cannot be shown."));
        QVERIFY(page.named("wifiNetwork").isEmpty());
        const auto power = page.named("wifiPower").first();
        QVERIFY(!power->property("unknown").toBool());
        writeFile(s.env.home + "/general-broken", "1");
        auto *network = page.backend_()->property("network").value<NetworkProvider *>();
        QVERIFY(network);
        network->refresh();
        QTRY_VERIFY(power->property("unknown").toBool());
        QTRY_VERIFY(!power->isEnabled());
        QTRY_COMPARE(page.named("wifiReadMessage").first()->property("text").toString(), QString("NetworkManager is not responding, so networks cannot be shown."));
    }

    void thePowerPageRunsEachTileAndFollowsItsState() {
        Sandbox s;
        desktopFixture(s);
        writeFile(s.env.sys("/sys/class/power_supply/BAT0/type"), "Battery\n");
        writeFile(s.env.sys("/sys/class/power_supply/BAT0/capacity"), "100\n");
        writeFile(s.env.sys("/sys/class/power_supply/BAT0/status"), "Full\n");
        writeFile(s.env.sys("/sys/class/power_supply/BAT0/cycle_count"), "8\n");
        const QString awake = s.env.stateHome + "/omarchy/indicators/stay-awake";
        writeFile(awake, "");
        s.omarchy(R"SH(
case "$*" in
  "toggle idle status") if [ -f "$HOME/.local/state/omarchy/indicators/stay-awake" ]; then echo '{"enabled":true}'; else echo '{"enabled":false}'; fi;;
  "hibernation available") exit 0;;
esac
exit 0)SH");
        s.command("omarchy-system-lock", "exit 0");
        s.command("omarchy-system", "exit 0");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("power");
        QTRY_COMPARE(page.named("actionTile").size(), 7);
        QStringList tiles;
        for (QQuickItem *tile : page.named("actionTile"))
            tiles << tile->property("entryId").toString();
        QCOMPARE(tiles, QStringList({"system.screensaver", "system.lock", "system.suspend", "system.hibernate", "system.logout", "system.reboot", "system.shutdown"}));
        // The rest of the twenty are rows, each once.
        QStringList rows = page.rowIds();
        QCOMPARE(rows.size(), 12);
        QCOMPARE(QSet<QString>(rows.begin(), rows.end()).size(), 12);
        QTRY_VERIFY(!page.named("batterySummary").isEmpty());
        // Idle timers paused is said while Stay awake is on, and only then.
        QTRY_COMPARE(page.named("panelNotice").size(), 1);
        QFile::remove(awake);
        QMetaObject::invokeMethod(page.backend_(), "refresh", Q_ARG(QString, "power.stayawake"));
        QTRY_COMPARE(page.named("panelNotice").size(), 0);
        // A tile runs exactly what its row would.
        QQuickItem *lock = nullptr, *shutdown = nullptr;
        for (QQuickItem *tile : page.named("actionTile")) {
            if (tile->property("entryId").toString() == "system.lock")
                lock = tile;
            if (tile->property("entryId").toString() == "system.shutdown")
                shutdown = tile;
        }
        QMetaObject::invokeMethod(lock, "clicked");
        QTRY_VERIFY(s.calls().contains("omarchy-system-lock "));
        // Shut down asks first, and runs only once accepted.
        auto *dialog = page.window->findChild<QObject *>("confirmDialog");
        QMetaObject::invokeMethod(shutdown, "clicked");
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(!s.calls().join('\n').contains("omarchy-system shutdown"));
        QMetaObject::invokeMethod(dialog, "accept_");
        QTRY_VERIFY(s.calls().join('\n').contains("omarchy-system shutdown"));
        // Search lands on a tile, and on the battery summary.
        page.open("power", "system.reboot");
        QTRY_VERIFY(page.window->activeFocusItem() && page.window->activeFocusItem()->property("entryId").toString() == "system.reboot");
        page.open("power", "block.battery");
        QTRY_VERIFY(page.focusWithin(page.named("batterySummary").first()));
    }
    void aComputerWithoutABatteryHasNoBatterySection() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("power");
        QTRY_COMPARE(page.named("actionTile").size(), 7);
        QVERIFY(page.named("batterySummary").isEmpty());
        QVERIFY(page.named("sectionLink_Battery").isEmpty());
        QVERIFY(!page.named("sectionLink_Power").isEmpty());
    }

    void theAppsPageOpensWhatSearchFindsAndRunsItsTools() {
        Sandbox s;
        desktopFixture(s);
        const QString apps = s.env.dataDirs.last() + "/applications/";
        writeFile(apps + "org.example.Docs.desktop", "[Desktop Entry]\nType=Application\nName=Docs\nMimeType=application/pdf;\n");
        writeFile(apps + "secret.desktop", "[Desktop Entry]\nType=Application\nName=Secret\nNoDisplay=true\n");
        // One association set elsewhere to an app that is not installed.
        s.command("xdg-mime", R"SH(case "$3" in application/pdf) echo org.example.Docs.desktop;; x-scheme-handler/mailto) echo hey.desktop;; *) echo;; esac)SH");
        s.command("omarchy-launch-floating-terminal-with-presentation", "exit 0");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("apps");
        QTRY_COMPARE(page.named("associationRow").size(), 7);
        // The unknown handler keeps its id; the known one shows its app's name.
        QStringList shown;
        for (QQuickItem *choice : page.named("associationChoice"))
            shown << choice->property("displayText").toString();
        QVERIFY(shown.contains("Docs"));
        QVERIFY(shown.contains("hey.desktop"));
        // Closed until asked: no agent rows, no library rows yet.
        QVERIFY(!page.row("setup.default.agent.claude"));
        QVERIFY(!page.row("install.gaming.retroarch"));
        QVERIFY(page.row("agent.account.copy"));
        // Search opens what holds the entry and lands on it.
        page.open("apps", "setup.default.agent.claude");
        QTRY_VERIFY(page.row("setup.default.agent.claude"));
        QTRY_VERIFY(page.focusWithin(page.row("setup.default.agent.claude")));
        page.open("apps", "install.gaming.retroarch");
        QTRY_VERIFY(page.row("install.gaming.retroarch"));
        QTRY_VERIFY(page.focusWithin(page.row("install.gaming.retroarch")));
        // Installed apps opens for its search destination, without hidden entries.
        page.open("apps", "block.installedApps");
        int listed = 0;
        bool secretListed = false;
        for (const auto &app : SystemProvider::readApps(s.env)) {
            listed += app.toMap().value("hidden").toBool() ? 0 : 1;
            secretListed = secretListed || app.toMap().value("name").toString() == "Secret";
        }
        QVERIFY(secretListed);
        QTRY_COMPARE(page.named("installedApp").size(), listed);
        QTRY_VERIFY(page.focusWithin(page.named("installedAppsFilter").first()));
        // The tools run their existing workflows.
        auto press = [&](const QString &id) {
            for (QQuickItem *child : page.row(id)->findChildren<QQuickItem *>())
                if (child->objectName() == "actionButton")
                    QMetaObject::invokeMethod(child, "clicked");
        };
        press("agent.account.copy");
        QTRY_VERIFY(s.calls().join('\n').contains("omarchy-launch-floating-terminal-with-presentation"));
        press("install.gaming.retroarch");
        QTRY_VERIFY(s.calls().filter("omarchy-launch-floating-terminal-with-presentation").size() >= 2);
    }

    void theAccountsPageReadsItsServicesAndRunsItsTools() {
        Sandbox s;
        desktopFixture(s);
        s.command("systemctl", R"SH(case "$*" in "is-active sshd") echo inactive; exit 3;; "is-active ufw") echo active;; *) echo '[]';; esac)SH");
        s.command("ufw", "exit 0");
        s.command("omarchy-launch-floating-terminal-with-presentation", "exit 0");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("accounts");
        QTRY_VERIFY(page.row("firewall.disable"));
        QStringList rows = page.rowIds();
        QCOMPARE(rows.size(), 20);
        QCOMPARE(QSet<QString>(rows.begin(), rows.end()).size(), 20);
        // The readings, each in its tone: SSH off is neutral, the firewall service running is good.
        QTRY_COMPARE(page.row("accounts.sshd")->property("tone").toString(), QString("neutral"));
        QTRY_COMPARE(page.row("accounts.firewall")->property("tone").toString(), QString("good"));
        // Search lands in each panel.
        page.open("accounts", "firewall.allow");
        QTRY_VERIFY(page.focusWithin(page.row("firewall.allow")));
        page.open("accounts", "setup.config.git");
        QTRY_VERIFY(page.focusWithin(page.row("setup.config.git")));
        auto press = [&](const QString &id) {
            for (QQuickItem *child : page.row(id)->findChildren<QQuickItem *>())
                if (child->objectName() == "actionButton")
                    QMetaObject::invokeMethod(child, "clicked");
        };
        // Allowing a port goes straight to its terminal; turning the firewall off asks first.
        press("firewall.allow");
        QTRY_VERIFY(s.calls().join('\n').contains("Port to allow"));
        auto *dialog = page.window->findChild<QObject *>("confirmDialog");
        press("firewall.disable");
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(!s.calls().join('\n').contains("ufw disable"));
        QMetaObject::invokeMethod(dialog, "accept_");
        QTRY_VERIFY(s.calls().join('\n').contains("ufw disable"));
    }

    void automaticTimezoneIsEditableOnTheSystemPage() {
        Sandbox s;
        desktopFixture(s);
        s.command("systemctl", kAutomaticTimezone);
        writeFile(s.env.home + "/timezone-enabled", "enabled");
        writeFile(s.env.home + "/timezone-active", "active");
        DesktopPage page(s);
        QVERIFY(page.window);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        const QString id = "system.timezone.auto";
        page.open("system", id);
        QTRY_VERIFY(page.row(id));
        QQuickItem *row = page.row(id);
        QTRY_COMPARE(row->property("status").toString(), QString("ok"));
        QVERIFY(row->property("editable").toBool());
        QQuickItem *toggle = nullptr;
        for (QQuickItem *item : row->findChildren<QQuickItem *>())
            if (item->property("label").toString() == "Automatic time zone updates" && item->property("on").isValid())
                toggle = item;
        QVERIFY(toggle);
        QVERIFY(toggle->isEnabled());
        QVERIFY(toggle->property("on").toBool());
        // Exercise the real control and shared backend, not just a settings-engine call.
        QMetaObject::invokeMethod(toggle, "clicked");
        QTRY_VERIFY(!toggle->property("on").toBool());
        QTRY_VERIFY(toggle->isEnabled());
        QMetaObject::invokeMethod(toggle, "clicked");
        QTRY_VERIFY(toggle->property("on").toBool());
        QVERIFY(s.calls().contains("systemctl --system disable --now automatic-timezone.timer"));
        QVERIFY(s.calls().contains("systemctl --system enable --now automatic-timezone.timer"));
        QVERIFY(s.calls().contains("systemctl --system start automatic-timezone.service"));
    }
    void theSystemPageManagesStorageStartupAndServices() {
        Sandbox s;
        desktopFixture(s);
        s.command("df", R"SH(cat <<'OUT'
Filesystem Type 1B-blocks Used Avail Use% Mounted on
/dev/a btrfs 1000000000000 420000000000 580000000000 42% /
/dev/a btrfs 1000000000000 420000000000 580000000000 42% /home
Notes.AppImage fuse.Notes.AppImage 180000000 180000000 0 100% /tmp/.mount_NotesAb12
OUT)SH");
        s.command("systemctl", R"SH(case "$*" in
*--user*list-units*) echo '[{"unit":"sync.service","active":"active","description":"Sync"},{"unit":"broken.service","active":"failed","description":"Broken"}]';;
*--system*list-units*) echo '[{"unit":"NetworkManager.service","active":"active","description":"Network Manager"}]';;
*list-unit-files*) echo '[]';;
*show*) for unit in "$@"; do :; done; printf 'Id=%s\nLoadState=loaded\nFragmentPath=/usr/lib/systemd/system/%s\nActiveState=active\nSubState=running\nUnitFileState=enabled\nType=simple\nResult=success\nExecMainStartTimestampMonotonic=10\n' "$unit" "$unit";;
esac)SH");
        for (int i = 0; i < 14; ++i)
            writeFile(s.env.sys(QString("/etc/xdg/autostart/agent%1.desktop").arg(i, 2, 10, QChar('0'))),
                      QString("[Desktop Entry]\nName=Agent %1\nComment=%2\nExec=agent\n").arg(i).arg(i == 7 ? "Keeps keys" : "Helps").toUtf8());
        s.command("omarchy-launch-about", "exit 0");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("system");

        // Disks apart from the app mounted from an image, which waits behind its count.
        QTRY_COMPARE(page.named("storageMount").size(), 2);
        QCOMPARE(page.named("temporaryMounts").size(), 1);

        // A first look at the startup list, the rest in place, and a filter over descriptions.
        QTRY_COMPARE(page.named("startupApp").size(), 12);
        QMetaObject::invokeMethod(page.named("startupShowAll").first(), "clicked");
        QTRY_COMPARE(page.named("startupApp").size(), 14);
        page.named("startupFilter").first()->setProperty("text", "keeps keys");
        QTRY_COMPARE(page.named("startupApp").size(), 1);
        QCOMPARE(page.named("startupApp").first()->property("appId").toString(), QString("agent07.desktop"));
        page.named("startupFilter").first()->setProperty("text", "nothing like this");
        QTRY_COMPARE(page.named("startupApp").size(), 0);
        page.named("startupFilter").first()->setProperty("text", "");

        // A failure opens Services by itself, and says so from outside.
        QTRY_VERIFY(!page.named("serviceRow").isEmpty());
        QCOMPARE(page.named("servicesFailed").size(), 1);
        auto button = [&](const QString &unit, const QString &name) -> QQuickItem * {
            for (QQuickItem *row : page.named("serviceRow"))
                if (row->property("unit").toString() == unit)
                    for (QQuickItem *child : row->findChildren<QQuickItem *>())
                        if (child->objectName() == name && child->isVisible())
                            return child;
            return nullptr;
        };
        // Your own service changes straight away.
        QTRY_VERIFY(button("broken.service", "serviceRestart"));
        QMetaObject::invokeMethod(button("broken.service", "serviceRestart"), "clicked");
        QTRY_VERIFY(s.calls().join('\n').contains("systemctl --user restart broken.service"));
        // A system one is confirmed first, with what it costs.
        QMetaObject::invokeMethod(page.named("servicesSystem").first(), "clicked");
        QTRY_VERIFY(button("NetworkManager.service", "serviceStartStop"));
        auto *dialog = page.window->findChild<QObject *>("confirmDialog");
        QMetaObject::invokeMethod(button("NetworkManager.service", "serviceStartStop"), "clicked");
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(dialog->property("body").toString().contains("freeze"));
        QVERIFY(!s.calls().join('\n').contains("--system stop"));
        QMetaObject::invokeMethod(dialog, "accept_");
        QTRY_VERIFY(s.calls().join('\n').contains("systemctl --system stop NetworkManager.service"));

        // Search lands on each block and lights its jump link.
        page.open("system", "block.startup");
        QTRY_VERIFY(page.focusWithin(page.named("startupFilter").first()));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Startup apps"));
        page.open("system", "block.services");
        QTRY_VERIFY(page.focusWithin(page.named("servicesFilter").first()));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Services"));

        // A library entry opens its group, lands, and runs its existing workflow.
        Catalog catalog;
        QString reset, computer;
        for (const auto *entry : catalog.entriesForPage("system")) {
            if (reset.isEmpty() && entry->section == "Reset to defaults")
                reset = entry->id;
            if (computer.isEmpty() && entry->section == "This computer")
                computer = entry->id;
        }
        QVERIFY(!reset.isEmpty() && !computer.isEmpty());
        QVERIFY(!page.row(reset));
        page.open("system", reset);
        QTRY_VERIFY(page.row(reset));
        QTRY_VERIFY(page.focusWithin(page.row(reset)));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Maintenance"));
        page.open("system", computer);
        QTRY_VERIFY(page.row(computer));
        QTRY_VERIFY(page.focusWithin(page.row(computer)));
        auto press = [&](const QString &id) {
            for (QQuickItem *child : page.row(id)->findChildren<QQuickItem *>())
                if (child->objectName() == "actionButton")
                    QMetaObject::invokeMethod(child, "clicked");
        };
        press("about");
        QTRY_VERIFY(s.calls().join('\n').contains("omarchy-launch-about"));
        // A reset asks first and runs nothing when declined.
        const int before = s.calls().size();
        press(reset);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QMetaObject::invokeMethod(dialog, "close");
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(s.calls().size(), before);
    }

    void theAdvancedPageShowsHistoryDiagnosticsAndTools() {
        Sandbox s;
        s.debugHelper();
        desktopFixture(s);
        s.command("omarchy-launch-floating-terminal-with-presentation", "exit 0");
        // Twelve records, every outcome among them, oldest first as the journal keeps them.
        QByteArray journal;
        auto add = [&](int i, const QString &setting, const QString &label, const QString &before, const QString &after, const QString &phase,
                       const QString &message = {}, const QString &extra = {}) {
            journal += QString(R"J({"id":"tx-%1","setting":"%2","label":"%3","kind":"setting","before":"%4","after":"%5","recovery":"","phase":"%6","message":"%7","at":"2026-11-0%8T12:%9:00Z"%10})J")
                           .arg(i).arg(setting, label, before, after, phase, message).arg(i < 4 ? 5 : 6).arg(i, 2, 10, QChar('0')).arg(extra).toUtf8() + "\n";
        };
        add(1, "gone.one", "Window gaps", "5", "10", "committed");
        add(2, "gone.two", "Repeat rate", "25", "40", "rolled-back", "Put back.");
        add(3, "gone.three", "Scale", "1.25", "1.5", "partial");
        add(4, "gone.four", "Font", "Iosevka", "Missing", "failed", "font not installed");
        add(5, "gone.five", "Power profile", "balanced", "performance", "interrupted");
        add(6, "gone.six", "Night light", "off", "on", "not-applied");
        add(7, "gone.seven", "Theme", "Jade Horse", "Catppuccin", "committed", {}, R"J(,"undoneBy":"tx-8")J");
        add(8, "gone.seven", "Theme", "Catppuccin", "Jade Horse", "committed");
        add(9, "appearance.icons", "Icon pack", "pack:breeze", "follow-theme", "committed");
        add(10, "appearance.icons", "Icon pack", "follow-theme", "pack:Yaru-blue", "committed");
        add(11, "gone.eleven", "Mouse speed", "0", "0.5", "committed");
        add(12, "gone.twelve", "Clock format", "24h", "12h", "committed");
        writeFile(s.env.appStateDir + "/journal.jsonl", journal);
        writeFile(s.env.configHome + "/omarchy/hooks/theme-set", "#!/bin/sh\n");
        QFile::setPermissions(s.env.configHome + "/omarchy/hooks/theme-set", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        writeFile(s.env.configHome + "/omarchy/hooks/post-update.sample", "#!/bin/sh\n");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("advanced");

        // A first look of nine, newest first, then all of them and back.
        QTRY_COMPARE(page.named("historyRecord").size(), 9);
        QCOMPARE(page.named("historyRecord").first()->property("recordId").toString(), QString("tx-12"));
        QMetaObject::invokeMethod(page.named("historyShowAll").first(), "clicked");
        QTRY_COMPARE(page.named("historyRecord").size(), 12);
        QStringList outcomes;
        for (QQuickItem *text : page.named("historyOutcome"))
            outcomes << text->property("text").toString();
        for (const char *expected : {"Done and confirmed", "Undone", "Failed, previous value restored", "Partly applied", "Not applied", "Interrupted; check the result"})
            QVERIFY2(outcomes.contains(expected), expected);
        // Undo only where the record allows it: not the undone one or those that did not land.
        QCOMPARE(page.named("historyUndo").size(), 6);
        QMetaObject::invokeMethod(page.named("historyShowAll").first(), "clicked");
        QTRY_COMPARE(page.named("historyRecord").size(), 9);

        // The filter reads labels, values as shown, and messages; the limit applies after it.
        QQuickItem *filter = page.named("historyFilter").first();
        filter->setProperty("text", "follow desktop");
        QTRY_COMPARE(page.named("historyRecord").size(), 2);
        filter->setProperty("text", "font not installed");
        QTRY_COMPARE(page.named("historyRecord").size(), 1);
        filter->setProperty("text", "nothing like this");
        QTRY_COMPARE(page.named("historyRecord").size(), 0);
        QCOMPARE(page.named("historyNoMatch").size(), 1);
        QMetaObject::invokeMethod(page.named("historyClearFilter").first(), "clicked");
        QTRY_COMPARE(page.named("historyRecord").size(), 9);

        // Undo goes through the engine, which refuses a setting that no longer exists and says so.
        QSignalSpy toasts(&page.backend, &Backend::toast);
        QMetaObject::invokeMethod(page.named("historyUndo").first(), "clicked");
        QTRY_VERIFY(!toasts.isEmpty());
        QVERIFY(toasts.first().at(0).toString().contains("no longer exists"));
        QTRY_VERIFY(page.named("historyUndo").first()->property("enabled").toBool());

        // Four readings, two more inside the details, and a report kept on this computer.
        QTRY_COMPARE(page.named("diagnosticsReading").size(), 4);
        QMetaObject::invokeMethod(page.named("diagnosticsDetails").first(), "clicked");
        QTRY_COMPARE(page.named("diagnosticsReading").size(), 6);
        QVERIFY(page.named("compatWarning").isEmpty());
        QMetaObject::invokeMethod(page.named("diagnosticsSave").first(), "clicked");
        QTRY_VERIFY(s.calls().join('\n').contains("omarchy debug --no-sudo --print"));
        QTRY_VERIFY(!page.named("diagnosticsSaved").isEmpty());
        QVERIFY(page.named("diagnosticsSaved").first()->property("text").toString().contains("diagnostics-"));

        // Config files: five first, the rest in place; the other sections closed with their counts.
        QStringList rows = page.rowIds();
        QCOMPARE(rows, QStringList({"setup.config.hyprland", "setup.input", "setup.keybindings", "config.shelljson", "config.shelltoml"}));
        QCOMPARE(page.named("fileFolder").first()->property("text").toString(), QString("~/.config/hypr/"));
        QMetaObject::invokeMethod(page.named("panelShowAll").first(), "clicked");
        QTRY_COMPARE(page.rowIds().size(), 20);
        QMetaObject::invokeMethod(page.named("panelShowAll").first(), "clicked");
        QTRY_COMPARE(page.rowIds().size(), 5);

        // Search lands in hidden places and lights the way there.
        page.open("advanced", "config.uwsm");
        QTRY_VERIFY(page.row("config.uwsm"));
        QTRY_VERIFY(page.focusWithin(page.row("config.uwsm")));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Config files"));
        page.open("advanced", "terminal.herdr");
        QTRY_VERIFY(page.row("terminal.herdr"));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Developer tools"));
        // A helper that is not installed says so, and its action waits.
        QTRY_VERIFY(page.row("terminal.herdr")->property("unavailable").toBool());
        page.open("advanced", "learn.bash");
        QTRY_VERIFY(page.focusWithin(page.row("learn.bash")));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Learn"));
        page.open("advanced", "block.hooks");
        QTRY_COMPARE(page.named("hookRow").size(), 1);
        QCOMPARE(page.named("hookRow").first()->property("visible").toBool(), true);
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("Hooks"));
        page.open("advanced", "block.history");
        QTRY_VERIFY(page.focusWithin(page.named("historyFilter").first()));
        QTRY_COMPARE(page.pageView()->property("activeTab").toString(), QString("History"));

        // The tools run their existing commands.
        auto press = [&](const QString &id) {
            for (QQuickItem *child : page.row(id)->findChildren<QQuickItem *>())
                if (child->objectName() == "actionButton")
                    QMetaObject::invokeMethod(child, "clicked");
        };
        page.open("advanced", "diagnostics.debug");
        QTRY_VERIFY(page.row("diagnostics.debug"));
        press("diagnostics.debug");
        QTRY_VERIFY(s.calls().join('\n').contains("omarchy-debug --no-sudo --print"));
        press("hook.install");
        QTRY_VERIFY(s.calls().join('\n').contains("omarchy-hook-install --help"));
    }
    void theAdvancedPageHasNoHistoryOrHooksYetAndPausedCommands() {
        Sandbox s;
        s.env.enforceCompat = true;
        desktopFixture(s);
        // A Hyprland newer than the one tested pauses its overrides until accepted.
        writeFile(s.bin("hyprctl"), readAll(s.bin("hyprctl")).replace("0.56.2", "0.57.0"));
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("advanced", "block.hooks");
        QTRY_COMPARE(page.named("historyEmpty").size(), 1);
        QVERIFY(page.named("historyFilter").isEmpty());
        QTRY_COMPARE(page.named("hooksEmpty").size(), 1);
        QTRY_COMPARE(page.named("compatWarning").size(), 1);
        QCOMPARE(page.named("compatCommand").first()->property("text").toString(), QString("Hyprland 0.57.0"));
        auto *dialog = page.window->findChild<QObject *>("confirmDialog");
        QMetaObject::invokeMethod(page.named("compatResume").first(), "clicked");
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(!page.named("compatWarning").isEmpty());
        QMetaObject::invokeMethod(dialog, "accept_");
        QTRY_VERIFY(page.named("compatWarning").isEmpty());
    }

    void aGithubCopyKeepsGithubAndARepositoryCopyUpdatesWithTheSystem() {
        QCoreApplication::setApplicationVersion("0.3.0");
        Sandbox s;
        desktopFixture(s);
        // pacman answers from two files: what the repository offers, and how the installed
        // copy was verified.
        writeFile(s.dir.path() + "/validated", "None");
        s.command("pacman", "case \"$1\" in\n"
                            "  -Si) printf 'Repository      : omarchy\\nName            : omarchy-control\\nVersion         : 0.3.1-1\\n';;\n"
                            "  -Qi) printf 'Name            : omarchy-control\\nVersion         : 0.3.0-1\\nValidated By    : %s\\n' \"$(cat '"
                                + s.dir.path().toUtf8() + "/validated')\";;\n"
                            "  *) exit 1;;\nesac");
        // As curl's -w writes it: the status after the body, with no newline to end it.
        s.command("curl", "printf '%s\\n200' '{\"tag_name\": \"v0.3.2\", \"assets\": []}'");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("about");
        auto text = [&](const QString &name) { return page.named(name).isEmpty() ? QString() : page.named(name).first()->property("text").toString(); };
        // Installed from GitHub: the repository changes nothing, and GitHub is asked as always.
        QTRY_VERIFY(!page.named("checkUpdates").isEmpty());
        QTest::qWait(300);
        QCOMPARE(page.backend.updates()->state(), QString("idle"));
        QVERIFY(page.named("updateSystem").isEmpty());
        QMetaObject::invokeMethod(page.named("checkUpdates").first(), "clicked");
        QTRY_VERIFY2(page.backend.updates()->state() == QLatin1String("available"), qPrintable(page.backend.updates()->error()));
        QCOMPARE(page.backend.updates()->latestVersion(), QString("0.3.2"));
        QVERIFY(!s.calls().filter("curl").isEmpty());
        CommandRunner runner(s.env);
        QVERIFY(!Updater::sourceBlocking(runner).fromRepository());

        // Installed, signed, by the repository: it updates with the system instead.
        writeFile(s.dir.path() + "/validated", "SHA-256 Sum  Signature");
        const int asked = s.calls().filter("curl").size();
        page.backend.updates()->readSource();
        QTRY_COMPARE(page.backend.updates()->state(), QString("repository"));
        QTRY_VERIFY(text("updateStatus").contains("Version 0.3.1 is waiting in the omarchy package repository"));
        QVERIFY(!page.named("updateSystem").isEmpty());
        QVERIFY(page.named("checkUpdates").isEmpty());
        page.backend.updates()->check();
        QTest::qWait(300);
        QCOMPARE(s.calls().filter("curl").size(), asked);
        QVERIFY(Updater::sourceBlocking(runner).fromRepository());
    }
    void theAboutPageShowsEveryUpdateState() {
        QCoreApplication::setApplicationVersion("0.1.0");
        Sandbox s;
        desktopFixture(s);
        // The answer GitHub gives is whatever this file says, so each state is one write away.
        writeFile(s.dir.path() + "/answer", "{}\n503");
        s.command("curl", "sleep \"$(cat '" + s.dir.path().toUtf8() + "/delay' 2>/dev/null || echo 0)\"; cat '" + s.dir.path().toUtf8() + "/answer'");
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.open("about");
        auto text = [&](const QString &name) { return page.named(name).isEmpty() ? QString() : page.named(name).first()->property("text").toString(); };
        auto shown = [&](const QString &name) { return !page.named(name).isEmpty(); };
        QTRY_VERIFY(shown("aboutVersion"));
        QVERIFY(text("aboutVersion").contains("0.1.0"));
        QCOMPARE(text("aboutName"), QString("Omarchy Control"));
        // Five facts, four project links and three credits with their icons, and no jump bar on a page without sections to jump to.
        QCOMPARE(page.named("aboutDetail").size(), 5);
        QCOMPARE(page.rowIds(), QStringList({"app.website", "app.releases", "app.issues", "app.license", "app.credits.iconic", "app.credits.inter", "app.credits.omarchy"}));
        for (const QString &id : page.rowIds())
            QVERIFY(!page.backend.entry(id).value("icon").toString().isEmpty());
        QVERIFY(page.named("sectionLinks").isEmpty());
        // Nothing is asked of the network until someone asks.
        QCOMPARE(text("updateStatus"), QString("Updates are only checked when you ask."));
        QVERIFY(s.calls().filter("curl").isEmpty());

        // Checking: said, and not asked twice.
        writeFile(s.dir.path() + "/delay", "2");
        QMetaObject::invokeMethod(page.named("checkUpdates").first(), "clicked");
        QTRY_COMPARE(text("updateStatus"), QString("Checking for updates…"));
        QVERIFY(!page.named("checkUpdates").first()->property("enabled").toBool());
        // A failure, in the provider's words, with the way to try again.
        QTRY_COMPARE_WITH_TIMEOUT(page.backend.updates()->state(), QString("error"), 6000);
        QVERIFY(text("updateStatus").contains("503"));
        QVERIFY(page.named("checkUpdates").first()->property("enabled").toBool());
        writeFile(s.dir.path() + "/delay", "0");

        writeFile(s.dir.path() + "/answer", "{\"message\":\"Not Found\"}\n404");
        page.backend.updates()->check();
        QTRY_COMPARE(text("updateStatus"), QString("No releases have been published yet."));
        writeFile(s.dir.path() + "/answer", "{\"tag_name\":\"v0.1.0\",\"assets\":[]}\n200");
        page.backend.updates()->check();
        QTRY_COMPARE(text("updateStatus"), QString("You have the latest version."));
        QVERIFY(shown("checkUpdates"));

        // A newer release with nothing for this computer: its notes, no install, and why.
        writeFile(s.dir.path() + "/answer", "{\"tag_name\":\"v0.2.0\",\"body\":\"Faster pages.\",\"assets\":[]}\n200");
        page.backend.updates()->check();
        QTRY_COMPARE(page.backend.updates()->state(), QString("available"));
        QTRY_COMPARE(text("updateAvailableVersion"), QString("0.2.0"));
        QCOMPARE(text("updateInstalledVersion"), QString("0.1.0"));
        QCOMPARE(text("updateNotes"), QString("Faster pages."));
        QVERIFY(shown("updateNoPackage"));
        QVERIFY(!shown("installUpdate"));
        QVERIFY(shown("checkUpdates"));

        // The project links open their pages through the usual route.
        s.command("xdg-open", "exit 0");
        for (QQuickItem *child : page.row("app.license")->findChildren<QQuickItem *>())
            if (child->objectName() == "actionButton")
                QMetaObject::invokeMethod(child, "clicked");
        QTRY_VERIFY(s.calls().join('\n').contains("LICENSE"));
    }

    void theBarEditorShowsTheModelItEdits() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        QAccessible::setActive(true);
        page.show();
        QTRY_VERIFY(page.chip("omarchy.clock"));

        // Five regions in the preview, the same five the editor has, holding the same widgets.
        QTRY_COMPARE(page.named("barPreviewRegion").size(), 5);
        QStringList regions;
        for (QQuickItem *region : page.named("barPreviewRegion"))
            regions << region->property("regionId").toString();
        QCOMPARE(regions, QStringList({"left", "centerBegin", "center", "centerEnd", "right"}));
        QCOMPARE(page.named("barPreviewRegion").at(4)->property("regionItems").toList().size(), 5);
        QVERIFY(page.named("barPreview").size() == 1);
        // The tray drawer is off the bar, so only the editor has it, with its empty state.
        QVERIFY(!page.named("barSection_tray").isEmpty());

        // The name from the running plugin list, its glyph, and the plain one for the unknown.
        QTRY_COMPARE(page.chip("omarchy.menu")->property("widgetName").toString(), QString("Omarchy menu"));
        QCOMPARE(page.chip("omarchy.menu")->property("widgetIcon").toString(), QString("logo"));
        QCOMPARE(page.chip("mystery.thing")->property("widgetName").toString(), QString("Thing"));
        QCOMPARE(page.chip("mystery.thing")->property("widgetIcon").toString(), QString("plugin"));

        QTRY_VERIFY(!page.named("activeBar").isEmpty());
        QVERIFY(QAccessible::queryAccessibleInterface(page.named("activeBar").first())->text(QAccessible::Name).contains("local.test-bar"));

        // Every placement stays reachable from the dropdown, with the reorders added on.
        QCOMPARE(page.chip("omarchy.network")->property("actions").toList().size(), 8);
        QCOMPARE(page.chip("omarchy.tray")->property("actions").toList().size(), 7);
        QAccessible::setActive(false);
    }

    void theArrowKeysReorderAndKeepTheKeyboardThere() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.show();
        QTRY_VERIFY(page.handle("omarchy.audio"));

        page.handle("omarchy.audio")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(page.window, Qt::Key_Left);
        QTRY_COMPARE(page.section("right"), QStringList({"omarchy.tray", "omarchy.audio", "omarchy.network", "omarchy.power", "mystery.thing"}));
        QCOMPARE(page.moves().last(), QString("omarchy.audio --section right --index 1 --from-section right --from-index 2"));
        QTRY_VERIFY(page.window->activeFocusItem() == page.handle("omarchy.audio"));
        QTRY_VERIFY(!page.backend.plugins()->changing());

        // Omarchy takes it out before putting it in, so one along is one along.
        QTest::keyClick(page.window, Qt::Key_Right);
        QTRY_COMPARE(page.section("right").at(2), QString("omarchy.audio"));
        QCOMPARE(page.moves().last(), QString("omarchy.audio --section right --index 2 --from-section right --from-index 1"));
        QTRY_VERIFY(!page.backend.plugins()->changing());
        QTRY_VERIFY(page.window->activeFocusItem() == page.handle("omarchy.audio"));

        // Up goes to the end of the region before it.
        QTest::keyClick(page.window, Qt::Key_Up);
        QTRY_COMPARE(page.section("centerEnd"), QStringList({"omarchy.weather", "omarchy.system-update", "omarchy.audio"}));
        QVERIFY(!page.section("right").contains("omarchy.audio"));
        QTRY_VERIFY(!page.backend.plugins()->changing());
        QTRY_VERIFY(page.window->activeFocusItem() == page.handle("omarchy.audio"));

        // Nowhere further to go is not a change at all.
        const int count = page.moves().size();
        page.handle("omarchy.menu")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(page.window, Qt::Key_Left);
        QTest::keyClick(page.window, Qt::Key_Up);
        QTest::qWait(100);
        QCOMPARE(page.moves().size(), count);

        // A press while one is landing would use positions about to change.
        page.handle("omarchy.tray")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(page.window, Qt::Key_Right);
        QTest::keyClick(page.window, Qt::Key_Right);
        QTRY_VERIFY(!page.backend.plugins()->changing());
        QCOMPARE(page.moves().size(), count + 1);
    }

    void draggingAWidgetPlacesItWhereItIsDropped() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        page.show();
        QTRY_VERIFY(page.handle("omarchy.tray"));
        QTest::qWait(150);
        const auto centre = [&](QQuickItem *item, qreal fx = 0.5) { return item->mapToScene(QPointF(item->width() * fx, item->height() / 2)).toPoint(); };
        const auto drag = [&](const QString &id, const QPoint &to) {
            const QPoint from = centre(page.handle(id));
            QTest::mousePress(page.window, Qt::LeftButton, Qt::NoModifier, from);
            for (int i = 1; i <= 6; ++i)
                QTest::mouseMove(page.window, from + (to - from) * i / 6);
            QVERIFY(!page.named("barDragGhost").isEmpty());
            QTest::mouseRelease(page.window, Qt::LeftButton, Qt::NoModifier, to);
        };

        // Further along its own region: dropped before power, which is index 2 once it has left.
        drag("omarchy.tray", centre(page.chip("omarchy.power"), 0.2));
        QTRY_COMPARE(page.section("right"), QStringList({"omarchy.network", "omarchy.audio", "omarchy.tray", "omarchy.power", "mystery.thing"}));
        QCOMPARE(page.moves().last(), QString("omarchy.tray --section right --index 2 --from-section right --from-index 0"));
        QTRY_VERIFY(!page.backend.plugins()->changing());
        QTRY_VERIFY(page.handle("omarchy.clock"));
        QTest::qWait(150);

        // Into another region, ahead of what is there.
        drag("omarchy.audio", centre(page.chip("omarchy.clock"), 0.2));
        QTRY_COMPARE(page.section("center"), QStringList({"omarchy.audio", "omarchy.clock"}));
        QTRY_VERIFY(!page.backend.plugins()->changing());
        QTest::qWait(150);

        // Into an empty one, which only has its label to aim at.
        QQuickItem *empty = page.named("barSection_tray").first();
        drag("omarchy.weather", empty->mapToScene(QPointF(empty->width() * 0.6, empty->height() / 2)).toPoint());
        QTRY_COMPARE(page.section("tray"), QStringList({"omarchy.weather"}));
        QTRY_VERIFY(!page.backend.plugins()->changing());
        QTest::qWait(150);

        // Back where it started, or out over nothing, or called off: none of them is a change.
        const int count = page.moves().size();
        const QPoint start = centre(page.handle("omarchy.network"));
        QTest::mousePress(page.window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(page.window, start + QPoint(20, 0));
        QTest::mouseMove(page.window, centre(page.chip("omarchy.network"), 0.3));
        QTest::mouseRelease(page.window, Qt::LeftButton, Qt::NoModifier, centre(page.chip("omarchy.network"), 0.3));
        drag("omarchy.network", QPoint(page.window->width() - 4, 4));
        QTest::mousePress(page.window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(page.window, start + QPoint(40, 0));
        QTest::keyClick(page.window, Qt::Key_Escape);
        QVERIFY(page.named("barDragGhost").isEmpty());
        QTest::mouseRelease(page.window, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 0));
        QTest::qWait(100);
        QCOMPARE(page.moves().size(), count);
    }

    void pinnedAppsShowIconsAndCanBeRemoved() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        QAccessible::setActive(true);
        page.show();
        QTRY_COMPARE(page.named("dockApp").size(), 3);
        // In their order, the real icon where there is one and the same fallback where not.
        QCOMPARE(page.named("dockApp").at(0)->property("appName").toString(), QString("Firefox"));
        QCOMPARE(page.named("dockApp").at(1)->property("appName").toString(), QString("Gone"));
        QCOMPARE(page.named("dockApp").at(2)->property("appName").toString(), QString("Files"));
        QTRY_COMPARE(page.named("dockAppIcon").size(), 2);
        QCOMPARE(page.named("dockAppFallback").size(), 1);

        QQuickItem *remove = page.named("dockRemove").at(1);
        QCOMPARE(QAccessible::queryAccessibleInterface(remove)->text(QAccessible::Name), QString("Unpin Gone"));
        remove->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(page.window, Qt::Key_Space);
        QTRY_COMPARE(page.named("dockApp").size(), 2);
        QVERIFY(s.calls().contains("omarchy dock unpin gone"));
        // The keyboard moves to whichever took its place.
        QTRY_VERIFY(page.window->activeFocusItem() == page.named("dockRemove").at(1));
        QAccessible::setActive(false);
    }

    void theFoldedToolsCountAndOpenInPlace() {
        Sandbox s;
        desktopFixture(s);
        DesktopPage page(s);
        QVERIFY(QTest::qWaitForWindowActive(page.window, 3000));
        QAccessible::setActive(true);
        page.show();
        QTRY_VERIFY(!page.named("disclosure_plugins").isEmpty());
        QVERIFY(!page.named("disclosure_shortcuts").isEmpty());
        QVERIFY(!page.named("disclosure_rules").isEmpty());
        QTRY_VERIFY(QAccessible::queryAccessibleInterface(page.named("disclosure_plugins").first())->text(QAccessible::Name).contains("2"));

        // Nothing is built until it is opened, and it opens where it is.
        QQuickItem *header = page.named("disclosure_plugins").first();
        QCOMPARE(page.named("iconPackSearch").size(), 0);
        const qreal below = page.named("disclosure_shortcuts").first()->mapToScene(QPointF(0, 0)).y();
        header->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(page.window, Qt::Key_Space);
        QTRY_VERIFY(page.named("disclosure_shortcuts").first()->mapToScene(QPointF(0, 0)).y() > below + 40);
        QCOMPARE(page.window->activeFocusItem(), header);
        QTest::keyClick(page.window, Qt::Key_Space);
        QTRY_COMPARE(page.named("disclosure_shortcuts").first()->mapToScene(QPointF(0, 0)).y(), below);

        // Search reaches each one by name, opens it, and leaves the keyboard on it.
        page.show("block.rules");
        QTRY_VERIFY(page.window->activeFocusItem() == page.named("disclosure_rules").first());
        QAccessible::setActive(false);
    }

    // --- Icon packs ---

    void cliIconSamplesExposeOwnInheritedAndMissingFilesWithoutRendering() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "Parent", {"utilities-terminal"});
        addPack(desk.system, "Child With Spaces", {"folder"}, "Parent");
        addPack(desk.system, "hicolor", {"web-browser"});
        const auto reply = CliApi::execute(s.env, {"icons", "samples", "Child With Spaces"});
        QCOMPARE(reply.exitCode, 0);
        QVERIFY(reply.body.value("verified").toBool());
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("complete").toBool());
        const auto rows = data.value("samples").toArray();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].toObject().value("from").toString(), QString("Child With Spaces"));
        QVERIFY(!rows[0].toObject().value("inherited").toBool());
        QVERIFY(!rows[1].toObject().value("found").toBool());
        QCOMPARE(rows[2].toObject().value("from").toString(), QString("Parent"));
        QVERIFY(rows[2].toObject().value("inherited").toBool());
        QCOMPARE(rows[3].toObject().value("from").toString(), QString("hicolor"));
        QVERIFY(QFileInfo(rows[0].toObject().value("path").toString()).isFile());
        QVERIFY(rows[0].toObject().value("fileUrl").toString().contains("%20"));
        QCOMPARE(QUrl(rows[0].toObject().value("fileUrl").toString()).toLocalFile(), rows[0].toObject().value("path").toString());
        QVERIFY(s.calls().isEmpty());
        QVERIFY(!QFileInfo::exists(desk.policy()));
    }

    void cliIconSearchSharesPickerPrecedenceAndReportsResultTruncation() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "Parent", {"alpha", "collision", "zulu"});
        addPack(desk.system, "Child", {"ALPHA", "Beta", "collision"}, "Parent");
        addPack(desk.system, "hicolor", {"omega"});
        desk.themeAsks("Child");
        auto reply = CliApi::execute(s.env, {"icons", "search", "Child", "--limit", "2"});
        QCOMPARE(reply.exitCode, 0);
        const auto data = reply.body.value("data").toObject();
        QVERIFY(data.value("complete").toBool());
        QVERIFY(data.value("truncated").toBool());
        QCOMPARE(data.value("matches").toInt(), 6);
        const auto rows = data.value("icons").toArray();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].toObject().value("name").toString(), QString("ALPHA"));
        QCOMPARE(rows[1].toObject().value("name").toString(), QString("alpha"));
        AppCore core(s.env);
        QCOMPARE(QJsonArray::fromVariantList(core.launchers.packIcons("", 2)), rows);
        reply = CliApi::execute(s.env, {"icons", "search", "Child", " CoLL "});
        const auto match = reply.body.value("data").toObject().value("icons").toArray().first().toObject();
        QCOMPARE(match.value("name").toString(), QString("collision"));
        QCOMPARE(match.value("from").toString(), QString("Child"));
        QVERIFY(match.value("path").toString().contains("/Child/"));
        QVERIFY(!match.value("inherited").toBool());
        QVERIFY(s.calls().isEmpty());
    }

    void cliIconReadsPreserveLegacyRoutingAndVersionedListFiltering() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.user, "Papirus", {"folder"});
        addPack(desk.system, "Hidden", {"folder"}, {}, "Hidden=true\n");
        writeFile(desk.system + "/Cursor/index.theme", "[Icon Theme]\nName=Cursor\n");
        const auto reply = CliApi::execute(s.env, {"icons", "list", "--api-version", "1"});
        QCOMPARE(reply.exitCode, 0);
        const auto rows = reply.body.value("data").toObject().value("packs").toArray();
        QCOMPARE(rows.size(), 3);
        for (const auto &row : rows)
            if (row.toObject().value("id") == QLatin1String("Papirus"))
                QCOMPARE(row.toObject().value("locations").toArray().size(), 2);
        QCOMPARE(CliApi::execute(s.env, {"icons", "samples", "Hidden"}).exitCode, 0);
        QVERIFY(!CliApi::requested({"icons", "list", "--json"}));
        QVERIFY(!CliApi::requested({"icons", "import", "/tmp/pack", "--yes"}));
        QVERIFY(CliApi::requested({"icons", "list", "--api-version", "1"}));
        QVERIFY(CliApi::requested({"icons", "search", "Papirus"}));
        QVERIFY(CliApi::requested({"icons", "samples", "Papirus", "--bad"}));
        QVERIFY(CliApi::requested({"icons", "--limit", "0", "search", "Papirus"}));
        QVERIFY(!CliApi::requested({"icons", "import", "--", "--limit"}));
        QProcess legacy;
        legacy.start(QCoreApplication::applicationFilePath(), {"--cli-sandbox", s.dir.path(), "icons", "list", "--json"});
        QVERIFY(legacy.waitForFinished(15000));
        QCOMPARE(legacy.exitCode(), 0);
        const auto old = QJsonDocument::fromJson(legacy.readAllStandardOutput()).object();
        QVERIFY(!old.contains("apiVersion"));
        QCOMPARE(old.value("packs").toArray(), rows);
    }

    void cliIconDiscoveryDoesNotTurnUnavailableSourcesIntoEmptySuccess_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"missing", "malformed", "oversized", "linked-index", "missing-parent", "unreadable-folder", "untraversable-root", "dangling-root", "dangling-icon"})
            QTest::newRow(mode) << QString(mode);
    }
    void cliIconDiscoveryDoesNotTurnUnavailableSourcesIntoEmptySuccess() {
        QFETCH(QString, mode);
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "Test", {"folder"});
        const QString index = desk.user + "/Test/index.theme";
        QString repaired;
        if (mode == "missing") QDir(desk.system + "/Test").removeRecursively();
        if (mode == "malformed") writeFile(index, "broken descriptor");
        if (mode == "oversized") writeFile(index, QByteArray(1024 * 1024 + 1, 'x'));
        if (mode == "linked-index") { QDir().mkpath(QFileInfo(index).absolutePath()); QVERIFY(QFile::link(index + ".missing", index)); }
        if (mode == "missing-parent") addPack(desk.user, "Test", {"folder"}, "NotInstalled");
        if (mode == "unreadable-folder") {
            if (::getuid() == 0) QSKIP("Permission check needs an unprivileged user");
            repaired = desk.system + "/Test/scalable/apps";
            QVERIFY(QFile::setPermissions(repaired, {}));
        }
        if (mode == "untraversable-root") {
            if (::getuid() == 0) QSKIP("Permission check needs an unprivileged user");
            repaired = desk.system;
            QVERIFY(QFile::setPermissions(repaired, QFile::ReadOwner));
        }
        if (mode == "dangling-root") { QDir().mkpath(QFileInfo(desk.user).absolutePath()); QVERIFY(QFile::link(desk.user + ".missing", desk.user)); }
        if (mode == "dangling-icon") QVERIFY(QFile::link(desk.system + "/missing.svg", desk.system + "/Test/scalable/apps/dangling.svg"));
        const auto reply = CliApi::execute(s.env, {"icons", "search", "Test"});
        if (!repaired.isEmpty()) QVERIFY(QFile::setPermissions(repaired, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(!reply.body.value("verified").toBool());
        QCOMPARE(reply.body.value("error").toObject().value("code").toString(), QString("read_unavailable"));
        QVERIFY(!reply.body.value("data").toObject().value("complete").toBool());
        QVERIFY(s.calls().isEmpty());
    }

    void cliIconListAndSamplesReportInaccessibleSourcesAndSearchStaysWithinItsContexts() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "Test", {"folder", "app"});
        const auto index = desk.system + "/Test/index.theme";
        writeFile(index, readAll(index) + "\n[unused]\nSize=48\nContext=Actions\n");
        writeFile(desk.system + "/Test/scalable/apps/.hidden.svg", kSvg);
        writeFile(desk.system + "/Test/scalable/apps/UPPER.SVG", kSvg);
        QDir().mkpath(desk.system + "/Test/scalable/apps/not-a-file.svg");
        auto reply = CliApi::execute(s.env, {"icons", "search", "Test", "absent"});
        QCOMPARE(reply.exitCode, 0);
        QCOMPARE(reply.body.value("data").toObject().value("matches").toInt(), 0);
        reply = CliApi::execute(s.env, {"icons", "search", "Test"});
        QCOMPARE(reply.exitCode, 0);
        QCOMPARE(reply.body.value("data").toObject().value("matches").toInt(), 3);
        if (::getuid() == 0) QSKIP("Permission check needs an unprivileged user");
        QVERIFY(QFile::setPermissions(desk.system, QFile::ReadOwner));
        const auto list = CliApi::execute(s.env, {"icons", "list", "--api-version", "1"});
        const auto samples = CliApi::execute(s.env, {"icons", "samples", "Test"});
        QVERIFY(QFile::setPermissions(desk.system, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(list.exitCode, 1);
        QCOMPARE(samples.exitCode, 1);
    }

    void iconDiscoveryBoundsEnumerationAndInheritanceWithoutLosingReadDiagnostics() {
        Sandbox s;
        IconDesk desk(s);
        QStringList diagnostics;
        int remaining = 1;
        const auto names = IconThemes::directoryNames(desk.system, &remaining, &diagnostics);
        QCOMPARE(names.size(), 1);
        QVERIFY(!diagnostics.isEmpty());
        for (int i = 0; i < 15; ++i)
            addPack(desk.system, "Chain" + QString::number(i), {"icon"}, i == 14 ? QString() : "Chain" + QString::number(i + 1));
        const auto reply = CliApi::execute(s.env, {"icons", "samples", "Chain0"});
        QCOMPARE(reply.exitCode, 1);
        QVERIFY(!reply.body.value("data").toObject().value("diagnostics").toArray().isEmpty());
    }

    void iconSearchCountsEmptyDirectoryVisitsAgainstItsBudget() {
        Sandbox s;
        const QString root = s.env.dataHome + "/icons";
        QStringList directories;
        QByteArray groups;
        for (int i = 0; i < 5000; ++i) {
            const QString directory = "apps/empty" + QString::number(i);
            directories << directory;
            groups += "[" + directory.toUtf8() + "]\nSize=48\nContext=Applications\n";
        }
        const QByteArray contents = "[Icon Theme]\nName=Empty\nDirectories=" + directories.join(',').toUtf8() + "\n" + groups;
        QVERIFY(contents.size() < 1024 * 1024);
        writeFile(root + "/Empty/index.theme", contents);
        QStringList parents;
        for (int i = 0; i < 21; ++i) {
            const QString id = "Parent" + QString::number(i);
            parents << id;
            QVERIFY(QFile::link(root + "/Empty", root + "/" + id));
        }
        writeFile(root + "/Root/index.theme", "[Icon Theme]\nName=Root\nInherits=" + parents.join(',').toUtf8() + "\n");
        const auto reply = CliApi::execute(s.env, {"icons", "search", "Root", "--limit", "1"});
        QCOMPARE(reply.exitCode, 1);
        const auto data = reply.body.value("data").toObject();
        QVERIFY(!data.value("complete").toBool());
        QVERIFY(data.value("icons").toArray().isEmpty());
        QVERIFY(!data.value("diagnostics").toArray().isEmpty());
    }

    void iconPacksComeFromEveryRootWithHiddenAndCursorOnesLeftOut() {
        Sandbox s;
        IconDesk desk(s);
        addPack(s.env.home + "/.icons", "Legacy");
        addPack(desk.user, "Mine");
        addPack(desk.system, "Secret", {"folder"}, {}, "Hidden=true\n");
        addPack(desk.system, "follow-theme");
        writeFile(desk.system + "/CursorOnly/index.theme", "[Icon Theme]\nName=Pointers\nInherits=Adwaita\n");
        writeFile(desk.system + "/LooseIcons/some.png", "x");
        writeFile(desk.system + "/Broken/index.theme", "no group here\n");
        QStringList ids;
        for (const IconPack &pack : IconThemes::discover(s.env))
            ids << pack.id;
        QCOMPARE(ids, QStringList({"follow-theme", "Legacy", "Mine", "Numix", "Papirus", "Yaru-blue"}));

        // What is in use stays visible, hidden or not, and a pack's name is not a mode.
        QVERIFY(std::any_of(IconThemes::discover(s.env, "Secret").cbegin(), IconThemes::discover(s.env, "Secret").cend(), [](const IconPack &p) { return p.id == "Secret"; }));
        AppCore core(s.env);
        QCOMPARE(core.engine.plan("appearance.icons", "follow-theme", {{QVariantMap{{"value", "pack:follow-theme"}}}, {QVariantMap{{"value", "follow-theme"}}}}).normalized, QString("follow-theme"));
        QCOMPARE(IconThemeProvider::normalizeChoice("follow-theme"), QString("follow-theme"));
        QCOMPARE(IconThemeProvider::normalizeChoice("pack:follow-theme"), QString("pack:follow-theme"));
        QCOMPARE(IconThemeProvider::normalizeChoice("Papirus"), QString("pack:Papirus"));
    }

    void aThemeSpreadAcrossRootsIsOneTheme() {
        Sandbox s;
        IconDesk desk(s);
        // The user's copy of a descriptor wins, and the system copy's folders still count.
        addPack(desk.user, "Papirus", {"folder"}, {}, "Comment=Mine\n");
        const IconPack pack = IconThemes::load(s.env, "Papirus");
        QCOMPARE(pack.comment, QString("Mine"));
        QCOMPARE(pack.locations.size(), 2);
        QVERIFY(pack.indexPath.startsWith(desk.user));
        QString from;
        QVERIFY(IconThemes::findIcon(s.env, "Papirus", {"utilities-terminal"}, 48, &from).startsWith(desk.system));
        QVERIFY(IconThemes::findIcon(s.env, "Papirus", {"folder"}, 48, &from).startsWith(desk.user));
        int seen = 0;
        for (const IconPack &each : IconThemes::discover(s.env))
            seen += each.id == "Papirus" ? 1 : 0;
        QCOMPARE(seen, 1);
    }

    void aPackNameIsTranslatedButItsIdIsNot() {
        const IconPack pack = IconThemes::parseIndex("[Icon Theme]\nName=Blue\nName[de]=Blau\nName[pt_BR]=Azul\nDirectories=a\n[a]\nSize=48\n", "the-id", "de_DE");
        QCOMPARE(pack.name, QString("Blau"));
        QCOMPARE(pack.id, QString("the-id"));
        QCOMPARE(IconThemes::parseIndex("[Icon Theme]\nName=Blue\nName[pt_BR]=Azul\n", "x", "pt_BR").name, QString("Azul"));
        QCOMPARE(IconThemes::parseIndex("[Icon Theme]\nName=Blue\n", "x", "fr_FR").name, QString("Blue"));
        QVERIFY(!IconThemes::parseIndex("Name=Blue\n", "x").found);
        // A pack does not get to point lookups outside itself.
        QVERIFY(IconThemes::parseIndex("[Icon Theme]\nDirectories=../../etc,/etc\n[../../etc]\nSize=48\n[/etc]\nSize=48\n", "x").directories.isEmpty());
    }

    void dataRootsFollowTheEnvironmentAndIgnoreRelativePaths() {
        const QByteArray home = qgetenv("XDG_DATA_HOME"), dirs = qgetenv("XDG_DATA_DIRS");
        qputenv("XDG_DATA_HOME", "/custom/data");
        qputenv("XDG_DATA_DIRS", "/a:relative/b:/c:/a");
        Env env = Env::system();
        QCOMPARE(env.dataHome, QString("/custom/data"));
        QCOMPARE(env.dataDirs, QStringList({"/a", "/c"}));
        QCOMPARE(IconThemes::searchRoots(env).first(), QString("/custom/data/icons"));
        qputenv("XDG_DATA_HOME", "not/absolute");
        qunsetenv("XDG_DATA_DIRS");
        env = Env::system();
        QCOMPARE(env.dataHome, env.home + "/.local/share");
        QCOMPARE(env.dataDirs, QStringList({"/usr/local/share", "/usr/share"}));
        home.isNull() ? qunsetenv("XDG_DATA_HOME") : qputenv("XDG_DATA_HOME", home);
        dirs.isNull() ? qunsetenv("XDG_DATA_DIRS") : qputenv("XDG_DATA_DIRS", dirs);
    }

    void aPreviewOnlyEverShowsThatPacksOwnChain() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "Parent", {"folder", "web-browser"});
        addPack(desk.system, "Child", {"folder"}, "Parent");
        addPack(desk.system, "Unrelated", {"secret-icon"});
        addPack(desk.system, "hicolor", {"last-resort"});
        QString from;
        QVERIFY(IconThemes::findIcon(s.env, "Child", {"folder"}, 48, &from).contains("/Child/"));
        QCOMPARE(from, QString("Child"));
        QVERIFY(IconThemes::findIcon(s.env, "Child", {"web-browser"}, 48, &from).contains("/Parent/"));
        QCOMPARE(from, QString("Parent"));
        QVERIFY(IconThemes::findIcon(s.env, "Child", {"secret-icon"}, 48, &from).isEmpty());
        QVERIFY(IconThemes::findIcon(s.env, "Child", {"last-resort"}, 48, &from).contains("/hicolor/"));

        // A family that inherits in a circle, or from something missing, still ends.
        addPack(desk.system, "LoopA", {"a"}, "LoopB,NotInstalled");
        addPack(desk.system, "LoopB", {"b"}, "LoopA");
        QVERIFY(IconThemes::findIcon(s.env, "LoopA", {"nothing"}, 48).isEmpty());
        QVERIFY(IconThemes::findIcon(s.env, "LoopA", {"b"}, 48).contains("/LoopB/"));
        QVERIFY(IconThemes::findIcon(s.env, "../Unrelated", {"secret-icon"}, 48).isEmpty());

        // The tile says so when nothing shown is really the pack's own.
        addPack(desk.system, "Sparse", {"unrelated"}, "Parent");
        AppCore core(s.env);
        QCOMPARE(core.icons.samples("Sparse").value("borrowedFrom").toString(), QString("Parent"));
        QCOMPARE(core.icons.samples("Child").value("borrowedFrom").toString(), QString());
        const auto samples = core.icons.samples("Child").value("samples").toList();
        QCOMPARE(samples.size(), 4);
        QVERIFY(samples.first().toMap().value("url").toString().startsWith("image://iconpacks/Child/folder/"));
    }

    void previewsDrawBoundedAndNeverTouchTheThemeInUse() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "A Pack With Spaces");
        IconPackImageProvider provider(s.env);
        QVERIFY(provider.flags() & QQmlImageProviderBase::ForceAsynchronousImageLoading);
        QSize size;
        QImage image = provider.requestImage("A%20Pack%20With%20Spaces/folder/1", &size, QSize(64, 64));
        QCOMPARE(size, QSize(64, 64));
        QCOMPARE(image.pixelColor(32, 32), QColor("#ff0000"));
        // An absurd request is limited, and a missing sample is an empty square, not a failure.
        QCOMPARE(provider.requestImage("Papirus/folder/1", &size, QSize(9000, 9000)).size(), QSize(192, 192));
        QCOMPARE(provider.requestImage("Papirus/nonsense/1", &size, QSize(32, 32)).pixelColor(16, 16).alpha(), 0);
        QCOMPARE(provider.requestImage("NotInstalled/folder/1", &size, QSize(32, 32)).pixelColor(16, 16).alpha(), 0);
        QByteArray huge = kSvg;
        huge.append(QByteArray(3 * 1024 * 1024, ' '));
        writeFile(desk.system + "/Papirus/scalable/apps/web-browser.svg", huge);
        QCOMPARE(provider.requestImage("Papirus/browser/1", &size, QSize(32, 32)).pixelColor(16, 16).alpha(), 0);
        QCOMPARE(desk.writes(), 0);
    }

    void gsettingsOutputIsReadAsAGVariantString() {
        bool ok = false;
        QCOMPARE(IconThemes::parseGVariantString("'Yaru-blue'\n", &ok), QString("Yaru-blue"));
        QVERIFY(ok);
        QCOMPARE(IconThemes::parseGVariantString("\"a b'c\"\n", &ok), QString("a b'c"));
        QVERIFY(ok);
        QCOMPARE(IconThemes::parseGVariantString("'it\\'s'", &ok), QString("it's"));
        QVERIFY(ok);
        IconThemes::parseGVariantString("@ms nothing", &ok);
        QVERIFY(!ok);
    }

    void iniEditsTouchOnlyTheValue() {
        QByteArray doc = "# keep me\r\n[Other]\ngtk-icon-theme-name = untouched\n\n[Settings]\ngtk-theme-name=X\n  gtk-icon-theme-name  =  Old Name  \r\n; gtk-icon-theme-name=comment\n";
        const auto read = FileOps::iniGet(doc, "Settings", "gtk-icon-theme-name");
        QVERIFY(read.found && !read.ambiguous);
        QCOMPARE(read.value, QString("Old Name"));
        QVERIFY(FileOps::iniSet(&doc, "Settings", "gtk-icon-theme-name", "New"));
        QCOMPARE(doc, QByteArray("# keep me\r\n[Other]\ngtk-icon-theme-name = untouched\n\n[Settings]\ngtk-theme-name=X\n  gtk-icon-theme-name  =  New  \r\n; gtk-icon-theme-name=comment\n"));
        QString error;
        QVERIFY(!FileOps::iniSet(&doc, "Settings", "absent", "x", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!FileOps::iniSet(&doc, "NoGroup", "gtk-theme-name", "x"));
        QByteArray twice = "[A]\nk=1\nk=2\n";
        QVERIFY(FileOps::iniGet(twice, "A", "k").ambiguous);
        QVERIFY(!FileOps::iniSet(&twice, "A", "k", "3"));
        QCOMPARE(twice, QByteArray("[A]\nk=1\nk=2\n"));
    }

    void iniUpsertAddsAMissingKeyWhereItBelongs() {
        // After the group's last line, before the next group, everything else as it was.
        QByteArray doc = "# mine\n[Settings]\ngtk-theme-name=Adwaita\n\n[Other]\nx=1\n";
        QVERIFY(FileOps::iniUpsert(&doc, "Settings", "gtk-icon-theme-name", "Papirus"));
        QCOMPARE(doc, QByteArray("# mine\n[Settings]\ngtk-theme-name=Adwaita\ngtk-icon-theme-name=Papirus\n\n[Other]\nx=1\n"));
        // An existing key is replaced in place, like iniSet.
        QVERIFY(FileOps::iniUpsert(&doc, "Settings", "gtk-icon-theme-name", "Numix"));
        QVERIFY(doc.contains("gtk-icon-theme-name=Numix\n\n[Other]"));
        // A missing group goes at the end; an empty file becomes just that group.
        QByteArray kde = "[General]\nName=x";
        QVERIFY(FileOps::iniUpsert(&kde, "Icons", "Theme", "Papirus"));
        QCOMPARE(kde, QByteArray("[General]\nName=x\n\n[Icons]\nTheme=Papirus\n"));
        QByteArray empty;
        QVERIFY(FileOps::iniUpsert(&empty, "Settings", "gtk-icon-theme-name", "Papirus"));
        QCOMPARE(empty, QByteArray("[Settings]\ngtk-icon-theme-name=Papirus\n"));
        // A group that ends the file without a newline, and a CRLF file, stay well formed.
        QByteArray last = "[Settings]\r\na=1";
        QVERIFY(FileOps::iniUpsert(&last, "Settings", "b", "2"));
        QCOMPARE(last, QByteArray("[Settings]\r\na=1\r\nb=2\r\n"));
        QByteArray twice = "[A]\nk=1\nk=2\n";
        QVERIFY(!FileOps::iniUpsert(&twice, "A", "k", "3"));
        QCOMPARE(twice, QByteArray("[A]\nk=1\nk=2\n"));
    }

    void openingAppearanceChangesNothing() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Papirus");
        AppCore core(s.env);
        const ReadResult read = readSetting(core.engine, "appearance.icons");
        QCOMPARE(read.status, QString("ok"));
        QCOMPARE(read.value, QString("pack:Papirus"));
        QCOMPARE(read.extra.value("mode").toString(), QString("unmanaged"));
        QVERIFY(read.reason.isEmpty());
        QCOMPARE(desk.writes(), 0);
        QVERIFY(!QFile::exists(desk.policy()));
        QVERIFY(!QFile::exists(desk.hook()));

        // With no choice saved, Omarchy's own value is simply following the theme.
        desk.setEffective("Yaru-blue");
        QCOMPARE(readSetting(core.engine, "appearance.icons").value, QString("follow-theme"));
        desk.themeAsks("Numix");
        QCOMPARE(readSetting(core.engine, "appearance.icons").value, QString("pack:Yaru-blue"));

        // A pack that is not installed is shown as it is, and not adopted or replaced.
        desk.setEffective("Yaru-gray");
        const ReadResult missing = readSetting(core.engine, "appearance.icons");
        QCOMPARE(missing.value, QString("pack:Yaru-gray"));
        QVERIFY(missing.reason.contains("not installed"));
        QCOMPARE(desk.writes(), 0);
    }

    void choosingAPackSavesAHookAndGoesBackExactly() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QVERIFY(desk.isUnset());
        const ChangePlan plan = core.engine.plan("appearance.icons", "Papirus", {QVariantMap{{"value", "pack:Papirus"}}});
        QVERIFY(plan.ok);
        QCOMPARE(plan.normalized, QString("pack:Papirus"));
        QVERIFY(plan.files.contains(desk.hook()));
        QVERIFY(plan.effects.join(' ').contains("hook"));
        QVERIFY(!QFile::exists(desk.hook()));

        const Transaction tx = applySetting(core.engine, "appearance.icons", "pack:Papirus");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(desk.effective(), QString("Papirus"));
        QCOMPARE(QJsonDocument::fromJson(readAll(desk.policy())).object().value("theme").toString(), QString("Papirus"));
        QCOMPARE(readAll(desk.hook()), IconThemeProvider::hookScript());
        QVERIFY(readAll(desk.hook()).contains("omarchy-control icons reapply"));
        QVERIFY(tx.undoable());
        const ReadResult after = readSetting(core.engine, "appearance.icons");
        QCOMPARE(after.value, QString("pack:Papirus"));
        QCOMPARE(after.extra.value("mode").toString(), QString("fixed"));

        // A second choice leaves the hook alone, and undoing it returns to the first.
        const Transaction second = applySetting(core.engine, "appearance.icons", "Numix");
        QCOMPARE(second.phase, QString("committed"));
        QCOMPARE(undoTx(core.engine, second.id).phase, QString("committed"));
        QCOMPARE(desk.effective(), QString("Papirus"));
        QVERIFY(QFile::exists(desk.hook()));

        // Undoing the first goes back to a key no one ever set, and removes what it added.
        const Transaction back = undoTx(core.engine, tx.id);
        QCOMPARE(back.phase, QString("committed"));
        QVERIFY(desk.isUnset());
        QVERIFY(!QFile::exists(desk.policy()));
        QVERIFY(!QFile::exists(desk.hook()));
        QVERIFY(s.calls().contains("gsettings reset org.gnome.desktop.interface icon-theme"));
        QCOMPARE(readSetting(core.engine, "appearance.icons").extra.value("mode").toString(), QString("unmanaged"));
    }

    void undoLeavesLaterChangesAlone() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Numix");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.phase, QString("committed"));

        desk.setEffective("Yaru-blue");
        const Transaction refused = undoTx(core.engine, tx.id);
        QCOMPARE(refused.phase, QString("failed"));
        QCOMPARE(desk.effective(), QString("Yaru-blue"));
        QVERIFY(QFile::exists(desk.hook()));

        // The saved choice was edited by someone: not this undo's to overwrite.
        desk.setEffective("Papirus");
        const QByteArray saved = readAll(desk.policy());
        writeFile(desk.policy(), QJsonDocument(QJsonObject{{"version", 1}, {"mode", "fixed"}, {"theme", "Numix"}}).toJson());
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("failed"));
        QCOMPARE(desk.effective(), QString("Papirus"));
        writeFile(desk.policy(), saved);

        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(desk.effective(), QString("Numix"));
        QVERIFY(!QFile::exists(desk.hook()));
    }

    void anEditedHookDoesNotStopAnUndoAndIsKept() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Numix");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        writeFile(desk.hook(), "#!/bin/bash\n# edited by hand\n");
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(desk.effective(), QString("Numix"));
        QCOMPARE(readAll(desk.hook()), QByteArray("#!/bin/bash\n# edited by hand\n"));
        QVERIFY(!QFile::exists(desk.policy()));
    }

    void undoingBackToFollowingUsesTheThemeInUseNow() {
        Sandbox s;
        IconDesk desk(s);
        desk.themeAsks("Numix");
        desk.setEffective("Numix");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.before, QString("follow-theme"));
        // A theme change later, the pack from before it would be the wrong one to go back to.
        desk.themeAsks("Yaru-blue");
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(desk.effective(), QString("Yaru-blue"));
        QCOMPARE(readSetting(core.engine, "appearance.icons").value, QString("follow-theme"));
        QVERIFY(readSetting(core.engine, "appearance.icons").reason.isEmpty());
    }

    void aFailedRollbackStillRestoresEverythingElse() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Numix");
        // Sorted first, so stopping at this file would leave the choice and the hook behind.
        const QString gtk3 = s.env.configHome + "/gtk-3.0/settings.ini";
        writeFile(gtk3, "[Settings]\ngtk-icon-theme-name=Old\n");
        // GSettings ignores the write, so this fails after the file was already patched, and
        // by then something else has rewritten the file.
        writeFile(s.env.home + "/.fake-icon-theme-deaf", "");
        s.command("gsettings", QByteArray(kFakeGsettings) + "\n[ \"$1\" = get ] && [ -f '" + gtk3.toUtf8() + "' ] && grep -q Papirus '" + gtk3.toUtf8() + "' && printf '[Settings]\\ngtk-icon-theme-name=TheirEdit\\n' > '" + gtk3.toUtf8() + "'\nexit 0\n");
        AppCore core(s.env);
        const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.phase, QString("partial"));
        QVERIFY(tx.message.contains("left as it is"));
        QVERIFY(readAll(gtk3).contains("TheirEdit"));
        QVERIFY(!QFile::exists(desk.policy()));
        QVERIFY(!QFile::exists(desk.hook()));
    }

    void aChangeBetweenPlanningAndTheLockIsNotRecordedAsTheWayBack() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Numix");
        AppCore core(s.env);
        // Two picks in a row, the second planned before the first has run.
        Transaction first, second;
        bool firstDone = false, secondDone = false;
        core.icons.submit("pack:Papirus", "set", "Icon pack", [&](const Transaction &tx) { first = tx; firstDone = true; });
        core.icons.submit("pack:Yaru-blue", "set", "Icon pack", [&](const Transaction &tx) { second = tx; secondDone = true; });
        QTRY_VERIFY(firstDone && secondDone);
        const QStringList phases{first.phase, second.phase};
        QVERIFY2(phases.count("committed") >= 1, qPrintable(phases.join(' ')));
        // Whichever ran second, its way back is what was really there when it ran.
        const Transaction &last = second.phase == QLatin1String("committed") ? second : first;
        const QString wayBack = last.extra.value("gsettings").toObject().value("before").toString();
        QVERIFY(first.phase != QLatin1String("committed") || second.phase != QLatin1String("committed") || wayBack == QLatin1String("Papirus"));
        QCOMPARE(undoTx(core.engine, last.id).phase, QString("committed"));
        QCOMPARE(desk.effective(), wayBack);
    }

    void aFailedPickKeepsAStandoffInPlace() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QString message;
        for (int i = 0; i < 3; ++i) {
            desk.setEffective("Their-Pack");
            core.icons.reapply(&message);
        }
        QVERIFY(readSetting(core.engine, "appearance.icons").extra.value("suspended").toBool());
        writeFile(s.env.home + "/.fake-icon-theme-deaf", "");
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Numix").phase, QString("rolled-back"));
        QVERIFY(readSetting(core.engine, "appearance.icons").extra.value("suspended").toBool());
    }

    void anOlderSwitchsHookLeavesItToTheNewerOne() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        writeFile(s.env.stateHome + "/omarchy/current/theme.name", "newer\n");
        desk.setEffective("Numix");
        const int writes = desk.writes();
        QString message;
        // However often this happens it is not another icon manager, and it writes nothing.
        for (int i = 0; i < 5; ++i)
            QCOMPARE(core.icons.reapply(&message, "older"), 0);
        QCOMPARE(desk.writes(), writes);
        QVERIFY(!readSetting(core.engine, "appearance.icons").extra.value("suspended").toBool());
        QVERIFY(readAll(IconThemeProvider(&core.runner, &core.coordinator).hookPath()).contains("reapply \"$1\""));
        QCOMPARE(core.icons.reapply(&message, "newer"), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));
    }

    void aThemeChangeLeavesAHandEditedConfigAlone() {
        Sandbox s;
        IconDesk desk(s);
        const QString gtk3 = s.env.configHome + "/gtk-3.0/settings.ini";
        writeFile(gtk3, "[Settings]\ngtk-icon-theme-name=Old\n");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QCOMPARE(readAll(gtk3), QByteArray("[Settings]\ngtk-icon-theme-name=Papirus\n"));
        writeFile(gtk3, "[Settings]\ngtk-icon-theme-name=Mine\n");
        desk.setEffective("Numix");
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));
        QCOMPARE(readAll(gtk3), QByteArray("[Settings]\ngtk-icon-theme-name=Mine\n"));
        QVERIFY(readSetting(core.engine, "appearance.icons").extra.value("mixed").toBool());
        // A deliberate choice is what brings it back into line.
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QCOMPARE(readAll(gtk3), QByteArray("[Settings]\ngtk-icon-theme-name=Papirus\n"));
    }

    void anImportDoesNotHoldUpOtherChanges() {
        Sandbox s;
        IconDesk desk(s);
        const QString source = s.dir.path() + "/in/Long";
        addPack(s.dir.path() + "/in", "Long");
        const QByteArray bulk(2 * 1024 * 1024, 'x');
        for (int i = 0; i < 60; ++i)
            writeFile(source + "/scalable/apps/bulk" + QString::number(i) + ".svg", bulk);
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QSignalSpy finished(&importer, &IconThemeImporter::finished);
        importer.choosePath(source);
        QVERIFY(importer.busy());
        // The theme hook's lock is free the whole time, and a second import is told to wait.
        QLockFile change(s.env.appStateDir + "/change.lock");
        QVERIFY(change.tryLock(0));
        change.unlock();
        IconThemeImporter second(s.env, &core.coordinator);
        addPack(s.dir.path() + "/in", "Second");
        QCOMPARE(importPack(second, s.dir.path() + "/in/Second"), QString("failed"));
        QVERIFY(second.message().contains("Another icon pack"));
        // And a new window does not take a live staging folder for a leftover.
        QVERIFY(second.recoverStaging().isEmpty());
        QVERIFY(finished.count() || finished.wait(30000));
        QCOMPARE(importer.state(), QString("imported"));
        QVERIFY(QFile::exists(source + "/scalable/apps/bulk59.svg"));
        QCOMPARE(QFileInfo(desk.user + "/Long/scalable/apps/bulk59.svg").size(), qint64(bulk.size()));
    }

    void aHiddenPackImportsButIsNotOfferedAsAChoice() {
        Sandbox s;
        IconDesk desk(s);
        addPack(s.dir.path() + "/in", "Base", {"folder"}, {}, "Hidden=true\n");
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, s.dir.path() + "/in/Base"), QString("imported"));
        QVERIFY(!importer.usable());
        QVERIFY(importer.message().contains("hidden"));
        addPack(s.dir.path() + "/in", "Shown");
        QCOMPARE(importPack(importer, s.dir.path() + "/in/Shown"), QString("imported"));
        QVERIFY(importer.usable());
    }

    void anEditedHookIsKeptAndExplained() {
        Sandbox s;
        IconDesk desk(s);
        writeFile(desk.hook(), "#!/bin/bash\n# mine\n");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QCOMPARE(readAll(desk.hook()), QByteArray("#!/bin/bash\n# mine\n"));
        const ReadResult read = readSetting(core.engine, "appearance.icons");
        QCOMPARE(read.extra.value("hook").toString(), QString("edited"));
        QVERIFY(read.reason.contains("edited"));
    }

    void aFailedChangeLeavesEverythingAsItWas() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Numix");
        AppCore core(s.env);

        writeFile(s.env.home + "/.fake-icon-theme-broken", "");
        Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.phase, QString("rolled-back"));
        QCOMPARE(desk.effective(), QString("Numix"));
        QVERIFY(!QFile::exists(desk.policy()));
        QVERIFY(!QFile::exists(desk.hook()));
        QFile::remove(s.env.home + "/.fake-icon-theme-broken");

        // A write that reports success without changing anything is caught by reading back.
        writeFile(s.env.home + "/.fake-icon-theme-deaf", "");
        tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.phase, QString("rolled-back"));
        QVERIFY(tx.message.contains("Numix"));
        QVERIFY(!QFile::exists(desk.policy()));
        QVERIFY(!QFile::exists(desk.hook()));
        QFile::remove(s.env.home + "/.fake-icon-theme-deaf");

        // A pack that is not on offer never reaches a write.
        const int before = desk.writes();
        QCOMPARE(applySetting(core.engine, "appearance.icons", "pack:NotInstalled").phase, QString("failed"));
        QCOMPARE(desk.writes(), before);
    }

    void aMissingCommandKeepsPreviewsButStopsChanges() {
        Sandbox s;
        IconDesk desk(s);
        QFile::remove(s.bin("gsettings"));
        AppCore core(s.env);
        QCOMPARE(readSetting(core.engine, "appearance.icons").status, QString("unavailable"));
        QVERIFY(!core.engine.plan("appearance.icons", "follow-theme").ok);
        QCOMPARE(core.icons.samples("Papirus").value("samples").toList().size(), 4);
    }

    // As Iconic does: the GTK files and kdeglobals always name the pack, made if missing,
    // and the qt*ct files only where they exist. Undo puts back exactly what was there.
    void configFilesNameThePackAsIconicDoes() {
        Sandbox s;
        IconDesk desk(s);
        const QString gtk3 = s.env.configHome + "/gtk-3.0/settings.ini";
        const QString gtk4 = s.env.configHome + "/gtk-4.0/settings.ini";
        const QString kde = s.env.configHome + "/kdeglobals";
        const QString qt6 = s.env.configHome + "/qt6ct/qt6ct.conf";
        const QByteArray gtk3Text = "# mine\n[Settings]\ngtk-icon-theme-name=Old\ngtk-theme-name=Adwaita\n";
        const QByteArray gtk4Text = "[Settings]\ngtk-theme-name=Adwaita\n";
        writeFile(gtk3, gtk3Text);
        writeFile(gtk4, gtk4Text);
        // Present, so it counts, whatever Qt's platform theme is.
        writeFile(qt6, "[Appearance]\nicon_theme=Old\n");
        AppCore core(s.env);
        desk.setEffective("Numix");
        const ReadResult mixed = readSetting(core.engine, "appearance.icons");
        QVERIFY(mixed.extra.value("mixed").toBool());
        QVERIFY(mixed.reason.contains("GTK 3"));
        // A file that names no pack yet is no disagreement.
        QVERIFY(!mixed.reason.contains("GTK 4"));
        const QStringList effects = core.engine.plan("appearance.icons", "pack:Papirus", {QVariantMap{{"value", "pack:Papirus"}}}).effects;
        QVERIFY(effects.join(' ').contains("Creates KDE"));
        QVERIFY(effects.join(' ').contains("Adds the pack to GTK 4"));

        const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.phase, QString("committed"));
        QCOMPARE(readAll(gtk3), QByteArray("# mine\n[Settings]\ngtk-icon-theme-name=Papirus\ngtk-theme-name=Adwaita\n"));
        QCOMPARE(readAll(gtk4), QByteArray("[Settings]\ngtk-theme-name=Adwaita\ngtk-icon-theme-name=Papirus\n"));
        QCOMPARE(readAll(kde), QByteArray("[Icons]\nTheme=Papirus\n"));
        QCOMPARE(readAll(qt6), QByteArray("[Appearance]\nicon_theme=Papirus\n"));
        QVERIFY(!QFile::exists(s.env.configHome + "/qt5ct/qt5ct.conf"));
        QVERIFY(readSetting(core.engine, "appearance.icons").reason.isEmpty());

        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(readAll(gtk3), gtk3Text);
        QCOMPARE(readAll(gtk4), gtk4Text);
        QVERIFY(!QFile::exists(kde));
        QCOMPARE(readAll(qt6), QByteArray("[Appearance]\nicon_theme=Old\n"));
        QCOMPARE(desk.effective(), QString("Numix"));
    }

    // A theme change fills in a file that names no pack, and leaves one someone edited.
    void aThemeChangeFillsMissingFilesButNotEditedOnes() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        const QString gtk3 = s.env.configHome + "/gtk-3.0/settings.ini";
        const QString kde = s.env.configHome + "/kdeglobals";
        QFile::remove(kde);
        writeFile(gtk3, "[Settings]\ngtk-icon-theme-name=Mine\n");
        desk.setEffective("Yaru-blue");
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));
        QCOMPARE(readAll(kde), QByteArray("[Icons]\nTheme=Papirus\n"));
        QCOMPARE(readAll(gtk3), QByteArray("[Settings]\ngtk-icon-theme-name=Mine\n"));
    }

    // ---- The launcher icon layer -------------------------------------------------
    void choosingAPackGivesTheLauncherItsIcons() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        AppCore core(s.env);
        // Nothing chosen yet: Omarchy is in charge, and there is nothing to follow.
        const ReadResult before = readSetting(core.engine, "appearance.icons.launcher");
        QCOMPARE(before.value, QString("true"));
        QVERIFY(before.reason.contains("Starts once you choose"));
        QVERIFY(!QFileInfo::exists(launchers.layer));

        const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
        QCOMPARE(tx.phase, QString("committed"));
        const QString apps = launchers.layer + "/apps/";
        QCOMPARE(readAll(apps + "utilities-terminal.svg"), QByteArray(kSvg));
        QCOMPARE(readAll(apps + "org.example.web-browser.svg"), QByteArray(kSvg));
        // A PNG becomes an SVG, so it comes before every PNG in the shell's index, and it
        // is no larger than the launcher needs.
        const QByteArray wrapped = readAll(apps + "ownapp.svg");
        QVERIFY(wrapped.startsWith("<svg"));
        QVERIFY(wrapped.contains("data:image/png;base64,"));
        QVERIFY(wrapped.contains("width=\"256\""));
        QVERIFY(!QFileInfo::exists(apps + "no-such-icon.svg"));
        QVERIFY(QFileInfo::exists(apps + "folder.svg"));
        // Only the finished layer is ever where the shell looks.
        QCOMPARE(launchers.entries(s), QStringList{"omarchy-control"});
        QVERIFY(QFileInfo::exists(s.env.configHome + "/omarchy/hooks/post-update.d/90-omarchy-control-icons"));

        const ReadResult row = readSetting(core.engine, "appearance.icons.launcher");
        QVERIFY(row.reason.isEmpty());
        QCOMPARE(row.extra.value("pack").toString(), QString("Papirus"));
        // Shown launchers only: the hidden helper is in the layer but not in the count.
        QCOMPARE(row.extra.value("total").toInt(), 4);
        QCOMPARE(row.extra.value("fromPack").toInt(), 2);
        QCOMPARE(row.extra.value("own").toInt(), 1);
        QCOMPARE(row.extra.value("missing").toInt(), 1);
        QCOMPARE(row.extra.value("gaps").toList().first().toMap().value("apps").toStringList(), QStringList{"Nothing"});

        // Undoing the first choice hands icons back to Omarchy, and the layer goes with it.
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QVERIFY(!QFileInfo::exists(launchers.layer));
        QVERIFY(!QFileInfo::exists(s.env.configHome + "/omarchy/hooks/post-update.d/90-omarchy-control-icons"));
    }

    // An app's icon from the pack, never an action icon of the same name.
    void aPacksActionIconDoesNotStandInForAnApp() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        writeFile(desk.system + "/Actiony/index.theme", "[Icon Theme]\nName=Actiony\nDirectories=scalable/actions,scalable/apps\n\n"
                                                        "[scalable/actions]\nSize=48\nContext=Actions\nType=Scalable\n\n"
                                                        "[scalable/apps]\nSize=48\nContext=Applications\nType=Scalable\n");
        writeFile(desk.system + "/Actiony/scalable/actions/zoom.svg", kSvg);
        writeFile(desk.system + "/Actiony/scalable/apps/utilities-terminal.svg", kSvg);
        addLauncher(launchers.apps, "zoom", "Zoom", "zoom");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Actiony").phase, QString("committed"));
        QVERIFY(QFileInfo::exists(launchers.layer + "/apps/utilities-terminal.svg"));
        QVERIFY(!QFileInfo::exists(launchers.layer + "/apps/zoom.svg"));
    }

    void theLauncherSwitchRemovesAndRestoresTheLayer() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        const Transaction off = applySetting(core.engine, "appearance.icons.launcher", "false");
        QCOMPARE(off.phase, QString("committed"));
        QVERIFY(!QFileInfo::exists(launchers.layer));
        QCOMPARE(readSetting(core.engine, "appearance.icons.launcher").value, QString("false"));
        QCOMPARE(QJsonDocument::fromJson(readAll(s.env.appConfigDir + "/icon-layer.json")).object().value("enabled"), QJsonValue(false));
        // The pack itself is untouched.
        QCOMPARE(desk.effective(), QString("Papirus"));
        // Off stays off through a theme change.
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QVERIFY(!QFileInfo::exists(launchers.layer));
        QCOMPARE(undoTx(core.engine, off.id).phase, QString("committed"));
        QVERIFY(QFileInfo::exists(launchers.layer + "/apps/utilities-terminal.svg"));
    }

    void aThemeChangeMovesTheLauncherToTheNewPack() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        addPack(desk.system, "Numix", {"utilities-terminal"});
        writeFile(desk.system + "/Numix/scalable/apps/utilities-terminal.svg", "<svg id=\"numix\"/>");
        desk.themeAsks("Papirus");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "follow-theme").phase, QString("committed"));
        QCOMPARE(readSetting(core.engine, "appearance.icons.launcher").extra.value("pack").toString(), QString("Papirus"));
        // Omarchy switches theme with this app closed: its script sets the new pack, then
        // the hook runs.
        desk.themeAsks("Numix");
        desk.setEffective("Numix");
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(readAll(launchers.layer + "/apps/utilities-terminal.svg"), QByteArray("<svg id=\"numix\"/>"));
        QCOMPARE(IconLayer::status(s.env).value("pack").toString(), QString("Numix"));
    }

    void anOutOfDateLayerIsRebuiltAndACurrentOneLeftAlone() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        const QByteArray manifest = readAll(launchers.layer + "/manifest.json");
        QTest::qWait(20);
        QCOMPARE(core.icons.syncLayer(true), 0);
        QCOMPARE(readAll(launchers.layer + "/manifest.json"), manifest);
        // An app installed later, as an update or a web app would add it.
        addLauncher(s.env.dataHome + "/applications", "late", "Late App", "text-x-generic");
        QCOMPARE(core.icons.syncLayer(true), 0);
        QVERIFY(QFileInfo::exists(launchers.layer + "/apps/text-x-generic.svg"));
    }

    void aLauncherThatIndexesDifferentlyPausesTheLayer() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QVERIFY(QFileInfo::exists(launchers.layer));
        // As if Omarchy's index followed the chosen pack, as upstream #9408 proposes.
        writeFile(s.env.omarchyPath + "/shell/services/AppLibrary.qml", "function iconSource(icon) { return Quickshell.iconPath(value, true) }\n");
        QCOMPARE(core.icons.syncLayer(true), 0);
        QVERIFY(!QFileInfo::exists(launchers.layer));
        QVERIFY(readSetting(core.engine, "appearance.icons.launcher").reason.contains("changed how its launcher finds icons"));
    }

    void aLayerFolderThatIsNotOursIsLeftAlone() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        writeFile(launchers.layer + "/apps/mine.svg", kSvg);
        AppCore core(s.env);
        // The choice itself still goes through.
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QCOMPARE(QDir(launchers.layer + "/apps").entryList(QDir::Files), QStringList{"mine.svg"});
        QVERIFY(readSetting(core.engine, "appearance.icons.launcher").reason.contains("not made by Omarchy Control"));
        QString message;
        core.icons.removeIntegration(&message);
        QVERIFY(QFileInfo::exists(launchers.layer + "/apps/mine.svg"));
    }

    void removingTheIntegrationTakesTheLayerAndHooks() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QString message;
        QCOMPARE(core.icons.removeIntegration(&message), 0);
        QVERIFY(!QFileInfo::exists(launchers.layer));
        QVERIFY(!QFileInfo::exists(desk.hook()));
        QVERIFY(!QFileInfo::exists(s.env.configHome + "/omarchy/hooks/post-update.d/90-omarchy-control-icons"));
        QCOMPARE(desk.effective(), QString("Papirus"));
    }

    // Iconic's hook runs after this app's, so both are never left fighting: its files are
    // handed over, kept, and can be put back exactly.
    void iconicIsHandedOverAndCanBeRestored() {
        Sandbox s;
        IconDesk desk(s);
        LauncherDesk launchers(s);
        const QString iconicLayer = s.env.home + "/.icons/iconic-apps/apps/utilities-terminal.svg";
        const QString iconicHook = s.env.configHome + "/omarchy/hooks/theme-set.d/iconic";
        const QString iconicConfig = s.env.configHome + "/iconic/config.toml";
        writeFile(iconicLayer, kSvg);
        writeFile(iconicHook, "#!/bin/bash\nexec iconic hook theme-set\n");
        writeFile(iconicConfig, "mode = \"lock\"\n");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        const ReadResult row = readSetting(core.engine, "appearance.icons.launcher");
        QVERIFY(row.reason.contains("Iconic"));
        QCOMPARE(row.extra.value("iconic").toStringList().size(), 3);

        QString message;
        QCOMPARE(core.icons.retireIconic(&message), 0);
        QVERIFY(!QFileInfo::exists(iconicLayer) && !QFileInfo::exists(iconicHook) && !QFileInfo::exists(iconicConfig));
        QCOMPARE(launchers.entries(s), QStringList{"omarchy-control"});
        QVERIFY(readSetting(core.engine, "appearance.icons.launcher").reason.isEmpty());
        QVERIFY(IconLayer::iconicRetired(s.env));
        // A second hand-over would mix two sets, so it waits for a restore.
        QCOMPARE(core.icons.retireIconic(&message), 1);

        QCOMPARE(core.icons.restoreIconic(&message), 0);
        QCOMPARE(readAll(iconicLayer), QByteArray(kSvg));
        QCOMPARE(readAll(iconicHook), QByteArray("#!/bin/bash\nexec iconic hook theme-set\n"));
        QCOMPARE(readAll(iconicConfig), QByteArray("mode = \"lock\"\n"));
        QVERIFY(!IconLayer::iconicRetired(s.env));
    }

    void anAmbiguousConfigIsReportedAndLeftAlone() {
        Sandbox s;
        IconDesk desk(s);
        const QString gtk3 = s.env.configHome + "/gtk-3.0/settings.ini";
        const QByteArray text = "[Settings]\ngtk-icon-theme-name=A\ngtk-icon-theme-name=B\n";
        writeFile(gtk3, text);
        AppCore core(s.env);
        QVERIFY(core.engine.plan("appearance.icons", "follow-theme", {QVariantMap{{"value", "follow-theme"}}}).effects.join(' ').contains("more than once"));
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QCOMPARE(readAll(gtk3), text);
    }

    void aSymlinkedConfigStaysALink() {
        Sandbox s;
        IconDesk desk(s);
        const QString target = s.env.home + "/dotfiles/settings.ini";
        const QString link = s.env.configHome + "/gtk-3.0/settings.ini";
        writeFile(target, "[Settings]\ngtk-icon-theme-name=Old\n");
        QDir().mkpath(QFileInfo(link).absolutePath());
        QFile::link(target, link);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(readAll(target), QByteArray("[Settings]\ngtk-icon-theme-name=Papirus\n"));
    }

    void changesElsewhereAreShownAndNeverForcedBack() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        const int writes = desk.writes();
        desk.setEffective("Numix");
        const ReadResult read = readSetting(core.engine, "appearance.icons");
        QCOMPARE(read.value, QString("pack:Papirus"));
        QVERIFY(read.extra.value("conflict").toBool());
        QVERIFY(read.reason.contains("Numix"));
        QCOMPARE(desk.effective(), QString("Numix"));
        QCOMPARE(desk.writes(), writes);

        // A chosen pack that goes away is remembered, and explained, not replaced.
        QDir(desk.system + "/Papirus").removeRecursively();
        const ReadResult gone = readSetting(core.engine, "appearance.icons");
        QCOMPARE(gone.value, QString("pack:Papirus"));
        QVERIFY(gone.reason.contains("not installed any more"));
        QVERIFY(readAll(desk.policy()).contains("Papirus"));
    }

    void followingTheThemeClearsTheFixedChoice() {
        Sandbox s;
        IconDesk desk(s);
        desk.themeAsks("Numix");
        desk.setEffective("Numix");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        desk.themeAsks("Yaru-blue");
        // Not the pack captured earlier, but what the theme in use asks for now.
        QCOMPARE(applySetting(core.engine, "appearance.icons", "follow-theme").phase, QString("committed"));
        QCOMPARE(desk.effective(), QString("Yaru-blue"));
        const QJsonObject policy = QJsonDocument::fromJson(readAll(desk.policy())).object();
        QCOMPARE(policy.value("mode").toString(), QString("follow-theme"));
        QVERIFY(!policy.contains("theme"));
        QVERIFY(QFile::exists(desk.hook()));
        QCOMPARE(readSetting(core.engine, "appearance.icons").value, QString("follow-theme"));

        // Use default means the same thing here.
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        Transaction reset;
        bool done = false;
        core.engine.reset("appearance.icons", [&](const Transaction &tx) { reset = tx; done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(reset.phase, QString("committed"));
        QCOMPARE(reset.kind, QString("reset"));
        QCOMPARE(desk.effective(), QString("Yaru-blue"));
    }

    void aThemeChangeKeepsAFixedChoiceWithNoWindowOpen() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        // Nothing chosen here: what Omarchy set stands, and nothing is even read.
        QString message;
        desk.setEffective("Numix");
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Numix"));
        QCOMPARE(desk.writes(), 0);

        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        // Omarchy's theme change writes the theme's pack, then runs the hook.
        desk.themeAsks("Numix");
        desk.setEffective("Numix");
        QCOMPARE(Cli::run(s.env, {"icons", "reapply"}), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));

        // Already right: no write at all.
        const int writes = desk.writes();
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.writes(), writes);

        // A pack that was removed is left out of it, with the choice kept.
        QDir(desk.system + "/Papirus").removeRecursively();
        desk.setEffective("Numix");
        QCOMPARE(core.icons.reapply(&message), 0);
        QVERIFY(message.contains("not installed"));
        QCOMPARE(desk.effective(), QString("Numix"));
        QVERIFY(readAll(desk.policy()).contains("Papirus"));
    }

    void followingKeepsUpWithTheThemeWithNoWindowOpen() {
        Sandbox s;
        IconDesk desk(s);
        const QString gtk3 = s.env.configHome + "/gtk-3.0/settings.ini";
        writeFile(gtk3, "[Settings]\ngtk-icon-theme-name=Old\n");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "follow-theme").phase, QString("committed"));
        // Stock Omarchy does not update this file, which is what the hook is still there for.
        desk.themeAsks("Numix");
        desk.setEffective("Numix");
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(readAll(gtk3), QByteArray("[Settings]\ngtk-icon-theme-name=Numix\n"));
        QCOMPARE(desk.effective(), QString("Numix"));
    }

    void anotherIconManagerEndsAStandoffNotAFight() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QString message;
        // The first two look like coincidence and are corrected.
        for (int i = 0; i < 2; ++i) {
            desk.setEffective("Their-Pack");
            QCOMPARE(core.icons.reapply(&message), 0);
            QCOMPARE(desk.effective(), QString("Papirus"));
        }
        const int writes = desk.writes();
        desk.setEffective("Their-Pack");
        QCOMPARE(core.icons.reapply(&message), 0);
        QVERIFY(message.contains("another icon manager"));
        QCOMPARE(desk.effective(), QString("Their-Pack"));
        QCOMPARE(desk.writes(), writes);
        const ReadResult read = readSetting(core.engine, "appearance.icons");
        QVERIFY(read.extra.value("suspended").toBool());
        QVERIFY(read.reason.contains("no longer reapplied"));
        // It stays out of it until someone chooses, and that settles it.
        message.clear();
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Their-Pack"));
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QVERIFY(!readSetting(core.engine, "appearance.icons").extra.value("suspended").toBool());
        desk.setEffective("Numix");
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));
    }

    void reapplyDoesNotRaceAChangeInProgress() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        desk.setEffective("Numix");
        // Another process is mid-change. Its lock has a long wait, so a stale reader is
        // what this guards against: the reapply reads the choice only once it has the lock.
        writeFile(desk.policy(), QJsonDocument(QJsonObject{{"version", 1}, {"mode", "fixed"}, {"theme", "Yaru-blue"}}).toJson());
        QString message;
        QCOMPARE(core.icons.reapply(&message), 0);
        QCOMPARE(desk.effective(), QString("Yaru-blue"));
    }

    void removingTheIntegrationLeavesTheIconsAsTheyAre() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        QCOMPARE(Cli::run(s.env, {"icons", "remove-integration"}), 0);
        QVERIFY(!QFile::exists(desk.hook()));
        QVERIFY(!QFile::exists(desk.policy()));
        QCOMPARE(desk.effective(), QString("Papirus"));

        writeFile(desk.hook(), "#!/bin/bash\n# mine\n");
        QString message;
        QCOMPARE(core.icons.removeIntegration(&message), 0);
        QVERIFY(QFile::exists(desk.hook()));
        QVERIFY(message.contains("edited"));
    }

    void aDamagedChoiceIsReportedAndIgnored() {
        Sandbox s;
        IconDesk desk(s);
        for (const QByteArray &bad : {QByteArray("not json"), QByteArray("{\"version\":99,\"mode\":\"fixed\",\"theme\":\"X\"}"), QByteArray("{\"version\":1,\"mode\":\"fixed\",\"theme\":\"../x\"}")}) {
            writeFile(desk.policy(), bad);
            AppCore core(s.env);
            desk.setEffective("Numix");
            const ReadResult read = readSetting(core.engine, "appearance.icons");
            QCOMPARE(read.extra.value("mode").toString(), QString("unmanaged"));
            QVERIFY(read.reason.contains("could not be read"));
            QString message;
            QCOMPARE(core.icons.reapply(&message), 0);
            QCOMPARE(desk.effective(), QString("Numix"));
            QCOMPARE(readAll(desk.policy()), bad);
        }
    }

    void aPausedOmarchyPausesTheHookToo() {
        Sandbox s;
        IconDesk desk(s);
        {
            AppCore core(s.env);
            QCOMPARE(applySetting(core.engine, "appearance.icons", "Papirus").phase, QString("committed"));
        }
        writeFile(s.bin("omarchy-theme-set-gnome"), "#!/bin/bash\n# not what was reviewed\n");
        QFile::setPermissions(s.bin("omarchy-theme-set-gnome"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        desk.setEffective("Numix");
        Env strict = s.env;
        strict.enforceCompat = true;
        AppCore core(strict);
        const CatalogEntry *entry = core.catalog.find("appearance.icons");
        QVERIFY(entry);
        QCOMPARE(core.compat.changedFor(*entry, {}), QString("omarchy-theme-set-gnome"));
        QVERIFY(!core.engine.plan("appearance.icons", "Papirus", {QVariantMap{{"value", "pack:Papirus"}}}).ok);
        QCOMPARE(Cli::run(strict, {"icons", "reapply"}), 0);
        QCOMPARE(desk.effective(), QString("Numix"));
    }

    void aStoppedChangeIsReportedAndNotBlindlyRestored() {
        Sandbox s;
        IconDesk desk(s);
        QString firstId;
        {
            AppCore core(s.env);
            const Transaction tx = applySetting(core.engine, "appearance.icons", "Papirus");
            QCOMPARE(tx.phase, QString("committed"));
            firstId = tx.id;
            QVERIFY(!tx.extra.isEmpty());
        }
        // The journal keeps what files cannot: whether the key had ever been set.
        AppCore core(s.env);
        QCOMPARE(core.coordinator.transaction(firstId).extra.value("gsettings").toObject().value("beforeUnset").toBool(), true);
        Transaction stuck = core.coordinator.transaction(firstId);
        stuck.id = "stoppedhere0";
        stuck.phase = "applying";
        QFile journal(core.coordinator.journalPath());
        QVERIFY(journal.open(QIODevice::Append));
        journal.write(QJsonDocument(stuck.toJson()).toJson(QJsonDocument::Compact) + "\n");
        journal.close();
        const int writes = desk.writes();
        QCOMPARE(core.coordinator.recoverInterrupted().size(), 1);
        QCOMPARE(core.coordinator.transaction("stoppedhere0").phase, QString("interrupted"));
        QCOMPARE(desk.writes(), writes);
        QCOMPARE(desk.effective(), QString("Papirus"));

        // A record with nothing to go back with says so instead of guessing.
        Transaction old = core.coordinator.transaction(firstId);
        old.id = "oldrecord000";
        old.phase = "committed";
        old.extra = {};
        QVERIFY(journal.open(QIODevice::Append));
        journal.write(QJsonDocument(old.toJson()).toJson(QJsonDocument::Compact) + "\n");
        journal.close();
        QCOMPARE(undoTx(core.engine, "oldrecord000").phase, QString("failed"));
    }

    void theCliGivesIconStateItsStructure() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.system, "Orphan", {"folder"}, "NotInstalled");
        AppCore core(s.env);
        QStringList problems;
        const QVariantList packs = core.icons.listJson(&problems);
        QVERIFY(std::any_of(packs.cbegin(), packs.cend(), [](const QVariant &p) { return p.toMap().value("id") == "Papirus"; }));
        QVERIFY(problems.join('\n').contains("Orphan: inherits NotInstalled"));
        QCOMPARE(Cli::run(s.env, {"icons", "list", "--json"}), 0);
        QCOMPARE(Cli::run(s.env, {"get", "appearance.icons", "--json"}), 0);
        QCOMPARE(Cli::run(s.env, {"plan", "appearance.icons", "Papirus"}), 0);
        QCOMPARE(Cli::run(s.env, {"apply", "appearance.icons", "Papirus"}), 0);
        QCOMPARE(desk.effective(), QString("Papirus"));
        QCOMPARE(Cli::run(s.env, {"apply", "appearance.icons", "NotInstalled"}), 1);
        QCOMPARE(Cli::run(s.env, {"icons", "nonsense"}), 1);
    }

    // --- Importing a pack ---

    void importingCopiesAPackAndLeavesTheSourceAlone() {
        Sandbox s;
        IconDesk desk(s);
        const QString source = s.dir.path() + "/downloads/Fresh";
        addPack(s.dir.path() + "/downloads", "Fresh");
        writeFile(source + "/LICENSE", "free");
        writeFile(source + "/install.sh", "#!/bin/sh\ntouch " + s.dir.path().toUtf8() + "/ran\n");
        QFile::setPermissions(source + "/install.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        writeFile(source + "/.git/config", "x");
        QVERIFY(QFile::link("folder.svg", source + "/scalable/apps/inode-directory.svg"));
        QDir().mkpath(source + "/scalable/places");
        QVERIFY(QFile::link("../apps/folder.svg", source + "/scalable/places/folder.svg"));
        const QStringList before = QDir(source).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);

        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, source), QString("imported"));
        QCOMPARE(importer.themeId(), QString("Fresh"));
        const QString copy = desk.user + "/Fresh";
        QVERIFY(QFile::exists(copy + "/index.theme"));
        QCOMPARE(readAll(copy + "/LICENSE"), QByteArray("free"));
        QCOMPARE(readAll(copy + "/scalable/apps/folder.svg"), QByteArray(kSvg));
        QVERIFY(QFileInfo(copy + "/scalable/places/folder.svg").isSymLink());
        QCOMPARE(QFile::symLinkTarget(copy + "/scalable/places/folder.svg"), QFileInfo(copy + "/scalable/apps/folder.svg").absoluteFilePath());
        QVERIFY(!QFile::exists(copy + "/.git"));
        QVERIFY(!QFile::exists(copy + "/.omarchy-control-import.json"));
        // Nothing from a pack can be run: not by the import, and not by accident later.
        QVERIFY(!QFile::exists(s.dir.path() + "/ran"));
        QVERIFY(!(QFile::permissions(copy + "/install.sh") & (QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther)));
        QCOMPARE(QDir(source).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), before);
        QVERIFY(QFile::exists(source + "/.git/config"));
        QCOMPARE(QDir(desk.user).entryList(QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot), QStringList({"Fresh"}));

        // Importing never selects, and it is on record with no pretense of undo.
        QCOMPARE(desk.writes(), 0);
        QVERIFY(desk.isUnset());
        const Transaction tx = core.coordinator.history(1).first();
        QCOMPARE(tx.settingId, QString("appearance.icons.import"));
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(!tx.undoable());
        QVERIFY(std::any_of(IconThemes::discover(s.env).cbegin(), IconThemes::discover(s.env).cend(), [](const IconPack &p) { return p.id == "Fresh"; }));
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Fresh").phase, QString("committed"));
    }

    // ---- Archives -------------------------------------------------------------------
    void anArchiveWithSeveralPacksImportsEachInOrder() {
        Sandbox s;
        IconDesk desk(s);
        const QString file = s.dir.path() + "/downloads/dawn-icons.tar.gz";
        // Dark first in the archive, but it builds on Dawn, so Dawn goes in first; and its
        // icon is a link into Dawn, which is allowed for a pack from the same archive.
        writeArchive(file, {
            {"dawn-icons-1.0/Dawn-Dark/index.theme", 'f', packIndex("Dawn Dark", "Dawn,hicolor")},
            {"dawn-icons-1.0/Dawn-Dark/scalable/apps", 'd'},
            {"dawn-icons-1.0/Dawn-Dark/scalable/apps/utilities-terminal.svg", 'l', {}, "../../../Dawn/scalable/apps/utilities-terminal.svg"},
            {"dawn-icons-1.0/Dawn/index.theme", 'f', packIndex("Dawn")},
            {"dawn-icons-1.0/Dawn/scalable/apps/utilities-terminal.svg", 'f', kSvg},
            {"dawn-icons-1.0/Dawn/scalable/apps/web-browser.svg", 'h', {}, "dawn-icons-1.0/Dawn/scalable/apps/utilities-terminal.svg"},
            {"dawn-icons-1.0/Dawn/icon-theme.cache", 'f', "stale"},
            {"dawn-icons-1.0/Cursors/index.theme", 'f', "[Icon Theme]\nName=Dawn Cursors\n"},
            {"dawn-icons-1.0/dev", 'c'},
            {"__MACOSX/dawn-icons-1.0/._Dawn", 'f', "junk"},
        });
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, file), QString("imported"));
        QVERIFY(importer.message().contains("Imported Dawn, Dawn Dark from dawn-icons.tar.gz"));
        QVERIFY(importer.message().contains("Dawn Cursors has no icon folders"));
        QVERIFY(importer.message().contains("special files"));
        QCOMPARE(importer.themeId(), QString("Dawn"));
        QCOMPARE(readAll(desk.user + "/Dawn-Dark/scalable/apps/utilities-terminal.svg"), QByteArray(kSvg));
        QVERIFY(QFileInfo(desk.user + "/Dawn-Dark/scalable/apps/utilities-terminal.svg").isSymLink());
        // A hard link becomes a file of its own.
        QCOMPARE(readAll(desk.user + "/Dawn/scalable/apps/web-browser.svg"), QByteArray(kSvg));
        QVERIFY(!QFileInfo(desk.user + "/Dawn/scalable/apps/web-browser.svg").isSymLink());
        // A cache made elsewhere is dropped; no tool here makes a new one.
        QVERIFY(!QFile::exists(desk.user + "/Dawn/icon-theme.cache"));
        QCOMPARE(QDir(desk.user).entryList(QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot), QStringList({"Dawn", "Dawn-Dark"}));
        // The unpacked copy is gone.
        QCOMPARE(QDir(s.env.appCacheDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList());
        // Each pack is a change of its own in History.
        int imports = 0;
        for (const Transaction &tx : core.coordinator.history())
            imports += tx.settingId == QLatin1String("appearance.icons.import") && tx.phase == QLatin1String("committed");
        QCOMPARE(imports, 2);
    }

    // A pack at the archive's top level is named after the archive, and a zip works too.
    void aPackAtTheTopOfAZipIsNamedAfterIt() {
        Sandbox s;
        IconDesk desk(s);
        const QString file = s.dir.path() + "/downloads/Sunny.zip";
        writeArchive(file, {{"index.theme", 'f', packIndex("Sunny")}, {"scalable/apps/folder.svg", 'f', kSvg}});
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, file), QString("imported"));
        QCOMPARE(importer.themeId(), QString("Sunny"));
        QCOMPARE(readAll(desk.user + "/Sunny/scalable/apps/folder.svg"), QByteArray(kSvg));
    }

    void aPackAlreadyInstalledIsSkippedAndTheRestImported() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.user, "Old");
        const QString file = s.dir.path() + "/downloads/two.tar.gz";
        writeArchive(file, {{"Old/index.theme", 'f', packIndex("Old")}, {"Old/scalable/apps/a.svg", 'f', kSvg},
                            {"New/index.theme", 'f', packIndex("New")}, {"New/scalable/apps/a.svg", 'f', kSvg}});
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, file), QString("imported"));
        QVERIFY(importer.message().contains("Imported New"));
        QVERIFY(importer.message().contains("A pack called Old is already in your icons folder"));
        QCOMPARE(readAll(desk.user + "/Old/scalable/apps/folder.svg"), QByteArray(kSvg));
    }

    // Archives that try to reach outside, or to pass for something else: nothing is
    // installed, nothing outside is touched, and the unpacked copy is gone.
    void hostileArchivesInstallNothing() {
        Sandbox s;
        IconDesk desk(s);
        const QString in = s.dir.path() + "/downloads";
        const QString outside = s.dir.path() + "/outside";
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        const auto refused = [&](const QString &name, const QList<ArchiveItem> &items, const QString &words) {
            writeArchive(in + "/" + name, items);
            const QString state = importPack(importer, in + "/" + name);
            const bool clean = QDir(desk.user).entryList(QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty() && !QFileInfo::exists(outside)
                               && QDir(s.env.appCacheDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
            if (state != QLatin1String("failed") || !importer.message().contains(words) || !clean) {
                qWarning() << name << state << importer.message() << clean;
                return false;
            }
            return true;
        };
        const ArchiveItem index{"Evil/index.theme", 'f', packIndex("Evil")};
        QVERIFY(refused("climb.tar.gz", {index, {"Evil/../../outside", 'f', "x"}}, "outside itself"));
        QVERIFY(refused("absolute.tar.gz", {index, {outside, 'f', "x"}}, "outside itself"));
        // A link the archive makes, then a file written through it.
        QVERIFY(refused("through.tar.gz", {index, {"Evil/door", 'l', {}, "../../../.."}, {"Evil/door/outside", 'f', "x"}}, "writes through a link"));
        QVERIFY(refused("abslink.tar.gz", {index, {"Evil/scalable/apps/a.svg", 'l', {}, "/etc/passwd"}}, "points outside the archive"));
        QVERIFY(refused("hardlink.tar.gz", {index, {"Evil/scalable/apps/a.svg", 'h', {}, "../../etc/passwd"}}, "not a file in it"));
        // A relative link out of the pack is unpacked, then refused by the importer's own check.
        QVERIFY(refused("escape.tar.gz", {index, {"Evil/scalable/apps/a.svg", 'l', {}, "../../../../../../outside"}}, "points outside the pack"));
        QVERIFY(refused("huge.tar.gz", {index, {"Evil/scalable/apps/big.svg", 'f', {}, {}, qint64(65) * 1024 * 1024}}, "too large"));
        QVERIFY(refused("empty.tar.gz", {{"readme.txt", 'f', "hello"}}, "no icon pack in it"));
        QVERIFY(!QFileInfo::exists(outside));
    }

    void anUnpackedArchiveLeftBehindIsCleanedUp() {
        Sandbox s;
        IconDesk desk(s);
        writeFile(s.env.appCacheDir + "/.omarchy-control-import-abc/content/Pack/index.theme", "x");
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        importer.recoverStaging();
        QCOMPARE(QDir(s.env.appCacheDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList());
    }

    // ---- Editing an app's launcher --------------------------------------------------
    void aPackagesLauncherIsChangedThroughACopy() {
        Sandbox s;
        IconDesk desk(s);
        s.env.process.insert("LANG", "de_DE.UTF-8");
        const QString package = s.env.dataDirs.last() + "/applications/org.example.Web.desktop";
        const QByteArray original = "[Desktop Entry]\nType=Application\nName=Web\nName[de]=Netz\nName[fr]=Toile\nIcon=web-browser\nExec=web %u\n\n"
                                    "[Desktop Action new]\nName=New Window\nName[de]=Neues Fenster\n";
        writeFile(package, original);
        AppCore core(s.env);
        LauncherEditor::Edit edit;
        edit.name = QStringLiteral("  Browser  ");
        edit.hidden = true;
        const Transaction tx = launcherChange([&](auto done) { core.launchers.apply("org.example.Web", edit, done); });
        QCOMPARE(tx.phase, QString("committed"));
        // The package's own file is exactly as installed.
        QCOMPARE(readAll(package), original);
        const QString copy = s.env.dataHome + "/applications/org.example.Web.desktop";
        const QByteArray changed = readAll(copy);
        QVERIFY(changed.contains("\nName=Browser\n"));
        // This session's language takes the new name; every other keeps its own, and the
        // actions are untouched.
        QVERIFY(changed.contains("\nName[de]=Browser\n"));
        QVERIFY(changed.contains("\nName[fr]=Toile\n"));
        QVERIFY(changed.contains("[Desktop Action new]\nName=New Window\nName[de]=Neues Fenster\n"));
        QVERIFY(changed.contains("NoDisplay=true"));
        QVERIFY(changed.contains("X-OmarchyControl-Source=" + package.toUtf8()));
        QVERIFY(changed.contains("X-OmarchyControl-Changed=Name;NoDisplay;"));
        const LauncherEditor::Entry entry = core.launchers.find("org.example.Web");
        QCOMPARE(entry.kind, QString("override"));
        QCOMPARE(entry.name, QString("Browser"));
        QVERIFY(entry.modified && !entry.visible && !entry.outdated);
        QCOMPARE(core.coordinator.history().first().before, QString("Netz"));
        QCOMPARE(core.coordinator.history().first().after, QString("Browser"));

        // Undo takes the copy away again.
        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(core.launchers.find("org.example.Web").kind, QString("package"));

        // A package update is noticed, and brought in with the same changes.
        edit = {};
        edit.icon = QStringLiteral("utilities-terminal");
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("org.example.Web", edit, done); }).phase, QString("committed"));
        writeFile(package, original + "Keywords=new;\n");
        QVERIFY(core.launchers.find("org.example.Web").outdated);
        QCOMPARE(launcherChange([&](auto done) { core.launchers.update("org.example.Web", done); }).phase, QString("committed"));
        const QByteArray updated = readAll(copy);
        QVERIFY(updated.contains("Keywords=new;"));
        QVERIFY(updated.contains("\nIcon=utilities-terminal\n"));
        QVERIFY(!core.launchers.find("org.example.Web").outdated);

        // Reset takes the copy away; with the package gone, the copy is an orphan.
        QCOMPARE(launcherChange([&](auto done) { core.launchers.reset("org.example.Web", done); }).phase, QString("committed"));
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("org.example.Web", edit, done); }).phase, QString("committed"));
        QFile::remove(package);
        QCOMPARE(core.launchers.find("org.example.Web").kind, QString("orphan"));
    }

    // A launcher of the user's own, such as a web app, is edited where it is, after one
    // backup that Reset goes back to.
    void anOwnLauncherIsEditedInPlaceAndResetFromItsBackup() {
        Sandbox s;
        IconDesk desk(s);
        const QString path = s.env.dataHome + "/applications/HEY.desktop";
        const QByteArray original = "[Desktop Entry]\nType=Application\nName=HEY\nExec=omarchy-launch-webapp https://hey.com\nIcon=/home/me/hey.png\n";
        writeFile(path, original);
        AppCore core(s.env);
        LauncherEditor::Edit edit;
        edit.name = QStringLiteral("Mail");
        const Transaction tx = launcherChange([&](auto done) { core.launchers.apply("HEY.desktop", edit, done); });
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(readAll(path).contains("Name=Mail"));
        QVERIFY(!readAll(path).contains("X-OmarchyControl"));
        QCOMPARE(readAll(core.launchers.backupPath("HEY.desktop")), original);
        QCOMPARE(core.launchers.find("HEY").kind, QString("own"));
        // A second change keeps the first backup: Reset goes all the way back.
        edit.name = QStringLiteral("Post");
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("HEY", edit, done); }).phase, QString("committed"));
        QCOMPARE(readAll(core.launchers.backupPath("HEY.desktop")), original);
        const Transaction reset = launcherChange([&](auto done) { core.launchers.reset("HEY", done); });
        QCOMPARE(reset.phase, QString("committed"));
        QCOMPARE(readAll(path), original);
        QVERIFY(!QFileInfo::exists(core.launchers.backupPath("HEY.desktop")));
        // Undoing the reset brings the edit and its backup back.
        QCOMPARE(undoTx(core.engine, reset.id).phase, QString("committed"));
        QVERIFY(readAll(path).contains("Name=Post"));
        QCOMPARE(readAll(core.launchers.backupPath("HEY.desktop")), original);
    }

    void aChosenImageIsCopiedInAndBadInputRefused() {
        Sandbox s;
        IconDesk desk(s);
        writeFile(s.env.dataDirs.last() + "/applications/tool.desktop", "[Desktop Entry]\nType=Application\nName=Tool\nIcon=tool\nExec=tool\n");
        const QString picture = s.dir.path() + "/Pictures/my icon.png";
        QDir().mkpath(QFileInfo(picture).absolutePath());
        QImage image(32, 32, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(picture));
        AppCore core(s.env);
        QString error;
        const QString copy = core.launchers.importIcon(picture, &error);
        QVERIFY2(copy.startsWith(core.launchers.iconFolder() + "/my_icon-"), qPrintable(copy + error));
        QVERIFY(core.launchers.importIcon(s.dir.path() + "/Pictures/nope.png", &error).isEmpty());
        writeFile(s.dir.path() + "/notes.txt", "x");
        QVERIFY(core.launchers.importIcon(s.dir.path() + "/notes.txt", &error).isEmpty());

        LauncherEditor::Edit edit;
        edit.icon = copy;
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("tool", edit, done); }).phase, QString("committed"));
        QVERIFY(readAll(s.env.dataHome + "/applications/tool.desktop").contains("Icon=" + copy.toUtf8()));
        // An image nothing uses any more goes; one in use stays.
        const QString unused = core.launchers.importIcon(picture.left(picture.lastIndexOf('/')) + "/../Pictures/my icon.png", &error);
        writeFile(core.launchers.iconFolder() + "/stale-0000000000.png", "x");
        core.launchers.cleanIcons();
        QVERIFY(QFileInfo::exists(copy));
        QCOMPARE(unused, copy);
        QVERIFY(!QFileInfo::exists(core.launchers.iconFolder() + "/stale-0000000000.png"));

        edit = {};
        edit.name = QStringLiteral("   ");
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("tool", edit, done); }).phase, QString("failed"));
        edit = {};
        edit.icon = QStringLiteral("/no/such/icon.png");
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("tool", edit, done); }).phase, QString("failed"));
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("missing-app", edit, done); }).phase, QString("failed"));
    }

    // ---- Removing a pack ------------------------------------------------------------
    void removingAPackMovesItToTheTrashAndUndoBringsItBack() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Papirus");
        addPack(desk.user, "Mine");
        AppCore core(s.env);
        // Only what the user installed is offered for removal.
        QVariantList choices;
        bool read = false;
        core.icons.readChoices([&](const QVariantList &list) { choices = list; read = true; });
        QTest::qWaitFor([&] { return read; }, 5000);
        QStringList removable;
        for (const QVariant &choice : choices)
            if (choice.toMap().value("removable").toBool())
                removable << choice.toMap().value("id").toString();
        QCOMPARE(removable, QStringList{"Mine"});

        const Transaction tx = removeIconPack(core.icons, "Mine");
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(!QFileInfo::exists(desk.user + "/Mine"));
        const QString trash = s.env.dataHome + "/Trash";
        QCOMPARE(readAll(trash + "/files/Mine/scalable/apps/folder.svg"), QByteArray(kSvg));
        const QByteArray info = readAll(trash + "/info/Mine.trashinfo");
        QVERIFY(info.startsWith("[Trash Info]\nPath=" + QUrl::toPercentEncoding(desk.user + "/Mine", "/") + "\nDeletionDate="));
        QCOMPARE(core.coordinator.history().first().settingId, QString("appearance.icons.remove"));

        QCOMPARE(undoTx(core.engine, tx.id).phase, QString("committed"));
        QCOMPARE(readAll(desk.user + "/Mine/scalable/apps/folder.svg"), QByteArray(kSvg));
        QVERIFY(!QFileInfo::exists(trash + "/files/Mine"));
        QVERIFY(!QFileInfo::exists(trash + "/info/Mine.trashinfo"));

        // A second removal of the same name does not overwrite what the Trash already holds.
        writeFile(trash + "/files/Mine/keep", "x");
        writeFile(trash + "/info/Mine.trashinfo", "[Trash Info]\n");
        QCOMPARE(removeIconPack(core.icons, "Mine").phase, QString("committed"));
        QVERIFY(QFileInfo(trash + "/files/Mine.2").isDir());
        QCOMPARE(readAll(trash + "/files/Mine/keep"), QByteArray("x"));
    }

    void anEmptiedTrashIsSaidPlainlyOnUndo() {
        Sandbox s;
        IconDesk desk(s);
        desk.setEffective("Papirus");
        addPack(desk.user, "Gone");
        AppCore core(s.env);
        const Transaction tx = removeIconPack(core.icons, "Gone");
        QCOMPARE(tx.phase, QString("committed"));
        QVERIFY(QDir(s.env.dataHome + "/Trash/files/Gone").removeRecursively());
        const Transaction undone = undoTx(core.engine, tx.id);
        QCOMPARE(undone.phase, QString("failed"));
        QVERIFY(undone.message.contains("no longer in the Trash"));
    }

    // What is in use, or what another pack needs, stays; and system packs belong to packages.
    void packsThatAreNeededStay() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.user, "Base");
        addPack(desk.user, "Child", {"folder"}, "Base");
        addPack(desk.user, "Target");
        addPack(desk.user, "Linker", {});
        QDir().mkpath(desk.user + "/Linker/scalable/apps");
        QVERIFY(QFile::link("../../../Target/scalable/apps/folder.svg", desk.user + "/Linker/scalable/apps/folder.svg"));
        addPack(desk.user, "Used");
        addPack(desk.user, "Asked");
        addPack(desk.user, "Chosen");
        AppCore core(s.env);
        QCOMPARE(applySetting(core.engine, "appearance.icons", "Chosen").phase, QString("committed"));
        const auto blocked = [&](const QString &id, const QString &words) {
            const IconThemeProvider::Removal plan = core.icons.planRemoval(id);
            if (plan.ok || !plan.reason.contains(words)) {
                qWarning() << id << plan.ok << plan.reason;
                return false;
            }
            // Trying anyway changes nothing.
            return removeIconPack(core.icons, id).phase == QLatin1String("failed") && (id == QLatin1String("Papirus") || QFileInfo::exists(desk.user + "/" + id));
        };
        QVERIFY(blocked("Papirus", "came with the system"));
        QVERIFY(blocked("Chosen", "your chosen pack"));
        QVERIFY(blocked("Base", "Child builds on Base"));
        QVERIFY(blocked("Target", "Linker links into Target"));
        desk.themeAsks("Asked");
        QVERIFY(blocked("Asked", "current Omarchy theme asks for"));
        // Set elsewhere, so it is the pack in use without being this app's choice.
        writeFile(desk.policy(), "{\"version\":1,\"mode\":\"follow-theme\"}\n");
        desk.setEffective("Used");
        QVERIFY(blocked("Used", "is in use"));
        // With nothing needing it any more, it can go.
        QVERIFY(core.icons.planRemoval("Child").ok);
    }

    void badFoldersAreExplainedAndNothingIsCopied() {
        Sandbox s;
        IconDesk desk(s);
        const QString in = s.dir.path() + "/in";
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        const auto refused = [&](const QString &path, const QString &words) {
            const QString state = importPack(importer, path);
            if (state != QLatin1String("failed") || !importer.message().contains(words)) {
                qWarning() << path << state << importer.message();
                return false;
            }
            return QDir(desk.user).entryList(QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
        };

        QVERIFY(refused(in + "/nothing-here", "could not be found"));
        // Not really an archive, whatever its name says.
        writeFile(in + "/pack.tar.gz", "x");
        QVERIFY(refused(in + "/pack.tar.gz", "pack.tar.gz"));
        writeFile(in + "/pack.rar", "x");
        QVERIFY(refused(in + "/pack.rar", "RAR archives cannot be read here"));
        writeFile(in + "/file.txt", "x");
        QVERIFY(refused(in + "/file.txt", "This is a file"));
        QDir().mkpath(in + "/empty");
        QVERIFY(refused(in + "/empty", "no index.theme"));
        addPack(in + "/wrapper", "Inner");
        QVERIFY(refused(in + "/wrapper", "Choose that folder instead"));
        addPack(in + "/two", "One");
        addPack(in + "/two", "Other");
        QVERIFY(refused(in + "/two", "holds 2 packs"));
        writeFile(in + "/Cursors/index.theme", "[Icon Theme]\nName=Cursors\nInherits=x\n");
        QVERIFY(refused(in + "/Cursors", "cursor theme"));
        writeFile(in + "/Bad/index.theme", "garbage\n");
        QVERIFY(refused(in + "/Bad", "not valid"));
        writeFile(in + "/Ghost/index.theme", "[Icon Theme]\nName=Ghost\nDirectories=a\n[a]\nSize=48\n");
        QVERIFY(refused(in + "/Ghost", "would show no icons"));

        addPack(in, "Escape");
        QVERIFY(QFile::link("../../../../../../etc/passwd", in + "/Escape/scalable/apps/passwd.svg"));
        QVERIFY(refused(in + "/Escape", "points outside"));
        addPack(in, "Absolute");
        QVERIFY(QFile::link("/etc/passwd", in + "/Absolute/scalable/apps/passwd.svg"));
        QVERIFY(refused(in + "/Absolute", "points outside"));
        addPack(in, "Loop");
        QVERIFY(QFile::link("b.svg", in + "/Loop/scalable/apps/a.svg"));
        QVERIFY(QFile::link("a.svg", in + "/Loop/scalable/apps/b.svg"));
        QVERIFY(refused(in + "/Loop", "in a circle"));
        addPack(in, "Ancestor");
        QVERIFY(QFile::link("..", in + "/Ancestor/scalable/apps/up"));
        QVERIFY(refused(in + "/Ancestor", "in a circle"));
        // Only following the links shows this one leaves: its text alone stays inside.
        addPack(in, "Sneaky");
        QDir().mkpath(in + "/Sneaky/deep/er");
        QDir().mkpath(in + "/Sneaky/other");
        QVERIFY(QFile::link("../../other", in + "/Sneaky/deep/er/hop"));
        QVERIFY(QFile::link("hop/../..", in + "/Sneaky/deep/er/out"));
        QVERIFY(refused(in + "/Sneaky", "points outside"));
        addPack(in, "Device");
        QCOMPARE(QProcess::execute("mkfifo", {in + "/Device/pipe"}), 0);
        QVERIFY(refused(in + "/Device", "device or socket"));

        // The source and the active choice are untouched by any of that.
        QVERIFY(QFile::exists(in + "/Escape/index.theme"));
        QCOMPARE(desk.writes(), 0);
    }

    void anOccupiedDestinationIsNeverReplaced() {
        Sandbox s;
        IconDesk desk(s);
        addPack(desk.user, "Taken", {"mine"});
        addPack(s.dir.path() + "/in", "Taken", {"theirs"});
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, s.dir.path() + "/in/Taken"), QString("failed"));
        QVERIFY(importer.message().contains("already in your icons folder"));
        QVERIFY(QFile::exists(desk.user + "/Taken/scalable/apps/mine.svg"));
        QVERIFY(!QFile::exists(desk.user + "/Taken/scalable/apps/theirs.svg"));
    }

    void hidingAnotherCopyOfAPackAsksFirst() {
        Sandbox s;
        IconDesk desk(s);
        addPack(s.dir.path() + "/in", "Papirus", {"mine"}, "NotInstalled");
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QCOMPARE(importPack(importer, s.dir.path() + "/in/Papirus"), QString("confirm"));
        QVERIFY(importer.message().contains(desk.system + "/Papirus"));
        QVERIFY(!QFile::exists(desk.user + "/Papirus"));
        importer.cancel();
        QCOMPARE(importer.state(), QString("idle"));

        QCOMPARE(importPack(importer, s.dir.path() + "/in/Papirus"), QString("confirm"));
        QSignalSpy finished(&importer, &IconThemeImporter::finished);
        importer.confirm();
        QVERIFY(finished.wait(20000));
        QCOMPARE(importer.state(), QString("imported"));
        QVERIFY(QFile::exists(desk.user + "/Papirus/scalable/apps/mine.svg"));
        // What it needs is named, and not fetched from anywhere.
        QVERIFY(importer.message().contains("NotInstalled"));
        QVERIFY(QDir(desk.system + "/Papirus").exists());

        addPack(s.dir.path() + "/in", "Numix");
        QCOMPARE(Cli::run(s.env, {"icons", "import", s.dir.path() + "/in/Numix"}), 1);
        QVERIFY(!QFile::exists(desk.user + "/Numix"));
        QCOMPARE(Cli::run(s.env, {"icons", "import", s.dir.path() + "/in/Numix", "--yes"}), 0);
        QVERIFY(QFile::exists(desk.user + "/Numix/index.theme"));
    }

    void aCancelledImportLeavesNoTrace() {
        Sandbox s;
        IconDesk desk(s);
        const QString source = s.dir.path() + "/in/Big";
        addPack(s.dir.path() + "/in", "Big");
        const QByteArray bulk(2 * 1024 * 1024, 'x');
        for (int i = 0; i < 40; ++i)
            writeFile(source + "/scalable/apps/bulk" + QString::number(i) + ".svg", bulk);
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QSignalSpy finished(&importer, &IconThemeImporter::finished);
        importer.choosePath(source);
        QVERIFY(importer.busy());
        // A second folder while copying is not a second import.
        importer.choosePath(s.dir.path() + "/elsewhere");
        QVERIFY(importer.busy());
        importer.cancel();
        QVERIFY(finished.count() || finished.wait(20000));
        QCOMPARE(finished.first().first().toBool(), false);
        QCOMPARE(importer.state(), QString("idle"));
        QVERIFY(importer.message().contains("cancelled"));
        QVERIFY(QDir(desk.user).entryList(QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(QFile::exists(source + "/scalable/apps/bulk0.svg"));
        // Nothing changed, so there is nothing for History to say.
        QVERIFY(core.coordinator.history(1).isEmpty());
    }

    void aSourceThatChangesMidCopyIsNotImported() {
        Sandbox s;
        IconDesk desk(s);
        addPack(s.dir.path() + "/in", "Moving");
        AppCore core(s.env);
        IconThemeImporter importer(s.env, &core.coordinator);
        QSignalSpy finished(&importer, &IconThemeImporter::finished);
        const QString source = s.dir.path() + "/in/Moving";
        const QByteArray bulk(2 * 1024 * 1024, 'x');
        for (int i = 0; i < 40; ++i)
            writeFile(source + "/scalable/apps/bulk" + QString::number(i) + ".svg", bulk);
        importer.choosePath(source);
        QVERIFY(importer.busy());
        writeFile(source + "/added-during-copy.svg", "x");
        QVERIFY(finished.count() || finished.wait(30000));
        if (importer.state() == QLatin1String("failed")) {
            QVERIFY(importer.message().contains("changed while"));
            QVERIFY(QDir(desk.user).entryList(QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        } else {
            // The copy finished before the file landed, in which case it is an exact one.
            QCOMPARE(importer.state(), QString("imported"));
            QVERIFY(QFile::exists(desk.user + "/Moving/index.theme"));
        }
    }

    void leftoversFromAStoppedImportAreSortedOutSafely() {
        Sandbox s;
        IconDesk desk(s);
        AppCore core(s.env);
        const auto leftover = [&](const QString &name, const QJsonObject &paths) {
            const QString folder = desk.user + "/.omarchy-control-import-" + name;
            writeFile(folder + "/.omarchy-control-import.json", QJsonDocument(QJsonObject{{"paths", paths}}).toJson());
            return folder;
        };
        const QString clean = leftover("clean", {{"index.theme", 4}, {"sub", 0}, {"sub/a.svg", 10}});
        writeFile(clean + "/index.theme", "abcd");
        writeFile(clean + "/sub/a.svg", "partial");
        const QString touched = leftover("touched", {{"index.theme", 4}});
        writeFile(touched + "/index.theme", "abcd");
        writeFile(touched + "/somebody-elses.txt", "keep me");
        const QString unowned = desk.user + "/.omarchy-control-import-unowned";
        writeFile(unowned + "/precious.txt", "keep me");

        IconThemeImporter importer(s.env, &core.coordinator);
        // Never while something else may be doing an import.
        {
            QLockFile lock(s.env.appStateDir + "/icon-import.lock");
            QVERIFY(lock.tryLock(0));
            QVERIFY(importer.recoverStaging().isEmpty());
            QVERIFY(QDir(clean).exists());
        }
        const QStringList messages = importer.recoverStaging();
        QCOMPARE(messages.size(), 3);
        QVERIFY(!QDir(clean).exists());
        QVERIFY(QFile::exists(touched + "/somebody-elses.txt"));
        QVERIFY(QFile::exists(unowned + "/precious.txt"));
        QVERIFY(messages.join('\n').contains(touched));
        QVERIFY(IconThemes::discover(s.env).size() == 3);
    }

    void appearanceShowsPacksAndChoosingOneWorks() {
        Sandbox s;
        fakeDesktop(s);
        IconDesk desk(s);
        addPack(desk.system, "A Very Long Icon Pack Name That Has To Be Cut Off Somewhere Sensible");
        for (int i = 0; i < 12; ++i)
            addPack(desk.system, "Extra" + QString::number(i));
        desk.setEffective("Yaru-blue");
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        engine.addImageProvider("icons", new IconProvider);
        engine.addImageProvider("iconpacks", new IconPackImageProvider(s.env));
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString());
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window, 3000));
        QAccessible::setActive(true);
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "appearance"), Q_ARG(QVariant, "appearance.icons"));
        const auto named = [&](const QString &objectName) {
            QList<QQuickItem *> found;
            std::function<void(QQuickItem *)> search = [&](QQuickItem *item) {
                if (item->objectName() == objectName && item->isVisible())
                    found << item;
                for (QQuickItem *child : item->childItems())
                    search(child);
            };
            search(window->contentItem());
            return found;
        };
        const auto tile = [&](const QString &name) -> QQuickItem * {
            for (QQuickItem *item : named("iconPackTile")) {
                QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(item);
                if (accessible && accessible->text(QAccessible::Name).startsWith(name))
                    return item;
            }
            return nullptr;
        };
        QTRY_VERIFY(tile("Follow desktop theme"));
        // Omarchy's own value counts as following it, and the tile names the pack it means.
        QTRY_VERIFY(tile("Follow desktop theme")->property("selected").toBool());
        QVERIFY(QAccessible::queryAccessibleInterface(tile("Follow desktop theme"))->text(QAccessible::Name).contains("Yaru-blue"));
        // Search lands on the pack in use, ready to change it.
        QTRY_VERIFY(window->activeFocusItem() == tile("Follow desktop theme"));
        QVERIFY(named("iconPackSample").size() >= 4);
        QVERIFY(!named("iconImportButton").isEmpty());
        QVERIFY(!named("iconPackSearch").isEmpty());
        QCOMPARE(desk.writes(), 0);

        // Searching narrows the list and keeps what is in use on screen.
        named("iconPackSearch").first()->setProperty("text", "numix");
        QTRY_VERIFY(tile("Numix"));
        QVERIFY(!tile("Papirus"));
        QVERIFY(tile("Follow desktop theme"));
        QTest::qWait(50);  // The keyboard goes back to its pack on the next tick.

        // Choosing confirms through a read back, and focus stays where it was.
        QQuickItem *numix = tile("Numix");
        numix->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(desk.effective(), QString("Numix"));
        QTRY_VERIFY(tile("Numix") && tile("Numix")->property("selected").toBool());
        QTRY_VERIFY(!backend.states()->value(Backend::stateKey("appearance.icons")).toMap().value("busy").toBool());
        QTRY_VERIFY(window->activeFocusItem() == tile("Numix"));
        named("iconPackSearch").first()->setProperty("text", "");

        // A dropped folder goes through the importer, and using it is a separate step.
        addPack(s.dir.path() + "/downloads", "Dropped");
        backend.iconImporter()->choose(QUrl::fromLocalFile(s.dir.path() + "/downloads/Dropped"));
        QTRY_COMPARE(backend.iconImporter()->state(), QString("imported"));
        QCOMPARE(desk.effective(), QString("Numix"));
        QTRY_VERIFY(!named("iconUseImported").isEmpty());
        QMetaObject::invokeMethod(named("iconUseImported").first(), "clicked");
        QTRY_COMPARE(desk.effective(), QString("Dropped"));
        QCOMPARE(backend.iconImporter()->state(), QString("idle"));
        // The button that was pressed has gone, and the keyboard did not go with it.
        QTRY_VERIFY(window->activeFocusItem() == named("iconImportButton").first());

        // The chooser loads and opens, whichever kind this platform provides.
        QMetaObject::invokeMethod(named("iconImportButton").first(), "clicked");
        QTest::qWait(100);
        QAccessible::setActive(false);
    }

    // ---- My Omarchy: profiles -------------------------------------------------------
    void profileAcceptsValidFixtures_data() {
        QTest::addColumn<QString>("path");
        const QDir dir(PROJECT_ROOT "/tests/fixtures/profiles/valid");
        for (const QString &name : dir.entryList({"*.json"}, QDir::Files, QDir::Name))
            QTest::newRow(qPrintable(name)) << dir.filePath(name);
    }
    void profileAcceptsValidFixtures() {
        QFETCH(QString, path);
        const ProfileModel::Parsed parsed = ProfileModel::parse(readAll(path));
        QVERIFY2(parsed.ok, qPrintable(parsed.errors.join("\n")));
        // Canonical bytes read back as the same profile, and write the same bytes again.
        const QByteArray canonical = parsed.profile.serialize();
        const ProfileModel::Parsed again = ProfileModel::parse(canonical);
        QVERIFY(again.ok);
        QCOMPARE(again.profile.serialize(), canonical);
        QCOMPARE(again.profile.digest(), parsed.profile.digest());
    }
    void profileRefusesInvalidFixtures_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        const QHash<QString, QString> expected{
            {"not-json", "not valid JSON"}, {"comments", "not valid JSON"}, {"trailing-comma", "not valid JSON"},
            {"array-root", "one object"}, {"newer-schema", "newer Omarchy Control"}, {"schema-zero", "schemaVersion"},
            {"schema-text", "schemaVersion"}, {"schema-fraction", "schemaVersion"}, {"missing-name", "needs name"},
            {"blank-name", "needs a name"}, {"duplicate-key", "name appears twice"}, {"duplicate-setting", "settings.power.idle.lock appears twice"},
            {"unknown-section", "unknown field, hooks"}, {"source-home-path", "unknown field, home"}, {"bad-mode", "mode must be"},
            {"inherit-with-value", "cannot also have a value"}, {"set-without-value", "needs value"}, {"setting-number-value", "must be text"},
            {"bad-setting-id", "not a setting id"}, {"setting-extra-field", "unknown field, command"}, {"control-character", "no control characters"},
            {"package-both-ways", "Package firefox appears twice"}, {"package-bad-name", "not a package name"}, {"package-bad-source", "source must be"},
            {"package-bad-intent", "intent must be"}, {"package-install-command", "unknown field, install"},
            {"unknown-file-adapter", "zshrc file adapter"}, {"file-path-traversal", "plain file name"}, {"file-hidden-name", "plain file name"},
            {"file-duplicate", "ghostty/config appears twice"}, {"asset-bad-digest", "SHA-256"}, {"asset-oversized", "25 MiB"},
            {"profile-too-large", "100 MiB"}, {"dock-duplicate", "pinned twice"}, {"dock-not-desktop-id", "not an app id"},
            {"shortcut-missing-command", "needs command"}, {"shortcut-duplicate-id", "Shortcut a appears twice"},
            {"window-rule-unknown-action", "not a window rule"}, {"launcher-changes-nothing", "changes nothing"}, {"deeply-nested", "nested too deeply"},
            {"asset-reference-missing", "names a file the profile does not hold"},
            {"theme-source-not-https", "must be an https address"}, {"theme-source-with-password", "must be an https address"},
            {"theme-folder-not-a-name", "not a theme's folder name"}, {"bar-widget-runs-command", "exec option is not one a profile carries"},
            {"bar-widget-not-omarchys", "not one of Omarchy's own widgets"}, {"plugins-not-omarchys", "not one of Omarchy's own plugins"},
            {"bar-option-home-path", "path in a home folder"},
        };
        const QDir dir(PROJECT_ROOT "/tests/fixtures/profiles/invalid");
        const QStringList files = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
        QCOMPARE(files.size(), expected.size());
        for (const QString &name : files) {
            QVERIFY2(expected.contains(name.chopped(5)), qPrintable(name + " has no expected error"));
            QTest::newRow(qPrintable(name)) << dir.filePath(name) << expected.value(name.chopped(5));
        }
    }
    void profileRefusesInvalidFixtures() {
        QFETCH(QString, path);
        QFETCH(QString, expected);
        const ProfileModel::Parsed parsed = ProfileModel::parse(readAll(path));
        QVERIFY(!parsed.ok);
        QVERIFY2(parsed.errors.join("\n").contains(expected), qPrintable(parsed.errors.join("\n")));
    }
    void profileSchemaMatchesTheValidator() {
        // The published schema and the validator describe one contract: the same sections,
        // record fields, and closed sets of values.
        const QJsonObject schema = QJsonDocument::fromJson(readAll(PROJECT_ROOT "/docs/profile.schema.json")).object();
        QVERIFY(!schema.isEmpty());
        QCOMPARE(schema.value("additionalProperties").toBool(true), false);
        const QJsonObject properties = schema.value("properties").toObject();
        const QStringList sections = properties.keys();
        QCOMPARE(QSet<QString>(sections.begin(), sections.end()),
                 (QSet<QString>{"schemaVersion", "name", "source", "settings", "packages", "desktop", "files", "assets"}));
        QCOMPARE(properties.value("schemaVersion").toObject().value("maximum").toInt(), Profile::SchemaVersion);
        const auto enumOf = [](const QJsonValue &value) {
            QStringList out;
            for (const QJsonValue &item : value.toObject().value("enum").toArray())
                out << item.toString();
            return out;
        };
        const QJsonObject file = properties.value("files").toObject().value("items").toObject().value("properties").toObject();
        QCOMPARE(enumOf(file.value("adapter")), ProfileModel::fileAdapters());
        const QJsonObject package = properties.value("packages").toObject().value("items").toObject().value("properties").toObject();
        QCOMPARE(enumOf(package.value("source")), (QStringList{"repository", "aur", "manual"}));
        QCOMPARE(enumOf(package.value("intent")), (QStringList{"present", "absent"}));
        const QStringList desktop = properties.value("desktop").toObject().value("properties").toObject().keys();
        QCOMPARE(QSet<QString>(desktop.begin(), desktop.end()),
                 (QSet<QString>{"dockPins", "launchers", "associations", "shortcuts", "windowRules", "background", "themes", "bar", "plugins"}));
        QCOMPARE(schema.value("$defs").toObject().value("size").toObject().value("maximum").toInteger(), ProfileModel::MaxFileBytes);
        // The bar options a profile carries, and the sections, are one list in both places.
        QStringList options = schema.value("$defs").toObject().value("barSection").toObject().value("items").toObject().value("properties").toObject().keys();
        options.removeAll("id");
        QStringList carried = ProfileModel::barOptionKeys();
        carried.sort();
        QCOMPARE(options, carried);
        const QStringList barSections = properties.value("desktop").toObject().value("properties").toObject().value("bar").toObject().value("properties").toObject()
                                            .value("layout").toObject().value("properties").toObject().keys();
        const QStringList drawn = ProfileModel::barSections();
        QCOMPARE(QSet<QString>(barSections.begin(), barSections.end()), QSet<QString>(drawn.begin(), drawn.end()));
        // Every id the catalog has fits the pattern the schema gives setting ids.
        const QRegularExpression id(properties.value("settings").toObject().value("propertyNames").toObject().value("pattern").toString());
        const Catalog catalog;
        for (const CatalogEntry &entry : catalog.entries())
            if (entry.isEditable())
                QVERIFY2(id.match(entry.id).hasMatch() && ProfileModel::isSettingId(entry.id), qPrintable(entry.id));
    }
    void profileBytesAreCanonical() {
        // The same choices written in any order give the same file, and where they were
        // saved from is not a choice.
        const auto a = ProfileModel::parse(R"({"name":"P","schemaVersion":1,"settings":{"power.idle.lock":{"value":"600","mode":"set"},"appearance.theme":{"mode":"set","value":"Nord"}},
            "packages":[{"name":"zed","source":"repository","intent":"present"},{"name":"btop","source":"repository","intent":"present"}]})");
        const auto b = ProfileModel::parse(R"({"schemaVersion":1,"settings":{"appearance.theme":{"mode":"set","value":"Nord"},"power.idle.lock":{"mode":"set","value":"600"}},"name":"P",
            "source":{"omarchy":"4.5.0-3"},"packages":[{"intent":"present","name":"btop","source":"repository"},{"name":"zed","source":"repository","intent":"present"}]})");
        QVERIFY(a.ok && b.ok);
        QVERIFY(a.profile.sameChoices(b.profile));
        QVERIFY(a.profile.serialize() != b.profile.serialize());
        Profile c = b.profile;
        c.source = QJsonObject();
        QCOMPARE(c.serialize(), a.profile.serialize());
        QVERIFY(a.profile.serialize().indexOf("btop") < a.profile.serialize().indexOf("zed"));
        // Saving refuses what it could not read back, and never follows a link.
        QTemporaryDir dir;
        Profile bad = a.profile;
        bad.settings.insert("not an id", ProfileSetting{"set", "x"});
        QString error;
        QVERIFY(!ProfileModel::save(dir.path(), bad, &error));
        QVERIFY(!QFile::exists(ProfileModel::manifestPath(dir.path())));
        writeFile(dir.path() + "/elsewhere.json", "{}");
        QVERIFY(QFile::link(dir.path() + "/elsewhere.json", ProfileModel::manifestPath(dir.path())));
        QVERIFY(!ProfileModel::save(dir.path(), a.profile, &error));
        QVERIFY(!ProfileModel::load(dir.path()).ok);
        QCOMPARE(readAll(dir.path() + "/elsewhere.json"), QByteArray("{}"));
    }
    void profileMetadataCoversEveryEditableSetting() {
        // Being editable does not make a setting portable: every one is reviewed either way,
        // so a new setting cannot travel, or vanish from a capture, without a decision.
        const Catalog catalog;
        QHash<QString, int> order;
        for (int i = 0; i < catalog.entries().size(); ++i)
            order.insert(catalog.entries()[i].id, i);
        int portable = 0;
        for (const CatalogEntry &entry : catalog.entries()) {
            const ProfileTraits traits = ProfileTraits::of(entry);
            if (!entry.isEditable()) {
                QVERIFY2(entry.profile.isEmpty(), qPrintable(entry.id));
                continue;
            }
            QVERIFY2(QStringList({"portable", "later", "machine", "runtime"}).contains(traits.portability), qPrintable(entry.id));
            QVERIFY2(!traits.category.isEmpty(), qPrintable(entry.id));
            if (!traits.portable()) {
                QVERIFY2(!traits.reason.isEmpty(), qPrintable(entry.id + " needs a reason"));
                QVERIFY2(!traits.hasStock && !traits.inherit, qPrintable(entry.id));
                continue;
            }
            ++portable;
            QVERIFY2(ProfileTraits::categories().contains(traits.category), qPrintable(entry.id));
            QVERIFY2(entry.target.isEmpty(), qPrintable(entry.id + " belongs to one device"));
            // "Use the default" only where returning to it is reliable.
            const QString kind = entry.write.value("kind").toString();
            if (traits.inherit)
                QVERIFY2(kind == "json" || kind == "hypr" || !entry.reset.isEmpty(), qPrintable(entry.id));
            for (const QString &before : traits.after) {
                QVERIFY2(order.contains(before), qPrintable(entry.id + " is after an unknown " + before));
                QVERIFY2(order.value(before) < order.value(entry.id), qPrintable(entry.id + " comes before " + before));
            }
            for (const ProfileTraits::Overlap &overlap : traits.overlaps)
                QVERIFY2(catalog.find(overlap.setting), qPrintable(entry.id));
            for (const QString &key : traits.activation)
                QVERIFY2(ProfileTraits::describeActivation(key) != key, qPrintable(entry.id + ": " + key));
        }
        QCOMPARE(portable, 41);
    }
    void profileRoundTripsBetweenMachines() {
        QTemporaryDir store;
        const QString dir = store.path() + "/my-omarchy";
        // Machine A: someone's own choices, made the usual ways.
        {
            ProfileMachine a;
            AppCore core(a.s.env);
            core.capabilities.scanServicesBlocking();
            QCOMPARE(applySetting(core.engine, "appearance.theme", "Catppuccin").phase, QString("committed"));
            QCOMPARE(applySetting(core.engine, "power.idle.lock", "600").phase, QString("committed"));
            QCOMPARE(applySetting(core.engine, "input.repeat_rate", "31").phase, QString("committed"));
            QCOMPARE(applySetting(core.engine, "apps.default.browser", "firefox").phase, QString("committed"));
            ProfileCapture::Selection selection;
            selection.settings = {"appearance.theme", "power.idle.lock", "power.idle.screensaver", "input.repeat_rate", "apps.default.browser"};
            const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
            QVERIFY2(result.ok, qPrintable(result.error));
            QVERIFY(result.changed);
            QString error;
            QVERIFY2(core.profileCapture.save(dir, result, &error), qPrintable(error));
            const ProfileModel::Parsed saved = ProfileModel::load(dir);
            QVERIFY2(saved.ok, qPrintable(saved.errors.join("\n")));
            QCOMPARE(saved.profile.settings.size(), 5);
            QCOMPARE(saved.profile.settings.value("appearance.theme"), (ProfileSetting{"set", "Catppuccin"}));
            QCOMPARE(saved.profile.settings.value("power.idle.lock"), (ProfileSetting{"set", "600"}));
            QCOMPARE(saved.profile.settings.value("power.idle.screensaver"), (ProfileSetting{"set", "150"}));
            QCOMPARE(saved.profile.settings.value("input.repeat_rate"), (ProfileSetting{"set", "31"}));
            QCOMPARE(saved.profile.settings.value("apps.default.browser"), (ProfileSetting{"set", "firefox"}));
            QCOMPARE(saved.profile.source.value("hyprland").toString(), QString("0.56.2"));
            // Nothing of this computer's home folder travels in it.
            QVERIFY(!readAll(ProfileModel::manifestPath(dir)).contains(a.s.env.home.toUtf8()));
            // Capturing the same choices again writes nothing at all.
            const QByteArray bytes = readAll(ProfileModel::manifestPath(dir));
            const ProfileCapture::Result again = captureProfile(core, &saved.profile, {});
            QVERIFY(again.ok);
            QVERIFY(!again.changed);
            QVERIFY(core.profileCapture.save(dir, again, &error));
            QCOMPARE(readAll(ProfileModel::manifestPath(dir)), bytes);
        }

        // Machine B: a fresh installation, with another home folder.
        ProfileMachine b;
        QVERIFY(b.s.dir.path() != store.path());
        const QByteArray shellJson = readAll(b.config());
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, dir);
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        QCOMPARE(planned(plan, "appearance.theme").status, QString("ready"));
        QCOMPARE(planned(plan, "power.idle.lock").status, QString("ready"));
        QCOMPARE(planned(plan, "power.idle.screensaver").status, QString("unchanged"));
        QCOMPARE(planned(plan, "input.repeat_rate").status, QString("ready"));
        QVERIFY(planned(plan, "input.repeat_rate").needsOwnership);
        // Nothing shows whether this computer's browser was chosen, so it is a decision.
        QCOMPARE(planned(plan, "apps.default.browser").status, QString("conflict"));
        QVERIFY(!planned(plan, "apps.default.browser").willRun());
        // Planning changed nothing.
        QCOMPARE(readAll(b.config()), shellJson);
        QVERIFY(!QFile::exists(b.state("theme")));

        ProfilePlanner::Options options;
        options.useSaved = {"apps.default.browser"};
        const ProfilePlan decided = planProfile(core, dir, options);
        QVERIFY(planned(decided, "apps.default.browser").willRun());
        QCOMPARE(decided.leftOut(), 0);
        // Restoring follows the reviewed order: the theme before anything else.
        QCOMPARE(decided.items.first().target, QString("appearance.theme"));
        QString error;
        QVERIFY(core.profilePlanner.save(decided, &error));
        const ProfilePlan reloaded = core.profilePlanner.load(decided.id, &error);
        QCOMPARE(reloaded.items.size(), decided.items.size());

        const ProfileRun run = runPlan(core, reloaded);
        QCOMPARE(run.state, QString("completed"));
        QCOMPARE(run.count("verified"), 4);
        QCOMPARE(readSetting(core.engine, "appearance.theme").value, QString("Catppuccin"));
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("600"));
        QCOMPARE(readSetting(core.engine, "input.repeat_rate").value, QString("31"));
        QCOMPARE(readSetting(core.engine, "apps.default.browser").value, QString("firefox"));
        // Only the restored value changed; the user's own key and the rest stay as they were.
        QCOMPARE(readAll(b.config()), QByteArray(shellJson).replace("\"lock\": 300", "\"lock\": 600"));
        QVERIFY(readAll(b.s.env.configHome + "/hypr/hyprland.lua").contains("hypr.mine"));
        // Each change went through the journal, tied to this restore, and can be undone alone.
        QCOMPARE(changesBy(core.coordinator, run.id), 4);
        for (const Transaction &tx : core.coordinator.history())
            if (tx.runId == run.id)
                QVERIFY2(tx.undoable() || tx.recovery == "best-effort", qPrintable(tx.settingId));
        QVERIFY(!core.coordinator.leaseHeldElsewhere());
        QVERIFY(core.coordinator.leaseRun().isEmpty());

        // A second restore of the same profile does nothing at all.
        const ProfilePlan second = planProfile(core, dir);
        QCOMPARE(second.count("ready") + second.count("conflict"), 0);
        QCOMPARE(second.count("unchanged"), 5);
        const int journal = core.coordinator.history().size();
        const ProfileRun noop = runPlan(core, second);
        QCOMPARE(noop.state, QString("completed"));
        QVERIFY(noop.items.isEmpty());
        QCOMPARE(core.coordinator.history().size(), journal);
        QCOMPARE(QString::fromUtf8(readAll(b.state("theme-sets"))).count('\n'), 1);
    }
    void profileCaptureListsWhatItCannotCarry() {
        ProfileMachine m;
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        ProfileCapture::Selection selection;
        selection.categories = {"input"};
        selection.settings = {"displays.scale", "no.such.setting", "appearance.background"};
        const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
        QVERIFY2(result.ok, qPrintable(result.error));
        QHash<QString, ProfileCapture::Item> items;
        for (const ProfileCapture::Item &item : result.items)
            items.insert(item.id, item);
        // Shown with a reason, never dropped quietly: the device toggles and dictation.
        QCOMPARE(items.value("input.touchpad").change, QString("excluded"));
        QVERIFY(items.value("input.touchpad").reason.contains("touchpad"));
        QCOMPARE(items.value("input.dictation.mode").change, QString("excluded"));
        QCOMPARE(items.value("displays.scale").change, QString("excluded"));
        QCOMPARE(items.value("appearance.background").change, QString("excluded"));
        QCOMPARE(items.value("no.such.setting").change, QString("excluded"));
        QCOMPARE(items.value("input.repeat_rate").change, QString("added"));
        QVERIFY(!result.profile.settings.contains("input.touchpad"));
        // Touchpad choices cannot be read without a touchpad, so they are shown, not saved.
        QCOMPARE(items.value("input.tp_natural").change, QString("excluded"));
        QVERIFY(items.value("input.tp_natural").reason.contains("touchpad"));
        QCOMPARE(result.profile.settings.size(), 7);
        // A category that does not exist is refused, not guessed at.
        selection = {};
        selection.categories = {"everything"};
        QVERIFY(!captureProfile(core, nullptr, selection).ok);
        // So is a first capture that finds nothing to remember.
        selection = {};
        selection.settings = {"displays.scale"};
        QVERIFY(!captureProfile(core, nullptr, selection).ok);
        // "Use the default" only where that is reliable.
        selection = {};
        selection.inherit = {"appearance.theme", "power.idle.lock"};
        const ProfileCapture::Result inherit = captureProfile(core, nullptr, selection);
        QVERIFY(inherit.ok);
        QCOMPARE(inherit.profile.settings.value("power.idle.lock"), (ProfileSetting{"inherit", QString()}));
        QVERIFY(!inherit.profile.settings.contains("appearance.theme"));
    }
    void profileSuggestionsFollowProvenance() {
        ProfileMachine m;
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        // Stock everywhere: nothing to suggest but what nothing can tell about.
        ProfileCapture::Selection selection;
        selection.suggested = true;
        const ProfileCapture::Result stock = captureProfile(core, nullptr, selection);
        QVERIFY(!stock.profile.settings.contains("appearance.theme"));
        QVERIFY(!stock.profile.settings.contains("power.idle.lock"));
        QVERIFY(!stock.profile.settings.contains("input.repeat_rate"));
        QVERIFY(!stock.profile.settings.contains("apps.default.browser"));
        QCOMPARE(applySetting(core.engine, "appearance.theme", "Everforest").phase, QString("committed"));
        QCOMPARE(applySetting(core.engine, "power.idle.lock", "900").phase, QString("committed"));
        QCOMPARE(applySetting(core.engine, "input.repeat_rate", "35").phase, QString("committed"));
        const ProfileCapture::Result changed = captureProfile(core, nullptr, selection);
        QVERIFY(changed.ok);
        QCOMPARE(changed.profile.settings.value("appearance.theme").value, QString("Everforest"));
        QCOMPARE(changed.profile.settings.value("power.idle.lock").value, QString("900"));
        QCOMPARE(changed.profile.settings.value("input.repeat_rate").value, QString("35"));
        QVERIFY(!changed.profile.settings.contains("power.idle.screensaver"));
    }
    void profileCaptureKeepsWhatItCannotRead() {
        ProfileMachine m;
        QTemporaryDir store;
        writeProfile(store.path(), {{"appearance.theme", setTo("Catppuccin")}, {"power.idle.lock", setTo("600")}, {"future.setting", setTo("x")}});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        writeFile(m.state("theme-unreadable"), "");
        const ProfileModel::Parsed saved = ProfileModel::load(store.path());
        QVERIFY(saved.ok);
        const ProfileCapture::Result result = captureProfile(core, &saved.profile, {});
        QVERIFY(result.ok);
        // A failed read never turns into forgetting the choice, or into a different one.
        QCOMPARE(result.profile.settings.value("appearance.theme"), (ProfileSetting{"set", "Catppuccin"}));
        QCOMPARE(result.profile.settings.value("future.setting"), (ProfileSetting{"set", "x"}));
        QCOMPARE(result.profile.settings.value("power.idle.lock"), (ProfileSetting{"set", "300"}));
        bool attention = false;
        for (const ProfileCapture::Item &item : result.items) {
            if (item.id == "appearance.theme") {
                QCOMPARE(item.change, QString("kept"));
                attention = item.attention;
            }
            if (item.id == "power.idle.lock")
                QCOMPARE(item.change, QString("modified"));
        }
        QVERIFY(attention);
        // Forgetting is explicit, and only then does a choice leave the profile.
        ProfileCapture::Selection forget;
        forget.forget = {"appearance.theme"};
        const ProfileCapture::Result forgotten = captureProfile(core, &saved.profile, forget);
        QVERIFY(!forgotten.profile.settings.contains("appearance.theme"));
    }
    void profilePlanFindsLocalEdits() {
        ProfileMachine m;
        QTemporaryDir store;
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        // No record of an agreed value, and this computer has its own: a decision.
        QCOMPARE(applySetting(core.engine, "power.idle.lock", "900").phase, QString("committed"));
        writeProfile(store.path(), {{"power.idle.lock", setTo("600")}});
        ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "power.idle.lock").status, QString("conflict"));
        QVERIFY(!planned(plan, "power.idle.lock").willRun());
        ProfilePlanner::Options keep;
        keep.keepLocal = {"all"};
        QCOMPARE(planned(planProfile(core, store.path(), keep), "power.idle.lock").resolution, QString("keep-local"));
        ProfilePlanner::Options use;
        use.useSaved = {"power.idle.lock"};
        plan = planProfile(core, store.path(), use);
        QVERIFY(planned(plan, "power.idle.lock").willRun());
        QCOMPARE(runPlan(core, plan).state, QString("completed"));
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("600"));

        // Restored, so this computer agrees. A new saved value is safe to restore...
        writeProfile(store.path(), {{"power.idle.lock", setTo("1200")}});
        QCOMPARE(planned(planProfile(core, store.path()), "power.idle.lock").status, QString("ready"));
        // ...unless the value was changed here since, outside this app.
        writeFile(m.config(), QByteArray(readAll(m.config())).replace("600", "1800"));
        plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "power.idle.lock").status, QString("conflict"));
        QVERIFY(planned(plan, "power.idle.lock").reason.contains("Changed on this computer"));
    }
    void profileApplyRefusesAStalePlan() {
        ProfileMachine m;
        QTemporaryDir store;
        writeProfile(store.path(), {{"power.idle.lock", setTo("600")}, {"appearance.theme", setTo("Catppuccin")}});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "power.idle.lock").status, QString("ready"));
        // Edited after the review: the plan no longer describes this computer.
        writeFile(m.config(), QByteArray(readAll(m.config())).replace("300", "450"));
        const QByteArray edited = readAll(m.config());
        const ProfileRun refused = runPlan(core, plan);
        QVERIFY(refused.state.isEmpty());
        QVERIFY2(refused.message.contains("Lock after"), qPrintable(refused.message));
        QCOMPARE(readAll(m.config()), edited);
        QVERIFY(!QFile::exists(m.state("theme")));
        QVERIFY(core.coordinator.leaseRun().isEmpty());
        QVERIFY(!core.coordinator.leaseHeldElsewhere());

        // So is a plan whose profile changed after it was made.
        const ProfilePlan fresh = planProfile(core, store.path());
        writeProfile(store.path(), {{"power.idle.lock", setTo("60")}});
        const ProfileRun changed = runPlan(core, fresh);
        QVERIFY(changed.state.isEmpty());
        QVERIFY(changed.message.contains("profile changed"));
        QCOMPARE(readAll(m.config()), edited);
    }
    void profileRefusesABrokenProfileBeforeAnything() {
        ProfileMachine m;
        QTemporaryDir store;
        writeFile(ProfileModel::manifestPath(store.path()), R"({"schemaVersion": 7, "name": "From the future"})");
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY(!plan.valid);
        QVERIFY(plan.items.isEmpty());
        QVERIFY(plan.errors.join(" ").contains("newer"));
        const ProfileRun run = runPlan(core, plan);
        QVERIFY(run.state.isEmpty());
        QCOMPARE(core.coordinator.history().size(), 0);
        // A capture never overwrites a profile it cannot read.
        QCOMPARE(Cli::run(m.s.env, {"profile", "capture", store.path(), "--category", "idle"}), 1);
        QVERIFY(readAll(ProfileModel::manifestPath(store.path())).contains("From the future"));
    }
    void profilePlanHoldsBackMaskedAndUnsupportedItems() {
        ProfileMachine m;
        QTemporaryDir store;
        const QJsonObject settings{{"appearance.gaps", setTo("false")},
                                   {"appearance.gaps_in", setTo("12")},
                                   {"appearance.background", setTo("/home/someone/Pictures/x.png")},
                                   {"displays.scale", setTo("2")},
                                   {"future.setting", setTo("x")},
                                   {"appearance.theme", setTo("Not Installed Here")},
                                   {"input.repeat_rate", QJsonObject{{"mode", "inherit"}}}};
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Mixed"}, {"settings", settings},
                                            {"packages", QJsonArray{QJsonObject{{"name", "firefox"}, {"source", "repository"}, {"intent", "present"}}}}})
                      .toJson());
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY(plan.valid);
        QCOMPARE(planned(plan, "appearance.gaps").status, QString("ready"));
        QCOMPARE(planned(plan, "appearance.gaps_in").status, QString("blocked"));
        QVERIFY(planned(plan, "appearance.gaps_in").reason.contains("Window gaps"));
        QCOMPARE(planned(plan, "appearance.background").status, QString("manual"));
        QCOMPARE(planned(plan, "displays.scale").status, QString("manual"));
        QCOMPARE(planned(plan, "future.setting").status, QString("manual"));
        QCOMPARE(planned(plan, "appearance.theme").status, QString("blocked"));
        QCOMPARE(planned(plan, "input.repeat_rate").status, QString("unchanged"));
        // This machine has no pacman to ask, so the app is held back, with the reason.
        QCOMPARE(planned(plan, "firefox").status, QString("blocked"));
        QVERIFY(planned(plan, "firefox").reason.contains("could not be read"));
        QCOMPARE(plan.leftOut(), 6);
        // A restore that leaves things out says so, and is never called complete.
        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("partial"));
        QCOMPARE(run.leftOut, 6);
        QVERIFY(QFile::exists(m.s.env.stateHome + "/omarchy/toggles/hypr/window-no-gaps.lua"));
        QVERIFY(!readAll(m.s.env.configHome + "/hypr/omarchy-control.lua").contains("gaps_in"));
    }
    void profileInheritRemovesAnOverride() {
        ProfileMachine m;
        QTemporaryDir store;
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        QCOMPARE(applySetting(core.engine, "input.repeat_rate", "31").phase, QString("committed"));
        // Saved here as this computer's default, after being set: the capture is the agreement.
        ProfileCapture::Selection selection;
        selection.settings = {"input.repeat_rate"};
        QVERIFY(saveProfile(core, store.path(), selection));
        writeProfile(store.path(), {{"input.repeat_rate", QJsonObject{{"mode", "inherit"}}}});
        const ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "input.repeat_rate").status, QString("ready"));
        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("completed"));
        QCOMPARE(readSetting(core.engine, "input.repeat_rate").value, QString("40"));
        QVERIFY(!readSetting(core.engine, "input.repeat_rate").isOverride);
        QCOMPARE(planned(planProfile(core, store.path()), "input.repeat_rate").status, QString("unchanged"));
    }
    void profileRunResumesAfterACrash() {
        ProfileMachine m;
        QTemporaryDir store;
        writeProfile(store.path(), {{"power.idle.lock", setTo("600")}, {"power.idle.screensaver", setTo("60")}, {"appearance.theme", setTo("Catppuccin")}});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(plan.count("ready"), 3);

        // The record a run leaves when its process dies partway: the theme landed but was
        // not yet confirmed, the lock never started, and the screensaver was underway and
        // then changed by someone else.
        ProfileRun run;
        run.id = "run-0123456789ab";
        run.planId = plan.id;
        run.directory = plan.directory;
        run.profileName = plan.profileName;
        run.profileDigest = plan.profileDigest;
        run.state = "running";
        run.startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        for (const ProfilePlanItem &item : plan.items) {
            ProfileRunItem step;
            step.id = item.id;
            step.kind = item.kind;
            step.target = item.target;
            step.label = item.label;
            step.mode = item.mode;
            step.desired = item.desired;
            step.planned = item.current;
            step.state = item.target == "power.idle.lock" ? "pending" : "running";
            run.items << step;
        }
        writeFile(ProfileStore::runsDir(m.s.env) + "/" + run.id + ".json", QJsonDocument(run.toJson()).toJson());
        writeFile(m.state("theme"), "Catppuccin\n");
        writeFile(m.config(), QByteArray(readAll(m.config())).replace("150", "90"));
        Transaction underway;
        underway.id = "crashed00001";
        underway.settingId = "appearance.theme";
        underway.label = "Theme";
        underway.phase = "applying";
        underway.runId = run.id;
        writeFile(core.coordinator.journalPath(), QJsonDocument(underway.toJson()).toJson(QJsonDocument::Compact) + "\n");

        const ProfileRun resumed = resumeRun(core, run.id);
        QCOMPARE(resumed.state, QString("incomplete"));
        QHash<QString, ProfileRunItem> items;
        for (const ProfileRunItem &item : resumed.items)
            items.insert(item.target, item);
        // Found done by reading it, so it is not run again.
        QCOMPARE(items.value("appearance.theme").state, QString("verified"));
        QVERIFY(!QFile::exists(m.state("theme-sets")));
        // Never started, still as planned: done now.
        QCOMPARE(items.value("power.idle.lock").state, QString("verified"));
        QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("600"));
        // Neither planned nor wanted: someone else's value, left for a person to check.
        QCOMPARE(items.value("power.idle.screensaver").state, QString("interrupted"));
        QCOMPARE(readSetting(core.engine, "power.idle.screensaver").value, QString("90"));
        // The coordinator settled its own record of the interrupted change too.
        QCOMPARE(core.coordinator.transaction("crashed00001").phase, QString("interrupted"));
        QCOMPARE(core.coordinator.transaction("crashed00001").runId, run.id);
        // A finished run has nothing left to resume.
        QVERIFY(resumeRun(core, run.id).state.isEmpty());
        QVERIFY(resumeRun(core, "../../etc/passwd").message.contains("not a restore id"));
    }
    void profileCancellationCrossesRunnerInstancesAndStopsBetweenOperations() {
        ProfileMachine m;
        QTemporaryDir store;
        writeProfile(store.path(), {{"power.idle.lock", setTo("600")}, {"power.idle.screensaver", setTo("60")}});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const auto plan = planProfile(core, store.path());
        QCOMPARE(plan.count("ready"), 2);
        bool requested = false, finished = false; ProfileRun result;
        const auto connection = connect(&core.profileRunner, &ProfileRunner::itemChanged, this, [&](const ProfileRun &run, int index) {
            if (requested || run.items[index].state != QLatin1String("running")) return;
            const auto dry = CliApi::execute(m.s.env, {"profile", "cancel", run.id, "--dry-run"});
            QVERIFY(dry.body.value("ok").toBool());
            QVERIFY(!QFile::exists(ProfileStore::runsDir(m.s.env) + "/" + run.id + ".cancel"));
            const auto cancel = CliApi::execute(m.s.env, {"profile", "cancel", run.id});
            QVERIFY(cancel.body.value("ok").toBool());
            QCOMPARE(cancel.body.value("outcome").toString(), QString("cancellation_requested"));
            QVERIFY(!cancel.body.value("verified").toBool());
            requested = true;
        });
        core.profileRunner.apply(plan, [&](const ProfileRun &run) { result = run; finished = true; });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 15000);
        disconnect(connection);
        QVERIFY(requested);
        QCOMPARE(result.state, QString("cancelled"));
        int verified = 0, pending = 0;
        for (const auto &item : result.items) { verified += item.state == "verified"; pending += item.state == "pending"; }
        QCOMPARE(verified, 1); QCOMPARE(pending, 1);
        QVERIFY(!CliApi::execute(m.s.env, {"profile", "cancel", result.id}).body.value("ok").toBool());
        QCOMPARE(resumeRun(core, result.id).state, QString("completed"));
        QVERIFY(!QFile::exists(ProfileStore::runsDir(m.s.env) + "/" + result.id + ".cancel"));
        QVERIFY(!CliApi::execute(m.s.env, {"profile", "cancel", "../invalid"}).body.value("ok").toBool());
    }
    void profileLeaseExcludesOtherChanges() {
        ProfileMachine m;
        QTemporaryDir store;
        writeProfile(store.path(), {{"power.idle.lock", setTo("600")}});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        {
            // Another process holds the lease: a restore is running there.
            QLockFile other(core.coordinator.leasePath());
            QVERIFY(other.tryLock(0));
            QVERIFY(core.coordinator.leaseHeldElsewhere());
            const Transaction refused = applySetting(core.engine, "power.idle.screensaver", "60");
            QCOMPARE(refused.phase, QString("failed"));
            QVERIFY(refused.message.contains("restore is running"));
            QCOMPARE(Cli::run(m.s.env, {"apply", "power.idle.screensaver", "60"}), 1);
            // Nor can a second restore start.
            const ProfileRun second = runPlan(core, plan);
            QVERIFY(second.state.isEmpty());
            QVERIFY(second.message.contains("Another restore"));
            QCOMPARE(readSetting(core.engine, "power.idle.lock").value, QString("300"));
        }
        QVERIFY(!core.coordinator.leaseHeldElsewhere());
        QCOMPARE(applySetting(core.engine, "power.idle.screensaver", "60").phase, QString("committed"));

        // While this process holds it, only the run's own changes go ahead.
        QVERIFY(core.coordinator.acquireLease("run-aaaaaaaaaaaa"));
        QCOMPARE(applySetting(core.engine, "power.idle.screensaver", "120").phase, QString("failed"));
        Transaction own;
        bool done = false;
        core.engine.apply("power.idle.screensaver", "120", [&](const Transaction &tx) { own = tx; done = true; }, "set", "run-aaaaaaaaaaaa");
        QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
        QCOMPARE(own.phase, QString("committed"));
        QCOMPARE(own.runId, QString("run-aaaaaaaaaaaa"));
        core.coordinator.releaseLease();
        // A change claiming a run that holds no lease is refused too.
        done = false;
        core.engine.apply("power.idle.screensaver", "150", [&](const Transaction &tx) { own = tx; done = true; }, "set", "run-aaaaaaaaaaaa");
        QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
        QCOMPARE(own.phase, QString("failed"));
    }
    void profileCliRoundTrip() {
        ProfileMachine a;
        QTemporaryDir store;
        const QString dir = store.path() + "/profile";
        QCOMPARE(Cli::run(a.s.env, {"apply", "power.idle.lock", "600"}), 0);
        // A first capture needs a choice of what to remember.
        QCOMPARE(Cli::run(a.s.env, {"profile", "capture", dir}), 1);
        QVERIFY(!QFile::exists(ProfileModel::manifestPath(dir)));
        QCOMPARE(Cli::run(a.s.env, {"profile", "capture", dir, "--setting", "power.idle.lock", "--dry-run"}), 0);
        QVERIFY(!QFile::exists(ProfileModel::manifestPath(dir)));
        QCOMPARE(Cli::run(a.s.env, {"profile", "capture", dir, "--category", "idle", "--name", "Laptop"}), 0);
        QVERIFY(ProfileModel::load(dir).ok);
        QCOMPARE(ProfileModel::load(dir).profile.name, QString("Laptop"));
        QCOMPARE(Cli::run(a.s.env, {"profile", "inspect", dir, "--json"}), 0);

        ProfileMachine b;
        QCOMPARE(Cli::run(b.s.env, {"profile", "plan", dir}), 0);
        const QStringList plans = QDir(ProfileStore::plansDir(b.s.env)).entryList({"plan-*.json"}, QDir::Files);
        QCOMPARE(plans.size(), 1);
        const QString planId = plans.first().chopped(5);
        // Nothing happens without --yes, and nothing changed by planning.
        QCOMPARE(Cli::run(b.s.env, {"profile", "apply", planId}), 1);
        QVERIFY(readAll(b.config()).contains("\"lock\": 300"));
        QCOMPARE(Cli::run(b.s.env, {"profile", "apply", planId, "--yes"}), 0);
        QVERIFY(readAll(b.config()).contains("\"lock\": 600"));
        QCOMPARE(Cli::run(b.s.env, {"profile", "status", "--json"}), 0);
        QCOMPARE(Cli::run(b.s.env, {"profile", "apply", "plan-nothere00000", "--yes"}), 1);
        QCOMPARE(Cli::run(b.s.env, {"profile", "resume", "not-a-run"}), 1);
        QCOMPARE(Cli::run(b.s.env, {"profile", "frobnicate"}), 1);
        QCOMPARE(Cli::run(b.s.env, {"profile"}), 0);
    }

    // ---- My Omarchy: apps -----------------------------------------------------------
    void packageInventoryReadsPacmanWithoutChangingAnything() {
        PackageMachine m;
        AppCore core(m.s.env);
        PackageProvider::Inventory inventory;
        bool done = false;
        core.packages.readInventory([&](const PackageProvider::Inventory &read) { inventory = read; done = true; });
        QVERIFY(QTest::qWaitFor([&] { return done; }, 15000));
        QVERIFY2(inventory.ok, qPrintable(inventory.error));
        QVERIFY(inventory.installed.value("chromium").explicitly);
        QCOMPARE(inventory.installed.value("chromium").repository, QString("extra"));
        QCOMPARE(inventory.installed.value("omarchy").repository, QString("omarchy"));
        QVERIFY(!inventory.installed.value("hyprland").explicitly);
        QVERIFY(inventory.installed.value("my-local-tool").foreign);
        QVERIFY(inventory.installed.value("my-local-tool").repository.isEmpty());
        QVERIFY(inventory.hasBaseline);
        QVERIFY(inventory.baseline.contains("libreoffice-fresh"));

        PackageProvider::Preview preview;
        const auto wait = [&](const std::function<void(std::function<void(const PackageProvider::Preview &)>)> &start) {
            done = false;
            start([&](const PackageProvider::Preview &result) { preview = result; done = true; });
            return QTest::qWaitFor([&] { return done; }, 15000);
        };
        QVERIFY(wait([&](auto callback) { core.packages.previewInstall({"firefox"}, callback); }));
        QVERIFY(preview.ok);
        QStringList brings = preview.packages;
        brings.sort();
        QCOMPARE(brings, (QStringList{"firefox 140.0-1 extra", "libvpx 1.15-1 extra", "nss 3.110-1 extra"}));
        QVERIFY(wait([&](auto callback) { core.packages.previewInstall({"no-such-app"}, callback); }));
        QVERIFY(!preview.ok);
        QCOMPARE(preview.missing, QStringList{"no-such-app"});
        QVERIFY(wait([&](auto callback) { core.packages.previewRemoval({"hyprland"}, callback); }));
        QVERIFY(!preview.ok);
        QCOMPARE(preview.requiredBy, QStringList{"omarchy"});
        QVERIFY(wait([&](auto callback) { core.packages.previewRemoval({"libreoffice-fresh"}, callback); }));
        QVERIFY(preview.ok);
        QCOMPARE(preview.packages, QStringList{"libreoffice-fresh 25.2-1"});

        QVERIFY(PackageProvider::isProtected("omarchy-control"));
        QVERIFY(PackageProvider::isProtected("linux-firmware-intel"));
        QVERIFY(!PackageProvider::isProtected("firefox"));
        QVERIFY(PackageProvider::isSystemPackage("nvidia-open-dkms"));
        QVERIFY(PackageProvider::isSystemPackage("intel-ucode"));
        QVERIFY(!PackageProvider::isSystemPackage("ripgrep"));
        // Reading all of that ran no transaction and asked sudo for nothing.
        QVERIFY(m.transactions().isEmpty());
        QVERIFY(!m.s.calls().join('\n').contains("sudo"));
    }
    void profileCapturesApps() {
        PackageMachine m;
        QTemporaryDir store;
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        ProfileCapture::Selection selection;
        selection.apps = {"btop", "ripgrep", "my-local-tool", "linux-firmware", "--force", "thermald"};
        selection.excludeApps = {"libreoffice-fresh", "hyprland", "obsidian"};
        selection.suggestedApps = true;
        selection.settings = {"apps.default.browser"};
        const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
        QVERIFY2(result.ok, qPrintable(result.error));
        QHash<QString, ProfileCapture::Item> items;
        for (const ProfileCapture::Item &item : result.items)
            if (item.kind == "package")
                items.insert(item.label, item);
        const Profile &profile = result.profile;
        QCOMPARE(profile.package("btop"), (ProfilePackage{"btop", "repository", "present", "extra"}));
        QCOMPARE(items.value("ripgrep").change, QString("excluded"));
        // In no repository: kept in view, for a person to install.
        QCOMPARE(profile.package("my-local-tool").source, QString("manual"));
        QCOMPARE(items.value("linux-firmware").change, QString("excluded"));
        QCOMPARE(items.value("thermald").change, QString("excluded"));
        QVERIFY(items.value("thermald").reason.contains("hardware"));
        QCOMPARE(items.value("--force").change, QString("excluded"));
        QCOMPARE(profile.package("libreoffice-fresh").intent, QString("absent"));
        QVERIFY(items.value("libreoffice-fresh").reason.contains("Still installed"));
        QCOMPARE(items.value("hyprland").change, QString("excluded"));
        QCOMPARE(profile.package("obsidian").intent, QString("absent"));
        // The default browser brings the app it opens.
        QCOMPARE(profile.settings.value("apps.default.browser").value, QString("chromium"));
        QCOMPARE(profile.package("chromium").intent, QString("present"));
        QVERIFY(items.value("chromium").reason.contains("Web browser"));
        // Omarchy's defaults are not suggested; one missing here is shown, never recorded.
        QVERIFY(!profile.hasPackage("mpv"));
        QVERIFY(!profile.hasPackage("omarchy"));
        QCOMPARE(items.value("typora").change, QString("suggested"));
        QVERIFY(!profile.hasPackage("typora"));
        // vi is there in another package's name, so it is not missing.
        QVERIFY(!items.contains("vi"));
        // What Omarchy installed for this hardware is never an app choice, asked for or not.
        QVERIFY(!profile.hasPackage("thermald"));
        // An app someone chose that Omarchy does not ship is suggested.
        QCOMPARE(profile.package("ex-vi-compat").intent, QString("present"));
        QString error;
        QVERIFY2(core.profileCapture.save(store.path(), result, &error), qPrintable(error));

        // Gone from this computer later: still remembered, and flagged.
        writeFile(m.pacman("installed"), QByteArray(readAll(m.pacman("installed"))).replace("btop 1.4.7-1 explicit -\n", ""));
        const ProfileModel::Parsed saved = ProfileModel::load(store.path());
        QVERIFY(saved.ok);
        const ProfileCapture::Result again = captureProfile(core, &saved.profile, {});
        QVERIFY(again.ok);
        QCOMPARE(again.profile.package("btop").intent, QString("present"));
        bool flagged = false;
        for (const ProfileCapture::Item &item : again.items)
            if (item.label == "btop")
                flagged = item.change == "kept" && item.attention;
        QVERIFY(flagged);
        // An app the profile's own default needs cannot also be unwanted.
        ProfileCapture::Selection exclude;
        exclude.excludeApps = {"chromium"};
        const ProfileCapture::Result refused = captureProfile(core, &saved.profile, exclude);
        QCOMPARE(refused.profile.package("chromium").intent, QString("present"));
    }
    void profileRestoresApps() {
        // The P2 gate: an app and a command-line tool installed, a default switched to the new
        // app, an unwanted default removed after it, and everything else left as it was.
        PackageMachine m;
        QTemporaryDir store;
        writeAppProfile(store.path(), {{"apps.default.browser", setTo("firefox")}},
                        {app("firefox"), app("ripgrep"), app("btop"), app("libreoffice-fresh", "absent")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        core.profileRunner.setPackageTerminal(true);
        ProfilePlanner::Options options;
        options.useSaved = {"apps.default.browser"};
        const ProfilePlan plan = planProfile(core, store.path(), options);
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        QCOMPARE(planned(plan, "firefox").action, QString("install"));
        QCOMPARE(planned(plan, "firefox").status, QString("ready"));
        QCOMPARE(planned(plan, "firefox").transaction.size(), 3);
        QCOMPARE(planned(plan, "ripgrep").status, QString("ready"));
        QCOMPARE(planned(plan, "btop").status, QString("unchanged"));
        QCOMPARE(planned(plan, "libreoffice-fresh").action, QString("remove"));
        QCOMPARE(planned(plan, "libreoffice-fresh").status, QString("ready"));
        QVERIFY(planned(plan, "libreoffice-fresh").reason.contains("Omarchy installs"));
        QCOMPARE(planned(plan, "apps.default.browser").needs, QStringList{"package:firefox"});
        QVERIFY(planned(plan, "apps.default.browser").willRun());
        QCOMPARE(plan.leftOut(), 0);
        // Apps first, then the settings that use them, then removals.
        QStringList order;
        for (const ProfilePlanItem &item : plan.items)
            order << item.target;
        QVERIFY(order.indexOf("firefox") < order.indexOf("apps.default.browser"));
        QVERIFY(order.indexOf("apps.default.browser") < order.indexOf("libreoffice-fresh"));
        // Planning installed nothing.
        QVERIFY(m.transactions().isEmpty());

        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("completed"));
        QCOMPARE(run.count("verified"), 4);
        QCOMPARE(m.reason("firefox"), QString("explicit"));
        QCOMPARE(m.reason("nss"), QString("dependency"));
        QCOMPARE(m.reason("ripgrep"), QString("explicit"));
        QVERIFY(!m.has("libreoffice-fresh"));
        QCOMPARE(readSetting(core.engine, "apps.default.browser").value, QString("firefox"));
        // Nothing else moved.
        for (const char *untouched : {"chromium", "btop", "mpv", "libmpv", "my-local-tool", "omarchy", "hyprland"})
            QVERIFY2(m.has(untouched), untouched);
        // One transaction for both installs, one for the removal, each journaled under the run.
        QCOMPARE(m.transactions(), (QStringList{"pacman -S --needed -- firefox ripgrep", "pacman -R -- libreoffice-fresh"}));
        QCOMPARE(changesBy(core.coordinator, run.id), 3);
        for (const Transaction &tx : core.coordinator.history())
            if (tx.runId == run.id && tx.settingId == "packages")
                QVERIFY(!tx.undoable());

        // Again, with nothing left to do: no transaction, no change.
        const ProfilePlan second = planProfile(core, store.path());
        QCOMPARE(second.count("ready") + second.count("conflict"), 0);
        QCOMPARE(runPlan(core, second).state, QString("completed"));
        QCOMPARE(m.transactions().size(), 2);
    }
    void profileRefusesUnsafeAppRemovals() {
        PackageMachine m;
        QTemporaryDir store;
        writeAppProfile(store.path(), {},
                        {app("hyprland", "absent"), app("thermald", "absent"), app("libmpv", "absent"), app("obsidian", "absent"), app("my-local-tool", "absent", "manual"),
                         app("no-such-app"), app("some-aur-app", "present", "aur"), app("my-build", "present", "manual"), app("libvpx")});
        writeFile(m.pacman("installed"), QByteArray(readAll(m.pacman("installed"))) + "libvpx 1.15-1 dependency -\n");
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY(plan.valid);
        QCOMPARE(planned(plan, "hyprland").status, QString("blocked"));
        QVERIFY(planned(plan, "hyprland").reason.contains("never removes"));
        QCOMPARE(planned(plan, "thermald").status, QString("blocked"));
        QVERIFY(planned(plan, "thermald").reason.contains("hardware"));
        QCOMPARE(planned(plan, "libmpv").status, QString("blocked"));
        QVERIFY(planned(plan, "libmpv").reason.contains("Needed by mpv"));
        QCOMPARE(planned(plan, "obsidian").status, QString("unchanged"));
        // Not one of Omarchy's, so possibly installed here on purpose: a decision.
        QCOMPARE(planned(plan, "my-local-tool").status, QString("conflict"));
        QCOMPARE(planned(plan, "no-such-app").status, QString("blocked"));
        QVERIFY(planned(plan, "no-such-app").reason.contains("No configured repository"));
        QCOMPARE(planned(plan, "some-aur-app").status, QString("blocked"));
        QVERIFY(planned(plan, "some-aur-app").reason.contains("AUR"));
        QCOMPARE(planned(plan, "my-build").status, QString("manual"));
        // Already installed as a dependency: it only becomes wanted.
        QCOMPARE(planned(plan, "libvpx").action, QString("mark-explicit"));

        // A default app whose own app the profile removes is refused outright.
        writeAppProfile(store.path(), {{"apps.default.browser", setTo("chromium")}}, {app("chromium", "absent")});
        const ProfilePlan contradiction = planProfile(core, store.path());
        QVERIFY(!contradiction.valid);
        QVERIFY(contradiction.errors.join(" ").contains("also to remove chromium"));
    }
    void profileAppFailuresAreReportedAsTheyAre() {
        PackageMachine m;
        QTemporaryDir store;
        writeAppProfile(store.path(), {{"apps.default.browser", setTo("firefox")}}, {app("firefox")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        core.profileRunner.setPackageTerminal(true);
        ProfilePlanner::Options options;
        options.useSaved = {"all"};

        // A refused password changes nothing, and what needed the app does not happen.
        writeFile(m.pacman("deny-sudo"), "");
        ProfileRun run = runPlan(core, planProfile(core, store.path(), options));
        QCOMPARE(run.state, QString("incomplete"));
        QHash<QString, ProfileRunItem> items;
        for (const ProfileRunItem &item : run.items)
            items.insert(item.target, item);
        QCOMPARE(items.value("firefox").state, QString("failed"));
        QVERIFY2(items.value("firefox").message.contains("did not finish"), qPrintable(items.value("firefox").message));
        QCOMPARE(items.value("apps.default.browser").state, QString("blocked"));
        QVERIFY(items.value("apps.default.browser").message.contains("firefox"));
        QVERIFY(!m.has("firefox"));
        QCOMPARE(readSetting(core.engine, "apps.default.browser").value, QString("chromium"));
        QFile::remove(m.pacman("deny-sudo"));

        // A transaction pacman fails is a failure, whatever came before.
        writeFile(m.pacman("fail-install"), "");
        run = runPlan(core, planProfile(core, store.path(), options));
        QCOMPARE(run.state, QString("incomplete"));
        QVERIFY(!m.has("firefox"));
        QFile::remove(m.pacman("fail-install"));

        // What pacman would do changed after the review: nothing starts.
        const ProfilePlan reviewed = planProfile(core, store.path(), options);
        writeFile(m.pacman("repo"), QByteArray(readAll(m.pacman("repo"))).replace("firefox 140.0-1 nss,libvpx", "firefox 141.0-1 nss,libvpx"));
        const int before = m.transactions().size();
        run = runPlan(core, reviewed);
        QCOMPARE(run.state, QString("incomplete"));
        for (const ProfileRunItem &item : run.items)
            if (item.target == "firefox")
                QVERIFY2(item.message.contains("changed since the plan"), qPrintable(item.message));
        QCOMPARE(m.transactions().size(), before);
        QVERIFY(!m.has("firefox"));
    }
    void profilePausesWhilePacmanIsBusy() {
        PackageMachine m;
        QTemporaryDir store;
        writeAppProfile(store.path(), {}, {app("ripgrep")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        core.profileRunner.setPackageTerminal(true);
        core.profileRunner.setPacmanWait(0);
        const QString lock = m.s.env.sys("/var/lib/pacman/db.lck");
        writeFile(lock, "");
        const ProfileRun paused = runPlan(core, planProfile(core, store.path()));
        QCOMPARE(paused.state, QString("paused"));
        QVERIFY(paused.resumable());
        QCOMPARE(paused.items.first().state, QString("pending"));
        // pacman's lock is waited for, never removed.
        QVERIFY(QFile::exists(lock));
        QVERIFY(m.transactions().isEmpty());
        QVERIFY(!core.coordinator.leaseHeldElsewhere());
        QVERIFY(core.coordinator.leaseRun().isEmpty());
        QFile::remove(lock);
        const ProfileRun resumed = resumeRun(core, paused.id);
        QCOMPARE(resumed.state, QString("completed"));
        QCOMPARE(m.reason("ripgrep"), QString("explicit"));
    }
    void profileResumesAfterAnInterruptedPackageWorker() {
        PackageMachine m;
        QTemporaryDir store;
        writeAppProfile(store.path(), {}, {app("firefox"), app("ripgrep")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        core.profileRunner.setPackageTerminal(true);
        const ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(plan.count("ready"), 2);
        // The run died while pacman was working: the record says both were underway, and
        // pacman had installed one of them.
        ProfileRun run;
        run.id = "run-00000000beef";
        run.planId = plan.id;
        run.directory = plan.directory;
        run.profileName = plan.profileName;
        run.profileDigest = plan.profileDigest;
        run.state = "running";
        for (const ProfilePlanItem &item : plan.items) {
            ProfileRunItem step;
            step.id = item.id;
            step.kind = item.kind;
            step.target = item.target;
            step.label = item.label;
            step.mode = item.mode;
            step.desired = item.desired;
            step.action = item.action;
            step.packages = item.transaction;
            step.planned = item.currentStatus;
            step.state = "running";
            run.items << step;
        }
        writeFile(ProfileStore::runsDir(m.s.env) + "/" + run.id + ".json", QJsonDocument(run.toJson()).toJson());
        writeFile(m.pacman("installed"), QByteArray(readAll(m.pacman("installed"))) + "nss 3.110-1 dependency -\nlibvpx 1.15-1 dependency -\nfirefox 140.0-1 explicit nss,libvpx\n");

        const ProfileRun resumed = resumeRun(core, run.id);
        QCOMPARE(resumed.state, QString("completed"));
        QHash<QString, ProfileRunItem> items;
        for (const ProfileRunItem &item : resumed.items)
            items.insert(item.target, item);
        QCOMPARE(items.value("firefox").state, QString("verified"));
        QVERIFY(items.value("firefox").message.contains("already done"));
        QCOMPARE(items.value("ripgrep").state, QString("verified"));
        // Only what had not happened ran again.
        QCOMPARE(m.transactions(), QStringList{"pacman -S --needed -- ripgrep"});
    }
    void profileCliRestoresApps() {
        PackageMachine a;
        QTemporaryDir store;
        const QString dir = store.path() + "/profile";
        QCOMPARE(Cli::run(a.s.env, {"profile", "capture", dir, "--app", "btop", "--app", "mpv", "--exclude-app", "libreoffice-fresh"}), 0);
        const ProfileModel::Parsed saved = ProfileModel::load(dir);
        QVERIFY(saved.ok);
        QCOMPARE(saved.profile.packages.size(), 3);
        QVERIFY(a.transactions().isEmpty());

        // Another computer, where btop is missing and libreoffice-fresh is still there.
        PackageMachine b;
        writeFile(b.pacman("installed"), QByteArray(readAll(b.pacman("installed"))).replace("btop 1.4.7-1 explicit -\n", ""));
        QCOMPARE(Cli::run(b.s.env, {"profile", "plan", dir}), 0);
        const QStringList plans = QDir(ProfileStore::plansDir(b.s.env)).entryList({"plan-*.json"}, QDir::Files);
        QCOMPARE(plans.size(), 1);
        QCOMPARE(Cli::run(b.s.env, {"profile", "apply", plans.first().chopped(5)}), 1);
        QVERIFY(b.transactions().isEmpty());
        QCOMPARE(Cli::run(b.s.env, {"profile", "apply", plans.first().chopped(5), "--yes"}), 0);
        QCOMPARE(b.transactions(), (QStringList{"pacman -S --needed -- btop", "pacman -R -- libreoffice-fresh"}));
        QCOMPARE(b.reason("btop"), QString("explicit"));
        QVERIFY(!b.has("libreoffice-fresh"));
    }
    void profileResolvesAurAppsOnlyWhenAsked() {
        PackageMachine m;
        m.s.command("curl", kFakeAurCurl);
        writeFile(m.pacman("aur"), "my-local-tool 1.0-1 -\n");
        writeFile(m.pacman("installed"), QByteArray(readAll(m.pacman("installed"))) + "another-build 2.0-1 explicit -\n");
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        ProfileCapture::Selection selection;
        selection.apps = {"my-local-tool", "another-build"};
        // Without being asked, nothing reaches the network, and neither is guessed at.
        const ProfileCapture::Result offline = captureProfile(core, nullptr, selection);
        QCOMPARE(offline.profile.package("my-local-tool").source, QString("manual"));
        QVERIFY(!QFile::exists(m.pacman("curl-calls")));
        selection.resolveAur = true;
        const ProfileCapture::Result online = captureProfile(core, nullptr, selection);
        QVERIFY(online.ok);
        QCOMPARE(online.profile.package("my-local-tool").source, QString("aur"));
        QCOMPARE(online.profile.package("another-build").source, QString("manual"));
        // One request, holding only the names asked about.
        QCOMPARE(QString::fromUtf8(readAll(m.pacman("curl-calls"))).trimmed(),
                 QString("https://aur.archlinux.org/rpc/v5/info?arg%5B%5D=another-build&arg%5B%5D=my-local-tool"));
        // The AUR unreachable: nothing is assumed, and the capture says so.
        writeFile(m.pacman("aur-down"), "");
        const ProfileCapture::Result down = captureProfile(core, nullptr, selection);
        QVERIFY(down.ok);
        QCOMPARE(down.profile.package("my-local-tool").source, QString("manual"));
        bool said = false;
        for (const ProfileCapture::Item &item : down.items)
            said = said || item.reason.contains("could not be asked");
        QVERIFY(said);
        QFile::remove(m.pacman("aur-down"));
        // A default app's own app is known from the catalog, with no lookup at all.
        writeFile(m.state("browser"), "chrome\n");
        writeFile(m.pacman("installed"), QByteArray(readAll(m.pacman("installed"))) + "google-chrome 142.0-1 explicit -\n");
        const int calls = QString::fromUtf8(readAll(m.pacman("curl-calls"))).count('\n');
        ProfileCapture::Selection browser;
        browser.settings = {"apps.default.browser"};
        const ProfileCapture::Result known = captureProfile(core, nullptr, browser);
        QCOMPARE(known.profile.package("google-chrome").source, QString("aur"));
        QCOMPARE(QString::fromUtf8(readAll(m.pacman("curl-calls"))).count('\n'), calls);
    }
    void profileBuildsAppsFromTheAur() {
        PackageMachine m;
        m.s.command("yay", kFakeYay);
        writeFile(m.pacman("aur"), "google-chrome 142.0-1 google-chrome-stable\n");
        QTemporaryDir store;
        writeAppProfile(store.path(), {{"apps.default.browser", setTo("chrome")}}, {app("google-chrome", "present", "aur"), app("ripgrep")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        core.profileRunner.setPackageTerminal(true);
        ProfilePlanner::Options options;
        options.useSaved = {"all"};
        const ProfilePlan plan = planProfile(core, store.path(), options);
        QCOMPARE(planned(plan, "google-chrome").action, QString("build"));
        QCOMPARE(planned(plan, "google-chrome").status, QString("ready"));
        QVERIFY(planned(plan, "google-chrome").reason.contains("review"));
        QCOMPARE(planned(plan, "apps.default.browser").needs, QStringList{"package:google-chrome"});
        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("completed"));
        QCOMPARE(m.reason("google-chrome"), QString("explicit"));
        QCOMPARE(readSetting(core.engine, "apps.default.browser").value, QString("chrome"));
        // Built by yay with its review offered, never with a saved answer standing in for it.
        QVERIFY(m.transactions().contains("yay -S --needed --aur --diffmenu --noanswerdiff -- google-chrome"));
        QVERIFY(m.transactions().contains("pacman -S --needed -- ripgrep"));
        for (const Transaction &tx : core.coordinator.history())
            if (tx.runId == run.id && tx.label.contains("AUR"))
                QVERIFY(!tx.undoable());
    }
    void profileFailedAurBuildBlocksOnlyWhatNeedsIt() {
        PackageMachine m;
        m.s.command("yay", kFakeYay);
        writeFile(m.pacman("aur"), "google-chrome 142.0-1 google-chrome-stable\n");
        writeFile(m.pacman("fail-build"), "");
        QTemporaryDir store;
        writeAppProfile(store.path(), {{"apps.default.browser", setTo("chrome")}, {"power.idle.lock", setTo("600")}},
                        {app("google-chrome", "present", "aur"), app("ripgrep")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        core.profileRunner.setPackageTerminal(true);
        ProfilePlanner::Options options;
        options.useSaved = {"all"};
        const ProfileRun run = runPlan(core, planProfile(core, store.path(), options));
        QCOMPARE(run.state, QString("incomplete"));
        QHash<QString, ProfileRunItem> items;
        for (const ProfileRunItem &item : run.items)
            items.insert(item.target, item);
        QCOMPARE(items.value("google-chrome").state, QString("failed"));
        QCOMPARE(items.value("apps.default.browser").state, QString("blocked"));
        // Work that did not depend on the build still happened, and was verified.
        QCOMPARE(items.value("ripgrep").state, QString("verified"));
        QCOMPARE(items.value("power.idle.lock").state, QString("verified"));
        QVERIFY(!m.has("google-chrome"));
        QCOMPARE(readSetting(core.engine, "apps.default.browser").value, QString("chromium"));

        // Without yay there is nothing to build with, and the plan says so.
        QFile::remove(m.s.bin("yay"));
        const ProfilePlan plan = planProfile(core, store.path(), options);
        QCOMPARE(planned(plan, "google-chrome").status, QString("blocked"));
        QVERIFY(planned(plan, "google-chrome").reason.contains("needs yay"));
    }
    void profileWithoutATerminalLeavesAppsAlone() {
        PackageMachine m;
        QTemporaryDir store;
        writeAppProfile(store.path(), {{"power.idle.lock", setTo("600")}}, {app("ripgrep")});
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfileRun run = runPlan(core, planProfile(core, store.path()));
        QCOMPARE(run.state, QString("incomplete"));
        for (const ProfileRunItem &item : run.items) {
            if (item.target == "ripgrep") {
                QCOMPARE(item.state, QString("blocked"));
                QVERIFY(item.message.contains("terminal"));
            } else {
                QCOMPARE(item.state, QString("verified"));
            }
        }
        QVERIFY(m.transactions().isEmpty());
        QVERIFY(!m.s.calls().join('\n').contains("sudo"));
    }

    // ---- My Omarchy: desktop choices ------------------------------------------------
    void profileCapturesDesktopChoices() {
        DesktopMachine a;
        AppCore core(a.s.env);
        core.capabilities.scanServicesBlocking();
        writeFile(a.state("pins"), "firefox\norg.gnome.Nautilus\n");
        a.setMime("text/plain", "org.example.TextEditor.desktop");
        LauncherEditor::Edit rename;
        rename.name = "Web";
        rename.hidden = true;
        QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("org.example.Browser.desktop", rename, done); }).phase, QString("committed"));
        HyprModule module(a.s.env);
        QVERIFY(module.load());
        module.setBinds(QJsonArray{QJsonObject{{"keys", "SUPER + ALT + N"}, {"description", "Notes"}, {"command", "uwsm-app -- obsidian"}}});
        module.setRules(QJsonArray{QJsonObject{{"match", "org.gnome.Calculator"}, {"action", "float"}, {"value", ""}}});
        QVERIFY(module.save());

        ProfileCapture::Selection selection;
        selection.dock = selection.launchers = selection.associations = selection.shortcuts = selection.windowRules = true;
        const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
        QVERIFY2(result.ok, qPrintable(result.error));
        const QJsonObject desktop = result.profile.desktop;
        QCOMPARE(desktop.value("dockPins").toArray(), (QJsonArray{"firefox", "org.gnome.Nautilus"}));
        // Only what was changed here, nothing the package set.
        QCOMPARE(desktop.value("launchers").toObject().value("org.example.Browser.desktop").toObject(),
                 (QJsonObject{{"name", "Web"}, {"hidden", true}}));
        QVERIFY(!desktop.value("launchers").toObject().contains("org.example.TextEditor.desktop"));
        QCOMPARE(desktop.value("associations").toObject().value("text/plain").toString(), QString("org.example.TextEditor.desktop"));
        QCOMPARE(desktop.value("shortcuts").toArray().first().toObject().value("id").toString(), QString("super-alt-n"));
        QCOMPARE(desktop.value("shortcuts").toArray().first().toObject().value("command").toString(), QString("uwsm-app -- obsidian"));
        QCOMPARE(desktop.value("windowRules").toArray().first().toObject().value("id").toString(), QString("float-org-gnome-calculator"));
        // Saved, and read back exactly.
        QTemporaryDir store;
        QString error;
        QVERIFY2(core.profileCapture.save(store.path(), result, &error), qPrintable(error));
        const ProfileModel::Parsed saved = ProfileModel::load(store.path());
        QVERIFY2(saved.ok, qPrintable(saved.errors.join("\n")));
        // A change undone here leaves the profile on the next save, and an unreadable
        // association stays as saved.
        QCOMPARE(launcherChange([&](auto done) { core.launchers.reset("org.example.Browser.desktop", done); }).phase, QString("committed"));
        QFile::remove(a.state("mime-text_plain"));
        const ProfileCapture::Result again = captureProfile(core, &saved.profile, {});
        QVERIFY(again.ok);
        QVERIFY(!again.profile.desktop.contains("launchers"));
        QCOMPARE(again.profile.desktop.value("associations").toObject().value("text/plain").toString(), QString("org.example.TextEditor.desktop"));
    }
    void profileRestoresDesktopChoices() {
        // The P3 gate's desktop part: on another computer, the dock, a launcher, a file
        // association, and an owned shortcut and rule, each verified, then nothing to do.
        QTemporaryDir store;
        {
            DesktopMachine a;
            AppCore core(a.s.env);
            core.capabilities.scanServicesBlocking();
            writeFile(a.state("pins"), "firefox\norg.gnome.Nautilus\n");
            a.setMime("text/plain", "org.example.TextEditor.desktop");
            LauncherEditor::Edit rename;
            rename.name = "Web";
            QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("org.example.Browser.desktop", rename, done); }).phase, QString("committed"));
            HyprModule module(a.s.env);
            QVERIFY(module.load());
            module.setBinds(QJsonArray{QJsonObject{{"keys", "SUPER + ALT + N"}, {"description", "Notes"}, {"command", "uwsm-app -- obsidian"}}});
            module.setRules(QJsonArray{QJsonObject{{"match", "org.gnome.Calculator"}, {"action", "float"}, {"value", ""}}});
            QVERIFY(module.save());
            ProfileCapture::Selection selection;
            selection.dock = selection.launchers = selection.associations = selection.shortcuts = selection.windowRules = true;
            QVERIFY(saveProfile(core, store.path(), selection));
        }
        DesktopMachine b;
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        for (const char *target : {"dock", "launcher:org.example.Browser.desktop", "association:text/plain", "shortcut:super-alt-n", "rule:float-org-gnome-calculator"})
            QVERIFY2(planned(plan, target).status == "ready", qPrintable(QString(target) + ": " + planned(plan, target).status + " " + planned(plan, target).reason));
        // A restored shortcut runs a command, so the review says which.
        QVERIFY(planned(plan, "shortcut:super-alt-n").effects.join(" ").contains("uwsm-app -- obsidian"));
        QVERIFY(planned(plan, "shortcut:super-alt-n").needsOwnership);
        QCOMPARE(b.pins(), QString());
        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("completed"));
        QCOMPARE(b.pins(), QString("firefox\norg.gnome.Nautilus"));
        QCOMPARE(core.launchers.find("org.example.Browser.desktop").name, QString("Web"));
        QCOMPARE(b.mime("text/plain"), QString("org.example.TextEditor.desktop"));
        HyprModule module(b.s.env);
        QVERIFY(module.load());
        QCOMPARE(module.binds().size(), 1);
        QCOMPARE(module.rules().size(), 1);
        QVERIFY(module.importInstalled());
        QVERIFY(readAll(b.s.env.configHome + "/hypr/hyprland.lua").contains("hypr.mine"));
        // Each was one journaled change under the run.
        QCOMPARE(changesBy(core.coordinator, run.id), 5);
        const ProfilePlan second = planProfile(core, store.path());
        QCOMPARE(second.count("ready") + second.count("conflict"), 0);
        QCOMPARE(second.count("unchanged"), 5);
    }
    void profileDesktopConflictsAndBlocks() {
        DesktopMachine b;
        QTemporaryDir store;
        const QJsonObject desktop{
            {"dockPins", QJsonArray{"firefox"}},
            {"launchers", QJsonObject{{"org.example.Missing.desktop", QJsonObject{{"name", "Gone"}}},
                                      {"org.example.Browser.desktop", QJsonObject{{"icon", "sha256:" + QString(64, 'a')}}}}},
            {"shortcuts", QJsonArray{QJsonObject{{"id", "super-return"}, {"keys", "SUPER + RETURN"}, {"description", "Mine"}, {"command", "foot"}}}},
        };
        // The icon is one of the profile's files, recorded but not in its folder.
        const QJsonArray assets{QJsonObject{{"sha256", QString(64, 'a')}, {"size", 10}, {"kind", "icon"}, {"name", "web.png"}}};
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Desktop"}, {"desktop", desktop}, {"assets", assets}}).toJson());
        writeFile(b.state("pins"), "my-own-app\n");
        writeFile(b.s.env.home + "/.fake-binds", "64 RETURN Terminal\n");
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        ProfilePlan plan = planProfile(core, store.path());
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        // A dock someone made here is theirs until they choose otherwise.
        QCOMPARE(planned(plan, "dock").status, QString("conflict"));
        QCOMPARE(planned(plan, "launcher:org.example.Missing.desktop").status, QString("blocked"));
        QCOMPARE(planned(plan, "launcher:org.example.Browser.desktop").status, QString("blocked"));
        QVERIFY(planned(plan, "launcher:org.example.Browser.desktop").reason.contains("missing"));
        // Keys that already do something are only taken over by a decision.
        QCOMPARE(planned(plan, "shortcut:super-return").status, QString("conflict"));
        QVERIFY(planned(plan, "shortcut:super-return").reason.contains("Terminal"));

        ProfilePlanner::Options options;
        options.useSaved = {"shortcut:super-return"};
        plan = planProfile(core, store.path(), options);
        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("partial"));
        HyprModule module(b.s.env);
        QVERIFY(module.load());
        QVERIFY(module.binds().first().toObject().value("replaces").toBool());
        QVERIFY(readAll(module.path()).contains("hl.unbind(\"SUPER + RETURN\")"));
        QCOMPARE(b.pins(), QString("my-own-app"));

        // A module someone edited by hand is never written over.
        writeFile(module.path(), QByteArray(readAll(module.path())) + "-- my own line\n");
        // Its records can no longer be told apart from the edit, so even the one restored
        // above is held back, with the reason.
        const ProfilePlan edited = planProfile(core, store.path(), options);
        QCOMPARE(planned(edited, "shortcut:super-return").status, QString("blocked"));
        QVERIFY(planned(edited, "shortcut:super-return").reason.contains("edited by hand"));
        QJsonObject other{{"shortcuts", QJsonArray{QJsonObject{{"id", "super-alt-q"}, {"keys", "SUPER + ALT + Q"}, {"command", "true"}}}}};
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Desktop"}, {"desktop", other}}).toJson());
        const ProfilePlan handEdited = planProfile(core, store.path());
        QCOMPARE(planned(handEdited, "shortcut:super-alt-q").status, QString("blocked"));
        QVERIFY(planned(handEdited, "shortcut:super-alt-q").reason.contains("edited by hand"));
    }

    void profileCarriesBackgroundsAndIconFiles() {
        // Files go with the profile, by digest, and arrive where their adapter puts them,
        // under another user's home folder.
        QTemporaryDir store;
        QString backgroundDigest;
        {
            DesktopMachine a;
            useTheme(a.s, "tokyo-night");
            const QString mine = pngFile(a.s.env.configHome + "/omarchy/backgrounds/tokyo-night/mine.png", Qt::magenta);
            QFile::remove(a.s.env.stateHome + "/omarchy/current/background");
            QVERIFY(QFile::link(mine, a.s.env.stateHome + "/omarchy/current/background"));
            AppCore core(a.s.env);
            core.capabilities.scanServicesBlocking();
            QString error;
            const QString icon = core.launchers.importIcon(pngFile(a.s.dir.path() + "/downloads/web.png", Qt::yellow), &error);
            QVERIFY2(!icon.isEmpty(), qPrintable(error));
            LauncherEditor::Edit edit;
            edit.icon = icon;
            QCOMPARE(launcherChange([&](auto done) { core.launchers.apply("org.example.Browser.desktop", edit, done); }).phase, QString("committed"));
            ProfileCapture::Selection selection;
            selection.background = selection.launchers = true;
            const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
            QVERIFY2(result.ok, qPrintable(result.error));
            QVERIFY2(core.profileCapture.save(store.path(), result, &error), qPrintable(error));
            const ProfileModel::Parsed saved = ProfileModel::load(store.path());
            QVERIFY2(saved.ok, qPrintable(saved.errors.join("\n")));
            const QJsonObject background = saved.profile.desktop.value("background").toObject();
            QCOMPARE(background.value("theme").toString(), QString("tokyo-night"));
            QCOMPARE(background.value("name").toString(), QString("mine.png"));
            backgroundDigest = background.value("sha256").toString();
            QVERIFY(QFile::exists(ProfileFiles::assetPath(store.path(), backgroundDigest)));
            const QString iconRef = saved.profile.desktop.value("launchers").toObject().value("org.example.Browser.desktop").toObject().value("icon").toString();
            QVERIFY(iconRef.startsWith("sha256:"));
            QVERIFY(QFile::exists(ProfileFiles::assetPath(store.path(), iconRef.mid(7))));
            QCOMPARE(saved.profile.assets.size(), 2);
            // No path of this home folder is in the profile.
            QVERIFY(!readAll(ProfileModel::manifestPath(store.path())).contains(a.s.env.home.toUtf8()));
        }
        DesktopMachine b;
        useTheme(b.s, "tokyo-night");
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        QCOMPARE(planned(plan, "background").status, QString("ready"));
        QCOMPARE(planned(plan, "launcher:org.example.Browser.desktop").status, QString("ready"));
        QCOMPARE(runPlan(core, plan).state, QString("completed"));
        const QString placed = b.s.env.configHome + "/omarchy/backgrounds/tokyo-night/mine.png";
        QVERIFY(QFile::exists(placed));
        QCOMPARE(QFileInfo(b.s.env.stateHome + "/omarchy/current/background").canonicalFilePath(), QFileInfo(placed).canonicalFilePath());
        const QString restoredIcon = core.launchers.find("org.example.Browser.desktop").icon;
        QVERIFY2(restoredIcon.startsWith(core.launchers.iconFolder()), qPrintable(restoredIcon));
        const ProfilePlan second = planProfile(core, store.path());
        QCOMPARE(planned(second, "background").status, QString("unchanged"));
        QCOMPARE(planned(second, "launcher:org.example.Browser.desktop").status, QString("unchanged"));

        // A file that does not match its digest is never used, and neither is a missing one.
        DesktopMachine c;
        useTheme(c.s, "tokyo-night");
        AppCore other(c.s.env);
        other.capabilities.scanServicesBlocking();
        writeFile(ProfileFiles::assetPath(store.path(), backgroundDigest), "not the picture");
        ProfilePlan tampered = planProfile(other, store.path());
        QCOMPARE(planned(tampered, "background").status, QString("blocked"));
        QVERIFY(planned(tampered, "background").reason.contains("size") || planned(tampered, "background").reason.contains("digest"));
        QFile::remove(ProfileFiles::assetPath(store.path(), backgroundDigest));
        tampered = planProfile(other, store.path());
        QCOMPARE(planned(tampered, "background").status, QString("blocked"));
        QVERIFY(planned(tampered, "background").reason.contains("missing"));
        QVERIFY(!QFile::exists(c.s.env.configHome + "/omarchy/backgrounds/tokyo-night/mine.png"));
    }
    void profileRestoresAThemesOwnBackgroundForItsThemeOnly() {
        DesktopMachine b;
        useTheme(b.s, "catppuccin");
        QTemporaryDir store;
        const QJsonObject desktop{{"background", QJsonObject{{"theme", "tokyo-night"}, {"name", "2.png"}}}};
        writeFile(ProfileModel::manifestPath(store.path()), QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Background"}, {"desktop", desktop}}).toJson());
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        // Another theme in use, and the profile does not change it: the background waits.
        ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "background").status, QString("blocked"));
        QVERIFY(planned(plan, "background").reason.contains("tokyo-night"));
        // With that theme in use, one of its own backgrounds is chosen by name.
        useTheme(b.s, "tokyo-night");
        plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "background").status, QString("ready"));
        QCOMPARE(runPlan(core, plan).state, QString("completed"));
        QCOMPARE(QFileInfo(b.s.env.stateHome + "/omarchy/current/background").fileName(), QString("background"));
        QVERIFY(QFileInfo(b.s.env.stateHome + "/omarchy/current/background").canonicalFilePath().endsWith("/2.png"));
        QCOMPARE(planned(planProfile(core, store.path()), "background").status, QString("unchanged"));
    }

    void profileCarriesTerminalConfigs() {
        QTemporaryDir store;
        {
            ProfileMachine a;
            omarchyTerminalDefaults(a.s);
            writeFile(a.s.env.configHome + "/ghostty/config", kGhosttyMine);
            writeFile(a.s.env.configHome + "/alacritty/alacritty.toml", kAlacrittyMine);
            AppCore core(a.s.env);
            core.capabilities.scanServicesBlocking();
            ProfileCapture::Selection selection;
            selection.configFiles = {"ghostty", "alacritty"};
            selection.settings = {"power.idle.lock"};
            QVERIFY(saveProfile(core, store.path(), selection));
            const ProfileModel::Parsed saved = ProfileModel::load(store.path());
            QVERIFY2(saved.ok, qPrintable(saved.errors.join("\n")));
            QCOMPARE(saved.profile.files.size(), 2);
            QCOMPARE(readAll(ProfileFiles::configPath(store.path(), "ghostty", "config")), QByteArray(kGhosttyMine));
            QCOMPARE(readAll(ProfileFiles::configPath(store.path(), "alacritty", "alacritty.toml")), QByteArray(kAlacrittyMine));
        }
        ProfileMachine b;
        omarchyTerminalDefaults(b.s);
        // Omarchy's own Alacritty file is here; there is no Ghostty file at all.
        writeFile(b.s.env.configHome + "/alacritty/alacritty.toml", kAlacrittyOmarchy);
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        QCOMPARE(planned(plan, "file:ghostty/config").status, QString("ready"));
        QCOMPARE(planned(plan, "file:alacritty/alacritty.toml").status, QString("ready"));
        // Files come before settings, so a font or text size restored with them lands on top.
        QStringList order;
        for (const ProfilePlanItem &item : plan.items)
            order << item.target;
        QVERIFY(order.indexOf("file:ghostty/config") < order.indexOf("power.idle.lock"));
        QCOMPARE(runPlan(core, plan).state, QString("completed"));
        QCOMPARE(readAll(b.s.env.configHome + "/ghostty/config"), QByteArray(kGhosttyMine));
        QCOMPARE(readAll(b.s.env.configHome + "/alacritty/alacritty.toml"), QByteArray(kAlacrittyMine));
        const ProfilePlan second = planProfile(core, store.path());
        QCOMPARE(planned(second, "file:ghostty/config").status, QString("unchanged"));
        QCOMPARE(planned(second, "file:alacritty/alacritty.toml").status, QString("unchanged"));

        // A computer with its own Ghostty file decides; a damaged or edited copy is refused.
        ProfileMachine c;
        omarchyTerminalDefaults(c.s);
        writeFile(c.s.env.configHome + "/ghostty/config", "font-size = 14\n");
        AppCore other(c.s.env);
        other.capabilities.scanServicesBlocking();
        QCOMPARE(planned(planProfile(other, store.path()), "file:ghostty/config").status, QString("conflict"));
        writeFile(ProfileFiles::configPath(store.path(), "ghostty", "config"), QByteArray(kGhosttyMine).replace("12", "13"));
        const ProfilePlan damaged = planProfile(other, store.path());
        QCOMPARE(planned(damaged, "file:ghostty/config").status, QString("blocked"));
        QVERIFY(planned(damaged, "file:ghostty/config").reason.contains("digest"));
        // Edited to run a program, with its digest fixed to match: still refused, by review.
        const QByteArray runs = QByteArray(kGhosttyMine) + "command = /usr/bin/fish\n";
        writeFile(ProfileFiles::configPath(store.path(), "ghostty", "config"), runs);
        QJsonObject manifest = QJsonDocument::fromJson(readAll(ProfileModel::manifestPath(store.path()))).object();
        QJsonArray files = manifest.value("files").toArray();
        for (int i = 0; i < files.size(); ++i) {
            QJsonObject file = files[i].toObject();
            if (file.value("adapter").toString() == "ghostty") {
                file["sha256"] = QString::fromLatin1(QCryptographicHash::hash(runs, QCryptographicHash::Sha256).toHex());
                file["size"] = double(runs.size());
                files[i] = file;
            }
        }
        manifest["files"] = files;
        writeFile(ProfileModel::manifestPath(store.path()), QJsonDocument(manifest).toJson());
        const ProfilePlan edited = planProfile(other, store.path());
        QCOMPARE(planned(edited, "file:ghostty/config").status, QString("blocked"));
        QVERIFY(planned(edited, "file:ghostty/config").reason.contains("command"));
        QCOMPARE(readAll(c.s.env.configHome + "/ghostty/config"), QByteArray("font-size = 14\n"));
    }
    void profileLeavesOutConfigsItCannotCarry_data() {
        QTest::addColumn<QString>("adapter");
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("reason");
        QTest::newRow("ghostty runs a program") << "ghostty" << QByteArray("command = /usr/bin/fish\n") << "command";
        QTest::newRow("ghostty loads another file") << "ghostty" << QByteArray("config-file = ~/.config/ghostty/extra\n") << "loads";
        QTest::newRow("ghostty shader") << "ghostty" << QByteArray("custom-shader = ~/shaders/crt.glsl\n") << "custom-shader";
        QTest::newRow("home path") << "ghostty" << QByteArray("working-directory = /home/someone/code\n") << "home folder";
        QTest::newRow("alacritty shell") << "alacritty" << QByteArray("[terminal.shell]\nprogram = \"zsh\"\n") << "program";
        QTest::newRow("alacritty import") << "alacritty" << QByteArray("general.import = [ \"~/mine.toml\" ]\n") << "imports";
        QTest::newRow("alacritty hint") << "alacritty" << QByteArray("[[hints.enabled]]\ncommand = \"xdg-open\"\n") << "program";
    }
    void profileLeavesOutConfigsItCannotCarry() {
        QFETCH(QString, adapter);
        QFETCH(QByteArray, contents);
        QFETCH(QString, reason);
        ProfileMachine m;
        omarchyTerminalDefaults(m.s);
        ProfileFiles::ConfigAdapter target;
        QVERIFY(ProfileFiles::configAdapter(m.s.env, adapter, &target));
        writeFile(target.destination, contents);
        AppCore core(m.s.env);
        ProfileCapture::Selection selection;
        selection.configFiles = {adapter};
        selection.settings = {"power.idle.lock"};
        const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
        QVERIFY(result.ok);
        QVERIFY(result.profile.files.isEmpty());
        bool said = false;
        for (const ProfileCapture::Item &item : result.items)
            said = said || (item.change == "excluded" && item.reason.contains(reason));
        QVERIFY2(said, qPrintable(reason));
    }

    void profileThemeSourcesAreHttpsOnly() {
        // SSH needs keys a fresh computer lacks, so it travels as https where that is known
        // to be the same repository; a name or password never travels at all.
        QCOMPARE(ProfileDesktop::themeSource("https://github.com/a/omarchy-b-theme.git"), QString("https://github.com/a/omarchy-b-theme.git"));
        QCOMPARE(ProfileDesktop::themeSource("https://me:token@github.com/a/b.git"), QString("https://github.com/a/b.git"));
        QCOMPARE(ProfileDesktop::themeSource("git@github.com:a/b.git"), QString("https://github.com/a/b.git"));
        QCOMPARE(ProfileDesktop::themeSource("ssh://git@Codeberg.org/a/b.git"), QString("https://codeberg.org/a/b.git"));
        QCOMPARE(ProfileDesktop::themeSource("git@git.sr.ht:~a/b"), QString("https://git.sr.ht/~a/b"));
        for (const QString &origin : {"git@example.com:a/b.git", "http://github.com/a/b.git", "ext::sh -c touch% /tmp/x", "/home/me/themes/b",
                                      "file:///srv/b.git", "https://github.com/a/b?x=1", "https://me:p@ss@github.com/a/b", "https://github.com"}) {
            QString problem;
            QVERIFY2(ProfileDesktop::themeSource(origin, &problem).isEmpty(), qPrintable(origin));
            QVERIFY(!problem.isEmpty());
        }
        QVERIFY(ProfileDesktop::sameSource("https://GitHub.com/a/b.git/", "https://github.com/a/b"));
        QVERIFY(!ProfileDesktop::sameSource("https://github.com/a/b", "https://github.com/A/b"));
        QVERIFY(!ProfileDesktop::sameSource("", ""));
        for (const QString &name : {"tokyo-night", "_x+y.z", "9"})
            QVERIFY2(ProfileModel::isThemeFolder(name), qPrintable(name));
        for (const QString &name : {"..", ".hidden", "-x", "a/b", "Tokyo", "", "a b"})
            QVERIFY2(!ProfileModel::isThemeFolder(name), qPrintable(name));
        QCOMPARE(ProfileDesktop::themeName("obsidian-circuit"), QString("Obsidian Circuit"));
    }

    void profileCarriesThemeSources() {
        ProfileMachine a;
        const QString themes = a.s.env.configHome + "/omarchy/themes/";
        const auto gitTheme = [&](const QString &folder, const QString &origin) {
            writeFile(themes + folder + "/.git/config", ("[core]\n\tbare = false\n[remote \"origin\"]\n\turl = " + origin + "\n").toUtf8());
            writeFile(themes + folder + "/colors.toml", "accent = \"#123456\"\n");
        };
        gitTheme("obsidian-circuit", "git@github.com:someone/omarchy-obsidian-circuit-theme.git");
        gitTheme("secret", "https://someone:hunter2@example.com/themes/secret.git");
        gitTheme("private", "git@example.com:someone/private.git");
        writeFile(themes + "hand-made/colors.toml", "accent = \"#654321\"\n");
        writeFile(a.state("theme"), "Obsidian Circuit\n");
        AppCore core(a.s.env);
        core.capabilities.scanServicesBlocking();
        QTemporaryDir store;
        // Remembering the theme brings its source along, unasked: restoring it needs one.
        ProfileCapture::Selection selection;
        selection.settings = {"appearance.theme"};
        QVERIFY(saveProfile(core, store.path(), selection));
        ProfileModel::Parsed saved = ProfileModel::load(store.path());
        QVERIFY2(saved.ok, qPrintable(saved.errors.join("\n")));
        QCOMPARE(saved.profile.settings.value("appearance.theme").value, QString("Obsidian Circuit"));
        QCOMPARE(saved.profile.desktop.value("themes").toObject().keys(), QStringList{"obsidian-circuit"});
        QCOMPARE(saved.profile.desktop.value("themes").toObject().value("obsidian-circuit").toObject().value("url").toString(),
                 QString("https://github.com/someone/omarchy-obsidian-circuit-theme.git"));
        // Every Git theme when asked. What has no address a profile can carry is listed with
        // why, and a password in an address stays on this computer.
        selection = {};
        selection.themes = true;
        const ProfileCapture::Result result = captureProfile(core, &saved.profile, selection);
        QVERIFY2(result.ok, qPrintable(result.error));
        const QJsonObject captured = result.profile.desktop.value("themes").toObject();
        QCOMPARE(captured.keys(), (QStringList{"obsidian-circuit", "secret"}));
        QCOMPARE(captured.value("secret").toObject().value("url").toString(), QString("https://example.com/themes/secret.git"));
        QHash<QString, QString> excluded;
        for (const ProfileCapture::Item &item : result.items)
            if (item.change == "excluded")
                excluded.insert(item.id, item.reason);
        QVERIFY2(excluded.value("desktop:theme:hand-made").contains("not installed from Git"), qPrintable(excluded.value("desktop:theme:hand-made")));
        QVERIFY2(excluded.value("desktop:theme:private").contains("not https"), qPrintable(excluded.value("desktop:theme:private")));
        QString error;
        QVERIFY2(core.profileCapture.save(store.path(), result, &error), qPrintable(error));
        QVERIFY(!readAll(ProfileModel::manifestPath(store.path())).contains("hunter2"));
        // A theme removed here leaves the profile, as a shortcut removed here does.
        QDir(themes + "secret").removeRecursively();
        saved = ProfileModel::load(store.path());
        const ProfileCapture::Result again = captureProfile(core, &saved.profile, {});
        QVERIFY(again.ok);
        QCOMPARE(again.profile.desktop.value("themes").toObject().keys(), QStringList{"obsidian-circuit"});
    }

    void profileInstallsAThemeBeforeSelectingIt() {
        QTemporaryDir store;
        const QString url = "https://github.com/someone/omarchy-obsidian-circuit-theme.git";
        const QJsonObject desktop{{"themes", QJsonObject{{"obsidian-circuit", QJsonObject{{"url", url}}}}},
                                  {"background", QJsonObject{{"theme", "obsidian-circuit"}, {"name", "1.png"}}}};
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Theme"}, {"settings", QJsonObject{{"appearance.theme", setTo("Obsidian Circuit")}}},
                                            {"desktop", desktop}})
                      .toJson());
        ProfileMachine b;
        b.s.command("git", kFakeGit);
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        const ProfilePlan plan = planProfile(core, store.path());
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        const ProfilePlanItem theme = planned(plan, "theme:obsidian-circuit");
        QCOMPARE(theme.status, QString("ready"));
        QVERIFY(theme.effects.join(" ").contains(url));
        // The setting that selects it, and the background that belongs to it, wait for it.
        const ProfilePlanItem setting = planned(plan, "appearance.theme");
        QCOMPARE(setting.status, QString("ready"));
        QCOMPARE(setting.needs, QStringList{"desktop:theme:obsidian-circuit"});
        QVERIFY2(setting.reason.contains("Obsidian Circuit theme"), qPrintable(setting.reason));
        QCOMPARE(planned(plan, "background").status, QString("ready"));
        QCOMPARE(planned(plan, "background").needs, QStringList{"desktop:theme:obsidian-circuit"});
        QStringList order;
        for (const ProfilePlanItem &item : plan.items)
            order << item.target;
        QVERIFY(order.indexOf("theme:obsidian-circuit") < order.indexOf("appearance.theme"));
        QVERIFY(order.indexOf("appearance.theme") < order.indexOf("background"));
        const ProfileRun run = runPlan(core, plan);
        QVERIFY2(run.state == "completed", qPrintable(run.message));
        QVERIFY(QFileInfo(b.s.env.configHome + "/omarchy/themes/obsidian-circuit/colors.toml").isFile());
        // Cloned without a prompt, over https alone, and the address never read as an option.
        QString clone;
        for (const QString &call : b.s.calls())
            if (call.startsWith("git "))
                clone = call;
        QVERIFY2(clone.contains("protocol.allow=never") && clone.endsWith("-- " + url + " " + b.s.env.configHome + "/omarchy/themes/obsidian-circuit"),
                 qPrintable(clone));
        QCOMPARE(readAll(b.state("theme")).trimmed(), QByteArray("Obsidian Circuit"));
        QVERIFY(QFileInfo(b.s.env.stateHome + "/omarchy/current/background").canonicalFilePath().endsWith("/obsidian-circuit/backgrounds/1.png"));
        const ProfilePlan second = planProfile(core, store.path());
        QCOMPARE(planned(second, "theme:obsidian-circuit").status, QString("unchanged"));
        QCOMPARE(planned(second, "appearance.theme").status, QString("unchanged"));
        QCOMPARE(planned(second, "background").status, QString("unchanged"));
    }

    void profileNeverReplacesAThemeAlreadyHere() {
        QTemporaryDir store;
        const QJsonObject themes{{"obsidian-circuit", QJsonObject{{"url", "https://github.com/someone/omarchy-obsidian-circuit-theme.git"}}},
                                 {"tokyo-night", QJsonObject{{"url", "https://github.com/someone/tokyo-night.git"}}}};
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Theme"}, {"settings", QJsonObject{{"appearance.theme", setTo("Obsidian Circuit")}}},
                                            {"desktop", QJsonObject{{"themes", themes}}}})
                      .toJson());
        ProfileMachine b;
        b.s.command("git", kFakeGit);
        const QString folder = b.s.env.configHome + "/omarchy/themes/obsidian-circuit";
        writeFile(folder + "/colors.toml", "accent = \"#000000\"\n");
        QDir().mkpath(b.s.env.omarchyPath + "/themes/tokyo-night");
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        // One made by hand under that name is left alone, and the setting selects it as it is.
        ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "theme:obsidian-circuit").status, QString("blocked"));
        QVERIFY2(planned(plan, "theme:obsidian-circuit").reason.contains("not from Git"), qPrintable(planned(plan, "theme:obsidian-circuit").reason));
        QVERIFY(planned(plan, "appearance.theme").needs.isEmpty());
        QCOMPARE(planned(plan, "appearance.theme").status, QString("ready"));
        // One Omarchy ships under that name is what the setting would select.
        QCOMPARE(planned(plan, "theme:tokyo-night").status, QString("unchanged"));
        QVERIFY(!planned(plan, "theme:tokyo-night").reason.isEmpty());
        // One from somewhere else is named, and left alone too.
        writeFile(folder + "/.git/config", "[remote \"origin\"]\n\turl = https://github.com/other/obsidian.git\n");
        plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "theme:obsidian-circuit").status, QString("blocked"));
        QVERIFY(planned(plan, "theme:obsidian-circuit").reason.contains("https://github.com/other/obsidian.git"));
        // The same source, written another way, is the same theme.
        writeFile(folder + "/.git/config", "[remote \"origin\"]\n\turl = git@github.com:someone/omarchy-obsidian-circuit-theme\n");
        QCOMPARE(planned(planProfile(core, store.path()), "theme:obsidian-circuit").status, QString("unchanged"));
        QCOMPARE(readAll(folder + "/colors.toml"), QByteArray("accent = \"#000000\"\n"));
        QVERIFY(!b.s.calls().join("\n").contains("git "));
    }

    void profileCarriesTheBarLayout() {
        QTemporaryDir store;
        const QStringList widgets{"omarchy.menu", "omarchy.clock", "omarchy.weather", "omarchy.tray"};
        {
            ProfileMachine a;
            omarchyPlugins(a.s, widgets);
            writeFile(a.config(), R"({"bar": {"position": "top", "layout": {
                "left": [{"id": "omarchy.menu"}],
                "center": [{"id": "omarchy.clock", "format": "HH:mm", "exec": "notify-send hi"}],
                "centerEnd": [{"id": "omarchy.weather", "units": "metric"}],
                "right": [{"id": "nixfred.blip"}, "omarchy.tray"]}}})");
            writeFile(a.state("disabled-plugins"), "omarchy.emojis\n");
            AppCore core(a.s.env);
            core.capabilities.scanServicesBlocking();
            ProfileCapture::Selection selection;
            selection.bar = true;
            selection.plugins = true;
            const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
            QVERIFY2(result.ok, qPrintable(result.error));
            // Omarchy's own widgets in order, with options that are only data. A widget from
            // a plugin installed separately, and an option that runs something, are named.
            const QJsonObject layout = result.profile.desktop.value("bar").toObject().value("layout").toObject();
            QCOMPARE(QJsonDocument(layout.value("center").toArray()).toJson(QJsonDocument::Compact), QByteArray(R"([{"format":"HH:mm","id":"omarchy.clock"}])"));
            QCOMPARE(QJsonDocument(layout.value("right").toArray()).toJson(QJsonDocument::Compact), QByteArray(R"([{"id":"omarchy.tray"}])"));
            QCOMPARE(layout.value("tray").toArray().size(), 0);
            QString reason;
            for (const ProfileCapture::Item &item : result.items)
                if (item.id == "desktop:bar")
                    reason = item.reason;
            QVERIFY2(reason.contains("nixfred.blip") && reason.contains("exec"), qPrintable(reason));
            QCOMPARE(result.profile.desktop.value("plugins").toObject().value("disabled").toArray(), QJsonArray{"omarchy.emojis"});
            QString error;
            QVERIFY2(core.profileCapture.save(store.path(), result, &error), qPrintable(error));
        }
        ProfileMachine b;
        omarchyPlugins(b.s, widgets);
        // A bar of its own, with a widget installed separately and an option the profile does
        // not carry: both stay.
        writeFile(b.config(), R"({"mine": {"keep": true}, "bar": {"position": "left", "layout": {
            "left": [{"id": "omarchy.menu"}],
            "center": [{"id": "omarchy.clock", "format": "dddd HH:mm", "verticalFormat": "HH", "compact": true}],
            "right": [{"id": "facet.hardware"}, {"id": "omarchy.tray"}]}}})");
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        ProfilePlan plan = planProfile(core, store.path());
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        QCOMPARE(planned(plan, "bar").status, QString("conflict"));
        QCOMPARE(planned(plan, "plugins").status, QString("ready"));
        ProfilePlanner::Options options;
        options.useSaved = {"bar"};
        plan = planProfile(core, store.path(), options);
        const ProfileRun run = runPlan(core, plan);
        QVERIFY2(run.state == "completed", qPrintable(run.message));
        const QJsonObject layout = barLayoutIn(b.config());
        QCOMPARE(QJsonDocument(layout.value("center").toArray()).toJson(QJsonDocument::Compact),
                 QByteArray(R"([{"compact":true,"format":"HH:mm","id":"omarchy.clock"}])"));
        QCOMPARE(QJsonDocument(layout.value("right").toArray()).toJson(QJsonDocument::Compact), QByteArray(R"([{"id":"omarchy.tray"},{"id":"facet.hardware"}])"));
        QCOMPARE(QJsonDocument(layout.value("centerEnd").toArray()).toJson(QJsonDocument::Compact), QByteArray(R"([{"id":"omarchy.weather","units":"metric"}])"));
        const QJsonObject config = QJsonDocument::fromJson(readAll(b.config())).object();
        QCOMPARE(config.value("mine").toObject().value("keep").toBool(), true);
        QCOMPARE(config.value("bar").toObject().value("position").toString(), QString("left"));
        QVERIFY(b.s.calls().contains("omarchy shell -q shell reloadConfig"));
        QVERIFY(b.s.calls().contains("omarchy plugin disable omarchy.emojis"));
        const ProfilePlan second = planProfile(core, store.path());
        QCOMPARE(planned(second, "bar").status, QString("unchanged"));
        QCOMPARE(planned(second, "plugins").status, QString("unchanged"));

        // A profile that names one section manages that one only, as a setting it leaves out
        // is left alone, except that a widget it places there moves rather than appear twice.
        QTemporaryDir partial;
        const QJsonObject left{{"layout", QJsonObject{{"left", QJsonArray{QJsonObject{{"id", "omarchy.weather"}}, QJsonObject{{"id", "omarchy.menu"}}}}}}};
        writeFile(ProfileModel::manifestPath(partial.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Left"}, {"desktop", QJsonObject{{"bar", left}}}}).toJson());
        ProfilePlanner::Options keep;
        keep.useSaved = {"bar"};
        QCOMPARE(runPlan(core, planProfile(core, partial.path(), keep)).state, QString("completed"));
        const QJsonObject after = barLayoutIn(b.config());
        QCOMPARE(QJsonDocument(after.value("left").toArray()).toJson(QJsonDocument::Compact), QByteArray(R"([{"id":"omarchy.weather"},{"id":"omarchy.menu"}])"));
        QCOMPARE(after.value("centerEnd").toArray().size(), 0);
        QCOMPARE(after.value("center").toArray(), layout.value("center").toArray());
        QCOMPARE(after.value("right").toArray(), layout.value("right").toArray());
        QCOMPARE(planned(planProfile(core, partial.path()), "bar").status, QString("unchanged"));
    }

    void profileBarAndPluginsRefuseWhatIsNotHere() {
        QTemporaryDir store;
        const QJsonObject desktop{{"bar", QJsonObject{{"layout", QJsonObject{{"left", QJsonArray{QJsonObject{{"id", "omarchy.globe"}}}}}}}},
                                  {"plugins", QJsonObject{{"disabled", QJsonArray{"omarchy.teleport"}}}}};
        writeFile(ProfileModel::manifestPath(store.path()), QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Bar"}, {"desktop", desktop}}).toJson());
        ProfileMachine b;
        omarchyPlugins(b.s, {"omarchy.menu"});
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        // A profile from a newer Omarchy can name what this one does not have.
        ProfilePlan plan = planProfile(core, store.path());
        {
            // With none of Omarchy's own widgets found, the bar is not read at all: never
            // saved empty, and never emptied by a restore.
            ProfileMachine bare;
            AppCore lost(bare.s.env);
            ProfileCapture::Selection bar;
            bar.bar = true;
            bar.settings = {"power.idle.lock"};
            const ProfileCapture::Result captured = captureProfile(lost, nullptr, bar);
            QVERIFY(captured.ok);
            QVERIFY(!captured.profile.desktop.contains("bar"));
            QTemporaryDir empty;
            writeFile(ProfileModel::manifestPath(empty.path()),
                      QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Empty bar"}, {"desktop", QJsonObject{{"bar", QJsonObject{{"layout", QJsonObject{{"left", QJsonArray()}}}}}}}}).toJson());
            QCOMPARE(planned(planProfile(lost, empty.path()), "bar").status, QString("blocked"));
        }
        QCOMPARE(planned(plan, "bar").status, QString("blocked"));
        QVERIFY(planned(plan, "bar").reason.contains("omarchy.globe"));
        QCOMPARE(planned(plan, "plugins").status, QString("blocked"));
        QVERIFY(planned(plan, "plugins").reason.contains("omarchy.teleport"));
        // Without the shell, its plugins cannot be read, and nothing is switched.
        writeFile(b.state("shell-down"), "");
        plan = planProfile(core, store.path());
        QVERIFY(planned(plan, "plugins").reason.contains("could not be read"));
        // A capture leaves out what it cannot read, rather than recording nothing as a choice.
        ProfileCapture::Selection selection;
        selection.plugins = true;
        selection.settings = {"power.idle.lock"};
        const ProfileCapture::Result result = captureProfile(core, nullptr, selection);
        QVERIFY(result.ok);
        QVERIFY(!result.profile.desktop.contains("plugins"));
    }

    // ---- My Omarchy: versions and GitHub -------------------------------------------
    void profileRepositorySavesVersionsWithoutTheNetwork() {
        QTemporaryDir hub;
        ProfileMachine m;
        useGitHub(m.s, hub.path());
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        ProfileRepository &repository = core.profileRepository;
        QCOMPARE(repositoryStatus(core).relation, QString("nothing"));
        // Nothing to save before there is a profile.
        QVERIFY(!repositoryDo([&](auto done) { repository.save("First", done); }).ok);
        ProfileCapture::Selection selection;
        selection.settings = {"power.idle.lock"};
        QVERIFY(saveProfile(core, repository.workingDirectory(), selection));
        QCOMPARE(repositoryStatus(core).relation, QString("unsaved-only"));
        const ProfileRepository::Result first = repositoryDo([&](auto done) { repository.save("First", done); });
        QVERIFY2(first.ok, qPrintable(first.error));
        // The same profile again is no new version.
        const ProfileRepository::Result again = repositoryDo([&](auto done) { repository.save("Again", done); });
        QVERIFY(again.ok);
        QCOMPARE(again.commit, first.commit);
        QVERIFY(again.message.contains("Nothing changed"));
        selection.settings = {"appearance.theme"};
        QVERIFY(saveProfile(core, repository.workingDirectory(), selection));
        ProfileRepository::Status status = repositoryStatus(core);
        QVERIFY(status.unsavedChanges);
        const ProfileRepository::Result second = repositoryDo([&](auto done) { repository.save("Second", done); });
        QVERIFY(second.ok && second.commit != first.commit);
        status = repositoryStatus(core);
        QCOMPARE(status.relation, QString("saved"));
        QVERIFY(!status.unsavedChanges);
        QCOMPARE(status.saved.parents, QStringList{first.commit});
        QList<ProfileRepository::Version> versions;
        bool listed = false;
        repository.history(false, 10, [&](const QList<ProfileRepository::Version> &list, const QString &) { versions = list; listed = true; });
        QVERIFY(QTest::qWaitFor([&] { return listed; }, 10000));
        QCOMPARE(versions.size(), 2);
        QCOMPARE(versions[0].message, QString("Second"));
        // A version holds the profile and nothing else, and the history has no hooks, no
        // remote, and none of the user's configuration.
        QCOMPARE(QString::fromLatin1(gitIn(repository.repositoryPath(), {"ls-tree", "-r", "--name-only", second.commit})), QString("profile.json"));
        QVERIFY(!QFileInfo::exists(repository.repositoryPath() + "/hooks"));
        QVERIFY(!readAll(repository.repositoryPath() + "/config").contains("remote"));
        const ProfileRepository::Result read = repositoryDo([&](auto done) { repository.read(first.commit.left(9), done); });
        QVERIFY2(read.ok, qPrintable(read.error));
        QVERIFY(ProfileModel::load(read.directory).profile.settings.contains("power.idle.lock"));
        QVERIFY(!ProfileModel::load(read.directory).profile.settings.contains("appearance.theme"));
        // Saving never reached GitHub.
        QVERIFY(!m.s.calls().join("\n").contains("gh "));
        // What changed between the two, record by record.
        const ProfileRepository::Result older = repositoryDo([&](auto done) { repository.read(first.commit, done); });
        const QJsonArray rows = ProfileRepository::differences(ProfileModel::load(older.directory).profile, ProfileModel::load(repository.workingDirectory()).profile);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toObject().value("id").toString(), QString("appearance.theme"));
        QCOMPARE(rows[0].toObject().value("change").toString(), QString("added"));
        QCOMPARE(Cli::run(m.s.env, {"profile", "diff", first.commit.left(7), "saved", "--json"}), 0);
        QCOMPARE(Cli::run(m.s.env, {"profile", "diff", "nothere"}), 1);
        // Connecting on a branch of its own takes the versions saved so far along.
        bareRepository(hub.path(), "tester/elsewhere");
        QVERIFY(repositoryDo([&](auto done) { repository.connectTo("tester/elsewhere", "profiles", false, done); }).ok);
        QCOMPARE(repositoryStatus(core).saved.commit, second.commit);
    }

    void profileRepositoryRoundTripsThroughGitHub() {
        QTemporaryDir hub;
        const QString remote = bareRepository(hub.path(), "tester/my-omarchy");
        QString published;
        {
            ProfileMachine a;
            useGitHub(a.s, hub.path());
            writeFile(a.config(), "{\"idle\": {\"lock\": 600, \"screensaver\": 150}}\n");
            AppCore core(a.s.env);
            core.capabilities.scanServicesBlocking();
            ProfileCapture::Selection selection;
            selection.settings = {"power.idle.lock"};
            QVERIFY(saveProfile(core, core.profileRepository.workingDirectory(), selection));
            QVERIFY(repositoryDo([&](auto done) { core.profileRepository.save("From the laptop", done); }).ok);
            const ProfileRepository::Result connected = repositoryDo([&](auto done) { core.profileRepository.connectTo("https://github.com/tester/my-omarchy", {}, false, done); });
            QVERIFY2(connected.ok, qPrintable(connected.error));
            QCOMPARE(core.profileRepository.connection().visibility, QString("private"));
            // Versions carry the account's no-reply address, never a real one.
            QCOMPARE(core.profileRepository.connection().authorEmail, QString("4242+tester@users.noreply.github.com"));
            QCOMPARE(repositoryStatus(core).relation, QString("ahead"));
            const ProfileRepository::Result pushed = repositoryDo([&](auto done) { core.profileRepository.publish(done); });
            QVERIFY2(pushed.ok, qPrintable(pushed.error));
            published = pushed.commit;
            QCOMPARE(QString::fromLatin1(gitIn(remote, {"rev-parse", "main"})), published);
            QCOMPARE(repositoryStatus(core).relation, QString("same"));
            // Publishing again adds nothing.
            QVERIFY(repositoryDo([&](auto done) { core.profileRepository.publish(done); }).message.contains("Already published"));
        }
        // A fresh computer: connect, fetch, and restore the published version.
        ProfileMachine b;
        useGitHub(b.s, hub.path());
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        QVERIFY(repositoryDo([&](auto done) { core.profileRepository.connectTo("tester/my-omarchy", {}, false, done); }).ok);
        QVERIFY(!QFileInfo::exists(core.profileRepository.repositoryPath() + "/HEAD") || repositoryStatus(core).published.isEmpty());
        QVERIFY(repositoryDo([&](auto done) { core.profileRepository.fetch(done); }).ok);
        QCOMPARE(repositoryStatus(core).relation, QString("published-only"));
        // Fetching changed nothing on the desktop.
        QVERIFY(readAll(b.config()).contains("\"lock\": 300"));
        const ProfileRepository::Result version = repositoryDo([&](auto done) { core.profileRepository.read("published", done); });
        QVERIFY2(version.ok, qPrintable(version.error));
        QCOMPARE(version.commit, published);
        const ProfilePlan plan = planProfile(core, version.directory);
        QVERIFY2(plan.valid, qPrintable(plan.errors.join("\n")));
        QCOMPARE(planned(plan, "power.idle.lock").status, QString("ready"));
        QCOMPARE(runPlan(core, plan).state, QString("completed"));
        QVERIFY(readAll(b.config()).contains("\"lock\": 600"));
        // Its own profile starts from the published one, so a save adds to that history.
        QVERIFY(repositoryDo([&](auto done) { core.profileRepository.prepareWorking(done); }).ok);
        ProfileCapture::Selection selection;
        selection.settings = {"appearance.theme"};
        QVERIFY(saveProfile(core, core.profileRepository.workingDirectory(), selection));
        const ProfileModel::Parsed working = ProfileModel::load(core.profileRepository.workingDirectory());
        QVERIFY(working.profile.settings.contains("power.idle.lock") && working.profile.settings.contains("appearance.theme"));
        const ProfileRepository::Result saved = repositoryDo([&](auto done) { core.profileRepository.save("From the desktop", done); });
        QCOMPARE(repositoryStatus(core).saved.parents, QStringList{published});
        QVERIFY(repositoryDo([&](auto done) { core.profileRepository.publish(done); }).ok);
        QCOMPARE(QString::fromLatin1(gitIn(remote, {"rev-parse", "main"})), saved.commit);
    }

    void profileRepositoryNeverLosesAVersion() {
        QTemporaryDir hub;
        const QString remote = bareRepository(hub.path(), "tester/my-omarchy");
        ProfileMachine a, b;
        useGitHub(a.s, hub.path());
        useGitHub(b.s, hub.path());
        AppCore one(a.s.env), two(b.s.env);
        one.capabilities.scanServicesBlocking();
        two.capabilities.scanServicesBlocking();
        ProfileCapture::Selection selection;
        selection.settings = {"power.idle.lock"};
        QVERIFY(saveProfile(one, one.profileRepository.workingDirectory(), selection));
        QVERIFY(repositoryDo([&](auto done) { one.profileRepository.save("Base", done); }).ok);
        for (AppCore *core : {&one, &two})
            QVERIFY(repositoryDo([&](auto done) { core->profileRepository.connectTo("tester/my-omarchy", {}, false, done); }).ok);
        QVERIFY(repositoryDo([&](auto done) { one.profileRepository.publish(done); }).ok);
        QVERIFY(repositoryDo([&](auto done) { two.profileRepository.fetch(done); }).ok);
        QVERIFY(repositoryDo([&](auto done) { two.profileRepository.prepareWorking(done); }).ok);
        // Both change the same profile after it.
        selection.settings = {"appearance.theme"};
        QVERIFY(saveProfile(one, one.profileRepository.workingDirectory(), selection));
        const QString theirs = repositoryDo([&](auto done) { one.profileRepository.save("Theme", done); }).commit;
        QVERIFY(repositoryDo([&](auto done) { one.profileRepository.publish(done); }).ok);
        selection.settings = {"power.idle.screensaver"};
        QVERIFY(saveProfile(two, two.profileRepository.workingDirectory(), selection));
        const QString mine = repositoryDo([&](auto done) { two.profileRepository.save("Screensaver", done); }).commit;
        // Publishing stops, with both versions where they were.
        const ProfileRepository::Result refused = repositoryDo([&](auto done) { two.profileRepository.publish(done); });
        QVERIFY(!refused.ok);
        QVERIFY(refused.error.contains("Both"));
        QCOMPARE(QString::fromLatin1(gitIn(remote, {"rev-parse", "main"})), theirs);
        QCOMPARE(repositoryStatus(two).saved.commit, mine);
        QCOMPARE(repositoryStatus(two).relation, QString("diverged"));
        // Offline: saving still works, and publishing fails without losing anything.
        {
            Sandbox offline;
            Env away = b.s.env;
            away.gitHub = "file://" + offline.dir.path() + "/nowhere";
            AppCore cut(away);
            const ProfileRepository::Result failed = repositoryDo([&](auto done) { cut.profileRepository.publish(done); });
            QVERIFY(!failed.ok);
            QVERIFY(failed.error.contains("stay saved"));
            QCOMPARE(repositoryStatus(cut).saved.commit, mine);
        }
        // Keeping this computer's: a new version with both behind it, which only adds.
        const ProfileRepository::Result kept = repositoryDo([&](auto done) { two.profileRepository.resolve("mine", done); });
        QVERIFY2(kept.ok, qPrintable(kept.error));
        QCOMPARE(repositoryStatus(two).saved.parents, (QStringList{theirs, mine}));
        QVERIFY(repositoryDo([&](auto done) { two.profileRepository.publish(done); }).ok);
        QCOMPARE(QString::fromLatin1(gitIn(remote, {"rev-parse", "main"})), kept.commit);
        // The other computer takes the published version; what it had is kept, not lost.
        selection.settings = {"input.repeat_rate"};
        QVERIFY(saveProfile(one, one.profileRepository.workingDirectory(), selection));
        QVERIFY(repositoryDo([&](auto done) { one.profileRepository.fetch(done); }).ok);
        const ProfileRepository::Result taken = repositoryDo([&](auto done) { one.profileRepository.resolve("published", done); });
        QVERIFY2(taken.ok, qPrintable(taken.error));
        QCOMPARE(repositoryStatus(one).saved.commit, kept.commit);
        QVERIFY(!repositoryStatus(one).unsavedChanges);
        const QStringList keptRefs = QString::fromLatin1(gitIn(one.profileRepository.repositoryPath(), {"for-each-ref", "--format=%(objectname)", "refs/omarchy-control/kept"}))
                                         .split('\n', Qt::SkipEmptyParts);
        QCOMPARE(keptRefs.size(), 1);
        // The unsaved change went into the kept version.
        QVERIFY(QString::fromLatin1(gitIn(one.profileRepository.repositoryPath(), {"show", keptRefs.first() + ":profile.json"})).contains("input.repeat_rate"));
        QVERIFY(ProfileModel::load(one.profileRepository.workingDirectory()).profile.settings.contains("power.idle.screensaver"));
    }

    void profileRepositoryRefusesWhatAProfileNeverHolds_data() {
        const QByteArray profile = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "By hand"}, {"settings", QJsonObject{{"power.idle.lock", setTo("600")}}}}).toJson();
        const QString digest = QString::fromLatin1(QCryptographicHash::hash("an image", QCryptographicHash::Sha256).toHex());
        const QJsonObject background{{"theme", "nord"}, {"name", "a.png"}, {"sha256", digest}};
        const QJsonObject asset{{"sha256", digest}, {"size", 8}, {"kind", "background"}, {"name", "a.png"}};
        const QByteArray withAsset =
            QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "By hand"}, {"desktop", QJsonObject{{"background", background}}}, {"assets", QJsonArray{asset}}}).toJson();
        const QByteArray future = QJsonDocument(QJsonObject{{"schemaVersion", 2}, {"name", "Future"}}).toJson();
        const QByteArray script = QByteArray("#!") + "/bin/sh\nid\n";
        QTest::addColumn<QList<GitEntry>>("entries");
        QTest::addColumn<QString>("problem");
        QTest::newRow("hook") << QList<GitEntry>{{"100644", "profile.json", profile}, {"100755", "hooks/post-checkout", script}} << "folder hooks";
        QTest::newRow("submodules file") << QList<GitEntry>{{"100644", "profile.json", profile}, {"100644", ".gitmodules", "[submodule \"x\"]\n"}} << ".gitmodules";
        QTest::newRow("attributes") << QList<GitEntry>{{"100644", "profile.json", profile}, {"100644", ".gitattributes", "* filter=x\n"}} << ".gitattributes";
        QTest::newRow("link") << QList<GitEntry>{{"120000", "profile.json", "/etc/passwd"}} << "is a link";
        QTest::newRow("program") << QList<GitEntry>{{"100644", "profile.json", profile}, {"100755", "files/ghostty/config", "font-size = 9\n"}} << "marked as a program";
        QTest::newRow("submodule") << QList<GitEntry>{{"100644", "profile.json", profile}, {"160000", "files/ghostty", QByteArray(40, 'a')}} << "submodule";
        QTest::newRow("too large") << QList<GitEntry>{{"100644", "profile.json", QByteArray(ProfileModel::MaxManifestBytes + 1, ' ')}} << "larger than a profile allows";
        QTest::newRow("no manifest") << QList<GitEntry>{{"100644", "files/ghostty/config", "font-size = 9\n"}} << "no profile.json";
        QTest::newRow("newer schema") << QList<GitEntry>{{"100644", "profile.json", future}} << "newer";
        QTest::newRow("damaged asset") << QList<GitEntry>{{"100644", "profile.json", withAsset}, {"100644", "assets/sha256/" + digest, "an imagX"}}
                                       << "digest";
    }
    void profileRepositoryRefusesWhatAProfileNeverHolds() {
        QFETCH(QList<GitEntry>, entries);
        QFETCH(QString, problem);
        QTemporaryDir hub;
        const QString remote = bareRepository(hub.path(), "tester/my-omarchy");
        const QString commit = commitByHand(remote, entries);
        ProfileMachine b;
        useGitHub(b.s, hub.path());
        AppCore core(b.s.env);
        QVERIFY(repositoryDo([&](auto done) { core.profileRepository.connectTo("tester/my-omarchy", {}, false, done); }).ok);
        QVERIFY(repositoryDo([&](auto done) { core.profileRepository.fetch(done); }).ok);
        const ProfileRepository::Result read = repositoryDo([&](auto done) { core.profileRepository.read("published", done); });
        QVERIFY(!read.ok);
        QVERIFY2((read.error + " " + read.problems.join(" ")).contains(problem), qPrintable(read.error + " | " + read.problems.join(" | ")));
        // Nothing of it is left to plan from, and nothing reached the working profile.
        QVERIFY(!QFileInfo::exists(ProfileStore::root(b.s.env) + "/revisions/" + commit));
        QVERIFY(!repositoryDo([&](auto done) { core.profileRepository.prepareWorking(done); }).ok);
        QVERIFY(!QFileInfo::exists(ProfileModel::manifestPath(core.profileRepository.workingDirectory())));
    }

    void profileRepositoryConnectsOnlyToGitHub() {
        ProfileRepository::Connection connection;
        for (const QString &form : {"owner/name", "https://github.com/owner/name", "https://github.com/owner/name.git", "github.com/owner/name/", "https://www.github.com/owner/name"}) {
            QVERIFY2(ProfileRepository::parseGitHub(form, &connection, nullptr), qPrintable(form));
            QCOMPARE(connection.fullName(), QString("owner/name"));
        }
        for (const QString &form : {"git@github.com:owner/name.git", "https://gitlab.com/owner/name", "https://user:token@github.com/owner/name", "https://github.com/owner/name/tree/main",
                                    "owner/name?x=1", "../name", "owner/..", "-owner/name", "http://github.com/owner/name", "file:///srv/name.git"})
            QVERIFY2(!ProfileRepository::parseGitHub(form, &connection, nullptr), qPrintable(form));
        QTemporaryDir hub;
        bareRepository(hub.path(), "tester/open");
        writeFile(hub.path() + "/tester/open.public", "");
        ProfileMachine m;
        useGitHub(m.s, hub.path());
        AppCore core(m.s.env);
        // A public repository is refused unless that is asked for by name.
        ProfileRepository::Result result = repositoryDo([&](auto done) { core.profileRepository.connectTo("tester/open", {}, false, done); });
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("public"));
        QVERIFY(core.profileRepository.connection().isEmpty());
        result = repositoryDo([&](auto done) { core.profileRepository.connectTo("tester/open", {}, true, done); });
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(core.profileRepository.connection().publicAllowed);
        // One this account cannot see is refused.
        QVERIFY(!repositoryDo([&](auto done) { core.profileRepository.connectTo("tester/missing", {}, false, done); }).ok);
        QVERIFY(!repositoryDo([&](auto done) { core.profileRepository.connectTo("tester/open", "bad..branch", true, done); }).ok);
        // Creating one makes it private.
        result = repositoryDo([&](auto done) { core.profileRepository.create("my-omarchy", done); });
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(core.profileRepository.connection().fullName(), QString("tester/my-omarchy"));
        QCOMPARE(core.profileRepository.connection().visibility, QString("private"));
        QVERIFY(m.s.calls().join("\n").contains("gh repo create my-omarchy --private"));
    }

    void profileCliSavesAndPublishes() {
        QTemporaryDir hub;
        const QString remote = bareRepository(hub.path(), "tester/my-omarchy");
        ProfileMachine a;
        useGitHub(a.s, hub.path());
        QCOMPARE(Cli::run(a.s.env, {"profile", "capture", "--setting", "power.idle.lock"}), 0);
        QCOMPARE(Cli::run(a.s.env, {"profile", "save", "--message", "First"}), 0);
        QCOMPARE(Cli::run(a.s.env, {"profile", "history", "--json"}), 0);
        QCOMPARE(Cli::run(a.s.env, {"profile", "publish"}), 1);  // Not connected yet.
        QCOMPARE(Cli::run(a.s.env, {"profile", "connect", "tester/my-omarchy"}), 0);
        QCOMPARE(Cli::run(a.s.env, {"profile", "publish"}), 0);
        QVERIFY(!gitIn(remote, {"rev-parse", "main"}).isEmpty());
        QCOMPARE(Cli::run(a.s.env, {"profile", "status"}), 0);
        QCOMPARE(Cli::run(a.s.env, {"profile", "inspect", "--revision", "saved"}), 0);
        ProfileMachine b;
        useGitHub(b.s, hub.path());
        QCOMPARE(Cli::run(b.s.env, {"profile", "connect", "https://github.com/tester/my-omarchy"}), 0);
        QCOMPARE(Cli::run(b.s.env, {"profile", "plan", "--revision", "published"}), 1);  // Not fetched yet.
        QCOMPARE(Cli::run(b.s.env, {"profile", "fetch"}), 0);
        QCOMPARE(Cli::run(b.s.env, {"profile", "plan", "--revision", "published"}), 0);
        QCOMPARE(Cli::run(b.s.env, {"profile", "resolve", "--use-published", "--keep-mine"}), 1);
    }

    // ---- My Omarchy: the window --------------------------------------------------------
    void profileWindowBackendSavesAndRestores() {
        // What the window does, through the provider it uses: the same capture, versions,
        // plan, and restore the command line runs, as values QML shows.
        QTemporaryDir hub;
        ProfileMachine m;
        useGitHub(m.s, hub.path());
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        ProfileProvider profiles(&core);
        const auto settle = [&] { return QTest::qWaitFor([&] { return !profiles.busy(); }, 30000); };
        profiles.startCapture(QVariantMap{{"categories", QStringList{"idle"}}});
        QVERIFY(settle());
        QVERIFY2(profiles.review().value("ok").toBool(), qPrintable(profiles.error()));
        QStringList labels;
        for (const QVariant &row : profiles.review().value("items").toList())
            labels << row.toMap().value("label").toString();
        QVERIFY2(labels.contains("Lock after"), qPrintable(labels.join(", ")));
        profiles.saveReview("From the window", false);
        QVERIFY(settle());
        QVERIFY2(profiles.error().isEmpty(), qPrintable(profiles.error()));
        profiles.refresh();
        QVERIFY(QTest::qWaitFor([&] { return profiles.status().value("saved").toMap().value("message").toString() == "From the window"; }, 10000));
        // Changed here after the save: a decision, never overwritten on its own.
        QCOMPARE(applySetting(core.engine, "power.idle.lock", "600").phase, QString("committed"));
        profiles.planRestore("saved");
        QVERIFY(settle());
        QVERIFY2(profiles.plan().value("valid").toBool(), qPrintable(profiles.error()));
        QCOMPARE(profiles.plan().value("undecided").toInt(), 1);
        QString lock;
        for (const QVariant &row : profiles.plan().value("items").toList())
            if (row.toMap().value("target").toString() == "power.idle.lock")
                lock = row.toMap().value("id").toString();
        QVERIFY(!lock.isEmpty());
        profiles.startRestore(false);  // Left out without a decision, so refused unconfirmed.
        QVERIFY(!profiles.error().isEmpty());
        profiles.choose(lock, "use-saved");
        QVERIFY(settle());
        QCOMPARE(profiles.plan().value("willRun").toInt(), 1);
        QCOMPARE(profiles.plan().value("undecided").toInt(), 0);
        profiles.startRestore(false);
        QVERIFY(QTest::qWaitFor([&] { return profiles.run().value("state").toString() == "completed"; }, 30000));
        QVERIFY(readAll(m.config()).contains("\"lock\": 300"));
        QCOMPARE(profiles.run().value("counts").toMap().value("verified").toInt(), 1);
        profiles.clearRun();
        profiles.clearPlan();
        QVERIFY(profiles.run().isEmpty());
    }

    void profileWindowHandsAppsToATerminal() {
        // Apps change only where sudo and pacman can ask: the window opens a terminal for
        // the restore, which holds the lease and keeps going if the window closes.
        PackageMachine m;
        m.s.command("omarchy-launch-floating-terminal-with-presentation", "printf '%s' \"$1\" > \"$(dirname \"$HOME\")/terminal\"");
        QTemporaryDir store;
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Apps"}, {"packages", QJsonArray{QJsonObject{{"name", "ripgrep"}, {"source", "repository"}, {"intent", "present"}}}}})
                      .toJson());
        AppCore core(m.s.env);
        core.capabilities.scanServicesBlocking();
        ProfileProvider profiles(&core);
        // A folder that is not this computer's own profile is planned through the same path.
        QVERIFY(copyFolder(store.path(), core.profileRepository.workingDirectory()));
        profiles.planRestore("working");
        QVERIFY(QTest::qWaitFor([&] { return !profiles.busy(); }, 30000));
        QVERIFY2(profiles.plan().value("valid").toBool(), qPrintable(profiles.error()));
        QVERIFY(profiles.plan().value("needsTerminal").toBool());
        profiles.startRestore(false);
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(m.s.dir.path() + "/terminal"); }, 10000));
        const QString command = QString::fromUtf8(readAll(m.s.dir.path() + "/terminal"));
        QVERIFY2(command.contains(" profile apply " + profiles.plan().value("id").toString() + " --yes"), qPrintable(command));
        QVERIFY(profiles.run().value("terminal").toBool());
        // Nothing was installed by the window itself.
        QVERIFY(!m.has("ripgrep"));
        profiles.clearRun();
    }

    void myOmarchyPageSavesThroughTheWindow() {
        QTemporaryDir hub;
        Sandbox s;
        fakeDesktop(s);
        useGitHub(s, hub.path());
        QQuickStyle::setStyle("Material");
        Translator translator(s.env.configHome + "/omarchy/language", ":/i18n", s.env.omarchyPath + "/default/i18n/locales.json");
        Theme theme(s.env);
        Backend backend(s.env);
        TrayHost trayHost;
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlApplicationEngine::warnings, this, [&](const QList<QQmlError> &list) {
            for (const auto &warning : list)
                warnings << warning.toString();
        });
        engine.addImageProvider("icons", new IconProvider);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("initialPage", QString("myomarchy"));
        engine.rootContext()->setContextProperty("initialSetting", QString());
        engine.rootContext()->setContextProperty("trayHost", &trayHost);
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, "myomarchy"), Q_ARG(QVariant, QString()));
        const auto find = [&](const QString &name) -> QQuickItem * {
            QQuickItem *found = nullptr;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
                if (!found && item->objectName() == name && item->isVisible())
                    found = item;
                for (QQuickItem *child : item->childItems())
                    walk(child);
            };
            walk(window->contentItem());
            return found;
        };
        const auto click = [&](const QString &name) {
            QQuickItem *item = nullptr;
            if (!QTest::qWaitFor([&] { return (item = find(name)) && item->isEnabled(); }, 20000))
                return false;
            QMetaObject::invokeMethod(item, "click");
            return true;
        };
        QVERIFY(click("profileStartSave"));
        QVERIFY(click("profileChoice_category.idle"));
        QVERIFY(QTest::qWaitFor([&] { return find("profileChoice_category.idle") && find("profileChoice_category.idle")->property("on").toBool(); }, 5000));
        QVERIFY(click("profileReviewSave"));
        QVERIFY(click("profileSaveHere"));
        QVERIFY(QTest::qWaitFor([&] { return find("profileSaved") && find("profileSaved")->property("text").toString().contains("Saved with Omarchy Control"); }, 20000));
        QVERIFY(click("profileStartRestore"));
        QVERIFY(click("profileRestoreSaved"));
        QVERIFY(QTest::qWaitFor([&] { return backend.profiles()->plan().value("valid").toBool(); }, 20000));
        QVERIFY(click("profileRestoreBack"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }

    void profileThemeInstallFailuresAreContained() {
        QTemporaryDir store;
        writeFile(ProfileModel::manifestPath(store.path()),
                  QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Theme"}, {"settings", QJsonObject{{"appearance.theme", setTo("Obsidian Circuit")}}},
                                            {"desktop", QJsonObject{{"themes", QJsonObject{{"obsidian-circuit", QJsonObject{{"url", "https://github.com/someone/obsidian.git"}}}}}}}})
                      .toJson());
        ProfileMachine b;
        AppCore core(b.s.env);
        core.capabilities.scanServicesBlocking();
        // Without git there is no way to install it, and nothing selects a theme not here.
        ProfilePlan plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "theme:obsidian-circuit").status, QString("blocked"));
        QVERIFY(planned(plan, "theme:obsidian-circuit").reason.contains("git"));
        QCOMPARE(planned(plan, "appearance.theme").status, QString("blocked"));
        // A clone that fails takes back what it left, and the setting that needed it waits.
        b.s.command("git", kFakeGit);
        writeFile(b.s.dir.path() + "/git-fails", "");
        plan = planProfile(core, store.path());
        QCOMPARE(planned(plan, "theme:obsidian-circuit").status, QString("ready"));
        const ProfileRun run = runPlan(core, plan);
        QCOMPARE(run.state, QString("incomplete"));
        QHash<QString, ProfileRunItem> steps;
        for (const ProfileRunItem &step : run.items)
            steps.insert(step.target, step);
        QCOMPARE(steps.value("theme:obsidian-circuit").state, QString("failed"));
        QVERIFY2(steps.value("theme:obsidian-circuit").message.contains("resolve host"), qPrintable(steps.value("theme:obsidian-circuit").message));
        QCOMPARE(steps.value("appearance.theme").state, QString("blocked"));
        QVERIFY(!QFileInfo::exists(b.s.env.configHome + "/omarchy/themes/obsidian-circuit"));
        QVERIFY(!QFile::exists(b.state("theme-sets")));
        // An installer that changed since it was reviewed pauses theme installs.
        ProfileMachine c;
        c.s.env.enforceCompat = true;
        c.s.command("git", kFakeGit);
        c.s.command("omarchy-theme-install", "exit 0");
        AppCore paused(c.s.env);
        paused.capabilities.scanServicesBlocking();
        plan = planProfile(paused, store.path());
        QCOMPARE(planned(plan, "theme:obsidian-circuit").status, QString("blocked"));
        QVERIFY2(planned(plan, "theme:obsidian-circuit").reason.contains("omarchy-theme-install"), qPrintable(planned(plan, "theme:obsidian-circuit").reason));
    }

};

int main(int argc, char *argv[]) {
    if (argc >= 4 && QString::fromLatin1(argv[1]) == QLatin1String("--cli-sandbox")) {
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(APP_VERSION);
        const QString root = app.arguments()[2];
        Env env = Env::sandbox(root, root + "/bin"); env.process.insert("OC_TEST_ROOT", root);
        return Cli::run(env, app.arguments().mid(3));
    }
    // Re-entered by watchdogSurvivesTheWindowDying as the detached process.
    // The real entry point, as DisplayProvider starts it, inside a test's sandbox.
    if (argc == 3 && QString::fromLatin1(argv[1]) == QLatin1String("display-watchdog")) {
        QCoreApplication app(argc, argv);
        const QString root = qEnvironmentVariable("OC_TEST_ROOT");
        if (root.isEmpty() || !QDir::isAbsolutePath(root)) return 2;
        return DisplayProvider::runWatchdog(Env::sandbox(root, root + "/bin"), QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 4 && QString::fromLatin1(argv[1]) == QLatin1String("--watchdog")) {
        QCoreApplication app(argc, argv);
        const QString root = QString::fromLocal8Bit(argv[2]);
        return DisplayProvider::runWatchdog(Env::sandbox(root, root + "/bin"), QString::fromLocal8Bit(argv[3]));
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    OmarchyControlTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "omarchy-control_tests.moc"
