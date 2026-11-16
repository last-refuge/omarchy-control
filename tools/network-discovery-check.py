#!/usr/bin/python3
"""Check network discovery with real nmcli on isolated D-Bus services.

Run only in an explicitly enabled disposable Docker lab. The real daemon marks
every interface unmanaged and disables connectivity probes. A second private bus
uses python-dbusmock Wi-Fi fixtures, with the actual nmcli client. The optional
scan checks send RequestScan only to these mocks, using the actual busctl client.
Optional radio changes also affect only these mocks, never the real daemon.
Neither case changes the host network, starts an agent/model or exercises connection previews.
Requires NetworkManager, dbus-run-session, python3-dbus and python3-dbusmock.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

NAME = "org.freedesktop.NetworkManager"
MOCK = "org.freedesktop.DBus.Mock"


def run(argv, env, timeout=20):
    result = subprocess.run([str(a) for a in argv], env=env, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError("Command failed: " + str(argv) + "\n" + result.stdout + result.stderr)
    return result.stdout


def check_private_bus(mode, control, include_scan_requests=False, include_radio_changes=False):
    import dbus
    bus = dbus.SessionBus()
    if bus.name_has_owner(NAME):
        raise RuntimeError("use a fresh private D-Bus, without an existing NetworkManager service")
    checks = []
    def check(value, label):
        if not value: raise AssertionError(label)
        checks.append(label)
    with tempfile.TemporaryDirectory(prefix="oc-network-read-") as directory:
        root = Path(directory)
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"],
                   HOME=str(root), XDG_CONFIG_HOME=str(root/"config"), XDG_DATA_HOME=str(root/"data"),
                   XDG_STATE_HOME=str(root/"state"), XDG_CACHE_HOME=str(root/"cache"),
                   LC_ALL="C.UTF-8", LANGUAGE="C")
        def api(section):
            return json.loads(run([control, "network", section], env))
        def scan(*args, expected=0):
            result = subprocess.run([str(control), "network", "rescan", *args], env=env, text=True, capture_output=True, timeout=40)
            check(result.returncode == expected, "scan process exit: " + " ".join(args))
            return json.loads(result.stdout)
        def radio(*args, expected=0):
            result = subprocess.run([str(control), "network", "radio", *args], env=env, text=True, capture_output=True, timeout=40)
            check(result.returncode == expected, "radio process exit: " + " ".join(args))
            return json.loads(result.stdout)
        with (root/"daemon.log").open("w") as log:
            if mode == "real":
                config = root/"NetworkManager.conf"
                config.write_text("[main]\nplugins=keyfile\nno-auto-default=*\ndns=none\nrc-manager=unmanaged\n"
                                  "[keyfile]\nunmanaged-devices=interface-name:*\n[connectivity]\nenabled=false\n")
                argv = ["/usr/sbin/NetworkManager", "--no-daemon", "--config", str(config), "--pid-file", str(root/"nm.pid")]
            else:
                argv = [sys.executable, "-m", "dbusmock", "--session", "-t", "networkmanager", "-p", json.dumps({"Version": "1.52.1"})]
            process = subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT)
            fixture_uuid = None
            try:
                deadline = time.monotonic() + 12
                while not bus.name_has_owner(NAME):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("private service did not start:\n" + (root/"daemon.log").read_text())
                    time.sleep(.05)
                if mode == "real":
                    # An unsaved profile can still have runtime state files.
                    # Use a unique identity and delete only this owned fixture.
                    label = "Fixture: Café\\lab"
                    candidate_uuid = str(uuid.uuid4())
                    run(["nmcli", "connection", "add", "save", "no", "type", "bridge", "ifname", "oc-fixture",
                         "con-name", label, "connection.uuid", candidate_uuid, "connection.autoconnect", "no"], env)
                    fixture_uuid = candidate_uuid
                    full = api("status")
                    data = full["data"]
                    check(full["verified"] and data["complete"], "real NetworkManager reports a complete scoped read")
                    check(data["devices"] and all(row["state"] == "unmanaged" for row in data["devices"]), "real daemon leaves all container interfaces unmanaged")
                    check(any(not row["shownInGui"] for row in data["devices"]), "real inventory retains device types omitted by the GUI")
                    check(data["accessPoints"] == [], "no-Wi-Fi daemon returns a known empty AP cache")
                    profile = next(row for row in data["saved"] if row["uuid"] == fixture_uuid)
                    check(profile["name"] == label and profile["type"] == "bridge" and not profile["active"], "real profile read preserves Unicode, escapes, type and inactive state")
                    saved = api("saved")["data"]
                    check(saved["devices"] is None and saved["states"]["devices"]["status"] == "not_requested", "scoped profile reads leave other sections unrequested")
                    check(data["connectivity"] == "unknown" and not data["connectivityKnown"], "disabled connectivity probing remains explicitly unknown")
                    if include_scan_requests:
                        refused = scan("--dry-run", expected=1)
                        check(not refused["ok"] and refused["outcome"] == "blocked", "private unmanaged daemon refuses a scan plan")
                    if include_radio_changes:
                        refused = radio("true", "--dry-run", expected=1)
                        check(not refused["ok"] and refused["error"]["code"] == "unavailable", "private daemon without Wi-Fi refuses a radio plan without changing radio state")
                else:
                    mock = dbus.Interface(bus.get_object(NAME, "/org/freedesktop"), MOCK)
                    device = mock.AddWiFiDevice("one", "wlan0", 30)
                    second = mock.AddWiFiDevice("two", "wlan1", 30)
                    ssid = "Café: lab\\wire"
                    mock.AddAccessPoint(device, "first", ssid, "02:00:00:00:00:01", 2, 2412, 5400, 80, 0x100)
                    mock.AddAccessPoint(device, "second", ssid, "02:00:00:00:00:02", 2, 2412, 5400, 20, 0x100)
                    mock.AddAccessPoint(second, "other_device", ssid, "02:00:00:00:00:01", 2, 2412, 5400, 50, 0)
                    mock.AddAccessPoint(device, "empty", "", "02:00:00:00:00:03", 2, 2412, 5400, 0, 0)
                    connection = mock.AddWiFiConnection(device, "saved", ssid, "none")
                    # User language/color overrides must not change the protocol.
                    env.update(LC_ALL="de_DE.UTF-8", LANGUAGE="de", CLICOLOR_FORCE="1")
                    full = api("status")
                    data = full["data"]
                    check(full["verified"] and data["complete"], "actual nmcli reads Wi-Fi D-Bus fixtures")
                    check(len(data["accessPoints"]) == 4, "cached read preserves APs sharing names/BSSIDs across devices and an empty SSID")
                    named = [row for row in data["accessPoints"] if not row["ssidEmpty"]]
                    check(len(named) == 3 and all(row["ssid"] == ssid for row in named), "actual nmcli preserves accented names, colons and backslashes")
                    check(any(row["secured"] for row in named) and any(not row["secured"] for row in named), "same-name secured and open APs retain their reported security")
                    check({(row["device"], row["bssid"]) for row in named} == {("wlan0", "02:00:00:00:00:01"), ("wlan0", "02:00:00:00:00:02"), ("wlan1", "02:00:00:00:00:01")}, "AP identity includes both interface and BSSID")
                    check(data["saved"][0]["name"] == ssid, "actual profile-name output survives locale overrides")
                    for section in ("general", "devices", "saved", "wifi"):
                        scoped = api(section)["data"]
                        check(scoped["requestedSections"] == [section] and scoped["complete"], "actual client scoped read: " + section)
                    for path in (device, second):
                        calls = dbus.Interface(bus.get_object(NAME, path), MOCK).GetMethodCalls("RequestScan")
                        check(not calls, "cached reads do not request a Wi-Fi scan on " + str(path))
                    manager = dbus.Interface(bus.get_object(NAME, "/org/freedesktop/NetworkManager"), MOCK)
                    check(not manager.GetMethodCalls("CheckConnectivity"), "status does not trigger connectivity checking")
                    check(not dbus.Interface(bus.get_object(NAME, connection), MOCK).GetMethodCalls("GetSecrets"), "status never asks for profile secrets")
                    if include_scan_requests:
                        adapters = [dbus.Interface(bus.get_object(NAME, path), MOCK) for path in (device, second)]
                        counts = lambda: [len(adapter.GetMethodCalls("RequestScan")) for adapter in adapters]
                        plan = scan("wlan0", "--dry-run")
                        check(plan["outcome"] == "planned" and not plan["verified"] and plan["data"]["details"]["devices"] == ["wlan0"], "scan plan discovers the selected managed interface")
                        check(counts() == [0, 0] and not list(root.rglob("journal.jsonl")), "dry run sends no scan and creates no journal")
                        accepted = scan("wlan0")
                        check(accepted["outcome"] == "scan_requested" and accepted["data"]["requestAccepted"] and not accepted["verified"], "actual nmcli path resolution and busctl void reply confirm request acceptance only")
                        check(counts() == [1, 0], "explicit interface sends exactly one RequestScan")
                        check(not accepted["data"]["scanCompletionVerified"] and not accepted["data"]["accessPointAgeKnown"], "accepted request makes no scan completion or freshness claim")
                        accepted_all = scan()
                        check(accepted_all["outcome"] == "scan_requested" and len(accepted_all["data"]["results"]) == 2 and counts() == [2, 1], "default scan requests each eligible interface once")
                        adapters[1].AddMethod("org.freedesktop.NetworkManager.Device.Wireless", "RequestScan", "a{sv}", "",
                                              "raise dbus.exceptions.DBusException('Fixture scan denied', name='org.freedesktop.NetworkManager.Device.PermissionDenied')")
                        partial = scan(expected=1)
                        results = partial["data"]["results"]
                        check(partial["outcome"] == "partial" and partial["data"]["requestAccepted"] is None and not partial["verified"], "mixed real D-Bus replies stay partial and unverified")
                        check(results[0]["requestAccepted"] is True and results[1]["requestAccepted"] is None and counts() == [3, 2], "partial batch retains per-interface evidence without retry")
                        check(not manager.GetMethodCalls("CheckConnectivity") and not manager.GetMethodCalls("ActivateConnection"), "scan requests do not check connectivity or activate a profile")
                        check(not dbus.Interface(bus.get_object(NAME, connection), MOCK).GetMethodCalls("GetSecrets"), "scan requests never ask for profile secrets")
                        after = api("status")["data"]
                        check(after["wifiEnabled"] == data["wifiEnabled"] and counts() == [3, 2] and not after["accessPointAgeKnown"] and not after["scanRequested"], "later cached reads preserve radio state and do not imply fresh data or request another scan")
                    if include_radio_changes:
                        properties = dbus.Interface(bus.get_object(NAME, "/org/freedesktop/NetworkManager"), "org.freedesktop.DBus.Properties")
                        def switch_state(): return bool(properties.Get(NAME, "WirelessEnabled"))
                        def writes(): return (root/"daemon.log").read_text().count("Set /org/freedesktop/NetworkManager org.freedesktop.NetworkManager.WirelessEnabled")
                        before_writes = writes()
                        plan = radio("false", "--dry-run")
                        check(plan["outcome"] == "planned" and plan["data"]["before"] == "true" and plan["data"]["details"]["hardwareEnabled"], "real-client radio plan reads switch and hardware independently")
                        refused = radio("false", expected=1)
                        check(refused["error"]["code"] == "confirmation_required" and switch_state() and writes() == before_writes, "dry run and missing consequence acknowledgement send no property write")
                        disabled = radio("false", "--yes")
                        check(disabled["verified"] and disabled["outcome"] == "committed" and not switch_state(), "actual busctl Properties.Set and readback disable the mock switch")
                        check(disabled["data"]["verificationScope"] == "NetworkManager-radio-switch-only" and not disabled["data"]["connectionVerified"] and not disabled["data"]["internetReachabilityVerified"], "verified switch state does not establish a connection or internet access")
                        enabled = radio("true")
                        check(enabled["verified"] and switch_state() and writes() == before_writes + 2, "actual client enables Wi-Fi with exactly one write per authorized call")
                        check(not properties.Get(NAME, "WwanEnabled"), "Wi-Fi writes leave the other radio type unchanged")
                        manager.UpdateProperties(NAME, {"WirelessHardwareEnabled": dbus.Boolean(False)})
                        blocked = radio("true", expected=1)
                        check(blocked["error"]["code"] == "hardware_blocked" and writes() == before_writes + 2, "hardware-blocked enabling sends no write")
                        manager.UpdateProperties(NAME, {"WirelessHardwareEnabled": dbus.Boolean(True)})
                        manager.AddMethod("org.freedesktop.DBus.Properties", "Set", "ssv", "",
                                          "raise dbus.exceptions.DBusException('Fixture radio denied', name='org.freedesktop.NetworkManager.PermissionDenied')")
                        denied = radio("false", "--yes", expected=1)
                        check(denied["outcome"] == "partial" and not denied["verified"] and denied["data"]["readback"] is None and switch_state(), "denied real D-Bus property write remains unverified without claiming Wi-Fi off")
                        check(len(manager.GetMethodCalls("Set")) == 1, "denied radio write is not retried")
                        manager.AddMethod("org.freedesktop.DBus.Properties", "Set", "ssv", "", "")
                        ignored = radio("false", "--yes", expected=1)
                        check(ignored["outcome"] == "partial" and not ignored["verified"] and ignored["data"]["readback"]["enabled"], "successful void reply without the requested state never commits")
                        check(not manager.GetMethodCalls("CheckConnectivity") and not manager.GetMethodCalls("ActivateConnection"), "radio control sends no explicit connectivity check or connection activation")
                        check(not dbus.Interface(bus.get_object(NAME, connection), MOCK).GetMethodCalls("GetSecrets"), "radio control never asks for profile secrets")
                check(not data["scanRequested"] and not data["connectivityCheckRequested"] and not data["accessPointAgeKnown"], "result leaves scan/probe/freshness claims explicit")
            finally:
                try:
                    if fixture_uuid and process.poll() is None:
                        run(["nmcli", "connection", "delete", "uuid", fixture_uuid], env, timeout=10)
                finally:
                    process.terminate()
                    process.wait(timeout=10)
    return {"mode": mode, "checks": checks, "count": len(checks)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--include-scan-requests", action="store_true", help="also request scans on private Wi-Fi mocks only")
    parser.add_argument("--include-radio-changes", action="store_true", help="also change Wi-Fi switches on private mocks only")
    parser.add_argument("--private-bus-case", choices=("real", "mock"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.getuid() != 0 or not Path("/.dockerenv").exists() or os.environ.get("OC_NETWORK_DISCOVERY_LAB") != "1":
        parser.error("use an explicitly enabled disposable Docker lab")
    if not args.control.is_absolute() or not args.control.is_file():
        parser.error("select the absolute production Linux binary")
    if args.private_bus_case:
        print(json.dumps(check_private_bus(args.private_bus_case, args.control, args.include_scan_requests, args.include_radio_changes)))
        return
    if not args.output:
        parser.error("--output is required")
    cases = []
    for mode in ("real", "mock"):
        result = run(["dbus-run-session", "--", sys.executable, Path(__file__).resolve(), "--control", args.control,
                      "--private-bus-case", mode] + (["--include-scan-requests"] if args.include_scan_requests else [])
                      + (["--include-radio-changes"] if args.include_radio_changes else []), dict(os.environ), timeout=120)
        cases.append(json.loads(result))
    report = {"ok": True, "count": sum(case["count"] for case in cases), "cases": cases,
              "nmcliVersion": run(["nmcli", "--version"], dict(os.environ)).strip(),
              "scanRequestsOnMocks": args.include_scan_requests,
              "radioChangesOnMocks": args.include_radio_changes,
              "scope": "real nmcli and optional busctl requests against a private unmanaged NetworkManager and private Wi-Fi D-Bus fixtures; no real hardware, desktop, model task or preview mutation"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
