"""Regression checks for detecting GUI operations omitted from CLI accounting."""

import importlib.util
import json
import pathlib
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "cli_coverage", pathlib.Path(__file__).resolve().parents[1] / "tools/cli-coverage.py"
)
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class OperationAccountingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        (self.root / "src/data").mkdir(parents=True)
        for name in ["settings", "workflows"]:
            (self.root / f"src/data/{name}.json").write_text("[]")
        self.header = self.root / "src/example.h"
        self.header.write_text("class Example : public QObject {\n Q_INVOKABLE void change();\n};\n")
        self.row = dict(id="example.change", guiMethods=["Example.change"],
                        classification="cli-gap", invocations=[], reason="No CLI adapter.", apiVersions=[])
        self.registry = self.root / "src/data/cli-operations.json"
        self.registry.write_text(json.dumps([self.row]))

    def test_new_gui_action_fails_accounting_until_classified(self):
        self.assertTrue(coverage.audit(self.root)["ok"])
        self.header.write_text(self.header.read_text().replace("};", " Q_INVOKABLE void added();\n};"))
        self.assertIn("Unclassified GUI method: Example.added", coverage.audit(self.root)["problems"])

    def test_removed_action_leaves_a_detectable_stale_mapping(self):
        self.header.write_text("class Example : public QObject {};\n")
        self.assertIn("Stale GUI method: Example.change", coverage.audit(self.root)["problems"])

    def test_unrecognized_declaration_fails_instead_of_disappearing(self):
        self.header.write_text("class Example : public QObject {\n Q_INVOKABLE\n void change();\n};\n")
        with self.assertRaisesRegex(ValueError, "Unrecognized invokable"):
            coverage.audit(self.root)

    def test_duplicate_mappings_and_advertised_missing_commands_fail(self):
        self.registry.write_text(json.dumps([self.row, dict(self.row, id="other.change")]))
        self.assertIn("GUI method accounted for twice: Example.change", coverage.audit(self.root)["problems"])
        self.registry.write_text(json.dumps([dict(self.row, invocations=[["example", "change"]])]))
        self.assertFalse(coverage.audit(self.root)["ok"])

    def test_catalog_routes_and_readers_cannot_silently_escape_accounting(self):
        (self.root / "src/data/workflows.json").write_text(json.dumps([
            {"id": "example", "launch": {"kind": "new-backend"}},
            {"id": "example", "launch": {"kind": "exec", "argv": ["true"], "nativeSetting": "missing", "nativeOperations": ["missing"]}},
        ]))
        (self.root / "src/data/settings.json").write_text(json.dumps([
            {"id": "setting", "read": {"kind": "new-reader"}},
        ]))
        problems = coverage.audit(self.root)["problems"]
        self.assertIn("Duplicate catalog id: example", problems)
        self.assertIn("Unclassified catalog destination: example", problems)
        self.assertIn("Unclassified setting reader: setting", problems)
        self.assertIn("Invalid native setting alternative: example", problems)
        self.assertIn("Invalid native operation alternative: example", problems)
        self.assertIn("Missing workflow automation review: example", problems)


if __name__ == "__main__":
    unittest.main()
