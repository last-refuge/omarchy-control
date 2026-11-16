#!/usr/bin/python3
"""Exercise durable CLI previews with actual Qt D-Bus clients on private mocks.

Explicitly enabled disposable Docker labs only. No real connection is activated,
no credentials/agents are used, and every receipt lives in a temporary home.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

NAME = "org.freedesktop.NetworkManager"
ROOT = "/org/freedesktop/NetworkManager"
MOCK = "org.freedesktop.DBus.Mock"
DEVICE = ROOT + "/Devices/one"
PROFILE = ROOT + "/Settings/target"
OLD_PROFILE = ROOT + "/Settings/old"
OLD_ACTIVE = ROOT + "/ActiveConnection/old"
TARGET_UUID = "22222222-2222-2222-2222-222222222222"
OLD_UUID = "11111111-1111-1111-1111-111111111111"

RESTORE = """
cp = str(args[0])
if cp not in objects:
    raise dbus.exceptions.DBusException('No checkpoint', name='org.freedesktop.NetworkManager.UnknownCheckpoint')
dev = objects['DEVICE']
dev.props['org.freedesktop.NetworkManager.Device'].update( {'State': dbus.UInt32(100), 'ActiveConnection': dbus.ObjectPath('OLD_ACTIVE')})
self.UpdateProperties('org.freedesktop.NetworkManager', {'Checkpoints': dbus.Array([], signature='o')})
self.RemoveObject(cp)
ret = {'DEVICE': dbus.UInt32(0)}
""".replace("DEVICE", DEVICE).replace("OLD_ACTIVE", OLD_ACTIVE)


def private_case(control, include_expiry):
    import dbus
    bus = dbus.SessionBus()
    if bus.name_has_owner(NAME):
        raise RuntimeError("use a new private D-Bus")
    checks = []
    def check(value, label):
        if not value: raise AssertionError(label)
        checks.append(label)
    with tempfile.TemporaryDirectory(prefix="oc-network-preview-") as d:
        root = Path(d)
        env = dict(os.environ, HOME=d, XDG_CONFIG_HOME=str(root/"config"), XDG_STATE_HOME=str(root/"state"),
                   XDG_CACHE_HOME=str(root/"cache"), XDG_DATA_HOME=str(root/"data"),
                   DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"], LC_ALL="C.UTF-8", LANGUAGE="C")
        with (root/"mock.log").open("w") as log:
            service = subprocess.Popen([sys.executable, "-m", "dbusmock", "--session", "-t", "networkmanager"], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic()+10
                while not bus.name_has_owner(NAME):
                    if service.poll() is not None or time.monotonic() > deadline: raise RuntimeError("mock did not start")
                    time.sleep(.05)
                top = dbus.Interface(bus.get_object(NAME, "/org/freedesktop"), MOCK)
                top.AddWiFiDevice("one", "wlan0", 100)
                manager = dbus.Interface(bus.get_object(NAME, ROOT), MOCK)
                device = dbus.Interface(bus.get_object(NAME, DEVICE), MOCK)
                manager.AddProperty(NAME, "Checkpoints", dbus.Array([], signature="o"))
                device.AddProperty(NAME+".Device", "Udi", "fixture-device-one")
                device.UpdateProperties(NAME+".Device", {"Managed": dbus.Boolean(True), "AvailableConnections": dbus.Array([PROFILE, OLD_PROFILE], signature="o")})
                device_props = dbus.Interface(bus.get_object(NAME, DEVICE), "org.freedesktop.DBus.Properties")
                device_props.Set(NAME+".Device", "ActiveConnection", dbus.ObjectPath(OLD_ACTIVE))
                for path, identifier in ((PROFILE, TARGET_UUID), (OLD_PROFILE, OLD_UUID)):
                    settings = {"connection": {"id": "Target" if path == PROFILE else "Old", "uuid": identifier, "type": "802-11-wireless"}, "802-11-wireless": {"ssid": dbus.ByteArray(b"Open fixture")}}
                    top.AddObject(path, NAME+".Settings.Connection", {"Flags": dbus.UInt32(0)}, [("GetSettings", "", "a{sa{sv}}", "ret="+repr(settings))])
                top.AddObject(OLD_ACTIVE, NAME+".Connection.Active", {"Uuid": OLD_UUID, "State": dbus.UInt32(2), "Connection": dbus.ObjectPath(OLD_PROFILE), "Devices": dbus.Array([DEVICE], signature="o")}, [])
                settings_mock = dbus.Interface(bus.get_object(NAME, ROOT+"/Settings"), MOCK)
                settings_mock.UpdateProperties(NAME+".Settings", {"Connections": dbus.Array([PROFILE, OLD_PROFILE], signature="o")})
                settings_mock.AddMethod(NAME+".Settings", "GetConnectionByUuid", "s", "o", "ret=dbus.ObjectPath('"+PROFILE+"') if str(args[0])=='"+TARGET_UUID+"' else dbus.ObjectPath('"+OLD_PROFILE+"')")
                manager.AddMethod(NAME, "GetDeviceByIpIface", "s", "o", "ret=dbus.ObjectPath('"+DEVICE+"')")
                manager.AddMethod(MOCK, "Mode", "s", "", "self.fixture_mode=str(args[0])")
                manager.Mode("")
                manager.AddMethod(NAME, "CheckpointCreate", "aouu", "o", """
import time, os
from gi.repository import GLib
if self.fixture_mode == 'checkpoint-denied':
    raise dbus.exceptions.DBusException('Denied', name='org.freedesktop.NetworkManager.PermissionDenied')
if self.props['org.freedesktop.NetworkManager']['Checkpoints']:
    raise dbus.exceptions.DBusException('Busy', name='org.freedesktop.NetworkManager.CheckpointExists')
self.fixture_counter = getattr(self, 'fixture_counter', 0)+1
cp='/org/freedesktop/NetworkManager/Checkpoint/'+str(self.fixture_counter)
self.AddObject(cp, 'org.freedesktop.NetworkManager.Checkpoint', {'Devices': dbus.Array(args[0], signature='o'), 'Created': dbus.Int64(time.clock_gettime_ns(time.CLOCK_BOOTTIME)//1000000), 'RollbackTimeout': dbus.UInt32(args[1])}, [])
self.UpdateProperties('org.freedesktop.NetworkManager', {'Checkpoints': dbus.Array([cp], signature='o')})
if self.fixture_mode == 'secure-after-checkpoint':
    objects['PROFILE'].AddMethod('org.freedesktop.NetworkManager.Settings.Connection', 'GetSettings', '', 'a{sa{sv}}', "ret={'connection': {'id': 'Secured', 'uuid': 'TARGET_UUID', 'type': '802-11-wireless'}, '802-11-wireless-security': {'key-mgmt': 'wpa-psk'}}")
def expired(manager=self, checkpoint=cp, fixture_objects=objects):
    if checkpoint in fixture_objects:
        fixture_objects['DEVICE'].props['org.freedesktop.NetworkManager.Device'].update( {'State': dbus.UInt32(100), 'ActiveConnection': dbus.ObjectPath('OLD_ACTIVE')})
        manager.UpdateProperties('org.freedesktop.NetworkManager', {'Checkpoints': dbus.Array([], signature='o')})
        manager.RemoveObject(checkpoint)
    return False
GLib.timeout_add(int(args[1])*1000, expired)
ret=dbus.ObjectPath(cp)
""".replace("DEVICE", DEVICE).replace("OLD_ACTIVE", OLD_ACTIVE).replace("PROFILE", PROFILE).replace("TARGET_UUID", TARGET_UUID))
                manager.AddMethod(NAME, "CheckpointDestroy", "o", "", "self.RemoveObject(str(args[0])); self.UpdateProperties('org.freedesktop.NetworkManager', {'Checkpoints': dbus.Array([], signature='o')})")
                manager.AddMethod(NAME, "CheckpointRollback", "o", "a{su}", RESTORE)
                manager.AddMethod(NAME, "ActivateConnection", "ooo", "o", """
import time, os
if self.fixture_mode == 'activate-denied':
    raise dbus.exceptions.DBusException('Denied', name='org.freedesktop.NetworkManager.PermissionDenied')
active='/org/freedesktop/NetworkManager/ActiveConnection/new'+str(self.fixture_counter)
self.AddObject(active, 'org.freedesktop.NetworkManager.Connection.Active', {'Uuid': 'TARGET_UUID', 'State': dbus.UInt32(2), 'Connection': dbus.ObjectPath('PROFILE'), 'Devices': dbus.Array(['DEVICE'], signature='o')}, [])
if self.fixture_mode != 'no-op':
    objects['DEVICE'].props['org.freedesktop.NetworkManager.Device'].update( {'State': dbus.UInt32(100), 'ActiveConnection': dbus.ObjectPath(active)})
if self.fixture_mode == 'postread-failure':
    objects['DEVICE'].props['org.freedesktop.NetworkManager.Device']['Managed'] = dbus.Boolean(False)
if self.fixture_mode == 'hold':
    open(os.environ['HOME']+'/activation-held', 'w').close()
    time.sleep(7)
ret=dbus.ObjectPath(active)
""".replace("TARGET_UUID", TARGET_UUID).replace("PROFILE", PROFILE).replace("DEVICE", DEVICE))
                manager.AddMethod(NAME, "DeactivateConnection", "o", "", "objects['"+DEVICE+"'].props['org.freedesktop.NetworkManager.Device'].update( {'State': dbus.UInt32(30), 'ActiveConnection': dbus.ObjectPath('/')})")
                def api(*args, expected=0):
                    result = subprocess.run([str(control), "network", *args], env=env, capture_output=True, text=True, timeout=35)
                    value = json.loads(result.stdout)
                    if result.returncode != expected: raise AssertionError((args, value, result.stderr))
                    check(True, "process exit: " + " ".join(args[:2]))
                    return value
                def calls(method): return len(manager.GetMethodCalls(method))
                def begin():
                    value = api("activate", TARGET_UUID, "wlan0", "--yes")
                    check(value["outcome"] == "preview" and not value["verified"] and value["data"]["targetStateVerified"], "activation returns a verified reported target inside an uncommitted preview")
                    return value["data"]["receipt"]["token"]
                def receipt_path(token): return root/"state/omarchy-control/network-previews"/(token+".json")
                profiles = api("saved")["data"]["saved"]
                check({row["uuid"] for row in profiles} == {TARGET_UUID, OLD_UUID}, "actual discovery provides the UUIDs used for preview calls")
                target_mock = dbus.Interface(bus.get_object(NAME, PROFILE), MOCK)
                secured_settings = {"connection": {"id": "Secured", "uuid": TARGET_UUID, "type": "802-11-wireless"}, "802-11-wireless": {}, "802-11-wireless-security": {"key-mgmt": "wpa-psk"}}
                target_mock.AddMethod(NAME+".Settings.Connection", "GetSettings", "", "a{sa{sv}}", "ret="+repr(secured_settings))
                secure = api("activate", TARGET_UUID, "wlan0", "--yes", expected=1)
                check(secure["error"]["code"] == "interaction_required" and secure["data"]["nextAction"]["argv"] == ["launch", "panel.network"] and calls("CheckpointCreate") == 0, "secured activation is a concrete handoff before any checkpoint or connection write")
                open_settings = {"connection": {"id": "Target", "uuid": TARGET_UUID, "type": "802-11-wireless"}, "802-11-wireless": {"ssid": dbus.ByteArray(b"Open fixture")}}
                target_mock.AddMethod(NAME+".Settings.Connection", "GetSettings", "", "a{sa{sv}}", "ret="+repr(open_settings))
                plan = api("activate", TARGET_UUID, "wlan0", "--dry-run")
                check(plan["outcome"] == "planned" and calls("CheckpointCreate") == 0 and not (root/"state/omarchy-control/network-previews").exists(), "dry run creates neither checkpoint nor preview receipt")
                required = api("activate", TARGET_UUID, "wlan0", expected=1)
                check(required["error"]["code"] == "confirmation_required" and calls("CheckpointCreate") == 0, "connectivity consequences require acknowledgement before checkpoint creation")
                token = begin()
                check(receipt_path(token).stat().st_mode & 0o777 == 0o600, "durable receipt is private")
                status = api("preview", "status", token)
                check(status["data"]["checkpointPresent"] and status["data"]["remainingSeconds"] > 0 and not status["data"]["internetReachabilityVerified"], "a separate process observes checkpoint lifetime without claiming internet access")
                count = calls("ActivateConnection")
                refused = api("activate", TARGET_UUID, "wlan0", "--yes", expected=1)
                check(refused["error"]["code"] == "busy" and calls("ActivateConnection") == count, "pending preview rejects duplicate activation")
                radio = api("radio", "false", "--yes", expected=1)
                check(radio["error"]["code"] == "busy", "radio changes honor durable preview ownership")
                keep = api("preview", "keep", token, expected=1)
                check(keep["error"]["code"] == "confirmation_required" and calls("CheckpointDestroy") == 0, "keep requires user usability acknowledgement")
                keep = api("preview", "keep", token, "--yes")
                check(keep["outcome"] == "kept" and keep["verified"] and calls("CheckpointDestroy") == 1, "a later process keeps only the owned unchanged target")
                replay = api("preview", "keep", token, "--yes")
                check(replay["data"]["historicalReceipt"] and not replay["verified"] and calls("CheckpointDestroy") == 1, "replayed terminal receipt sends no duplicate keep")
                historical_seed = json.loads(receipt_path(token).read_text())
                disconnected = api("disconnect", "wlan0", "--yes")
                token = disconnected["data"]["receipt"]["token"]
                # The fixture's rollback restores its original profile. Update the
                # expected baseline by starting this case from the original one.
                manager.Mode("")
                old = api("preview", "status", token)
                check(old["data"]["state"]["state"] == 30, "disconnect is observed through a separate process")
                # Restore the exact captured active connection for this checkpoint.
                captured = old["data"]["receipt"]["beforeUuid"]
                active_before = ROOT+"/ActiveConnection/new1"
                manager.AddMethod(NAME, "CheckpointRollback", "o", "a{su}", RESTORE.replace(OLD_ACTIVE, active_before))
                reverted = api("preview", "revert", token)
                check(reverted["outcome"] == "reverted" and reverted["verified"] and reverted["data"]["state"]["uuid"] == captured, "rollback verifies per-device results and the captured UUID")
                manager.AddMethod(NAME, "CheckpointRollback", "o", "a{su}", RESTORE)
                device_props.Set(NAME+".Device", "ActiveConnection", dbus.ObjectPath(OLD_ACTIVE))
                device.UpdateProperties(NAME+".Device", {"State": dbus.UInt32(100)})
                manager.Mode("postread-failure")
                interrupted = api("activate", TARGET_UUID, "wlan0", "--yes", expected=1)
                token = interrupted["data"]["receipt"]["token"]
                check(interrupted["data"]["receipt"]["phase"] == "applied", "successful writer acknowledgement is durably separate from failed readback")
                device.UpdateProperties(NAME+".Device", {"Managed": dbus.Boolean(True)})
                inspected = api("preview", "status", token)
                check(inspected["outcome"] == "preview" and inspected["data"]["targetStateVerified"], "fresh status can verify a durably acknowledged request without repeating the writer")
                api("preview", "revert", token)
                manager.Mode("")
                wired_settings = {"connection": {"id": "Wired target", "uuid": TARGET_UUID, "type": "802-3-ethernet"}, "802-3-ethernet": {}}
                target_mock.AddMethod(NAME+".Settings.Connection", "GetSettings", "", "a{sa{sv}}", "ret="+repr(wired_settings))
                device.UpdateProperties(NAME+".Device", {"Interface": "enp0s1", "DeviceType": dbus.UInt32(1), "State": dbus.UInt32(30)})
                device_props.Set(NAME+".Device", "ActiveConnection", dbus.ObjectPath("/"))
                wired = api("activate", TARGET_UUID, "enp0s1", "--yes")
                check(wired["outcome"] == "preview" and wired["data"]["state"]["type"] == 1, "ordinary Ethernet activation uses the same durable preview lifecycle")
                api("preview", "keep", wired["data"]["receipt"]["token"], "--yes")
                target_mock.AddMethod(NAME+".Settings.Connection", "GetSettings", "", "a{sa{sv}}", "ret="+repr(open_settings))
                device.UpdateProperties(NAME+".Device", {"Interface": "wlan0", "DeviceType": dbus.UInt32(2), "State": dbus.UInt32(100)})
                device_props.Set(NAME+".Device", "ActiveConnection", dbus.ObjectPath(OLD_ACTIVE))
                manager.Mode("checkpoint-denied")
                before = calls("ActivateConnection")
                denied = api("activate", TARGET_UUID, "wlan0", "--yes", expected=1)
                check(denied["error"]["code"] == "authorization_denied" and calls("ActivateConnection") == before, "checkpoint denial never falls back to an unprotected connection change")
                manager.Mode("")
                manager.Mode("secure-after-checkpoint")
                before = calls("ActivateConnection")
                changed_profile = api("activate", TARGET_UUID, "wlan0", "--yes", expected=1)
                check(changed_profile["error"]["code"] == "interaction_required" and calls("ActivateConnection") == before and "nextAction" not in changed_profile["data"], "a profile changed after checkpoint creation cannot trigger activation or an immediate panel bypass")
                target_mock.AddMethod(NAME+".Settings.Connection", "GetSettings", "", "a{sa{sv}}", "ret="+repr(open_settings))
                manager.Mode("")
                api("preview", "revert", changed_profile["data"]["receipt"]["token"])
                token = begin()
                device.UpdateProperties(NAME+".Device", {"Udi": "replacement-device"})
                before = calls("CheckpointDestroy")
                refused = api("preview", "keep", token, "--yes", expected=1)
                check(refused["error"]["code"] == "stale_target" and calls("CheckpointDestroy") == before, "reused interface names cannot keep a replacement device")
                device.UpdateProperties(NAME+".Device", {"Udi": "fixture-device-one"})
                original = json.loads(receipt_path(token).read_text())
                partial = dict(original, phase="creating", checkpoint="", createdBootMs="", expiresBootMs="", active="", requestedBootMs=str(max(0,int(time.clock_gettime(time.CLOCK_BOOTTIME)*1000)-65001)))
                receipt_path(token).write_text(json.dumps(partial))
                before = calls("CheckpointDestroy")
                unknown = api("preview", "status", token, expected=1)
                check(unknown["data"]["checkpointPresent"] is None and unknown["data"]["daemonHasCheckpoints"] and calls("CheckpointDestroy") == before, "interrupted checkpoint identity stays unknown rather than guessing ownership")
                api("preview", "keep", token, "--yes", expected=1)
                receipt_path(token).write_text(json.dumps(original))
                cp_mock = dbus.Interface(bus.get_object(NAME, original["checkpoint"]), MOCK)
                for prop, invalid, restored in [("RollbackTimeout", dbus.UInt32(61), dbus.UInt32(60)),
                                                ("Created", dbus.Int64(int(original["createdBootMs"])-1), dbus.Int64(int(original["createdBootMs"]))),
                                                ("Devices", dbus.Array([], signature="o"), dbus.Array([DEVICE], signature="o"))]:
                    cp_mock.UpdateProperties(NAME+".Checkpoint", {prop: invalid})
                    blocked = api("preview", "keep", token, "--yes", expected=1)
                    check(blocked["error"]["code"] == "stale_checkpoint" and calls("CheckpointDestroy") == before, "altered checkpoint identity refuses keep: "+prop)
                    cp_mock.UpdateProperties(NAME+".Checkpoint", {prop: restored})
                api("preview", "revert", token)
                partial = dict(json.loads(receipt_path(token).read_text()), phase="creating", checkpoint="", createdBootMs="", expiresBootMs="", active="", requestedBootMs=str(int(time.clock_gettime(time.CLOCK_BOOTTIME)*1000)))
                receipt_path(token).write_text(json.dumps(partial))
                pending = api("preview", "status", token, expected=1)
                check(pending["data"]["receipt"]["phase"] == "creating" and not pending["data"]["localMetadataRetired"], "recent unidentified creation stays blocking without guessing a result")
                api("activate", TARGET_UUID, "wlan0", "--dry-run", expected=1)
                partial['requestedBootMs'] = str(max(0,int(time.clock_gettime(time.CLOCK_BOOTTIME)*1000)-65001))
                receipt_path(token).write_text(json.dumps(partial))
                retired = api("preview", "status", token, expected=1)
                check(retired["outcome"] == "unknown" and not retired["verified"] and retired["data"]["receipt"]["phase"] == "absent" and retired["data"]["localMetadataRetired"], "expired unidentified creation can retire metadata only after a fresh same-daemon empty checkpoint inventory")
                count = calls("CheckpointCreate")
                api("activate", TARGET_UUID, "wlan0", "--dry-run")
                check(calls("CheckpointCreate") == count, "retired metadata permits a fresh read-only plan without sending another checkpoint request")
                # Failed per-device rollback must not destroy the fallback.
                token = begin()
                manager.AddMethod(NAME, "CheckpointRollback", "o", "a{su}", "ret={'"+DEVICE+"': dbus.UInt32(3)}")
                before = calls("CheckpointDestroy")
                failed = api("preview", "revert", token, expected=1)
                check(failed["error"]["code"] == "rollback_unconfirmed" and calls("CheckpointDestroy") == before, "failed rollback result retains the daemon checkpoint")
                manager.AddMethod(NAME, "CheckpointRollback", "o", "a{su}", RESTORE)
                api("preview", "revert", token)
                # Simulate process death after NM changed state but before the
                # activation method returned. The next process may revert, not keep.
                manager.Mode("hold")
                before = calls("ActivateConnection")
                child = subprocess.Popen([str(control), "network", "activate", TARGET_UUID, "wlan0", "--yes"], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                deadline = time.monotonic()+12
                while time.monotonic() < deadline:
                    index = json.loads((root/"state/omarchy-control/network-previews/current.json").read_text())
                    current_receipt = json.loads(receipt_path(index["token"]).read_text())
                    if current_receipt["phase"] == "applying" and (root/"activation-held").exists(): break
                    time.sleep(.02)
                check((root/"activation-held").exists(), "interrupted activation reached the daemon before process death")
                child.kill(); child.communicate(timeout=5)
                time.sleep(7.1)
                token = json.loads((root/"state/omarchy-control/network-previews/current.json").read_text())["token"]
                status = api("preview", "status", token)
                check(status["outcome"] == "applying" and not status["verified"], "process death preserves an inspectable unconfirmed operation")
                api("preview", "keep", token, "--yes", expected=1)
                manager.Mode("")
                reverted = api("preview", "revert", token)
                check(reverted["outcome"] == "reverted", "a different process can restore an interrupted activation")
                for i in range(150):
                    historical_token = format(i, "032x")
                    historical = dict(historical_seed, token=historical_token)
                    receipt_path(historical_token).write_text(json.dumps(historical))
                    receipt_path(historical_token).chmod(0o600)
                token = begin()
                check(len(list(receipt_path(token).parent.glob("*.json"))) <= 102, "receipt pruning retains at most 100 older terminal receipts, the current token and its index")
                api("preview", "revert", token)
                if include_expiry:
                    token = begin()
                    before = calls("CheckpointDestroy")
                    time.sleep(56)
                    late = api("preview", "keep", token, "--yes", expected=1)
                    check(not late["verified"] and calls("CheckpointDestroy") == before, "late keep never destroys the rollback fallback")
                    time.sleep(9.2)
                    expired = api("preview", "status", token, expected=1)
                    check(expired["outcome"] == "unknown" and expired["data"]["receipt"]["phase"] == "absent", "actual daemon-timer fixture expiry never becomes an inferred successful rollback")
                    check(calls("CheckpointRollback") > 0, "explicit rollback cases exercised the method independently of expiry")
                check(not manager.GetMethodCalls("CheckConnectivity"), "previews never trigger connectivity probes")
                check(not dbus.Interface(bus.get_object(NAME, PROFILE), MOCK).GetMethodCalls("GetSecrets"), "previews never request secrets")
                token = begin()
                service.terminate(); service.wait(timeout=10)
                service = subprocess.Popen([sys.executable, "-m", "dbusmock", "--session", "-t", "networkmanager"], env=env, stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic()+10
                while not bus.name_has_owner(NAME):
                    if service.poll() is not None or time.monotonic() > deadline: raise RuntimeError("replacement daemon did not start")
                    time.sleep(.05)
                changed = api("preview", "status", token, expected=1)
                check(changed["error"]["code"] == "stale_daemon" and changed["data"]["receipt"]["phase"] == "daemon_changed", "actual daemon replacement retires ownership without inventing a rollback")
                api("preview", "keep", token, "--yes", expected=1)
                replacement = dbus.Interface(bus.get_object(NAME, ROOT), MOCK)
                check(not replacement.GetMethodCalls("CheckpointDestroy") and not replacement.GetMethodCalls("CheckpointRollback"), "stale tokens never settle a replacement daemon's checkpoints")
            finally:
                service.terminate(); service.wait(timeout=10)
    return {"ok": True, "count": len(checks), "checks": checks, "expiryTested": include_expiry,
            "scope": "production Qt D-Bus CLI against private NetworkManager fixtures; no real connections, credentials, desktop or model tasks"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--include-expiry", action="store_true")
    parser.add_argument("--private-bus", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.getuid() != 0 or not Path("/.dockerenv").exists() or os.environ.get("OC_NETWORK_PREVIEW_LAB") != "1":
        parser.error("use an explicitly enabled disposable Docker lab")
    if not args.control.is_absolute() or not args.control.is_file(): parser.error("select the production Linux binary")
    if args.private_bus:
        print(json.dumps(private_case(args.control, args.include_expiry))); return
    if not args.output: parser.error("--output is required")
    result = subprocess.run(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--control", str(args.control), "--private-bus"]
                            + (["--include-expiry"] if args.include_expiry else []), capture_output=True, text=True, timeout=170)
    if result.returncode: raise RuntimeError(result.stdout+result.stderr)
    report = json.loads(result.stdout)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
