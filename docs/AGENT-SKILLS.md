# Per-user agent skill registration

Control registers its companion skill and a forwarding skill named
`omarchy`. The upcoming 0.6.0 packages include automatic per-user activation and
guarded removal; explicit registration is also available. Filesystem
checks do not establish that a particular agent version loads the skills. Separate
[harness probes](AGENT-LIFECYCLE.md) now verify metadata discovery for specific
macOS and Linux versions. Model-task evidence is recorded separately.

```sh
omarchy-control skill status
omarchy-control skill plan
omarchy-control skill install --json
omarchy-control skill reconcile
omarchy-control skill remove --dry-run
omarchy-control skill remove
```

These commands use API v1 implicitly and return its JSON envelope. The original
bare `skill install` retains human output; add `--json` or `--api-version 1` for
structured output. `--help` never registers anything. `status`, `plan`, and
`install|reconcile|remove --dry-run` create no directories, locks, manifests or
skills. `plan` describes installation/reconciliation; use `remove --dry-run` to
inspect removal. Neither planning nor registration starts an agent.

## Discovery directories and current evidence

The baseline is Omarchy 4.5.0's [user provisioning
script](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-provision-user).
The following distinguishes observed provisioning destinations from actual
discovery evidence. Verified entries below are fresh macOS and disposable Arch ARM
harness metadata checks, separate from model selection and instruction execution.

| Directory relative to the user's home | Default selection | Harness evidence |
| --- | --- | --- |
| `.claude/skills` | Selected for compatibility with the original installer, unless `CLAUDE_CONFIG_DIR` is set | Claude Code 2.1.322 discovery verified on macOS and Linux |
| `.agents/skills` | Selected when it exists | Codex 0.167.1 / Pi 0.64.0 discovery verified on macOS and Linux; Claude 2.1.322 did not discover this root |
| `.codex/skills` | Selected when it exists, unless `CODEX_HOME` is set | Codex 0.167.1 discovery verified on macOS and Linux |
| `.pi/agent/skills` | Selected when it exists | Pi 0.64.0 discovery verified on macOS and Linux |
| `.gemini/config/skills` | Selected when it exists | Provisioning path only; this is not Antigravity discovery evidence |
| `.hermes/skills` | Selected when it exists | Filesystem tests only; Hermes loading pending |
| `.hermes/profiles/*/skills` | Selected for existing profiles | Filesystem tests only; profile loading pending |

Previously configured explicit targets are remembered. Existing directory aliases
are resolved and deduplicated, provided their destination remains private and
inside the user's home. This prevents duplicate writes to the same directory;
it does not prove that a harness reading multiple distinct directories deduplicates
skills. Empty directories are left in place on removal.

Custom homes, profiles outside these defaults, and other harnesses can be selected
explicitly without guessing their discovery semantics:

```sh
omarchy-control skill plan --target /home/alex/custom-agent/skills
omarchy-control skill install --target /home/alex/custom-agent/skills --json
omarchy-control skill status --target /home/alex/custom-agent/skills
```

Repeat `--target` for several directories. These must be absolute, normalized
paths inside the invoking user's home. At most 32 distinct directories are
tracked. An explicit target selects the filesystem location, not a verified
harness support claim. Custom-home environment overrides produce a warning and
skip the corresponding default; use `--target` for the actual location. This
initial implementation refuses external, shared, linked data/state directories
and redirected configured target directories rather than following them silently.
Run mutations as the actual desktop user; root and elevated contexts are refused.

## Forwarding and owned files

The generated copy of the [forwarding skill](../src/data/skill-bridge.md) contains
enough discovery instructions to work when only `omarchy` was selected. It links
to the companion for operation details and to the current [stock
skill](https://github.com/omacom/omarchy/blob/v4.5.0/default/agents/skills/omarchy/SKILL.md)
for unsupported tasks. A refusal or failed verification does not authorize a raw
file bypass.

| Resource | Location |
| --- | --- |
| Companion | `$XDG_DATA_HOME/omarchy-control/agent-skills/omarchy-control/SKILL.md` |
| Forwarding skill | `$XDG_DATA_HOME/omarchy-control/agent-skills/omarchy/SKILL.md` |
| Current upstream guides | `omarchy/.upstream` links to `$OMARCHY_PATH/default/agents/skills/omarchy` |
| Ownership, original link text, pending intent, explicit targets and opt-out | `$XDG_STATE_HOME/omarchy-control/skill-integration/manifest.json` |
| Registration lock | Beside the manifest, as `registration.lock` |

Unset XDG data/state roots use `~/.local/share` and `~/.local/state`. The payload
comes from the binary's resources and remains per-user after package removal, so
an inactive user's forwarding skill can still lead to stock guidance when the
Control binary is absent. Manual registration does not prepare automatic cleanup.
The [packaged lifecycle integration](AGENT-LIFECYCLE.md) tests deferred
unregistration for users whose private recovery runtime was prepared before
removal; unprepared legacy/manual users retain fallback guidance and need explicit
cleanup or reinstall. No package-owned Omarchy
file is patched or copied as a frozen fork.
Development symlinks are retained in the upstream path, so stock topic-guide
relative links and changes remain visible. `.upstream` is hidden because Codex
otherwise discovers the stock guide as a second `omarchy` skill. Reconciliation
migrates an older visible `upstream` reference only when its recorded ownership
still matches; custom or modified references are preserved as conflicts.

The companion is registered even if stock guidance is unavailable; the response
warns and reports `registrationComplete: false`. If the stock skill already
mentions `omarchy-control`, bridge creation/updates are skipped conservatively and
reported as `upstream_reference_detected`. An existing owned bridge remains until
removal. That text check does not prove correct upstream routing. Inspect it
before claiming integration is complete.

Only a verified link to the current stock skill, a missing entry, or an entry
whose recorded ownership still matches can be registered. Custom directories,
unrelated/dangling links, hard-linked files, unsafe ancestors and modified
generated files survive. A legacy manually installed directory without a
manifest is also a conflict, even if its name matches. Inspect and relocate that
content deliberately before retrying; it is never deleted automatically.

## Results, repair and opt-out

`registrations` reports the current link, desired target, planned action,
ownership and state for each selected directory. `artifacts` reports generated
files and the upstream link, including whether they remain writable under recorded
ownership. States include
`registered`, `unregistered`, `stale`, `conflict`, `opted_out`,
`upstream_unavailable` and `upstream_reference_detected`.

`registrationComplete` requires current companion/forwarding content, registrations
and upstream reference for the selected targets. `harnessDiscoveryVerified`
remains false in the registration response: that command does not run a harness
probe. `automaticActivation` reports detection of the root-owned packaged
activation payload; it does not establish user-manager activation or harness
loading. Mutation `verified: true` establishes only
the reported filesystem changes, including companion-only registration where
stock guidance was unavailable. Refresh a running harness using its documented
mechanism or start a fresh session; changing files cannot update a conversation's
already-loaded instructions.

```sh
omarchy-control skill opt-out
omarchy-control skill reconcile     # Reports opted_out; does not re-enable
omarchy-control skill opt-in        # Explicitly re-enables configured targets
```

Opt-out is persisted before removal is attempted, so a conflicting entry cannot
silently re-enable future reconciliation. `remove` unregisters without setting
that preference; `remove --target <directory>` affects only the selected target.
Use opt-out to keep subsequent reconciliation disabled.

Writes are serialized and atomic per file/link, with pending intent saved before
replacement. This is not an atomic transaction across directories. Safe targets
may register while a conflicting target is preserved; inspect nonzero responses
and `outcome: partial`. Interrupted intent records accept only their recorded old
or new state. `reconcile` completes installation intent; `remove` finishes owned
cleanup. Modified manifests and unsupported schema/data-home changes block work
instead of resetting ownership.

Removal restores the original raw stock link only while the current entry is
still owned. Relinks performed by Omarchy are recognized by reconciliation and
their current raw target becomes the restoration target. User edits remain in
place. Generated guidance and its references are retained together if a modified
registration or generated file prevents safe cleanup. No recursive deletion is
used. The journal/undo commands for desktop changes do not manage these files.

Package/session behavior and its tested limits are in the
[lifecycle contract](AGENT-LIFECYCLE.md); release evidence and remaining desktop
gates are tracked in the [implementation plan](CLI-AGENT-PLAN.md).
