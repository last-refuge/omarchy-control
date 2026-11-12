#!/bin/bash
# Restores apps from a profile with the real omarchy-control and real pacman, into a
# disposable pacman root under fakeroot. Nothing reaches this machine's package database:
# `pacman` and `sudo` on PATH lead to that root, and the app's own state goes to a temporary
# directory. Checks what fake commands cannot: pacman's real output, its real dependency
# resolution, and what happens when someone answers its question either way.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
BIN="$ROOT/build/omarchy-control"

fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; }

"$ROOT/bin/build" >/dev/null

# A tiny repository. pkg-app needs pkg-lib; pkg-tool stands alone, like a command-line tool;
# pkg-extra is already installed and unwanted; pkg-holder needs pkg-needed, which therefore
# cannot be removed.
mkdir -p "$WORK/repo"
mkpkg() {
  local name=$1 deps=$2 dir="$WORK/src/$1"
  mkdir -p "$dir"
  cat >"$dir/PKGBUILD" <<P
pkgname=$name
pkgver=1.0
pkgrel=1
pkgdesc="My Omarchy package check"
arch=(any)
license=(MIT)
depends=($deps)
package() { mkdir -p "\$pkgdir/usr/share/$name"; echo $name >"\$pkgdir/usr/share/$name/marker"; }
P
  (cd "$dir" && PKGDEST="$WORK/repo" makepkg -f --nodeps --noconfirm >"$WORK/make-$name.log" 2>&1) || fail "could not build $name"
}
for spec in "pkg-lib:" "pkg-app:pkg-lib" "pkg-tool:" "pkg-extra:" "pkg-needed:" "pkg-holder:pkg-needed"; do
  mkpkg "${spec%%:*}" "${spec#*:}"
done
repo-add -q "$WORK/repo/checkrepo.db.tar.gz" "$WORK"/repo/*.pkg.tar.* >/dev/null

# The disposable root, and the commands that lead there.
mkdir -p "$WORK/root/var/lib/pacman" "$WORK/cache" "$WORK/gnupg" "$WORK/none" "$WORK/bin" "$WORK/state"
cat >"$WORK/pacman.conf" <<CONF
[options]
Architecture = auto
DisableSandbox
SigLevel = Never
LocalFileSigLevel = Never

[checkrepo]
Server = file://$WORK/repo
CONF
cat >"$WORK/bin/pacman" <<W
#!/bin/bash
exec fakeroot /usr/bin/pacman --config "$WORK/pacman.conf" --root "$WORK/root" --dbpath "$WORK/root/var/lib/pacman" \\
  --cachedir "$WORK/cache" --gpgdir "$WORK/gnupg" --logfile "$WORK/pacman.log" --hookdir "$WORK/none" "\$@"
W
cat >"$WORK/bin/sudo" <<'W'
#!/bin/bash
exec "$@"
W
chmod +x "$WORK/bin/pacman" "$WORK/bin/sudo"
export PATH="$WORK/bin:$PATH"
export XDG_STATE_HOME="$WORK/state"
pacman -Sy >/dev/null 2>&1 || fail "the check repository did not sync"
pacman -S --noconfirm pkg-extra pkg-holder >/dev/null 2>&1 || fail "the starting packages did not install"

installed() { pacman -Qq "$1" >/dev/null 2>&1; }
reason() { LC_ALL=C pacman -Qi "$1" 2>/dev/null | sed -n 's/^Install Reason *: //p'; }

mkdir -p "$WORK/profile"
cat >"$WORK/profile/profile.json" <<'J'
{
  "schemaVersion": 1,
  "name": "Package check",
  "packages": [
    {"name": "pkg-app", "source": "repository", "intent": "present"},
    {"name": "pkg-tool", "source": "repository", "intent": "present"},
    {"name": "pkg-extra", "source": "repository", "intent": "absent"},
    {"name": "pkg-needed", "source": "repository", "intent": "absent"},
    {"name": "pkg-missing", "source": "repository", "intent": "present"}
  ]
}
J

plan() {
  "$BIN" profile plan "$WORK/profile" --use-saved pkg-extra --json >"$WORK/plan.json" || fail "planning failed"
  jq -r .id "$WORK/plan.json"
}
status() { jq -r --arg t "$1" '.items[] | select(.target == $t) | .status' "$WORK/plan.json"; }

PLAN=$(plan)
[ "$(status pkg-app)" = ready ] && [ "$(status pkg-tool)" = ready ] && pass "real pacman's dry run plans both installs"
jq -e '.items[] | select(.target == "pkg-app") | .transaction | length == 2' "$WORK/plan.json" >/dev/null && pass "the plan lists the dependency pkg-app brings"
[ "$(status pkg-extra)" = conflict ] && pass "an unwanted app that is not one of Omarchy's is a decision, resolved here with --use-saved"
[ "$(status pkg-needed)" = blocked ] && jq -r '.items[] | select(.target == "pkg-needed") | .reason' "$WORK/plan.json" | grep -q pkg-holder && pass "a removal pkg-holder depends on is refused, naming it"
[ "$(status pkg-missing)" = blocked ] && pass "a package no repository offers is blocked"
installed pkg-app && fail "planning installed something"

# Someone answers no at pacman's question: nothing changes, and nothing claims otherwise.
printf 'n\nn\n' | "$BIN" profile apply "$PLAN" --yes --partial --json >"$WORK/declined.json" && fail "a declined restore reported success"
installed pkg-app && fail "a declined install still installed"
installed pkg-extra || fail "a declined removal still removed"
jq -e '.items[] | select(.target == "pkg-app") | .state == "failed"' "$WORK/declined.json" >/dev/null || fail "the declined run's JSON does not report the install as failed: $(head -c 1500 "$WORK/declined.json")"
pass "declining at pacman's question leaves everything, and the run says the apps failed"

# Someone answers yes.
PLAN=$(plan)
yes | "$BIN" profile apply "$PLAN" --yes --partial --json >"$WORK/run.json" || fail "the restore failed: $(jq -r .message "$WORK/run.json")"
installed pkg-app && installed pkg-tool && pass "both apps installed"
[ "$(reason pkg-app)" = "Explicitly installed" ] && [ "$(reason pkg-lib)" = "Installed as a dependency for another package" ] && pass "install reasons are right"
installed pkg-extra && fail "pkg-extra is still installed"
pass "the unwanted app was removed"
installed pkg-needed && installed pkg-holder && pass "what was refused, and what needs it, are untouched"
[ "$(jq -r .state "$WORK/run.json")" = partial ] && pass "a restore that left items out says partial, not complete"
grep -c "\[ALPM\] transaction started" "$WORK/pacman.log" | grep -qx 3 && pass "one pacman transaction to install, one to remove, after the setup's own"

# Again: nothing to do, and pacman is not asked.
PLAN=$(plan)
[ "$(status pkg-app)" = unchanged ] && [ "$(status pkg-extra)" = unchanged ] && pass "planning again finds nothing to do"
"$BIN" profile apply "$PLAN" --yes --partial --json >"$WORK/again.json" </dev/null || fail "the second restore failed"
grep -c "\[ALPM\] transaction started" "$WORK/pacman.log" | grep -qx 3 && pass "a second restore runs no transaction"
