# Agent registration and package lifecycle

Packages built from the upcoming 0.6.0 source stage the canonical companion skill,
root notification helper, user recovery helper, pacman hooks and user units through
one shared `stage-payload.py`. Both recipes use that payload when their source
release contains it. The external Omarchy package recipe retains its existing
published 0.3.1 source checksum; that older tarball does not acquire new features
by changing a recipe. Published releases must be updated separately.

## Installation and sessions

The post-install/upgrade hook enumerates already-running regular-user managers,
then asks each manager to reload units and start the reconciliation service. Root
does not scan homes, infer the invoking desktop user from sudo variables, or edit
user registrations. Errors are reported per user without overwriting conflicts.

After pacman succeeds, the curl installer also invokes the packaged helper as
its actual non-root caller. This covers a terminal without a running manager and
reports verified registration, opt-out or unconfirmed/conflicting integration.
Root invocations do not register root. Older packages without this payload retain
their prior installation behavior. The updater's package transactions use the
same hooks; its installation result does not assert fresh harness loading.
When a user manager is unavailable, registration proceeds only after complete
owned recovery units and the next-session timer link are durable. The result
reports `userManagerActivation: deferred`, separately from verified registration.

Global dependencies activate the same oneshot service at `default.target` and
`graphical-session.target`. This handles both new manager starts and environment
import later in graphical startup. The helper prepares private recovery first,
then runs the installed CLI's `skill reconcile` as the actual user. Files and links
belong to that user. Repeated activation is idempotent. A changed stock/harness
session context can refresh while HOME and XDG storage paths remain fixed;
changed storage/control paths are refused. Old cleanup units use the recorded
context if activation updated it.

The CLI preserves explicit opt-out, custom skills and hand edits. Unrecognized
links/manifests are conflicts. Explicit custom harness roots still require
`skill install --target <directory>`; merely setting CODEX_HOME or CLAUDE_CONFIG_DIR
does not promise registration of every custom root. `skill status` reports the
presence of root-owned activation payload separately from filesystem registration,
service completion and harness loading. A running model does not automatically
refresh cached instructions.

## Removal and dependency policy

The pre-removal hook runs the owned CLI cleanup while the installed binary is
available. Inactive users are not impersonated by root. A previously prepared,
private copy of the controller/helper and a user timer settle owned registrations
at the next manager start after package loss. The registration engine owns all
skill parsing/writing; Python owns only its private runtime and scheduling files.
Captured fingerprints and durable phases permit retries after interruption.

Package presence or the pacman lock defers cleanup. Altered runtime files,
registration conflicts, malformed results, missing libraries and timeouts preserve
state and stock-guide fallback. The copied controller still needs its shared
libraries and `/usr/bin/python3`; complete dependency pruning can prevent deferred
cleanup until those requirements are restored. No guarantee of automatic cleanup
is made after removing its execution dependencies. Run `skill remove` before
uninstalling when immediate removal is required. Unprepared legacy/manual users
retain upstream fallback and need explicit cleanup/reinstallation; root never
hunts through their homes to force it.

Payload paths are under `/usr/lib/omarchy-control/agent-integration`,
`/usr/lib/systemd/user`, `/usr/share/libalpm/hooks` and
`/usr/share/omarchy-control/agent-skills`. Per-user recovery state normally lives
under `~/.local/state/omarchy-control/agent-lifecycle`, with private runtime under
`~/.local/share/omarchy-control/agent-runtime`. Generated cleanup units live below
the user's XDG config directory. Opt-out and normal preferences survive removal.

## Verification

Evidence is under `build/cli-completion/`:

- `integrated-full.log`: 938 passing Qt cases including the network GUI adoption and
  reset/undo/history API closure. Final skill prose has a separate passing
  resource/registration follow-up.
- `session-check.log` and `archarm/session-check-final.log`: 41 real user-manager
  checks on Debian 13/systemd 257 and Arch Linux ARM/systemd 262 respectively.
- `archarm/package-transactions/report.json`: 36 fixture-package checks against
  real Arch pacman and user managers.
- `archarm/production-final/report.json`: 38 checks and six real pacman
  transactions using the production recipe, source build and declared dependencies.
  Cases include direct install, upgrade, same-API downgrade, reinstall conflicts,
  opt-out, custom skills, late graphical-target environment import, removal and
  deferred inactive-user cleanup. Only fixture stock skills/users are involved.
- `no-manager.json`: five actual helper/production CLI checks for invoking-user
  registration with deferred manager scheduling.
- The Python lifecycle suite has 35 ownership, notification, staging, context and
  recovery checks. The shared stage supports both manager and graphical targets.

The ARM lab uses a signed Arch Linux ARM root filesystem, verified against the
build-system fingerprint published on its [download page](https://archlinuxarm.org/about/downloads).
The lab Dockerfiles are in `tools/agent-lifecycle`; `lab-init.sh` masks host-wide
setup services and requires a read-only `/proc/sys` bind before systemd starts.
Use a fresh disposable container for each system integration run. Never stage the
test payload on a real desktop just to run these checks.

```sh
# In an explicitly prepared disposable systemd Docker lab, with source mounted
# read-only at /work and a writable /work/build artifact directory:
OC_AGENT_LIFECYCLE_LAB=1 python3 /work/tools/agent-lifecycle/check-session.py \
  --binary /work/build/path/to/omarchy-control
OC_AGENT_LIFECYCLE_LAB=1 python3 /work/tools/agent-lifecycle/check-package.py \
  --binary /work/build/path/to/omarchy-control \
  --production-source /work/build/isolated-source-copy \
  --output /work/build/package-evidence
```

`--production-source` must point to an isolated source copy below the checkout's
build directory. Without it, the package checker uses small fixture recipes and
does not claim production dependency/recipe validation. The graphical target test
uses a disposable session service that pulls in `graphical-session.target`, whose
systemd definition deliberately refuses direct manual activation.

## Harness evidence and remaining limits

`tools/agent-discovery-check.py` registers the real bundled skills in temporary
homes, starts fresh Codex/Pi/Claude processes, then inspects their metadata/control
interfaces. Recorded macOS versions are Codex CLI 0.167.1, Pi 0.64.0 and Claude Code
2.1.322. Sixteen scenarios cover default/custom roots, aliases, conflicts, opt-out
and removal. Fifteen discover the expected skills; Claude's generic `.agents`
root is explicitly unsupported without its own registration path. The hidden
`.upstream` reference prevents duplicate `omarchy` identities in recursive discovery.

The same 16 discovery scenarios also pass on disposable Arch ARM. Discovery
checks use no model turns or real user registrations. Separate stock-only Codex
model evaluations pass five representative tasks; Pi/Claude evidence covers
metadata discovery only. See the [readiness report](CLI-AGENT-READINESS.md) for
versions, scopes and artifacts. Physical desktop acceptance, incompatible API/ABI
downgrades, full dependency pruning and unprepared legacy cleanup remain distinct
limits. File registration, a completed service job and a successfully performed
user task are separate claims.

## Release preparation checks

The upcoming release is 0.6.0; published 0.5.0 predates the managed integration.
`check-package.py` derives the version from the selected primary recipe and
checks the exact upgraded version in pacman's database. Its fixture-package mode
uses the same source version rather than a hard-coded older release.

`tools/package-check.sh --upgrade-from <verified-published-package>` additionally
installs and runs the old package before upgrading to the current source package.
This uses a disposable pacman root with the integration hooks explicitly masked;
it verifies files, executable startup, package metadata and preserved preferences.
It does not establish user-manager activation during that published upgrade.
Native systemd checks cover activation separately, and physical Omarchy acceptance
remains open. Test evidence is under `build/cli-release-0.6.0/`.

The systemd lab mounts `/tmp` with `noexec`. For checks that create executable
fixture helpers, set `TMPDIR` to a short, owned executable directory inside the
container (for example `/qa-tmp` for the test user). Keep fixture homes private;
do not redirect a real desktop session into the lab.
