# Omarchy Control

A native control center for Omarchy: one searchable place to understand the machine, change its settings, and open the right specialist tool.

Version 0.4.0 is built, tested, and verified on real Hyprland in an isolated nested session, but has not yet changed anything on a real desktop session. See [what has and has not been verified](LIVE-CHECKS.md). The plan is grounded in a read-only audit of the local Omarchy 4.5 installation on November 5, 2026.

- [Implementation plan](PLAN.md): product direction, native design, architecture, change handling, milestones, and acceptance criteria.
- [Icon theme plan](ICON-THEMES-PLAN.md): installed icon pack selection, previews, persistent overrides, local folder import, and verification gates.
- [Blur plan](BLUR-PLAN.md): dedicated blur controls, why their values load after Omarchy's toggles, and what upstream is changing.
- [Coverage map](COVERAGE.md): configuration domains, integration choices, helper availability, and the process for keeping coverage complete.
- [P0 findings](P0-FINDINGS.md): what each Omarchy command actually does, the write strategy for each provider, and what was tried and rejected.
- [Live checks](LIVE-CHECKS.md): what is verified, what is not, and the steps for a first live run.
- [My Omarchy](MY-OMARCHY.md): saving a machine's choices to a profile and restoring them elsewhere. The profile contract, which settings travel and why, the command audit behind them, and progress by milestone. The [profile schema](profile.schema.json) defines `profile.json`.
- [Coverage ledger](coverage-ledger.json): one generated row per setting and workflow, from `bin/coverage`.
- [Discovery inventory](inventory-2026-11-05.json): installed versions, command metadata, stock menu entries, plugin manifests, configuration paths, and source fingerprints.
- [Overview concept image](../branding/omarchy-control-overview.png): an illustrative mockup using the current charcoal-and-amber theme, with fictional status values. Generated with the built-in image tool from [this prompt](../branding/overview-prompt.txt).

Recommended implementation: C++ and Qt 6 Quick/QML, built with qmake6 and make, packaged as `omarchy-control`. It follows the active Omarchy theme and language, reuses existing Omarchy commands and system services, and preserves settings managed outside the application.

The [project README](../README.md) covers installing and using it.
