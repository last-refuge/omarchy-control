# All of Omarchy's languages

Status: decisions settled by Jason on November 10, 2026, all as recommended (see [§8](#8-decisions)). L0 implemented the same day (see [§9](#9-as-built)). Written November 10, 2026, against Omarchy v4.5.0 and the 50-language registry on `basecamp/omarchy` main (added October 14, 2026, in basecamp/omarchy#15326).

Control speaks 5 of the 50 languages Omarchy ships: English, Arabic, Danish, German and Simplified Chinese, all four translations complete. Pick French under **Setup › Language** and the menu, the shell and Omarchy's notifications all switch, but Control stays in English. Control's own **Language** setting already offers all 50, because it lists Omarchy's registry.

**In one line:** ship a complete catalog for each of the 45 missing languages, add the checks that keep 50 catalogs honest as the English changes, and reuse Omarchy's own words wherever Control says the same thing.

---

## 1. Which languages, and why these

The usual advice for desktop software is to start with a top tier (French, Italian, German, Spanish, Japanese, Korean, both Chinese scripts, Brazilian Portuguese, Russian) and grow from there. Omarchy has already gone past that tier: its roster is omarchy.org's 50, which includes every top-tier language. Mirroring that roster exactly is the best choice for Control, for three reasons:

- **Control has no language picker of its own.** It reads `~/.config/omarchy/language`, which only ever holds a registry code. A language outside the roster can never be selected, and a roster language without a catalog shows English.
- **One desktop, one language.** Someone who picked Icelandic sees Icelandic everywhere except Control.
- **Omarchy already decided the regional variants:** `pt` is Portugal and `pt-BR` is Brazil, `es-MX` is the only Spanish, `zh-CN` and `zh-TW` are separate, `no` is Bokmål, `sr` is Cyrillic and `uz` is Latin. Control follows these choices and doesn't add `es-ES` or `sr-Latn`.

### The 45 to add

| Group | Languages | What they exercise |
|---|---|---|
| Latin script, one/other plurals | `nl` `sv` `no` `fi` `et` `hu` `tr` `sw` `uz` `is` `fil` | Text length (Finnish and Hungarian run long) |
| Latin script, richer plurals | `fr` `it` `ca` `es-MX` `pt` `pt-BR` (one/many/other), `ro` `lt` `cs` `sk` `hr` (few), `pl` (few/many), `sl` (two/few), `lv` (zero), `ga` (two/few/many) | Every CLDR category reaching the catalogs |
| Cyrillic and Greek | `ru` `uk` `bg` `sr` `el` | Few/many plurals; Inter covers these scripts |
| East and Southeast Asian | `ja` `ko` `zh-TW` `th` `vi` `id` `ms` | No plural forms; CJK and Thai fall back from Inter to Noto |
| South Asian | `hi` `bn` `ta` `si` | Indic shaping; `bn` formats with Bengali digits (০১২) |
| Right to left | `he` `fa` `ur` | Mirroring; `fa` uses Persian digits and `٫` as the decimal sign |

## 2. What already works

The runtime is ready. Adding a language today means adding one JSON file and one line in `resources.qrc`, which is how zh-CN was added in commit `77e9071`.

- **Catalogs:** `src/i18n/<code>.json` is flat JSON keyed by the English text. Counted strings are objects of CLDR categories. The tool `tools/i18n-report.py` lists 881 interface strings and 1,068 catalog strings, 1,949 in all.
- **Fallback:** `Translator` layers English, then the base language, then the regional file, so a missing string falls back one step at a time. `pt-BR` sits on top of `pt`, so any string missing from `pt-BR` shows in European Portuguese rather than English. That's only acceptable if `pt-BR` is complete.
- **Plurals:** `Translator::pluralCategory` already has CLDR rules for every one of the 50 codes. It mirrors Omarchy's `Plural.js`.
- **Numbers and dates:** these follow the registry's `formatLocale`, so the app's formatting matches the desktop.
- **Right to left:** the window mirrors itself through `QLocale::textDirection()`, and Arabic proves it works. Omarchy's own shell doesn't mirror yet, so Control is ahead of the desktop here.
- **Fonts:** Omarchy installs `noto-fonts` and `noto-fonts-cjk`, which together cover every script in the roster. Control bundles Inter (Latin, Cyrillic, Greek, Vietnamese), and Qt falls back to Noto for everything else.

## 3. Gaps to close first

Three of these are holes in today's checks; the rest are risks that grow with the number of languages.

1. **`buttonsSayWhatTheyDo` checks only da, de and ar.** The list is hard-coded, so zh-CN's verbs are already unchecked. Loop over every file in `src/i18n/` instead.
2. **Nothing fails when a catalog is incomplete.** `i18n-report.py` prints coverage but never fails. Add a `--check` mode, run as a test, that fails on:
   - a missing string;
   - an orphaned key: a translation whose English source no longer exists. Keys are the English text, so editing a sentence orphans its translation in every language without anyone noticing;
   - a plural entry missing a category its language needs, for example Arabic's `two` or Polish's `many`.
3. **`resources.qrc` is maintained by hand.** Add a test that every `src/i18n/*.json` is compiled in, or a catalog can ship without loading.
4. **Formatting isn't checked against the registry.** Add a test that, for each registry entry, `QLocale(formatLocale)` gives the registry's `zeroDigit` and `decimal`. Persian, Bengali and Arabic are where Qt's and Omarchy's defaults are most likely to disagree.
5. **Pull requests aren't tested.** The tests only run in the release workflow, when a tag is pushed. Add a workflow that runs `bin/test` on every pull request. Without it, 50 catalogs will drift.
6. **Screenshot checks only install DejaVu fonts.** The release container installs `ttf-dejavu`, so CJK, Indic and Thai text would show as boxes in a screenshot run. Install `noto-fonts` and `noto-fonts-cjk` wherever screenshots are taken.

## 4. Consistent wording with Omarchy

136 of Control's 1,949 English strings appear word for word in Omarchy's English catalogs (`default/i18n/en/{menu,shell,cli,keybindings}.json`). Most are the core nouns and verbs: *Theme*, *Font*, *Dock*, *Lock*, *Power*, *Show*, *Hide*, *Learn*, *About*.

- **Seed each language from Omarchy.** A tool, `tools/i18n-seed.py`, copies Omarchy's translation for every exact match into the new catalog before anything else is translated. That way Control's "Theme" is the same word as Omarchy's menu in every language.
- **Pass the matches on as a glossary.** The same tool writes those pairs as a per-language glossary for the translator, so related strings ("Theme pack", "Change the theme") use the same term.
- **Use Omarchy's translator brief.** Omarchy's `docs/i18n.md` sets out the register and the rules for brand names, technical tokens, key names, placeholders and regional variants. Control adopts that brief, plus its own voice notes: short, plain, and saying what will happen.

## 5. Producing the translations

The work is about 1,950 strings × 45 languages, roughly 88,000 entries in files of about 2,000 lines each. Omarchy did its 48 languages with one agent per language plus automated parity checks, and zh-CN here came from a community contributor. I recommend the same approach Omarchy used:

1. **Seed** from Omarchy (§4). Where Omarchy translates the same English more than one way, nothing is seeded: "Windows" is the operating system in Omarchy's menu, but in Control it means windows. Every seeded string is listed with where Control uses it, and the translator checks each one, since the same English can mean something else here.
2. **Translate the rest with one agent per language,** working from the brief, the glossary and each string's context: the page, the setting it belongs to, and the placeholders' meaning. A string's context comes from `i18n-report.py --list`, extended to say which file and setting each string came from.
3. **Gate the result:** the `--check` mode, placeholder parity, and the plural and qrc tests must all pass.
4. **Check the layout:** take screenshots of the busiest pages at the smallest window width in each language's script group. Catch clipped or overlapping text, and labels running into controls.
5. **Ask for native review where it counts:** open one tracking issue per language and ask native speakers to check the pilot languages and the most-used ones. Fix what they report; nothing waits on review to ship.

## 6. Phases

| Phase | Scope | Done when |
|---|---|---|
| **L0. Guardrails** | §3 items 1–6, `i18n-seed.py`, the brief, string context in `--list` | The current 5 languages pass every new check, and pull requests run the tests |
| **L1. Pilot** | `fr`, `ru`, `ja`, `fa`, `bn`: one language for each hard case (many plurals, few/many plurals, CJK, right to left with Persian digits, Indic script with Bengali digits) | All five complete, screenshots reviewed, the pipeline adjusted from what they turned up |
| **L2. Most used** | `es-MX`, `pt-BR`, `pt`, `it`, `ko`, `zh-TW`, `tr`, `pl`, `nl`, `uk`, `he` | Complete and gated |
| **L3. The rest** | The remaining 29 | Every registry code has a complete catalog; the README says 50 languages |
| **L4. Keeping up** | Section 7 | A string added in English reaches every catalog without anyone having to remember |

Each phase is one PR per language group, so a review can read one script's screenshots at a time.

## 7. Keeping 50 catalogs current

Every PR that adds or rewords English leaves 49 catalogs behind. Pick one policy:

- **A. Each PR translates its own strings** (today's practice, with the `--check` test enforcing it). It's simple, but every UI change becomes a 50-language change.
- **B. PRs add English only, and a sync job catches up.** This is Omarchy's model: the `--check` test reports missing translations as pending rather than failing, and a scheduled job (or `tools/i18n-sync.py` run locally) translates what's pending and opens one PR. Until that PR merges, users see English for the new strings, never a broken one. Orphaned keys and broken placeholders still fail at once.

I recommend **B**: it keeps feature PRs small, and the fallback to English is already safe. Note that Omarchy's sync job needs API and token secrets; running `i18n-sync.py` locally before each release avoids needing them.

## 8. Decisions

Settled November 10, 2026: all five as recommended.


1. **The roster:** mirror Omarchy's 50 exactly, with no extra variants. *Recommended.*
2. **Who translates:** agents with Omarchy seeding plus native-speaker review afterwards, or community contributions only (slower, but zh-CN shows it can work). *Recommended: agents.*
3. **Partial catalogs:** ship a language only when it's complete, or ship partial catalogs and let missing strings fall back to English. *Recommended: complete only*, so the README's count stays true.
4. **Sync policy:** A or B from §7. *Recommended: B.*
5. **Pseudo-localization:** add a fake language that lengthens every string by about 40% and wraps it in markers, to find clipping and hard-coded English before any real translation exists. It would be useful during L1 and costs about an hour. *Recommended: yes, kept to tests and screenshots, never in the language list.*

## 9. As built

### L0: guardrails (November 10, 2026)

- **`tools/i18n-report.py`** has new modes:
  - `--check` fails on translations whose English is gone, plural forms a language needs but lacks, plural categories that don't exist, a plain string where a counted one needs forms, and lost placeholders. It reports missing strings as pending.
  - `--strict` also fails on pending strings. A language PR uses it.
  - `--prune` removes translations whose English is gone.
  - `--context` lists each string with the file and line, or the page or setting, where it's used.
  - The extractor now recognises counted `QCoreApplication::translate(ctx, text, "", n)` calls, which it used to treat as plain strings.
- **Pruned 78 dead translations** each from `ar`, `da` and `de`: wording that has since changed, and features since removed. All were checked against the source, including the dynamic `qsTranslate("catalog", …)` lookups.
- **`tools/i18n-seed.py <omarchy-i18n-dir> <code>…`** seeds a catalog as described in §4 and §5. It writes `build/i18n/<code>.seeded.json` (for review) and `build/i18n/<code>.glossary.json`. French gets 129 seeded strings and a glossary of 1,082 terms.
- **New tests:**
  - every catalog is an Omarchy language and is compiled in;
  - every counted string has every form its language needs, using the app's own plural rules;
  - each registry entry's `formatLocale` gives the digits and decimal sign Omarchy specifies;
  - `--check` passes;
  - the `en-XA` pseudo-language.
  - `buttonsSayWhatTheyDo` now covers every catalog.
- **`tests/fixtures/omarchy-locales.json`** is a copy of Omarchy's registry. `tools/make-sandbox.sh` installs it, so a sandbox can run in any of the 50 languages.
- **`en-XA`** makes every string accented, about 40% longer, and bracketed, with placeholders and markup left intact. It isn't in the registry, so the picker never offers it.
- **`.github/workflows/test.yml`** runs `bin/test` and the coverage report on every pull request and on `main`, with Noto fonts installed.

