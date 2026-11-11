#!/usr/bin/env python3
"""Starts a language from Omarchy's own translations, so shared words match the desktop.

  tools/i18n-seed.py <omarchy-i18n-dir> <code>...

<omarchy-i18n-dir> is Omarchy's `default/i18n` folder: /usr/share/omarchy/default/i18n
on an Omarchy machine, or the same path in a checkout of basecamp/omarchy.

For each language:
- Every string of Control's whose English is exactly the English of one of Omarchy's
  strings gets Omarchy's translation, unless the catalog already has one. Counted
  strings are left alone, and so is a translation that does not keep the English's
  placeholders. Where Omarchy translates the same English more than one way, nothing
  is seeded: "Windows" is the operating system in one place and windows in another.
- build/i18n/<code>.seeded.json lists what was seeded, with where Control uses it.
  The same English can still mean something else here, so a translator checks each.
- build/i18n/<code>.glossary.json gets Omarchy's translations of every short string
  (four words or fewer), for whoever translates the rest to use the same terms. A
  term Omarchy translates more than one way lists every translation.
"""
import collections, importlib.util, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOGS = ["menu", "shell", "cli", "keybindings"]   # In order of precedence on a tie.
PLACEHOLDER = re.compile(r"%L?[0-9]|%n|\{target\}")

sys.dont_write_bytecode = True   # Importing the report would leave tools/__pycache__.
spec = importlib.util.spec_from_file_location("i18n_report", os.path.join(ROOT, "tools", "i18n-report.py"))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def flatten(value, path=()):
    if isinstance(value, dict):
        for key, child in value.items():
            yield from flatten(child, path + (key,))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from flatten(child, path + (str(index),))
    elif isinstance(value, str) and value.strip():
        yield path, value


def load(directory, code):
    """Omarchy's strings for one language, as {(catalog, path...): text}."""
    strings = {}
    for name in CATALOGS:
        path = os.path.join(directory, code, name + ".json")
        if os.path.exists(path):
            for key, text in flatten(json.load(open(path, encoding="utf-8"))):
                strings[(name,) + key] = text
    return strings


def seed(directory, code):
    english, local = load(directory, "en"), load(directory, code)
    if not local:
        sys.exit(f"{code}: Omarchy has no catalogs for it under {directory}")
    # English text -> every distinct translation Omarchy gives it, in catalog order.
    by_text = collections.defaultdict(list)
    for key, text in sorted(english.items(), key=lambda item: CATALOGS.index(item[0][0])):
        value = local.get(key)
        if value and set(PLACEHOLDER.findall(value)) >= set(PLACEHOLDER.findall(text)) and value not in by_text[text]:
            by_text[text].append(value)

    def only(text):
        values = by_text.get(text, [])
        return values[0] if len(values) == 1 else None

    ui, plural, catalog, origins = report.strings()
    path = os.path.join(ROOT, "src", "i18n", code + ".json")
    existed = os.path.exists(path)
    have = json.load(open(path, encoding="utf-8")) if existed else {}
    seeded = {}
    for text in ui + catalog:
        if text in have:
            continue
        value = only(text)
        if value is not None:
            have[text] = value
            seeded[text] = {"seeded": value, "used": origins.get(text, [])}
    with open(path, "w", encoding="utf-8") as out:
        # A new catalog is sorted; an existing one keeps its order, with additions at the end.
        out.write(json.dumps(have if existed else dict(sorted(have.items())), ensure_ascii=False, indent=1) + "\n")

    glossary = {text: values[0] if len(values) == 1 else values
                for text, values in by_text.items() if values and len(text.split()) <= 4}
    out_dir = os.path.join(ROOT, "build", "i18n")
    os.makedirs(out_dir, exist_ok=True)
    for name, data in (("seeded", seeded), ("glossary", glossary)):
        with open(os.path.join(out_dir, f"{code}.{name}.json"), "w", encoding="utf-8") as out:
            out.write(json.dumps(dict(sorted(data.items())), ensure_ascii=False, indent=1) + "\n")
    print(f"{code}: seeded {len(seeded)} strings from Omarchy, glossary of {len(glossary)} terms", file=sys.stderr)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    for code in sys.argv[2:]:
        seed(sys.argv[1], code)


if __name__ == "__main__":
    main()
