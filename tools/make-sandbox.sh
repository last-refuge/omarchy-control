#!/bin/bash
# Builds a synthetic machine under $1: fake commands on PATH, fake config, fictional
# values. The screenshot harness and manual runs point at this, never the real desktop.
set -eu
ROOT=$1
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,run,omarchy/default/i18n,omarchy/config/omarchy,sys-root}
H=$ROOT/home
mkdir -p "$H"/.config/{omarchy,hypr,autostart} "$H"/.local/state/omarchy/{current/theme/backgrounds,toggles/hypr,indicators,powerprofiles,defaults}

cat >"$H/.local/state/omarchy/current/theme/colors.toml" <<'T'
mode = "dark"
accent = "#ff9f0a"
selection = "#3a3a3c"
muted = "#8e8e93"
background = "#1c1c1e"
dark_background = "#161618"
darker_background = "#0e0e10"
lighter_background = "#2c2c2e"
foreground = "#f5f5f7"
dark_foreground = "#98989d"
red = "#ff5f4a"
yellow = "#ffc27a"
green = "#ff9f0a"
T
printf 'obsidian-circuit\n' >"$H/.local/state/omarchy/current/theme.name"
printf '[font]\nbase-size = 14\n' >"$H/.config/omarchy/shell.toml"

# Fictional themes and generated backgrounds, so the Appearance tiles have something to show.
sandbox_theme() {
  mkdir -p "$ROOT/omarchy/themes/$1"
  printf 'mode = "%s"\naccent = "%s"\nselection = "%s"\nbackground = "%s"\ndark_background = "%s"\nforeground = "%s"\n' "$2" "$3" "$4" "$5" "$6" "$7" >"$ROOT/omarchy/themes/$1/colors.toml"
}
sandbox_theme catppuccin dark "#cba6f7" "#45475a" "#1e1e2e" "#181825" "#cdd6f4"
sandbox_theme everforest dark "#a7c080" "#475258" "#2d353b" "#232a2e" "#d3c6aa"
sandbox_theme obsidian-circuit dark "#ff9f0a" "#3a3a3c" "#1c1c1e" "#161618" "#f5f5f7"
sandbox_theme tokyo-night dark "#7aa2f7" "#414868" "#1a1b26" "#16161e" "#c0caf5"
python3 - "$H/.local/state/omarchy/current/theme/backgrounds" <<'PY'
import math, struct, sys, zlib
def png(path, ink, seed, w=720, h=405):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            u, v = x / w, y / h
            wave = 0.0
            for i in range(4):
                edge = 0.3 + i * 0.17 + 0.14 * math.sin(u * 5.2 + seed + i * 1.4) + 0.06 * math.sin(u * 13 + i)
                wave += max(0.0, 1 - abs(v - edge) * (10 + i * 5)) ** 2 * (0.8 - i * 0.12)
            wave = min(1.0, wave)
            for c in ink:
                rows.append(int(12 + (c - 12) * wave))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))
out = sys.argv[1]
png(out + "/1-circuit.png", (230, 145, 40), 0.4)
png(out + "/2-ember.png", (214, 84, 40), 2.1)
png(out + "/3-dusk.png", (150, 90, 190), 4.2)
PY
ln -sf "$H/.local/state/omarchy/current/theme/backgrounds/1-circuit.png" "$H/.local/state/omarchy/current/background"
printf 'balanced\n' >"$H/.local/state/omarchy/powerprofiles/ac"
printf 'power-saver\n' >"$H/.local/state/omarchy/powerprofiles/battery"
printf 'claude\n' >"$H/.local/state/omarchy/defaults/agent"
cat >"$H/.config/omarchy/shell.json" <<'T'
{
  "version": 1,
  "bar": {"position": "top", "background": "opaque", "id": "local.harbor-bar", "layout": {"left": [{"id": "omarchy.menu"}, {"id": "omarchy.workspaces"}], "centerBegin": [{"id": "omarchy.indicators"}], "center": [{"id": "omarchy.clock"}], "centerEnd": [{"id": "omarchy.weather"}, {"id": "omarchy.system-update"}], "right": [{"id": "omarchy.tray"}, {"id": "omarchy.keyboard-layout"}, {"id": "omarchy.tailscale"}, {"id": "local.notes"}, {"id": "omarchy.bluetooth"}, {"id": "omarchy.network"}, {"id": "omarchy.audio"}, {"id": "omarchy.monitor"}, {"id": "omarchy.power"}], "tray": []}},
  "dock": {"pinned": ["firefox", "foot", "org.gnome.Nautilus", "omacalc", "not-installed-app"]},
  "idle": {"screensaver": 150, "lock": 300},
  "plugins": []
}
T
cp "$H/.config/omarchy/shell.json" "$ROOT/omarchy/config/omarchy/shell.json"
# Omarchy's glass, as Glassy windows and the Glassy bar load it.
mkdir -p "$ROOT/omarchy/default/hypr"
cat >"$ROOT/omarchy/default/hypr/glass.lua" <<'T'
return function()
  hl.config({
    decoration = {
      blur = {
        enabled = true,
        size = 4,
        passes = 2,
        new_optimizations = true,
        xray = false,
        contrast = 1.0,
        brightness = 0.75,
        noise = 0.01,
        vibrancy = 0.35,
        vibrancy_darkness = 0.35,
        popups = true,
        special = true,
      },
    },
  })
end
T
printf -- 'dofile("x")\nrequire("default.hypr.omarchy")\nrequire("hypr.monitors")\nrequire("hypr.autostart")\nrequire("default.hypr.toggles")\n' >"$H/.config/hypr/hyprland.lua"
# Omarchy's language registry, so a sandbox can run in any of its languages
# (or en-XA, the pseudo-language, written to ~/.config/omarchy/language).
cp "$HERE/../tests/fixtures/omarchy-locales.json" "$ROOT/omarchy/default/i18n/locales.json"
printf '[Desktop Entry]\nType=Application\nName=Sync Client\nComment=Keeps a folder in sync\nExec=sync-client\n' >"$H/.config/autostart/sync-client.desktop"
# Things the system starts at login, one of them turned off here, more than fit at first look.
X="$ROOT/sys-root/etc/xdg/autostart"
mkdir -p "$X"
for entry in "at-spi-dbus-bus:Accessibility bus:Lets assistive tools talk to apps:text-x-generic" "blueman:Bluetooth applet:Shows Bluetooth devices in the bar:" \
             "firefox-sync:Firefox:Opens your browser at login:web-browser" "gnome-keyring-pkcs11:Certificate and key storage:Unlocks keys when you log in:" \
             "gnome-keyring-secrets:Secret storage:Keeps passwords for apps:" "gnome-keyring-ssh:SSH key agent:Offers your SSH keys to apps:utilities-terminal" \
             "nm-applet:Network applet:Shows network status:" "org.gnome.Nautilus:Files:Watches removable drives:folder" \
             "polkit-agent:Authentication agent:Asks for your password when an app needs it:" "print-applet:Print queue:Shows print jobs:" \
             "tracker-miner:File indexer:Indexes files for search:text-x-generic" "user-dirs-update:User folders update:Keeps standard folder names current:folder" \
             "xdg-user-dirs:User folders:Creates standard folders on first login:folder" "xembedsniproxy:Tray icon bridge:Shows older tray icons in the bar:"; do
  IFS=: read -r id name comment icon <<<"$entry"
  printf '[Desktop Entry]\nType=Application\nName=%s\nComment=%s\nIcon=%s\nExec=%s\n' "$name" "$comment" "$icon" "$id" >"$X/$id.desktop"
done
printf '[Desktop Entry]\nType=Application\nName=Print queue\nComment=Shows print jobs\nExec=print-applet\nHidden=true\n' >"$H/.config/autostart/print-applet.desktop"
mkdir -p "$ROOT"/sys-root/sys/class/power_supply/BAT0 "$ROOT"/sys-root/sys/class/net/wlan0/wireless "$ROOT"/sys-root/sys/class/bluetooth/hci0 "$ROOT"/sys-root/sys/class/dmi/id
printf 'Battery\n' >"$ROOT/sys-root/sys/class/power_supply/BAT0/type"
printf '86\n' >"$ROOT/sys-root/sys/class/power_supply/BAT0/capacity"
printf 'Discharging\n' >"$ROOT/sys-root/sys/class/power_supply/BAT0/status"
printf '10\n' >"$ROOT/sys-root/sys/class/dmi/id/chassis_type"
printf 'Example Laptop 14\n' >"$ROOT/sys-root/sys/class/dmi/id/product_name"
printf 'Example\n' >"$ROOT/sys-root/sys/class/dmi/id/sys_vendor"
# The connectors the kernel reports, matching the fake Hyprland's two displays.
for c in card1-eDP-1 card1-DP-1; do mkdir -p "$ROOT/sys-root/sys/class/drm/$c"; printf 'connected\n' >"$ROOT/sys-root/sys/class/drm/$c/status"; done
# Fictional icon packs, drawn here, so the picker has something to show.
python3 - "$ROOT/sys-root/usr/share/icons" <<'PY'
import os, sys
root = sys.argv[1]
INDEX = """[Icon Theme]
Name=%s
Comment=A fictional pack for the sandbox
Inherits=hicolor
Directories=scalable/apps,scalable/places,scalable/mimetypes

[scalable/apps]
Size=48
MinSize=8
MaxSize=512
Type=Scalable

[scalable/places]
Size=48
MinSize=8
MaxSize=512
Type=Scalable

[scalable/mimetypes]
Size=48
MinSize=8
MaxSize=512
Type=Scalable
"""
def folder(a, b, r):
    return f'<rect x="4" y="12" width="40" height="30" rx="{r}" fill="{a}"/><path d="M4 16v-4a{r} {r} 0 0 1 {r}-{r}h12l5 5h-{17+r}z" fill="{b}"/>'
def document(a, b, r):
    return f'<rect x="10" y="4" width="28" height="40" rx="{r}" fill="#f4f1ea"/><rect x="15" y="13" width="18" height="3" fill="{a}"/><rect x="15" y="21" width="18" height="3" fill="{b}"/><rect x="15" y="29" width="11" height="3" fill="{b}"/>'
def terminal(a, b, r):
    return f'<rect x="4" y="8" width="40" height="32" rx="{r}" fill="#20232a"/><path d="M11 17l7 7-7 7" stroke="{a}" stroke-width="3" fill="none"/><rect x="22" y="29" width="12" height="3" fill="{b}"/>'
def browser(a, b, r):
    return f'<circle cx="24" cy="24" r="20" fill="{a}"/><path d="M4 24h40M24 4c8 6 8 34 0 40M24 4c-8 6-8 34 0 40" stroke="{b}" stroke-width="2.5" fill="none"/>'
def write(pack, sub, name, body):
    path = f"{root}/{pack}/scalable/{sub}"
    os.makedirs(path, exist_ok=True)
    open(f"{path}/{name}.svg", "w").write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">{body}</svg>')
for pack, name, a, b, r in [
    ("Harbor", "Harbor", "#4a90d9", "#2c6cb0", 4),
    ("Harbor-Dark", "Harbor Dark", "#5a6b86", "#3c4a60", 4),
    ("Lantern", "Lantern", "#e8a33d", "#c07f1f", 8),
    ("Moss", "Moss", "#7fa650", "#5b7f33", 2),
    ("Plum", "Plum", "#9b6bc2", "#774a9c", 10),
    ("Slate", "Slate", "#8894a3", "#677383", 0),
    ("Yaru-blue", "Yaru-blue", "#3584e4", "#1c71d8", 6),
]:
    os.makedirs(f"{root}/{pack}", exist_ok=True)
    open(f"{root}/{pack}/index.theme", "w").write(INDEX % name)
    write(pack, "places", "folder", folder(a, b, r))
    write(pack, "mimetypes", "text-x-generic", document(a, b, r))
    write(pack, "apps", "utilities-terminal", terminal(a, b, r))
    write(pack, "apps", "web-browser", browser(a, b, r))
PY
printf 'Yaru-blue' >"$H/.fake-icon-theme"
A="$ROOT/sys-root/usr/share/applications"
mkdir -p "$A"
for app in "firefox:Firefox:web-browser:x-scheme-handler/http;text/html" "foot:Foot:utilities-terminal:" "org.gnome.Nautilus:Files:folder:inode/directory" \
           "omacalc:Omacalc:text-x-generic:" "org.example.TextEditor:Text Editor:text-x-generic:text/plain" \
           "org.example.Documents:Document Viewer:text-x-generic:application/pdf" "org.example.Images:Image Viewer:text-x-generic:image/png" \
           "org.example.Media:Media Player:text-x-generic:video/mp4;audio/mpeg"; do
  IFS=: read -r id name icon mimes <<<"$app"
  printf '[Desktop Entry]\nType=Application\nName=%s\nIcon=%s\nExec=%s\nMimeType=%s\n' "$name" "$icon" "$id" "$mimes" >"$A/$id.desktop"
done
# Omarchy's launcher, only as far as the launcher icon layer reads it.
mkdir -p "$ROOT/omarchy/shell/services"
cat >"$ROOT/omarchy/shell/services/AppLibrary.qml" <<'T'
  function iconSource(icon) {
    var found = root.iconIndex[value]
    var themed = Quickshell.iconPath(value, true)
  }
  function iconIndexScanCommand() {
    return [
      'dirs="$HOME/.icons $HOME/.local/share/icons";',
      'find "$base" -path "*/apps/*" -o -path "*/devices/*"',
      'awk /.svg$/ { print; next } { png[++n] = $0 }'
    ].join(' ')
  }
T
cp "$HERE/fake-gsettings" "$ROOT/bin/gsettings"
cp "$HERE/fake-dconf" "$ROOT/bin/dconf"
cp "$HERE/fake-omarchy" "$ROOT/bin/omarchy"
cp "$HERE/fake-hyprctl" "$ROOT/bin/hyprctl"
for tool in pactl nmcli bluetoothctl systemctl timedatectl xdg-mime; do
  ln -sf "$ROOT/bin/omarchy" "$ROOT/bin/$tool"
done
# Two subvolumes of one disk, the boot partition, and an app running from an image.
cat >"$ROOT/bin/df" <<'DF'
#!/bin/sh
cat <<'OUT'
Filesystem Type 1B-blocks Used Avail Use% Mounted on
/dev/nvme0n1p2 btrfs 1000000000000 420000000000 580000000000 42% /
/dev/nvme0n1p2 btrfs 1000000000000 420000000000 580000000000 42% /home
/dev/nvme0n1p1 vfat 2000000000 400000000 1600000000 20% /boot
Notes.AppImage fuse.Notes.AppImage 180000000 180000000 0 100% /tmp/.mount_NotesAb12
OUT
DF
chmod +x "$ROOT"/bin/*
for tool in powerprofilesctl firefox ghostty foot nvim code; do printf '#!/bin/sh\nexit 0\n' >"$ROOT/bin/$tool"; chmod +x "$ROOT/bin/$tool"; done
