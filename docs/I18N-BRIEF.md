# Translator brief

For whoever translates Omarchy Control, person or agent. [I18N-PLAN.md](I18N-PLAN.md) covers how languages are added; this covers how the words should read.

## Voice

Translate as if the interface had been written in your language: short, natural, and in the register of consumer desktop software, the way Omarchy's own translations read.

- **Say what will happen.** "Restores the previous layout in 15 seconds" beats "Layout restoration timeout". Keep the English's meaning exactly, including caution: if the English says "may", so does the translation.
- **Short where space is short.** Sidebar entries, buttons, tabs, and setting names are one to three words where the language allows. Descriptions and messages can run as long as they need to.
- **Plain words over technical ones** unless the English uses the technical one on purpose.
- **Address the user the way Omarchy does in your language.** Look at Omarchy's translations (the glossary) for whether it uses a formal or informal "you", and match it everywhere.
- **Sentence case** as your language uses it: the English only capitalizes the first word and names.

## Keep exactly as they are

- Product and brand names: Omarchy, Omarchy Control, Hyprland, Walker, Waybar, NetworkManager, Bluetooth, Wi-Fi, app, font, and theme names.
- Technical tokens: DNS, DHCP, VPN, SSH, GPU, CPU, USB, HDMI, AUR, VRR, Lua, JSON, PATH, URLs, file paths, and commands.
- Key names: Super, Alt, Ctrl, Shift, Enter, Esc, Tab, F1 to F12.
- Placeholders: `%1`, `%2`, `%L1`, `%n`, and `{target}`, which the app fills with a display's name. Every placeholder in the English must appear in the translation. Their order may change to suit the grammar. Markup such as `<b>` and `&amp;` stays as is.
- Technical values: clock and date previews ("14:30", "Sun 3 May"), degrees, frequencies ("2.4 GHz"), version and model numbers, values the user types, and names with digits ("1Password", "GTK 4"). Translate month and weekday words in clock previews, and nothing else in them.

## Digits

The app formats live values (volume, brightness, counts) in your language's digits by itself. Digits written into a string follow Omarchy: where its registry gives your language its own digits (`bn` ০–৯, `fa` ۰–۹), write numbers in prose and in choice labels in those digits: durations ("৫ মিনিট"), percentages ("১২৫%"), and limits in sentences ("up to ۱۲۰ characters"). Persian also uses `٫` as the decimal sign ("۲٫۵"). The technical values listed above keep Western digits in every language. Every other language, including Arabic, uses Western digits throughout.

## Counted strings

A counted string has `%n` and takes an object of plural forms instead of a string, one per CLDR category your language uses. `pending` files list the categories under `forms`. Supply exactly those:

- **Japanese, Chinese, Korean, Thai, Vietnamese, Indonesian, Malay:** only `other`.
- **Russian, Ukrainian, Polish:** `one`, `few`, `many`, and `other`, which is shown when no count is given.
- **Arabic:** all six.

Keep `%n` in every form unless your language says the number as a word in that form.

## Regional variants

Omarchy's variants apply here too:

- `pt` is European Portuguese and `pt-BR` is Brazilian Portuguese.
- `es-MX` is Mexican Spanish.
- `zh-CN` is Simplified Chinese and `zh-TW` is Traditional Chinese in Taiwan usage.
- `no` is Norwegian Bokmål.
- `sr` is Serbian in Cyrillic and `uz` is Uzbek in Latin script.

## Working with the files

1. `tools/i18n-seed.py` has already copied Omarchy's translation of every string whose English matches Omarchy's exactly. `build/i18n/<code>.seeded.json` lists those strings with where Control uses them. Check each one against its use: the same English can mean something else here. Put any correction in a `done` file.
2. `build/i18n/<code>.glossary.json` holds Omarchy's translations of short terms. Use the same word for the same thing. Where a term has several translations, Omarchy uses it with more than one meaning; pick the one that fits.
3. For each `build/i18n/<code>/pending-NN.json`, write `done-NN.json` beside it. It's an object from each English `text` to its translation, or to an object of plural forms. `used` says where each string appears: open that file or setting when the English alone is ambiguous.
4. `tools/i18n-pending.py import <code>` merges the done files and refuses anything that breaks a rule above. Fix what it refuses and import again until `tools/i18n-pending.py export <code>` reports 0 pending.
