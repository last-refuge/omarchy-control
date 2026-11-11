#!/usr/bin/env python3
"""Lists every translatable string and reports per-language coverage.

Framework support for a language and translated strings are separate facts, so
this is what the docs quote rather than a claim that every language is finished.
  tools/i18n-report.py              coverage per shipped catalog
  tools/i18n-report.py --list       all source strings as JSON
  tools/i18n-report.py --context    every source string with where it is used, as JSON
  tools/i18n-report.py --check      fail on a broken catalog; missing strings are only pending
  tools/i18n-report.py --strict     --check, and fail on a missing string too
  tools/i18n-report.py --prune      drop translations whose English is gone, then --check

Catalogs are keyed by the English text, so rewording a sentence leaves its old
translation behind in every language. --check fails on those, and --prune
removes them. A string added in English is pending until it is translated, and
shows in English until then (docs/I18N-PLAN.md §7).
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
REGISTRY = os.path.join(ROOT, "tests", "fixtures", "omarchy-locales.json")
CATEGORIES = {"zero", "one", "two", "few", "many", "other"}
PLACEHOLDER = re.compile(r"%L?[0-9]|%n|\{target\}")


def strings():
    """(interface, counted, catalog) source strings, and where each one is used."""
    ui, plural = set(), set()
    origins = {}

    def seen(value, where):
        origins.setdefault(value, [])
        if where not in origins[value]:
            origins[value].append(where)

    pattern = re.compile(r'\b(?:qsTr|tr)\(\s*"((?:[^"\\]|\\.)*)"(\s*,\s*""\s*,)?')
    # A count after the text (`translate(ctx, text, "", n)`) makes it a counted string.
    translate = re.compile(r'(?:qsTranslate|QT_TRANSLATE_NOOP|QCoreApplication::translate)\(\s*"[A-Za-z]+"\s*,\s*"((?:[^"\\]|\\.)*)"(\s*,\s*(?:""|nullptr)\s*,)?')
    for path in sorted(glob.glob(SRC + "/*.qml") + glob.glob(SRC + "/*.cpp")):
        text = open(path, encoding="utf-8").read()
        name = os.path.relpath(path, ROOT)
        for match in pattern.finditer(text):
            value = bytes(match.group(1), "utf-8").decode("unicode_escape").encode("latin-1", "ignore").decode("utf-8", "ignore") if "\\" in match.group(1) else match.group(1)
            (plural if match.group(2) else ui).add(value)
            seen(value, f"{name}:{text.count(chr(10), 0, match.start()) + 1}")
        for match in translate.finditer(text):
            (plural if match.group(2) else ui).add(match.group(1))
            seen(match.group(1), f"{name}:{text.count(chr(10), 0, match.start()) + 1}")

    catalog = set()

    def add(values, where):
        for value in filter(None, values):
            catalog.add(value)
            seen(value, where)

    for page in json.load(open(SRC + "/data/pages.json", encoding="utf-8")):
        where = f"page {page.get('id', page['label'])}"
        add([page["label"], page["description"], *page.get("sections", []), *page.get("jumps", [])], where)
        for row in page.get("panels", []):
            for column in row.get("columns", []):
                for panel in column:
                    add([panel.get("title"), panel.get("footnote"), (panel.get("notice") or {}).get("text"), panel.get("description")], where)
                    for group in panel.get("library", []):
                        add([group.get("heading")] + [item.get("label") for item in group.get("items", [])], where)
    for name in ("settings", "workflows"):
        for entry in json.load(open(f"{SRC}/data/{name}.json", encoding="utf-8")):
            where = f"{name[:-1]} {entry.get('id', entry['label'])}"
            add([entry["label"], entry.get("description"), entry.get("section"), entry.get("confirmText"), entry.get("confirmAlways"), entry.get("placeholder"), entry.get("restart"),
                 entry.get("multiLabel"), entry.get("multiDescription"), (entry.get("read") or {}).get("unsupported")], where)
            add(entry.get("effects", []), where)
            add([(entry.get("link") or {}).get("label")], where)
            add([c["label"] for c in entry.get("choices", [])], where)
            add(entry.get("map", {}).values() if isinstance(entry.get("map"), dict) else [], where)
    return sorted(ui - plural), sorted(plural), sorted(catalog - ui - plural), origins


def plural_categories(code):
    """The CLDR categories whole counts reach in this language, plus `other`,
    which is shown when there is no count. This mirrors Translator::pluralCategory;
    the test pluralFormsCoverEachLanguage checks the catalogs against that directly."""
    other = {"other"}
    groups = [
        ({"ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms"}, other),
        ({"fr", "pt-BR", "ca", "es-MX", "it", "pt"}, {"one", "many", "other"}),
        ({"cs", "sk", "hr", "sr", "lt", "ro"}, {"one", "few", "other"}),
        ({"pl", "ru", "uk"}, {"one", "few", "many"}),
        ({"sl"}, {"one", "two", "few", "other"}),
        ({"lv"}, {"zero", "one", "other"}),
        ({"ar"}, {"zero", "one", "two", "few", "many", "other"}),
        ({"he"}, {"one", "two", "other"}),
        ({"ga"}, {"one", "two", "few", "many", "other"}),
    ]
    for codes, categories in groups:
        if code in codes:
            return categories | other
    return {"one", "other"}


def catalogs():
    for path in sorted(glob.glob(SRC + "/i18n/*.json")):
        yield os.path.basename(path)[:-5], path


def check(strict):
    ui, plural, catalog, _ = strings()
    source = set(ui) | set(plural) | set(catalog)
    registry = json.load(open(REGISTRY, encoding="utf-8"))
    problems, pending = [], {}
    for code, path in catalogs():
        try:
            have = json.load(open(path, encoding="utf-8"))
        except ValueError as error:
            problems.append(f"{code}: not valid JSON: {error}")
            continue
        if code not in registry:
            problems.append(f"{code}: not a language in Omarchy's registry")
        for key, value in have.items():
            if key not in source:
                problems.append(f"{code}: no longer in the source (run --prune): {key!r}")
                continue
            if isinstance(value, dict):
                if key not in plural:
                    problems.append(f"{code}: plural forms for a string that is not counted: {key!r}")
                unknown = set(value) - CATEGORIES
                if unknown:
                    problems.append(f"{code}: unknown plural categories {sorted(unknown)}: {key!r}")
                missing = plural_categories(code) - set(value) if code != "en" else set()
                if missing:
                    problems.append(f"{code}: missing plural forms {sorted(missing)}: {key!r}")
                forms = [v for v in value.values() if isinstance(v, str)]
            elif isinstance(value, str):
                if key in plural and code != "en":
                    problems.append(f"{code}: a counted string needs plural forms: {key!r}")
                forms = [value]
            else:
                problems.append(f"{code}: neither text nor plural forms: {key!r}")
                continue
            expected = set(PLACEHOLDER.findall(key))
            for form in forms:
                lost = {p for p in expected - set(PLACEHOLDER.findall(form)) if p != "%n"}
                if lost:
                    problems.append(f"{code}: drops {sorted(lost)}: {key!r} -> {form!r}")
        if code != "en":
            pending[code] = sorted(source - set(have))
    for code, missing in pending.items():
        if missing:
            print(f"{code}: {len(missing)} pending", file=sys.stderr)
            if strict:
                problems.extend(f"{code}: missing: {key!r}" for key in missing)
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


def prune():
    ui, plural, catalog, _ = strings()
    source = set(ui) | set(plural) | set(catalog)
    for code, path in catalogs():
        text = open(path, encoding="utf-8").read()
        have = json.loads(text)
        kept = {key: value for key, value in have.items() if key in source}
        if len(kept) == len(have):
            continue
        indent = len(re.match(r"\{\n( *)", text).group(1)) if text.startswith("{\n ") else 1
        with open(path, "w", encoding="utf-8") as out:
            out.write(json.dumps(kept, ensure_ascii=False, indent=indent) + "\n")
        print(f"{code}: removed {len(have) - len(kept)}", file=sys.stderr)


def main():
    if "--prune" in sys.argv:
        prune()
        sys.exit(check(strict=False))
    if "--check" in sys.argv or "--strict" in sys.argv:
        sys.exit(check(strict="--strict" in sys.argv))
    ui, plural, catalog, origins = strings()
    if "--list" in sys.argv:
        print(json.dumps({"ui": ui, "plural": plural, "catalog": catalog}, ensure_ascii=False, indent=1))
        return
    if "--context" in sys.argv:
        rows = [{"text": text, "kind": kind, "used": origins.get(text, [])}
                for kind, group in (("interface", ui), ("counted", plural), ("catalog", catalog)) for text in group]
        print(json.dumps(rows, ensure_ascii=False, indent=1))
        return
    print(f"source strings: {len(ui)} interface, {len(plural)} counted, {len(catalog)} catalog")
    for code, path in catalogs():
        have = json.load(open(path, encoding="utf-8"))
        done_ui = sum(s in have for s in ui + plural)
        done_catalog = sum(s in have for s in catalog)
        print(f"{code:6} interface {done_ui}/{len(ui) + len(plural)}  catalog {done_catalog}/{len(catalog)}")


if __name__ == "__main__":
    main()
