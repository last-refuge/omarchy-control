#pragma once

// Shell bodies for the fake commands. They live here because moc cannot tokenize
// these raw strings, and the test source includes this past its view.

namespace {

// A compositor with one display whose x position lives in a file, moved by eval.
// If `$root/lag` exists, that many monitors reads after an eval still report the
// old position. Hyprland 0.56 does this for one frame: eval returns, then the
// rule lands on the next render.
const char kStatefulCompositor[] = R"SH(
root="$(dirname "$HOME")"
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  monitors)
    if [ -f "$root/lag-left" ]; then
      left=$(cat "$root/lag-left")
      if [ "$left" -gt 0 ] 2>/dev/null; then
        printf '%s\n' "$((left - 1))" > "$root/lag-left"
      elif [ -f "$root/pending" ]; then
        cat "$root/pending" > "$root/x"
        rm -f "$root/lag-left" "$root/pending"
      fi
    fi
    printf '[{"name":"eDP-1","width":1920,"height":1200,"refreshRate":60,"x":%s,"y":0,"scale":1,"transform":0,"disabled":false,"focused":true,"mirrorOf":"none","availableModes":["1920x1200@60.00Hz"]}]' "$(cat "$root/x")";;
  eval)
    echo "${args[1]}" >> "$root/hypr-eval"
    if [ "$(cat "$root/obeys")" = "1" ]; then
      x=$(printf '%s' "${args[1]}" | sed -n 's/.*position = "\([0-9-]*\)x.*/\1/p')
      if [ -f "$root/lag" ]; then
        printf '%s\n' "$x" > "$root/pending"
        cat "$root/lag" > "$root/lag-left"
      else
        printf '%s\n' "$x" > "$root/x"
      fi
    fi;;
esac
)SH";

// Monitor names built to break out of a Lua string, as hostile EDID data could try.
const char kHostileMonitor[] = R"([{"name":"DP\"); os.execute(\"x","width":1,"height":1,"refreshRate":60,"x":0,"y":0,"scale":1,"transform":0,"availableModes":["1x1@60.00Hz"]}])";
const char kHostileMonitorNoModes[] = R"([{"name":"X\"); os.execute(\"y","width":1,"height":1,"refreshRate":60,"availableModes":[]}])";

const char kReloadsButCannotAnswer[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[],"touch":[]}';;
  configerrors) echo '[""]';;
  getoption) if grep -q repeat_rate "$HOME/.config/hypr/omarchy-control.lua"; then echo '{"int":31}'; else exit 1; fi;;
esac
)SH";

const char kScript0[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
state=")SH";
const char kScript1[] = R"SH(/hypr"
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[{"name":"test-touchpad"}],"touch":[]}';;
  configerrors) cat "$state-errors" 2>/dev/null || echo '[""]';;
  getoption) f="$state-${args[1]//:/_}"; if [ -f "$f" ]; then cat "$f"; else echo '{"int":40}'; fi;;
  reload) ;;
  binds) cat "$state-binds" 2>/dev/null || echo '[]';;
  monitors) cat "$state-monitors" 2>/dev/null || echo '[]';;
  eval) echo "${args[1]}" >> "$state-eval";;
esac
)SH";
const char kScript2[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
mod="$HOME/.config/hypr/omarchy-control.lua"
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[],"touch":[]}';;
  configerrors) echo '[""]';;
  getoption) v=$(sed -n 's/.*repeat_rate = \([0-9]*\).*/\1/p' "$mod" 2>/dev/null); echo "{\"int\":${v:-40}}";;
  reload) ;;
esac)SH";
const char kScript3[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[],"touch":[]}';;
  configerrors) if [ -f "$HOME/.config/hypr/omarchy-control.lua" ]; then echo '["old problem","bad value in omarchy-control.lua"]'; else echo '["old problem"]'; fi;;
  getoption) echo '{"int":40}';;
esac)SH";
const char kScript4[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
mod="$HOME/.config/hypr/omarchy-control.lua"
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[],"touch":[]}';;
  configerrors) echo '["an older problem in my own file"]';;
  getoption) v=$(sed -n 's/.*repeat_rate = \([0-9]*\).*/\1/p' "$mod" 2>/dev/null); echo "{\"int\":${v:-40}}";;
esac)SH";
const char kScript5[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[],"touch":[]}';;
  configerrors) echo '[""]';;
  getoption) echo '{"int":99}';;
esac)SH";
const char kScript6[] = R"SH(case "$*" in
  "-f json list sinks") echo '[{"name":"a","description":"A","volume":{},"properties":{"object.id":"1"}},{"name":"b","description":"B","volume":{},"properties":{"object.id":"2"}}]';;
  "-f json list sources"|"-f json list sink-inputs") echo '[]';;
  get-default-sink) echo a;;
esac)SH";
const char kScript7[] = R"SH(case "$*" in *CheckpointCreate*) echo 'o "/org/freedesktop/NetworkManager/Checkpoint/7"';; esac)SH";
const char kScript8[] = R"SH(case "$*" in *CheckpointCreate*) echo 'o "/org/freedesktop/NetworkManager/Checkpoint/8"';; esac)SH";
const char kScript9[] = R"SH(case "$*" in *CheckpointCreate*) echo 'o "/org/freedesktop/NetworkManager/Checkpoint/9"';; esac)SH";
const char kScript10[] = R"SH(case "$*" in *"connection up"*) echo 'secrets required' >&2; exit 4;; esac)SH";
const char kScript11[] = R"SH(case "$*" in *show*) printf 'ActiveState=failed\nResult=exit-code\n';; *list-*) echo '[]';; esac)SH";
const char kScript12[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
case "${args[0]}" in
  binds) if grep -q 'SUPER + Y' "$HOME/.config/hypr/omarchy-control.lua" 2>/dev/null; then echo '[{"modmask":64,"key":"Y","description":"Mine"}]'; else echo '[]'; fi;;
  reload|configerrors) echo '[""]';;
esac)SH";
const char kScript13[] = R"SH(case "$*" in "plugin list --json") echo '[{"id":"omarchy.a","name":"A","enabled":false}]';; esac)SH";
const char kScript14[] = R"SH(
args=("$@"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
case "${args[0]}" in
  monitors) echo '[{"name":"eDP-1","width":1,"height":1,"refreshRate":60,"availableModes":[]}]';;
  eval) exit 1;;
esac)SH";
const char kScript15[] = R"SH(case "$*" in version) echo 9.9.9;; *) exit 1;; esac)SH";
const char kScript16[] = R"SH(case "$*" in
  version) echo 4.5.0-3;;
  "shell shell ping") echo ok;;
  "hyprland corners") echo theme;;
  "plugin list --json") echo '[]';;
  "theme list"|"font list"|"powerprofiles list") echo Example;;
esac)SH";
const char kScript17[] = R"SH(case "$*" in *json*) echo '[]';; esac)SH";


// GSettings with one key, kept in a file. A file that does not exist is a key no one set.
// "-deaf" accepts a write and ignores it; "-broken" refuses it.
const char kFakeGsettings[] = R"SH(
state="$HOME/.fake-icon-theme"
case "$1" in
  get) if [ -f "$state" ]; then printf "'%s'\n" "$(cat "$state")"; else printf "'Adwaita'\n"; fi;;
  set) [ -f "$state-broken" ] && { echo "refused" >&2; exit 1; }; [ -f "$state-deaf" ] && exit 0; printf '%s' "$4" > "$state";;
  reset) rm -f "$state";;
esac
)SH";
const char kFakeDconf[] = R"SH(
state="$HOME/.fake-icon-theme"
[ -f "$state" ] && printf "'%s'\n" "$(cat "$state")"
exit 0
)SH";


// `omarchy bar move` that applies what the real one does: out first, then in at the index.
// Bash only, since the sandbox has no jq. Sections hold bare ids on one line each.
const char kFakeBar[] = R"SH(
state="$HOME/bar-state"
config="$HOME/.config/omarchy/shell.json"
write_config() {
  local out='{"bar":{"id":"local.test-bar","layout":{'
  local first=1 section
  for section in left centerBegin center centerEnd right tray; do
    [ $first = 1 ] || out+=','
    first=0
    out+="\"$section\":["
    local line="" sep="" id
    [ -f "$state/$section" ] && read -r line < "$state/$section"
    for id in $line; do out+="$sep\"$id\""; sep=','; done
    out+=']'
  done
  out+='}},"dock":{"pinned":['
  local sep=""
  for id in $(cat "$state/pins" 2>/dev/null); do out+="$sep\"$id\""; sep=','; done
  printf '%s]}}\n' "$out" > "$config"
}
case "$1 $2" in
  "bar move")
    id=$3; section=$5; index=$7; from=$9
    [ -f "$state/ignore" ] && exit 0
    read -r -a source < "$state/$from"
    kept=(); for w in "${source[@]}"; do [ "$w" = "$id" ] || kept+=("$w"); done
    printf '%s\n' "${kept[*]}" > "$state/$from"
    read -r -a target < "$state/$section"
    target=("${target[@]:0:$index}" "$id" "${target[@]:$index}")
    printf '%s\n' "${target[*]}" > "$state/$section"
    write_config;;
  "dock unpin")
    pins=$(cat "$state/pins"); out=""
    for p in $pins; do [ "$p" = "$3" ] || out+="$p "; done
    printf '%s\n' "$out" > "$state/pins"
    write_config;;
  "plugin list") echo '[{"id":"omarchy.menu","name":"Omarchy menu","kinds":["bar-widget"],"enabled":true,"active":true,"canDisable":false,"firstParty":true,"clonedFrom":""},{"id":"omarchy.clock","name":"Clock","kinds":["bar-widget"],"enabled":true,"active":true,"canDisable":true,"firstParty":true,"clonedFrom":""}]';;
  "version "*) echo 4.5.0-3;;
esac
)SH";

}  // namespace
// NetworkManager after `connection up`: the profile is listed as active.
const char kNmcliBringsUpCafe[] = R"SH(
case "$*" in
  *"connection up"*) [ -f "$(dirname "$HOME")/slow" ] && sleep 2; touch "$(dirname "$HOME")/up";;
  *"connection show"*) [ -f "$(dirname "$HOME")/up" ] && echo 'Cafe:11111111-1111-1111-1111-111111111111:802-11-wireless:yes';;
esac
exit 0)SH";

// An Omarchy machine for profile tests: each setting a test restores keeps its state in
// the sandbox, so a value read back is the value the last command left.
const char kProfileOmarchy[] = R"SH(
S="$HOME/.fake-omarchy"; mkdir -p "$S"
kv() { if [ -f "$S/$1" ]; then cat "$S/$1"; else echo "$2"; fi; }
setkv() { printf '%s\n' "$2" > "$S/$1"; }
case "$*" in
  version) echo 4.5.0-3;;
  "shell shell ping") echo ok;;
  "shell -q "*) ;;
  "theme current") [ -f "$S/theme-unreadable" ] && exit 1; kv theme "Tokyo Night";;
  "theme list") printf 'Catppuccin\nEverforest\nTokyo Night\n';;
  "theme set "*)
    t="${*:3}"
    case "$t" in
      Catppuccin|Everforest|"Tokyo Night") setkv theme "$t"; echo "$t" >> "$S/theme-sets";;
      *) echo "Theme '$t' does not exist"; exit 1;;
    esac;;
  "hyprland corners") kv corners theme;;
  "hyprland corners "*) setkv corners "$3";;
  "hyprland toggle "*)
    f="$HOME/.local/state/omarchy/toggles/hypr/$3.lua"
    if [ "$4" = on ]; then mkdir -p "$(dirname "$f")"; echo '-- toggle' > "$f"; else rm -f "$f"; fi;;
  "font current") echo "JetBrainsMono Nerd Font";;
  "font list") printf 'JetBrainsMono Nerd Font\n';;
  "font cjk") echo default;;
  "pkg defaults") cat "$HOME/.fake-pacman/baseline" 2>/dev/null;;
  "default browser") kv browser chromium;;
  "default browser "*) setkv browser "$3";;
  "default terminal") kv terminal alacritty;;
  "default editor") kv editor nvim;;
esac
exit 0
)SH";

// Hyprland as profile tests need it: an option reads what this app's module sets, and
// turning window gaps off zeroes the gaps whatever the module says, as Omarchy's toggle does.
const char kProfileHyprctl[] = R"SH(
args=("$@"); [ "${args[0]}" = "--instance" ] && args=("${args[@]:2}"); [ "${args[0]}" = "-j" ] && args=("${args[@]:1}")
mod="$HOME/.config/hypr/omarchy-control.lua"
nogaps="$HOME/.local/state/omarchy/toggles/hypr/window-no-gaps.lua"
case "${args[0]}" in
  version) echo '{"version":"0.56.2"}';;
  devices) echo '{"mice":[],"touch":[]}';;
  configerrors) echo '[""]';;
  reload) ;;
  getoption)
    opt="${args[1]}"; key="${opt##*:}"
    case "$opt" in
      input:repeat_rate) type=int; v=40;;
      input:repeat_delay) type=int; v=250;;
      input:sensitivity) type=float; v=0;;
      general:gaps_in) type=css; v=5;;
      general:gaps_out) type=css; v=10;;
      *) type=bool; v=false;;
    esac
    set=$(sed -n "s/^ *$key = \([^,]*\),\$/\1/p" "$mod" 2>/dev/null | head -n 1)
    [ -n "$set" ] && v=$set
    [ -f "$nogaps" ] && [ "${opt%%:*}" = general ] && v=0
    case $type in
      int) echo "{\"int\":$v}";;
      float) echo "{\"float\":$v}";;
      css) echo "{\"css\":\"$v $v $v $v\"}";;
      bool) echo "{\"bool\":$v}";;
    esac;;
esac
exit 0
)SH";

// pacman for profile tests, kept in the sandbox. A repository lists "repo name version deps",
// installed packages "name version reason deps". Installing an app also puts a command of
// its name on PATH, as a real package would. The lock and failures are files a test makes.
const char kFakePacman[] = R"SH(
P="$HOME/.fake-pacman"; mkdir -p "$P"; touch "$P/installed" "$P/repo"
lock="$(dirname "$HOME")/sys-root/var/lib/pacman/db.lck"
args=("$@"); names=(); after=0
for a in "${args[@]}"; do if [ $after = 1 ]; then names+=("$a"); elif [ "$a" = "--" ]; then after=1; fi; done
inst() { awk -v n="$1" '$1==n' "$P/installed"; }
repo() { awk -v n="$1" '$2==n' "$P/repo"; }
has() { [ -n "$(inst "$1")" ]; }
offered() { [ -n "$(repo "$1")" ]; }
deps_of() { repo "$1" | awk '{print $4}' | tr ',' '\n' | grep -v '^-$' | grep -v '^$'; }
record() { echo "pacman ${args[*]}" >> "$P/transactions"; }
case "${args[0]}" in
  -Q) awk '{print $1, $2}' "$P/installed";;
  -Qqe) awk '$3=="explicit"{print $1}' "$P/installed";;
  -Qqm)
    out=$(awk '{print $1}' "$P/installed" | while read -r n; do offered "$n" || echo "$n"; done)
    [ -n "$out" ] || exit 1; echo "$out";;
  -Sl) while read -r r n v d; do [ -n "$n" ] || continue; if has "$n"; then echo "$r $n $v [installed]"; else echo "$r $n $v"; fi; done < "$P/repo";;
  -T)
    # A name is satisfied when installed, or provided by something installed ("provider name").
    unmet=()
    for n in "${names[@]}"; do
      has "$n" && continue
      by=$(awk -v n="$n" '$2==n{print $1}' "$P/provides" 2>/dev/null | while read -r pr; do has "$pr" && echo "$pr"; done)
      [ -n "$by" ] || unmet+=("$n")
    done
    [ ${#unmet[@]} = 0 ] && exit 0
    printf '%s\n' "${unmet[@]}"; exit 127;;
  -S)
    status=0
    for n in "${names[@]}"; do offered "$n" || { echo "error: target not found: $n" >&2; status=1; }; done
    [ $status = 0 ] || exit 1
    if [ "${args[1]}" = "--print" ]; then
      { for n in "${names[@]}"; do has "$n" && continue; repo "$n" | awk '{print $2, $3, $1}'
          for d in $(deps_of "$n"); do has "$d" || repo "$d" | awk '{print $2, $3, $1}'; done; done; } | awk 'NF && !seen[$0]++'
      exit 0
    fi
    [ -f "$lock" ] && { echo "error: failed to init transaction (unable to lock database)" >&2; exit 1; }
    record
    [ -f "$P/fail-install" ] && { echo "error: failed to commit transaction (conflicting files)" >&2; exit 1; }
    for n in "${names[@]}"; do
      has "$n" && continue
      for d in $(deps_of "$n"); do has "$d" || repo "$d" | awk '{print $2, $3, "dependency", $4}' >> "$P/installed"; done
      repo "$n" | awk '{print $2, $3, "explicit", $4}' >> "$P/installed"
      ln -sf "$(dirname "$HOME")/safe-bin/true" "$(dirname "$0")/$n"
    done;;
  -R)
    status=0
    for n in "${names[@]}"; do has "$n" || { echo "error: target not found: $n" >&2; status=1; }; done
    while read -r pn pv pr pd; do
      [ -n "$pn" ] || continue
      skip=0; for n in "${names[@]}"; do [ "$pn" = "$n" ] && skip=1; done; [ $skip = 1 ] && continue
      for d in $(echo "$pd" | tr ',' ' '); do for n in "${names[@]}"; do
        [ "$d" = "$n" ] && { echo ":: removing $n breaks dependency '$n' required by $pn"; status=1; }; done; done
    done < "$P/installed"
    # As real pacman does: the error on stderr, the broken dependencies on stdout.
    [ $status = 0 ] || { echo "error: failed to prepare transaction (could not satisfy dependencies)" >&2; exit 1; }
    if [ "${args[1]}" = "--print" ]; then for n in "${names[@]}"; do inst "$n" | awk '{print $1, $2}'; done; exit 0; fi
    [ -f "$lock" ] && { echo "error: failed to init transaction (unable to lock database)" >&2; exit 1; }
    record
    for n in "${names[@]}"; do awk -v n="$n" '$1!=n' "$P/installed" > "$P/installed.new" && mv "$P/installed.new" "$P/installed"; rm -f "$(dirname "$0")/$n"; done;;
  -D)
    record
    for n in "${names[@]}"; do awk -v n="$n" '{ if ($1==n) $3="explicit"; print }' "$P/installed" > "$P/installed.new" && mv "$P/installed.new" "$P/installed"; done;;
esac
exit 0
)SH";

// sudo that refuses when the test says the password was wrong, and otherwise runs the command.
const char kFakeSudo[] = R"SH(
[ -f "$HOME/.fake-pacman/deny-sudo" ] && { echo "sudo: 3 incorrect password attempts" >&2; exit 1; }
exec "$@"
)SH";

// yay building from a fake AUR, "name version commands": records each build, fails when a
// test says so, and installs the package as wanted with its commands on PATH.
const char kFakeYay[] = R"SH(
P="$HOME/.fake-pacman"
args=("$@"); name="${args[-1]}"
echo "yay ${args[*]}" >> "$P/transactions"
[ -f "$P/fail-build" ] && { echo "==> ERROR: A failure occurred in build()." >&2; exit 1; }
line=$(awk -v n="$name" '$1==n' "$P/aur" 2>/dev/null)
[ -n "$line" ] || { echo " -> Could not find all required packages: $name" >&2; exit 1; }
set -- $line
echo "$1 $2 explicit -" >> "$P/installed"
cmds="$3"; [ "$cmds" = "-" ] && cmds="$1"
for c in $(echo "$cmds" | tr ',' ' '); do ln -sf "$(dirname "$HOME")/safe-bin/true" "$(dirname "$0")/$c"; done
exit 0
)SH";

// curl answering the AUR's info lookup from that fake AUR, and recording every request.
const char kFakeAurCurl[] = R"SH(
P="$HOME/.fake-pacman"
url="${@: -1}"
echo "$url" >> "$P/curl-calls"
[ -f "$P/aur-down" ] && { echo "curl: (22) The requested URL returned error: 503" >&2; exit 22; }
query="${url#*\?}"
first=1; count=0; out=""
for part in $(echo "$query" | tr '&' ' '); do
  name=$(echo "${part#arg%5B%5D=}" | sed -e 's/%2B/+/g' -e 's/%40/@/g')
  if awk -v n="$name" '$1==n{f=1} END{exit !f}' "$P/aur" 2>/dev/null; then
    [ $first = 1 ] || out="$out,"; first=0; count=$((count+1))
    out="$out{\"Name\":\"$name\",\"Version\":\"1.0-1\"}"
  fi
done
printf '{"resultcount":%d,"results":[%s],"type":"multiinfo","version":5}\n' "$count" "$out"
)SH";
