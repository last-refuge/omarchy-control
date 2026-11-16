"""Protocol/identity guards for packaged per-user agent activation."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "session_notify", Path(__file__).resolve().parents[1] / "tools/agent-lifecycle/session-notify.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

recovery_spec = importlib.util.spec_from_file_location(
    "user_recovery", Path(__file__).resolve().parents[1] / "tools/agent-lifecycle/user-recovery.py")
recovery = importlib.util.module_from_spec(recovery_spec)
recovery_spec.loader.exec_module(recovery)

stage_spec = importlib.util.spec_from_file_location(
    "stage_payload", Path(__file__).resolve().parents[1] / "tools/agent-lifecycle/stage-payload.py")
staging = importlib.util.module_from_spec(stage_spec)
stage_spec.loader.exec_module(staging)

package_spec = importlib.util.spec_from_file_location(
    "check_package", Path(__file__).resolve().parents[1] / "tools/agent-lifecycle/check-package.py")
package_check = importlib.util.module_from_spec(package_spec)
package_spec.loader.exec_module(package_check)


class PackageFixture(unittest.TestCase):
    def test_package_checks_follow_the_source_release_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            recipe = root / "pkgbuild/PKGBUILD"
            recipe.parent.mkdir()
            recipe.write_text("pkgver=0.9.42\npkgrel=1\n")
            self.assertEqual(package_check.source_version(root), "0.9.42")
            for malformed in ["pkgver=latest\n", "pkgver=0.6.0\npkgver=0.7.0\n", "pkgrel=1\n"]:
                recipe.write_text(malformed)
                with self.assertRaises(ValueError):
                    package_check.source_version(root)

    def test_alpm_chunk_boundaries_preserve_user_conflict_details(self):
        message = {"prototype": True, "users": [{"uid": 1003, "state": "unresolved", "error": 'custom "skill"\nkept'}]}
        encoded = json.dumps(message)
        chunked = "\n".join(encoded[i:i + 11] for i in range(0, len(encoded), 11))
        self.assertEqual(package_check.notifications("installing...\n" + chunked + "\nerror: hook failed\n"), [message])
        with self.assertRaises(ValueError):
            package_check.notifications(encoded[:-2])

    def test_staging_carries_canonical_skill_and_relocatable_user_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            binary = root / "control"
            binary.write_bytes(b"fixture")
            destination = root / "package"
            staging.stage(destination, binary)
            skill = destination / "usr/share/omarchy-control/agent-skills/omarchy-control/SKILL.md"
            self.assertEqual(skill.read_bytes(), (staging.SOURCE / "src/data/skill.md").read_bytes())
            wanted = destination / "usr/lib/systemd/user/default.target.wants/omarchy-control-agent-reconcile.service"
            self.assertEqual(os.readlink(wanted), "../omarchy-control-agent-reconcile.service")
            self.assertTrue(wanted.is_file())
            graphical = destination / "usr/lib/systemd/user/graphical-session.target.wants/omarchy-control-agent-reconcile.service"
            self.assertEqual(os.readlink(graphical), "../omarchy-control-agent-reconcile.service")
            self.assertTrue(graphical.is_file())
            self.assertEqual((destination / "usr/bin/omarchy-control").stat().st_mode & 0o777, 0o755)

    def test_staging_refuses_live_root_relative_and_linked_destinations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "outside").mkdir()
            (root / "linked").symlink_to(root / "outside", target_is_directory=True)
            (root / "package").mkdir()
            (root / "package/usr").symlink_to(root / "outside", target_is_directory=True)
            for destination in [Path("/"), Path("relative"), root / "linked", root / "package"]:
                with self.subTest(destination=destination), self.assertRaises(ValueError):
                    staging.stage(destination, root / "missing-binary")
            self.assertEqual(list((root / "outside").iterdir()), [])

    def test_staging_never_overwrites_existing_package_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            existing = root / "usr/share/omarchy-control/agent-skills/omarchy-control/SKILL.md"
            existing.parent.mkdir(parents=True)
            existing.write_text("custom")
            with self.assertRaises(ValueError):
                staging.stage(root, root / "missing-binary")
            self.assertEqual(existing.read_text(), "custom")
            self.assertFalse((root / "usr/bin/omarchy-control").exists())


def manager(uid):
    return {"unit": f"user@{uid}.service", "active": "active", "sub": "running"}


class PackageNotifications(unittest.TestCase):
    def test_uid_min_is_read_from_policy_instead_of_home_directory_names(self):
        self.assertEqual(bridge.regular_uid_min("# UID_MIN 500\nUID_MIN 1000 # normal users\n"), 1000)
        with self.assertRaises(ValueError):
            bridge.regular_uid_min("# no regular-user policy")

    def test_only_running_regular_user_managers_are_selected_and_deduplicated(self):
        rows = [manager(1001), manager(0), manager(999), manager(1000), manager(1001)]
        self.assertEqual(bridge.manager_uids(json.dumps(rows), 1000), [1000, 1001])
        for row in [manager("1000;id"), manager("001000"), dict(manager(1000), sub="exited")]:
            with self.assertRaises(ValueError):
                bridge.manager_uids(json.dumps([row]), 1000)

    def test_malformed_or_excessive_discovery_blocks_notifications(self):
        for value in ["{}", "bad", json.dumps([manager(1000)] * 257), json.dumps(["user@1000.service"])]:
            with self.assertRaises(ValueError):
                bridge.manager_uids(value, 1000)

    def test_install_reload_precedes_start_on_each_user_bus(self):
        calls = []
        def run(argv, timeout):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, json.dumps([manager(1000)]) if len(calls) == 1 else "", "")
        result = bridge.notify("install", runner=run, uid_min=1000)
        self.assertTrue(result["ok"])
        self.assertEqual(calls[1][-1], "daemon-reload")
        self.assertEqual(calls[2][-2:], ["start", "omarchy-control-agent-reconcile.service"])
        self.assertIn("--machine=1000@.host", calls[2])
        self.assertFalse(result["harnessDiscoveryVerified"])

    def test_remove_runs_before_payload_loss_and_reports_inactive_limit(self):
        calls = []
        def run(argv, timeout):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, json.dumps([manager(1000)]) if len(calls) == 1 else "", "")
        result = bridge.notify("remove", runner=run, uid_min=1000)
        self.assertEqual(calls[-1][-1], "omarchy-control-agent-remove.service")
        self.assertIn("retained runtime", result["inactiveUsers"])

    def test_dry_run_only_lists_and_never_contacts_user_managers(self):
        calls = []
        def run(argv, timeout):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, json.dumps([manager(1000)]), "")
        result = bridge.notify("upgrade", dry_run=True, runner=run, uid_min=1000)
        self.assertEqual(len(calls), 1)
        self.assertEqual(result["users"][0]["state"], "planned")

    def test_failed_reload_never_starts_a_service_and_other_users_continue(self):
        calls = []
        def run(argv, timeout):
            calls.append(argv)
            output = json.dumps([manager(1000), manager(1001)]) if len(calls) == 1 else ""
            fail = "--machine=1000@.host" in argv
            return subprocess.CompletedProcess(argv, int(fail), output, "user bus disappeared" if fail else "")
        result = bridge.notify("install", runner=run, uid_min=1000)
        self.assertFalse(result["ok"])
        self.assertEqual([row["state"] for row in result["users"]], ["unresolved", "service_completed"])
        self.assertFalse(any("start" in call and "--machine=1000@.host" in call for call in calls))

    def test_discovery_failure_cannot_be_reported_as_no_active_users(self):
        def run(argv, timeout):
            return subprocess.CompletedProcess(argv, 1, "", "system manager unavailable")
        with self.assertRaises(RuntimeError):
            bridge.notify("install", runner=run, uid_min=1000)


class DeferredRecovery(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name).resolve() / "home"
        self.home.mkdir(mode=0o700)
        self.control = self.home / "packaged-control"
        self.control.write_bytes(b"\x7fELFexample-reviewed-control-binary")
        self.control.chmod(0o755)
        self.env = {"HOME": str(self.home), "OMARCHY_PATH": "/usr/share/omarchy"}
        self.calls = []
        self.opted_out = False
        self.remove_response = {"apiVersion": 1, "ok": True, "verified": True, "outcome": "removed"}
        self.remove_code = 0
        self.package_lock = self.home / "package.lock"

    def run_child(self, argv, environment, timeout):
        self.calls.append(argv)
        if argv[0] == "/usr/bin/systemctl":
            return subprocess.CompletedProcess(argv, 0, "", "")
        self.assertEqual(environment["HOME"], str(self.home))
        action = argv[2]
        if action == "status":
            body = {"apiVersion": 1, "ok": True, "data": {"optedOut": self.opted_out}}
        elif action == "reconcile":
            body = {"apiVersion": 1, "ok": True, "verified": True, "outcome": "registered", "data": {"registrationComplete": True}}
        else:
            body = self.remove_response
        return subprocess.CompletedProcess(argv, self.remove_code if action == "remove" else 0, json.dumps(body), "")

    def runtime(self, **kwargs):
        return recovery.Runtime(self.env, self.control, runner=kwargs.pop("runner", self.run_child),
                                package_lock=self.package_lock, **kwargs)

    def test_status_is_read_only_and_does_not_prepare_autostart(self):
        result = self.runtime().run("status")
        self.assertEqual(result["phase"], "new")
        self.assertEqual(set(self.home.iterdir()), {self.control})
        self.assertEqual(self.calls, [])

    def test_prepare_captures_binary_and_user_timer_without_registering_skills(self):
        runtime = self.runtime()
        result = runtime.run("prepare")
        self.assertTrue(result["verified"])
        self.assertEqual(runtime.binary.read_bytes(), self.control.read_bytes())
        self.assertEqual(runtime.binary.stat().st_mode & 0o777, 0o700)
        self.assertEqual(os.readlink(runtime.wanted), "../" + runtime.timer.name)
        self.assertFalse(any("skill" in call for call in self.calls))
        before = runtime.state_path.read_bytes()
        inode = runtime.binary.stat().st_ino
        self.runtime().run("prepare")
        self.assertEqual(runtime.state_path.read_bytes(), before)
        self.assertEqual(runtime.binary.stat().st_ino, inode)

    def test_activation_prepares_recovery_before_publishing_registrations(self):
        runtime = self.runtime()
        result = runtime.run("activate")
        self.assertEqual(result["phase"], "registered")
        start = next(i for i, c in enumerate(self.calls) if c[-2:] == ["start", runtime.timer.name])
        registration = next(i for i, c in enumerate(self.calls) if c[1:3] == ["skill", "reconcile"])
        self.assertLess(start, registration)

    def test_opt_out_does_not_install_a_runtime_or_timer(self):
        self.opted_out = True
        runtime = self.runtime()
        self.assertEqual(runtime.run("activate")["outcome"], "opted_out")
        self.assertFalse(runtime.binary.exists())
        self.assertFalse(runtime.timer.exists())

    def test_activation_without_a_manager_registers_only_after_durable_recovery(self):
        def no_manager(argv, environment, timeout):
            if argv[0] == "/usr/bin/systemctl":
                return subprocess.CompletedProcess(argv, 1, "", "no user bus")
            if argv[1:3] == ["skill", "reconcile"]:
                self.assertTrue(runtime.service.is_file())
                self.assertTrue(runtime.timer.is_file())
                self.assertEqual(os.readlink(runtime.wanted), "../" + runtime.timer.name)
            return self.run_child(argv, environment, timeout)
        runtime = self.runtime(runner=no_manager)
        result = runtime.run("activate")
        self.assertTrue(result["verified"])
        self.assertEqual(result["outcome"], "activated")
        self.assertEqual(result["userManagerActivation"], "deferred")
        self.assertEqual(json.loads(runtime.state_path.read_text())["phase"], "registered")
        # Explicit prepare remains strict: callers asking to prepare a running
        # cleanup service must see its failed start.
        with self.assertRaisesRegex(recovery.RecoveryError, "manager"):
            runtime.run("prepare")

    def test_package_presence_and_transaction_lock_defer_without_running_control(self):
        runtime = self.runtime()
        runtime.run("prepare")
        before = runtime.state_path.read_bytes()
        self.calls.clear()
        self.assertEqual(self.runtime().run("cleanup")["outcome"], "deferred")
        self.control.unlink()
        self.package_lock.touch()
        self.assertEqual(self.runtime().run("cleanup")["outcome"], "deferred")
        self.assertEqual(self.calls, [])
        self.assertEqual(runtime.state_path.read_bytes(), before)

    def test_payload_loss_uses_retained_engine_and_retires_only_owned_runtime(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        self.calls.clear()
        result = self.runtime().run("cleanup")
        self.assertTrue(result["verified"])
        self.assertEqual(result["phase"], "retired")
        self.assertEqual(self.calls[0][0], str(runtime.binary))
        self.assertEqual(self.calls[0][1:3], ["skill", "remove"])
        self.assertTrue(runtime.state_path.exists())
        self.assertTrue(all(not os.path.lexists(p) for p in runtime.paths))

    def test_failed_or_unverified_engine_retains_runtime_for_retry(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        for body, code in [({"apiVersion": 1, "ok": True, "verified": False, "outcome": "removed"}, 0),
                           ({"apiVersion": 1, "ok": False, "outcome": "partial"}, 1), ([], 0), (None, 0)]:
            with self.subTest(body=body):
                self.remove_response, self.remove_code = body, code
                with self.assertRaises(recovery.RecoveryError):
                    self.runtime().run("cleanup")
                self.assertTrue(runtime.binary.exists())
                self.assertTrue(runtime.helper.exists())
                self.assertTrue(runtime.wanted.is_symlink())

    def test_missing_runtime_libraries_or_timeout_preserve_fallback_and_scheduler(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        def failed(argv, environment, timeout):
            return subprocess.CompletedProcess(argv, 127, "", "error while loading shared libraries")
        with self.assertRaisesRegex(recovery.RecoveryError, "runtime libraries"):
            self.runtime(runner=failed).run("cleanup")
        def timed_out(argv, environment, timeout):
            raise subprocess.TimeoutExpired(argv, timeout)
        with self.assertRaises(recovery.RecoveryError):
            self.runtime(runner=timed_out).run("cleanup")
        self.assertTrue(runtime.wanted.is_symlink())

    def test_edited_artifacts_are_never_overwritten_or_executed(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        for path in runtime.paths:
            with self.subTest(path=path):
                contents = path.read_bytes() if not path.is_symlink() else None
                if contents is None:
                    target = os.readlink(path)
                    path.unlink()
                    path.symlink_to("unrelated.timer")
                else:
                    path.write_bytes(contents + b"\nuser edit")
                self.calls.clear()
                with self.assertRaises(recovery.RecoveryError):
                    self.runtime().run("cleanup")
                self.assertEqual(self.calls, [])
                if contents is None:
                    path.unlink()
                    path.symlink_to(target)
                else:
                    path.write_bytes(contents)

    def test_state_links_and_external_config_roots_are_refused(self):
        runtime = self.runtime()
        runtime.state_path.parent.mkdir(parents=True)
        elsewhere = self.home / "personal.json"
        elsewhere.write_text("personal")
        runtime.state_path.symlink_to(elsewhere)
        with self.assertRaises(recovery.RecoveryError):
            runtime.run("prepare")
        self.assertEqual(elsewhere.read_text(), "personal")
        self.env["XDG_CONFIG_HOME"] = str(self.home.parent / "outside")
        with self.assertRaises(recovery.RecoveryError):
            self.runtime()

    def test_malformed_state_and_unknown_artifacts_block_cleanup(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        original = runtime.state_path.read_bytes()
        invalid_retired = json.loads(original)
        invalid_retired["phase"] = "retired"
        for raw in [b"broken", b'{"version":2}', original.replace(b'"version": 1', b'"version": true'),
                    json.dumps(invalid_retired).encode(), original.replace(b'"sha256": "', b'"unsupported": "', 1)]:
            runtime.state_path.write_bytes(raw)
            with self.assertRaises(recovery.RecoveryError):
                self.runtime().run("cleanup")
            self.assertEqual(runtime.state_path.read_bytes(), raw)
            self.assertTrue(runtime.binary.exists())
        runtime.state_path.write_bytes(original)

    def test_interrupted_runtime_write_accepts_only_recorded_old_or_new_state(self):
        runtime = self.runtime()
        runtime.run("prepare")
        state = json.loads(runtime.state_path.read_text())
        entry = state["artifacts"][str(runtime.binary)]
        entry["pending"] = entry["current"]
        entry["current"] = recovery.MISSING
        runtime.state_path.write_text(json.dumps(state))
        self.runtime().run("prepare")
        self.assertNotIn("pending", json.loads(runtime.state_path.read_text())["artifacts"][str(runtime.binary)])

    def test_interrupted_retirement_does_not_need_the_binary_again(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        state = json.loads(runtime.state_path.read_text())
        state["phase"] = "registrations_removed"
        state["artifacts"][str(runtime.binary)]["pending"] = recovery.MISSING
        runtime.state_path.write_text(json.dumps(state))
        runtime.binary.unlink()
        self.calls.clear()
        self.assertTrue(self.runtime().run("cleanup")["verified"])
        self.assertFalse(any("skill" in call for call in self.calls))

    def test_reinstall_during_engine_removal_preserves_recovery_for_reconciliation(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        def reinstall(argv, environment, timeout):
            if "skill" in argv:
                self.control.write_bytes(b"\x7fELFreinstalled")
            return self.run_child(argv, environment, timeout)
        result = self.runtime(runner=reinstall).run("cleanup")
        self.assertEqual(result["outcome"], "deferred_after_registration_removal")
        self.assertTrue(runtime.binary.exists())
        self.assertTrue(runtime.wanted.is_symlink())

    def test_active_user_remove_retires_runtime_before_package_payload_disappears(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.calls.clear()
        result = self.runtime().run("remove")
        self.assertTrue(result["verified"])
        self.assertEqual(self.calls[0][0], str(self.control))
        self.assertTrue(self.control.exists())

    def test_custom_service_conflict_prevents_new_registration(self):
        runtime = self.runtime()
        runtime.service.parent.mkdir(parents=True)
        runtime.service.write_text("my service")
        with self.assertRaises(recovery.RecoveryError):
            runtime.run("activate")
        self.assertEqual(runtime.service.read_text(), "my service")
        self.assertFalse(any(call[1:3] == ["skill", "reconcile"] for call in self.calls))

    def test_hardlinked_runtime_is_not_executed_or_deleted(self):
        runtime = self.runtime()
        runtime.run("prepare")
        linked = self.home / "another-name"
        os.link(runtime.binary, linked)
        self.control.unlink()
        self.calls.clear()
        with self.assertRaises(recovery.RecoveryError):
            self.runtime().run("cleanup")
        self.assertEqual(self.calls, [])
        self.assertTrue(linked.exists())

    def test_lifecycle_lock_blocks_writes_but_package_present_timer_can_defer(self):
        runtime = self.runtime()
        with runtime.lock():
            with self.assertRaisesRegex(recovery.RecoveryError, "Another lifecycle"):
                self.runtime().run("prepare")
            self.assertEqual(self.runtime().run("cleanup")["outcome"], "deferred")
        self.assertFalse(runtime.binary.exists())

    def test_late_session_context_updates_keep_storage_and_cleanup_compatible(self):
        runtime = self.runtime()
        runtime.run("activate")
        original = dict(self.env)
        self.env["OMARCHY_PATH"] = str(self.home / "alternate-omarchy")
        updated = self.runtime()
        updated.run("activate")
        self.assertEqual(updated.receipt["environment"]["OMARCHY_PATH"], self.env["OMARCHY_PATH"])
        older_unit = recovery.Runtime(original, self.control, runner=self.run_child, package_lock=self.package_lock)
        older_unit.load()
        self.assertEqual(older_unit.environment["OMARCHY_PATH"], self.env["OMARCHY_PATH"])
        self.env["XDG_CONFIG_HOME"] = str(self.home / "another-config")
        with self.assertRaisesRegex(recovery.RecoveryError, "context"):
            self.runtime().run("activate")

    def test_executable_and_unit_paths_must_stay_in_the_same_context(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.env["XDG_DATA_HOME"] = str(self.home / "new-data")
        with self.assertRaisesRegex(recovery.RecoveryError, "context"):
            self.runtime().run("prepare")
        self.assertTrue(runtime.binary.exists())

    def test_companion_only_registration_cannot_be_reported_as_activation_ready(self):
        def incomplete(argv, environment, timeout):
            result = self.run_child(argv, environment, timeout)
            if argv[1:3] == ["skill", "reconcile"]:
                body = json.loads(result.stdout)
                body["data"]["registrationComplete"] = False
                return subprocess.CompletedProcess(argv, 0, json.dumps(body), "")
            return result
        runtime = self.runtime(runner=incomplete)
        with self.assertRaisesRegex(recovery.RecoveryError, "complete stock-skill forwarding"):
            runtime.run("activate")
        self.assertTrue(runtime.binary.exists())
        self.assertEqual(json.loads(runtime.state_path.read_text())["phase"], "prepared")

    def test_unverified_child_writer_is_recorded_as_a_possible_partial_change(self):
        runtime = self.runtime()
        runtime.run("prepare")
        self.control.unlink()
        self.remove_response = {"apiVersion": 1, "ok": False, "outcome": "partial"}
        self.remove_code = 1
        attempt = self.runtime()
        with self.assertRaises(recovery.RecoveryError):
            attempt.run("cleanup")
        self.assertTrue(attempt.changed)
        self.assertTrue(runtime.wanted.is_symlink())


if __name__ == "__main__":
    unittest.main()
