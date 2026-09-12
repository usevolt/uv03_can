#!/bin/sh
#
# uninstall-uvcan.sh — remove uvcan and its desktop integration from Linux.
#
# Published on the public shelf beside get-uvcan.sh, so it can be run on a
# machine that no longer has the install package:
#
#   curl -fsSL https://files.usevolt.fi/pub/uvcan/uninstall-uvcan.sh | sh
#
# It removes exactly what install.sh installs: the binary and the uvcan-open
# launcher, the bash completion, the .desktop entry, the .uvsys / .uvdev MIME
# types and their icons in every theme install.sh writes to, and the PATH block
# in the shell rc. It also removes the uvcan.old that `uvcan --update` leaves
# behind.
#
# Both a per-user (~/.local) and a machine-wide (/usr/local + /usr/share)
# install are looked for, and whichever is found is removed; the machine-wide
# one needs root, so it is done through sudo when this is not already root.
#
# Your own .uvdev / .uvsys package files are never touched. Neither are the
# stored account settings, unless --purge is given.
#
# Usage:
#   uninstall-uvcan.sh [--user] [--system] [--purge] [-h|--help]
#
#   --user     only the per-user install
#   --system   only the machine-wide install
#   --purge    also delete the saved account settings (~/.config/uvcan)
#
# This is a deliberate copy of what install.sh's --uninstall does, so that
# removing uvcan never depends on still having the install package -- or on the
# network. KEEP THE TWO IN STEP: a file added to install.sh has to be added
# here.
set -eu

MODE=auto
PURGE=0

while [ $# -gt 0 ]; do
	case "$1" in
		--user)   MODE=user ;;
		--system) MODE=system ;;
		--purge)  PURGE=1 ;;
		-h|--help)
			sed -n '2,28p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

# These four lists mirror install.sh. Space separated rather than arrays, so
# this stays POSIX sh and can be piped straight into /bin/sh.
ICON_SIZES="16 24 32 48 64 128 256 512"
MIME_NAMES="application-x-uvsys application-x-uvdev"
DESKTOP_NAME="uvcan.desktop"
LEGACY_DESKTOP_NAME="uvcan-uvsys.desktop"
PATH_MARK_BEGIN="# >>> uvcan installer >>>"
PATH_MARK_END="# <<< uvcan installer <<<"

say()  { printf '\033[1;35m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }

removed_any=0

# remove_icons <theme-root> — undoes install.sh's install_mime_icon
remove_icons() {
	root="$1"
	[ -d "$root" ] || return 0
	for name in $MIME_NAMES; do
		for ctx in mimetypes apps; do
			$PRIV rm -f "$root/scalable/$ctx/$name.svg"
			for s in $ICON_SIZES; do
				$PRIV rm -f "$root/${s}x${s}/$ctx/$name.png"
			done
		done
	done
}

# remove_scope <user|system>
remove_scope() {
	scope="$1"
	if [ "$scope" = system ]; then
		BIN_DIR=/usr/local/bin
		DATA_DIR=/usr/share
		COMPLETION_DIR=/usr/share/bash-completion/completions
		if [ "$(id -u)" -ne 0 ]; then PRIV="sudo"; else PRIV=""; fi
	else
		BIN_DIR="$HOME/.local/bin"
		DATA_DIR="$HOME/.local/share"
		COMPLETION_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/bash-completion/completions"
		PRIV=""
	fi
	ICONS_ROOT="$DATA_DIR/icons"
	APPS_DIR="$DATA_DIR/applications"
	MIME_DIR="$DATA_DIR/mime"

	# Nothing of ours there: say nothing and leave the directories alone.
	if [ ! -e "$BIN_DIR/uvcan" ] && [ ! -e "$APPS_DIR/$DESKTOP_NAME" ] \
			&& [ ! -e "$BIN_DIR/uvcan-open" ]; then
		return 0
	fi

	if [ "$scope" = system ] && [ -n "$PRIV" ]; then
		say "Removing the machine-wide install (this needs your sudo password)"
	else
		say "Removing the ${scope} install"
	fi

	# uvcan.old is the copy `uvcan --update` keeps of the version it replaced.
	$PRIV rm -f "$BIN_DIR/uvcan" "$BIN_DIR/uvcan.old" "$BIN_DIR/uvcan-open"
	$PRIV rm -f "$COMPLETION_DIR/uvcan"
	$PRIV rm -f "$APPS_DIR/$DESKTOP_NAME" "$APPS_DIR/$LEGACY_DESKTOP_NAME"
	for name in $MIME_NAMES; do
		$PRIV rm -f "$MIME_DIR/packages/$name.xml"
	done

	# hicolor, plus the themes install.sh also writes to so the file icons beat
	# the generic one: Adwaita, and whatever the desktop's current theme is.
	remove_icons "$ICONS_ROOT/hicolor"
	remove_icons "$ICONS_ROOT/Adwaita"
	if command -v gsettings >/dev/null 2>&1; then
		cur="$(gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null | tr -d "'\"" || true)"
		case "$cur" in
			""|hicolor|Adwaita) ;;
			*) remove_icons "$ICONS_ROOT/$cur" ;;
		esac
	fi

	command -v update-mime-database    >/dev/null 2>&1 && $PRIV update-mime-database "$MIME_DIR" >/dev/null 2>&1 || true
	command -v update-desktop-database >/dev/null 2>&1 && $PRIV update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
	command -v gtk-update-icon-cache   >/dev/null 2>&1 && $PRIV gtk-update-icon-cache -f -t "$ICONS_ROOT/hicolor" >/dev/null 2>&1 || true

	removed_any=1
}

# The desktop's record of which application opens our two file types. Written
# by xdg-mime at install time, and left pointing at a .desktop file that no
# longer exists if it is not cleaned up -- which is how a file type ends up
# opening nothing at all with no visible reason. Both MIME types are ours, so
# every line naming them can go.
remove_mimeapps() {
	for f in "${XDG_CONFIG_HOME:-$HOME/.config}/mimeapps.list" \
			"$HOME/.local/share/applications/mimeapps.list"; do
		if [ -f "$f" ] && grep -qE '^application/x-uv(sys|dev)=' "$f"; then
			sed -i '/^application\/x-uv\(sys\|dev\)=/d' "$f"
			say "Removed the file-type associations from $f"
		fi
	done
}


# The PATH block only ever goes into a per-user install's shell rc.
remove_path_block() {
	case "$(basename "${SHELL:-/bin/bash}")" in
		zsh) rc="$HOME/.zshrc" ;;
		*)   rc="$HOME/.bashrc" ;;
	esac
	if [ -f "$rc" ] && grep -qF "$PATH_MARK_BEGIN" "$rc"; then
		sed -i "\|^$PATH_MARK_BEGIN\$|,\|^$PATH_MARK_END\$|d" "$rc"
		say "Removed the PATH entry from $rc"
	fi
}

case "$MODE" in
	user)   remove_scope user; remove_path_block; remove_mimeapps ;;
	system) remove_scope system ;;
	auto)   remove_scope user; remove_path_block; remove_mimeapps
	        remove_scope system ;;
esac

if [ "$PURGE" -eq 1 ]; then
	cfg="${XDG_CONFIG_HOME:-$HOME/.config}/uvcan"
	if [ -d "$cfg" ]; then
		rm -rf "$cfg"
		say "Removed the saved account settings in $cfg"
		# purging on a machine where the binary is already gone is a success,
		# not the "nothing found" case below
		removed_any=1
	fi
fi

if [ "$removed_any" -eq 0 ]; then
	warn "No uvcan install was found in ~/.local or /usr/local."
	warn "If it is somewhere else, delete that copy by hand:  command -v uvcan"
	exit 1
fi

say "Done. uvcan is removed; your .uvdev and .uvsys files are untouched."
if [ "$PURGE" -eq 0 ]; then
	printf '    The saved account settings were kept. Remove them with --purge.\n'
fi
