#!/bin/sh
#
# get-uvcan.sh — install uvcan on a machine that does not have it yet.
#
# Published on the public shelf of the Usevolt file server alongside uvcan
# itself, so a fresh machine can be pointed straight at it:
#
#   curl -fsSL https://files.usevolt.fi/pub/uvcan/get-uvcan.sh | sh
#
# It reads latest.json to find the current release, downloads that release's
# Linux package, unpacks it into a temporary directory and runs the install.sh
# inside it — which installs the runtime dependencies and registers the .uvsys
# and .uvdev file associations. Everything it fetches is public; no account is
# needed at any point.
#
# Arguments are passed on to install.sh, so `... | sh -s -- --system` does a
# machine-wide install and `--help` lists the rest.
#
# Once uvcan is installed it keeps itself up to date: `uvcan --checkupdate` and
# `uvcan --update` use the same shelf, and the UI checks once when it opens.
set -eu

BASE="https://files.usevolt.fi/pub/uvcan"

need() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "get-uvcan: '$1' is needed and is not installed." >&2
		exit 1
	}
}
need curl
need tar

# One field out of the manifest. sed rather than jq: jq is not installed by
# default anywhere this runs, and the manifest is written by uvcan's own
# makefile, one field per line.
field() {
	sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p"
}

echo "Looking up the current uvcan..."
manifest="$(curl -fsSL "$BASE/latest.json")" || {
	echo "get-uvcan: could not read $BASE/latest.json." >&2
	exit 1
}
name="$(printf '%s\n' "$manifest" | field name)"
pkg="$(printf '%s\n' "$manifest" | field package)"
if [ -z "$pkg" ]; then
	echo "get-uvcan: the published manifest names no Linux package." >&2
	exit 1
fi

tmp="$(mktemp -d)"
# Cleaned up however this exits, so a failed install leaves nothing behind.
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "Downloading uvcan ${name:-$pkg}..."
curl -fL --progress-bar -o "$tmp/$pkg" "$BASE/$pkg"

# Verified when the manifest carries a checksum for the package. Both come from
# the same place, so this catches a truncated download rather than a hostile
# one -- which is the failure that actually happens.
sum="$(printf '%s\n' "$manifest" | field package_sha256)"
if [ -n "$sum" ] && command -v sha256sum >/dev/null 2>&1; then
	got="$(sha256sum "$tmp/$pkg" | cut -d' ' -f1)"
	if [ "$got" != "$sum" ]; then
		echo "get-uvcan: the download does not match its checksum; stopping." >&2
		exit 1
	fi
	echo "Checksum ok."
fi

tar xzf "$tmp/$pkg" -C "$tmp"
cd "$tmp"
[ -x ./install.sh ] || {
	echo "get-uvcan: the package holds no install.sh." >&2
	exit 1
}
echo ""
./install.sh "$@"
