#!/usr/bin/env python3
"""Hands a language's untranslated strings to a translator in chunks, and takes them back.

  tools/i18n-pending.py export <code> [--size N]   build/i18n/<code>/pending-NN.json
  tools/i18n-pending.py import <code>              merge build/i18n/<code>/done-NN.json

export writes what the catalog still lacks, about N strings per file (default 150).
Each entry has the English, where it is used, and for a counted string the plural
forms the language needs. A translator writes done-NN.json next to each
pending-NN.json: an object from the English to the translation, or to an object of
plural forms for a counted string.

import merges every done file into src/i18n/<code>.json, refusing an entry whose
English is not pending or that drops a placeholder, then runs the report's check.
Strings seeded from Omarchy (tools/i18n-seed.py) are in build/i18n/<code>.seeded.json;
a translator who finds one wrong for its context puts the fix in a done file too.
"""
import glob, importlib.util, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLACEHOLDER = re.compile(r"%L?[0-9]|%n")

sys.dont_write_bytecode = True   # Importing the report would leave tools/__pycache__.
spec = importlib.util.spec_from_file_location("i18n_report", os.path.join(ROOT, "tools", "i18n-report.py"))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def catalog_path(code):
    return os.path.join(ROOT, "src", "i18n", code + ".json")


def work_dir(code):
    return os.path.join(ROOT, "build", "i18n", code)


def export(code, size):
    ui, plural, catalog, origins = report.strings()
    path = catalog_path(code)
    have = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
    categories = sorted(report.plural_categories(code), key=["zero", "one", "two", "few", "many", "other"].index)
    pending = []
    for kind, group in (("counted", plural), ("interface", ui), ("catalog", catalog)):
        for text in group:
            if text in have:
                continue
            entry = {"text": text, "used": origins.get(text, [])[:4]}
            if kind == "counted":
                entry["forms"] = categories
            pending.append(entry)
    directory = work_dir(code)
    os.makedirs(directory, exist_ok=True)
    for old in glob.glob(os.path.join(directory, "pending-*.json")):
        os.remove(old)
    chunks = [pending[i:i + size] for i in range(0, len(pending), size)]
    for index, chunk in enumerate(chunks, 1):
        with open(os.path.join(directory, f"pending-{index:02}.json"), "w", encoding="utf-8") as out:
            out.write(json.dumps(chunk, ensure_ascii=False, indent=1) + "\n")
    print(f"{code}: {len(pending)} pending in {len(chunks)} files under {os.path.relpath(directory, ROOT)}", file=sys.stderr)


def import_(code):
    ui, plural, catalog, _ = report.strings()
    source = set(ui) | set(plural) | set(catalog)
    path = catalog_path(code)
    existed = os.path.exists(path)
    have = json.load(open(path, encoding="utf-8")) if existed else {}
    needed = report.plural_categories(code)
    problems, merged = [], 0
    for done in sorted(glob.glob(os.path.join(work_dir(code), "done-*.json"))):
        name = os.path.basename(done)
        try:
            entries = json.load(open(done, encoding="utf-8"))
        except ValueError as error:
            problems.append(f"{name}: not valid JSON: {error}")
            continue
        for text, value in entries.items():
            if text not in source:
                problems.append(f"{name}: not a source string: {text!r}")
                continue
            expected = set(PLACEHOLDER.findall(text)) - {"%n"}
            if text in plural:
                if not isinstance(value, dict) or not needed <= set(value):
                    problems.append(f"{name}: needs forms {sorted(needed)}: {text!r}")
                    continue
                forms = [value[category] for category in needed]
            elif isinstance(value, str) and value.strip():
                forms = [value]
            else:
                problems.append(f"{name}: needs a translation: {text!r}")
                continue
            if any(expected - set(PLACEHOLDER.findall(form)) for form in forms):
                problems.append(f"{name}: drops a placeholder: {text!r} -> {value!r}")
                continue
            have[text] = value
            merged += 1
    for problem in problems:
        print(problem, file=sys.stderr)
    with open(path, "w", encoding="utf-8") as out:
        out.write(json.dumps(have if existed else dict(sorted(have.items())), ensure_ascii=False, indent=1) + "\n")
    print(f"{code}: merged {merged}, {len(problems)} refused", file=sys.stderr)
    return 1 if problems else report.check(strict=False)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("export", "import"):
        sys.exit(__doc__)
    if sys.argv[1] == "export":
        size = int(sys.argv[sys.argv.index("--size") + 1]) if "--size" in sys.argv else 150
        export(sys.argv[2], size)
    else:
        sys.exit(import_(sys.argv[2]))


if __name__ == "__main__":
    main()
