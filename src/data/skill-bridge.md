---
name: omarchy
description: Customize an installed Omarchy desktop, including appearance, shell plugins, shortcuts, displays, input, power and machine settings. Discover supported changes through Omarchy Control and use the current stock topic guides for tasks outside its scope. Excludes Omarchy source development.
---

# Omarchy

For desktop settings, first check whether `omarchy-control` is available:

```sh
command -v omarchy-control
omarchy-control commands --json --api-version 1
omarchy-control search "the requested change" --json --api-version 1
```

When API v1 is available, inspect the matching setting or operation with
`describe <id> --json --api-version 1`. For a typed setting, add `--live` to read
current choices/state. Provider descriptions are static: use their advertised
discovery command for current targets, then `--dry-run` for a mutation plan.
Workflow descriptions may name `nativeSetting` or `nativeOperations`; inspect
those alternatives to complete a change through Control instead of launching
the workflow's static helper.
Plan the change, then apply within the user's authorization. Typed settings use
`get`, `plan` and `apply`. Inspect
the response's `ok`, `outcome`, `verified`, error and verification scope before
reporting success. A helper launch, partial result or pending preview does not
establish a completed change. Routine authorized changes need no repeated
permission ritual; acknowledge consequential operations only as required.

Read the [Control skill](../omarchy-control/SKILL.md) for operation-specific
selectors, recovery limits and handoffs. This forwarding skill works even when
the harness did not initially select that companion skill.

If Control is unavailable, its API is unsupported, or discovery explicitly puts
the task outside its supported scope, read the [current stock Omarchy
skill](.upstream/SKILL.md) and its adjacent topic guides. The `.upstream` directory
links to the installed Omarchy source, preserving the guides' relative links.
Do not use raw edits or another writer to bypass a Control ownership conflict,
compatibility refusal or failed verification. Report that result and its recovery
instructions. Keep package-owned Omarchy files read-only for desktop tasks.

Registration does not refresh a running conversation. Start a new harness session
or use its documented skill-refresh mechanism after installation or repair.
