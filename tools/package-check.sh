#!/bin/bash
# Installs, upgrades, and removes the real package in a disposable pacman root, so the
# package's own behavior is checked without touching this machine's package database.
set -eu

BASELINE=""
if [ "$#" -gt 0 ]; then
  if [ "$#" != 2 ] || [ "$1" != --upgrade-from ] || [ ! -f "$2" ]; then
    echo "Usage: tools/package-check.sh [--upgrade-from <published-package>]" >&2
    exit 2
  fi
  BASELINE="$(realpath -- "$2")"
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WORK="$(mktemp -d)"
UPGRADE="$ROOT/.package-check"
VERSION="$(sed -n 's/^pkgver=//p' "$ROOT/pkgbuild/PKGBUILD")"
trap 'rm -rf "$WORK" "$UPGRADE"' EXIT

mkdir -p "$WORK/root/var/lib/pacman" "$WORK/cache" "$WORK/gnupg" "$WORK/none" "$UPGRADE"
# This root tests package files, not user-manager activation. Explicitly mask
# the shipped hooks too: --noscriptlet alone only suppresses .INSTALL scripts.
for hook in 90-omarchy-control-agent-install.hook 90-omarchy-control-agent-remove.hook; do
  ln -s /dev/null "$WORK/none/$hook"
done
cat >"$WORK/pacman.conf" <<CONF
[options]
Architecture = auto
DisableSandbox
SigLevel = Never
LocalFileSigLevel = Never
CONF
pac() { fakeroot pacman --config "$WORK/pacman.conf" --root "$WORK/root" --dbpath "$WORK/root/var/lib/pacman" --cachedir "$WORK/cache" --gpgdir "$WORK/gnupg" --logfile "$WORK/pacman.log" --hookdir "$WORK/none" --noconfirm --nodeps --nodeps --noscriptlet --arch "$(uname -m)" "$@"; }

fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; }

"$ROOT/bin/build" >/dev/null
(cd "$ROOT/pkgbuild" && PKGDEST="$WORK/v1" BUILDDIR="$WORK/b1" makepkg -f --nodeps --noconfirm >/dev/null 2>&1) || true
mkdir -p "$WORK/v1"
(cd "$ROOT/pkgbuild" && PKGDEST="$WORK/v1" makepkg -f --nodeps --noconfirm >"$WORK/make1.log" 2>&1) || fail "package did not build"
cp "$ROOT/pkgbuild/omarchy-control.desktop" "$ROOT/pkgbuild/omarchy-control.svg" "$ROOT/pkgbuild/omarchy-control-symbolic.svg" "$UPGRADE/"
sed 's/^pkgrel=.*/pkgrel=2/' "$ROOT/pkgbuild/PKGBUILD" >"$UPGRADE/PKGBUILD"
mkdir -p "$WORK/v2"
(cd "$UPGRADE" && PKGDEST="$WORK/v2" makepkg -f --nodeps --noconfirm >"$WORK/make2.log" 2>&1) || fail "upgrade package did not build"
V1="$(ls "$WORK"/v1/omarchy-control-[0-9]*.pkg.tar.*)"
V2="$(ls "$WORK"/v2/omarchy-control-[0-9]*.pkg.tar.*)"

desktop-file-validate "$ROOT/pkgbuild/omarchy-control.desktop" && pass "desktop entry is valid"
[ -z "$(bsdtar -tf "$V1" | grep -E '^\.INSTALL$' || true)" ] && pass "no install scriptlet; declared package hooks handle user activation"
bsdtar -tf "$V1" | grep -vE '^\.|/$' | grep -qvE '^usr/' && fail "package ships files outside /usr" || pass "ships only under /usr"

# A user's own state, which removal must leave alone.
HOME_DIR="$WORK/root/home/someone/.config/omarchy-control"
mkdir -p "$HOME_DIR"
echo '{"pins":["appearance.theme"]}' >"$HOME_DIR/preferences.json"

if [ -n "$BASELINE" ]; then
  OLD_VERSION="$(bsdtar -xOf "$BASELINE" .PKGINFO | sed -n 's/^pkgver = //p')"
  [ -n "$OLD_VERSION" ] || fail "baseline package has no version"
  pac -U "$BASELINE" >"$WORK/baseline.log" 2>&1 || fail "baseline install failed: $(tail -4 "$WORK/baseline.log")"
  "$WORK/root/usr/bin/omarchy-control" --version | grep -qF "${OLD_VERSION%-*}" || fail "published baseline binary did not run"
  pass "published baseline $OLD_VERSION installs and runs before upgrade"
fi

pac -U "$V1" >"$WORK/install.log" 2>&1 || fail "install failed: $(tail -4 "$WORK/install.log")"
for f in usr/bin/omarchy-control usr/share/applications/omarchy-control.desktop usr/share/icons/hicolor/scalable/apps/omarchy-control.svg usr/share/icons/hicolor/symbolic/apps/omarchy-control-symbolic.svg usr/share/licenses/omarchy-control/LICENSE usr/lib/omarchy-control/agent-integration/user-recovery usr/lib/omarchy-control/agent-integration/session-notify usr/lib/systemd/user/omarchy-control-agent-reconcile.service usr/share/libalpm/hooks/90-omarchy-control-agent-install.hook usr/share/libalpm/hooks/90-omarchy-control-agent-remove.hook usr/share/omarchy-control/agent-skills/omarchy-control/SKILL.md; do
  [ -e "$WORK/root/$f" ] || fail "missing after install: $f"
done
pass "install places the binary, launcher entry, icons, license, canonical skill and activation payload"
for target in default.target graphical-session.target; do
  [ "$(readlink "$WORK/root/usr/lib/systemd/user/$target.wants/omarchy-control-agent-reconcile.service")" = ../omarchy-control-agent-reconcile.service ] || fail "missing relocatable $target activation link"
done
pass "default and graphical user targets activate the same reconciliation service"
[ "$(bsdtar -xOf "$V1" .PKGINFO | sed -n 's/^depend = //p' | grep -cx python)" = 1 ] || fail "package does not declare the lifecycle interpreter"
pass "package declares Python for user lifecycle services"
"$WORK/root/usr/bin/omarchy-control" --version | grep -qF "$VERSION" && pass "installed binary runs"
if [ -n "$BASELINE" ]; then
  fakeroot pacman --config "$WORK/pacman.conf" --root "$WORK/root" --dbpath "$WORK/root/var/lib/pacman" -Q omarchy-control | grep -qx "omarchy-control $VERSION-1" || fail "published baseline was not upgraded"
  [ "$(cat "$HOME_DIR/preferences.json")" = '{"pins":["appearance.theme"]}' ] || fail "published upgrade changed user preferences"
  pass "published $OLD_VERSION upgrades to $VERSION-1 and preserves user preferences (hooks disabled in this fixture root)"
fi
grep -q 'Exec=omarchy-control open' "$WORK/root/usr/share/applications/omarchy-control.desktop" && grep -q 'Keywords=Settings;Control Panel;Preferences' "$WORK/root/usr/share/applications/omarchy-control.desktop" && pass "launcher opens the window and finds Settings and Control Panel"
need="$(readelf -d "$WORK/root/usr/bin/omarchy-control" | sed -n 's/.*NEEDED.*\[\(libQt6[A-Za-z]*\).*/\1/p' | sort -u | tr '\n' ' ')"
echo "      links: $need"

pac -U "$V2" >/dev/null 2>&1 || fail "upgrade failed"
fakeroot pacman --config "$WORK/pacman.conf" --root "$WORK/root" --dbpath "$WORK/root/var/lib/pacman" -Q omarchy-control | grep -qF "$VERSION-2" || fail "upgrade did not take"
[ -x "$WORK/root/usr/bin/omarchy-control" ] && pass "upgrade replaces $VERSION-1 with $VERSION-2"
[ -f "$HOME_DIR/preferences.json" ] && pass "upgrade keeps user preferences"

pac -R omarchy-control >/dev/null 2>&1 || fail "removal failed"
[ ! -e "$WORK/root/usr/bin/omarchy-control" ] && [ ! -e "$WORK/root/usr/share/applications/omarchy-control.desktop" ] && pass "removal takes every packaged file"
[ -f "$HOME_DIR/preferences.json" ] && pass "removal keeps user preferences"
rm -rf "$ROOT"/pkgbuild/pkg "$ROOT"/pkgbuild/src "$ROOT"/pkgbuild/*.pkg.tar.*
