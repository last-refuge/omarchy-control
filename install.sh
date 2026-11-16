#!/usr/bin/env bash
#
# Installs the latest Omarchy Control release from prebuilt packages:
#
#   curl -fsSL https://raw.githubusercontent.com/last-refuge/omarchy-control/main/install.sh | bash
#
# Nothing is compiled. It downloads the package from GitHub Releases, checks it against
# the release's SHA256SUMS, and hands it to pacman, which shows the change and asks
# before going ahead. Options, passed with `| bash -s -- <option>`:
#
#   --yes              Do not ask for confirmation, for use without a terminal
#   --version 0.2.0    Install that version instead of the latest
#
# Everything runs from main on the last line, so a download cut off midway runs nothing.
set -euo pipefail

say() { printf '%s\n' "$*"; }
die() {
  printf 'Omarchy Control was not installed: %s\n' "$*" >&2
  exit 1
}

main() {
  local assume_yes=0 version="" releases package tmp url
  local secure=(--proto '=https' --proto-redir '=https')

  while (($#)); do
    case "$1" in
      --yes) assume_yes=1 ;;
      --version)
        [[ $# -ge 2 ]] || die "--version needs a version number, such as 0.2.0."
        version="${2#v}"
        shift
        ;;
      *) die "unknown option $1." ;;
    esac
    shift
  done

  # A local mirror, used by tools/installer-check.sh, is the only reason to set this.
  releases="https://github.com/last-refuge/omarchy-control/releases"
  if [[ -n ${OMARCHY_CONTROL_RELEASES:-} ]]; then
    releases="$OMARCHY_CONTROL_RELEASES"
    secure=()
  fi

  [[ $(uname -m) == x86_64 ]] || die "packages are only built for x86_64, and this computer is $(uname -m)."
  command -v pacman >/dev/null || die "this needs pacman, and Omarchy runs on Arch Linux."
  command -v curl >/dev/null || die "curl is not installed."
  command -v sha256sum >/dev/null || die "sha256sum is not installed."
  command -v omarchy >/dev/null || say "Note: Omarchy was not found. This app manages an Omarchy desktop, so most of it will be unavailable."

  # stdin is this script when piped from curl, so pacman's question has to go to the terminal.
  local pacman_args=(pacman -U --needed)
  if ((assume_yes)); then
    pacman_args+=(--noconfirm)
  elif ! { : </dev/tty; } 2>/dev/null; then
    die "there is no terminal to ask for confirmation. Run it again with: bash -s -- --yes"
  fi
  local privilege=(sudo)
  ((EUID == 0)) && privilege=()
  if ((EUID != 0)) && ! command -v sudo >/dev/null; then
    die "sudo is not installed."
  fi

  if [[ -n $version ]]; then
    url="$releases/download/v$version"
  else
    url="$releases/latest/download"
  fi

  tmp="$(mktemp -d)"
  # shellcheck disable=SC2064 # Expanded now on purpose: tmp is local to main.
  trap "rm -rf '$tmp'" EXIT

  say "Finding the release..."
  curl -fsSL "${secure[@]}" --connect-timeout 15 --max-time 60 -o "$tmp/SHA256SUMS" "$url/SHA256SUMS" ||
    die "could not read the release from $url. Check your connection, or see https://github.com/last-refuge/omarchy-control/releases"

  package="$(sed -nE 's/^[0-9a-fA-F]{64} [ *](omarchy-control-[0-9][0-9.]*-[0-9]+-x86_64\.pkg\.tar\.zst)$/\1/p' "$tmp/SHA256SUMS" | head -n 1)"
  [[ -n $package ]] || die "the release does not list a package for x86_64."

  say "Downloading $package..."
  curl -fsSL "${secure[@]}" --connect-timeout 15 --max-time 300 -o "$tmp/$package" "$url/$package" ||
    die "the download failed. Check your connection and try again."

  (cd "$tmp" && grep -F " $package" SHA256SUMS | sha256sum --check --status -) ||
    die "the download did not match its published checksum."
  say "Checksum verified."

  if ((assume_yes)); then
    "${privilege[@]}" "${pacman_args[@]}" "$tmp/$package" </dev/null ||
      die "pacman stopped. If it could not find a dependency, update this computer with 'omarchy update' and try again."
  else
    "${privilege[@]}" "${pacman_args[@]}" "$tmp/$package" </dev/tty ||
      die "pacman stopped. If it could not find a dependency, update this computer with 'omarchy update' and try again."
  fi

  # Package hooks notify user managers. Also activate for the actual invoking
  # user, including a terminal without a running user manager. Older published
  # packages do not carry this helper and keep their prior install behavior.
  local integration=/usr/lib/omarchy-control/agent-integration/user-recovery
  if [[ -f $integration ]]; then
    if ((EUID == 0)); then
      say "Agent integration activates in desktop user sessions; root was not registered."
    elif /usr/bin/python3 "$integration" activate >"$tmp/agent-integration.json" 2>"$tmp/agent-integration.err"; then
      /usr/bin/python3 - "$tmp/agent-integration.json" <<'PY'
import json, sys
try:
    result = json.load(open(sys.argv[1]))
    if not isinstance(result, dict):
        raise ValueError("unexpected activation response")
    if result.get("outcome") == "opted_out":
        print("Agent skill integration remains opted out.")
    elif result.get("ok") and result.get("verified") and result.get("outcome") == "activated":
        print("Agent skills registered for this user. Start a fresh supported agent session to load them.")
        if result.get("userManagerActivation") == "deferred":
            print("User-manager scheduling is prepared for the next session.")
    else:
        print("Agent registration needs review. Run: omarchy-control skill status")
except (OSError, ValueError):
    print("Agent registration was not confirmed. Run: omarchy-control skill status")
PY
    else
      say "The app installed; agent registration needs review. Run: omarchy-control skill status"
    fi
  fi

  say ""
  say "Omarchy Control is installed. Find it in your app launcher, or run:"
  say "  omarchy-control open"
  say ""
  say "To add it to the Omarchy menu:"
  say "  omarchy-control integrate menu"
}

main "$@"
