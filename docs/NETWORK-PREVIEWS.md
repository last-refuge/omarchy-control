# Network preview lifecycle

The API v1 network commands use private receipts and NetworkManager checkpoints
to let separate CLI processes start, inspect and settle a connection preview.
This is not a claim of real Wi-Fi hardware or graphical-session verification.

```sh
omarchy-control network saved
omarchy-control network devices
omarchy-control network activate <uuid> <interface> --dry-run
omarchy-control network activate <uuid> <interface> --yes
omarchy-control network disconnect <interface> --dry-run
omarchy-control network disconnect <interface> --yes
omarchy-control network preview status
omarchy-control network preview status <token>
omarchy-control network preview revert <token>
omarchy-control network preview keep <token> --yes
```

Activation requires an explicit profile UUID and current interface; disconnect
requires the interface. Inputs are validated before bus access. Plans read
public profile settings, availability on the device, management/state, the
active UUID and device identity. Activation supports ordinary single-device
Ethernet and open Wi-Fi profiles. Secured Wi-Fi and 802.1X activation return
`interaction_required` with the concrete `launch panel.network` handoff.
Generated/external/volatile profiles, unmanaged devices, composite/VPN/port/
controller profiles, secondary connections and unstable device states are
refused. Disconnect can preview an existing managed single-device connection
without fetching its secrets. Already-active targets or already-disconnected
devices report `unchanged` without a writer. No profile, autoconnect preference
or credential is written. Public settings come from
[GetSettings](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.Settings.Connection.html),
which excludes secrets; Control never calls `GetSecrets` or registers a secret agent.

Both starts require `--yes` to acknowledge connectivity loss, including loss of
this remote session. A dry run creates neither checkpoint nor preview receipt.
Execution holds Control's shared change lock, honors restore leases and requires
no existing NetworkManager checkpoint. It persists intent before requesting a
60-second checkpoint for exactly the selected device, with no overlap/destroy-all
flags. Checkpoint denial never falls back to an unprotected CLI writer.
NetworkManager owns
[timed rollback](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html),
so the originating CLI need not stay alive. Daemon restart or failed driver
recovery can still prevent restoration; the timer does not prove usable recovery.

A start returns `outcome: preview`, `verified: false`, `data.receipt.token`,
`remainingSeconds` and suggested status/revert arguments. `targetStateVerified`
means NetworkManager reported the target active connection or disconnected
state. It does not commit the preview or establish internet access. The token
has 32 lowercase hex characters and works in later CLI processes sharing this
user's Control state home. Status without a token selects the latest receipt.
Keep requires `--yes` **after the user confirms the connection is usable**;
an agent must not immediately keep based on daemon readback. Keep refuses a
changed target, interrupted unconfirmed start or less than five seconds of
safety margin. Revert checks every per-device
[rollback result](https://networkmanager.dev/docs/api/latest/nm-dbus-types.html)
and observes the captured baseline before finalizing its own checkpoint.

Every request uses one pinned Qt D-Bus connection and the daemon's unique owner.
Receipts bind bus ID, daemon owner, device object path/interface/UDI/type,
baseline UUID/state, checkpoint path/boot-clock creation time/devices/timeout and
the returned active connection path. Reused names, changed devices, altered
checkpoints and unrelated later connection state cannot silently settle another
target. Reads have three-second RPC timeouts, writes five seconds, each client
stage a 25-second call budget, and state polling a 15-second deadline. Identity
strings, object lists and local receipts are bounded; Qt/D-Bus transport limits
apply to public profile maps. The nmcli/busctl process stream limits in the API
reference apply to those separate command-based adapters.

Private receipts live at
`$XDG_STATE_HOME/omarchy-control/network-previews/<token>.json`, normally under
`~/.local/state/omarchy-control/network-previews/`, with a `current.json` index.
The directory is owner-only `0700`, files are `0600`, and foreign ownership,
symlinks, public permissions, malformed/unknown schemas and extra fields are
refused. At most 100 older terminal receipts plus the indexed receipt are kept;
incomplete or unrecognized entries are preserved and bounded enumeration refuses
excessive state. This dedicated receipt is the preview journal. Generic settings
`history`/`undo` do not keep or revert it.

Status can reconcile **local receipt metadata only**. A durably acknowledged
`applied` request can become an inspectable `preview` after fresh target readback;
an interrupted writer without that acknowledgement cannot be kept by inference.
A missing checkpoint is
`unknown`, even when the observed device matches its old baseline. That does not
prove automatic rollback. After the recorded deadline plus five seconds, status
may retire an absent checkpoint when the daemon has no checkpoints. A changed
daemon retires ownership as `daemon_changed`. Neither case invents a successful
keep or rollback. A crash before checkpoint identity was saved remains unresolved
until the original 60-second request window plus five seconds has passed. Status
can then retire local metadata only if the same pinned daemon reports an empty
checkpoint inventory. It never guesses checkpoint ownership or infers rollback;
`localMetadataRetired` reports this metadata-only reconciliation. A remaining
checkpoint keeps the creation unresolved, regardless of age. Inspect status before
retrying; do not delete a receipt to bypass the refusal. Terminal receipts are historical, with
`verified: false`; repeating the same terminal keep/revert sends no duplicate
request, and a conflicting terminal action is refused. Uncertain writes are
never automatically retried.

Unresolved previews fence Control network mutations (including typed network
settings), GUI connection/scan/radio actions and new profile-restore leases.
Guards are per user/state home; other users and external NetworkManager clients
are not serialized. An external change can still be affected by the daemon's
already-armed rollback timer. The GUI discovers an owned CLI preview and exposes
status, Keep and Go back through the same token protocol. Keep stays disabled
when status is unconfirmed. Its original GUI-started connection path retains
desktop credential-agent behavior and its older local checkpoint handling,
including a checkpoint-refusal fallback. That path is a compatibility boundary;
the CLI always requires its owned checkpoint. Full convergence, driver recovery
and real-session usability require separate validation.

## Reproducible client checks

[`network-preview-check.py`](../tools/network-preview-check.py) exercises the
production Qt D-Bus CLI against isolated Wi-Fi fixtures. In an explicitly enabled
disposable Docker lab with `python3-dbusmock` and `dbus-run-session`, run:

```sh
OC_NETWORK_PREVIEW_LAB=1 python3 tools/network-preview-check.py \
  --control /absolute/path/to/omarchy-control --include-expiry \
  --output build/network-preview-tests/integration.json
```

The expiry case waits for the full fixture timer. These are real client/process
and protocol checks, including process death. They do not activate a real
connection or prove hardware rollback, a graphical secret agent or model behavior.
