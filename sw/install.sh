#!/bin/bash
#
# uvcan installer
# ----------------
# Installs the uvcan binary plus the desktop integration that makes .uvsys
# system packages double-clickable:
#   * the uvcan binary and a uvcan-open launcher wrapper
#   * the application/x-uvsys MIME type and its file icon (Usevolt wordmark)
#   * a .desktop entry registered as the default handler for .uvsys files
#
# Default is a per-user install under ~/.local (no root needed for the files;
# only the optional apt dependency step uses sudo). Pass --system for a
# machine-wide install under /usr/local + /usr/share (needs root).
#
# Usage:
#   ./install.sh [--system] [--no-deps] [--build] [--uninstall] [-h|--help]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$SCRIPT_DIR/packaging"

MODE="user"          # user | system
DO_DEPS=1            # install apt dependencies
DO_BUILD="auto"      # auto | yes — build uvcan if no prebuilt binary
ACTION="install"     # install | uninstall

ICON_SIZES=(16 24 32 48 64 128 256 512)

# ---- argument parsing -------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
		--system)    MODE="system" ;;
		--user)      MODE="user" ;;
		--no-deps)   DO_DEPS=0 ;;
		--build)     DO_BUILD="yes" ;;
		--uninstall) ACTION="uninstall" ;;
		-h|--help)
			sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*)
			echo "Unknown option: $1" >&2
			echo "Try './install.sh --help'." >&2
			exit 2 ;;
	esac
	shift
done

# ---- resolve install locations ---------------------------------------------
if [ "$MODE" = "system" ]; then
	BIN_DIR="/usr/local/bin"
	DATA_DIR="/usr/share"
	# elevate via sudo for file operations when not already root
	if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; else SUDO=""; fi
else
	BIN_DIR="$HOME/.local/bin"
	DATA_DIR="$HOME/.local/share"
	SUDO=""
fi

ICON_BASE="$DATA_DIR/icons/hicolor"
MIME_DIR="$DATA_DIR/mime"
APPS_DIR="$DATA_DIR/applications"
DESKTOP_NAME="uvcan-uvsys.desktop"

# run a privileged command (no-op prefix in user mode)
priv() { $SUDO "$@"; }

say()  { printf '\033[1;35m==>\033[0m %s\n' "$*"; }   # purple arrow
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }

# ---- uninstall --------------------------------------------------------------
if [ "$ACTION" = "uninstall" ]; then
	say "Uninstalling uvcan desktop integration ($MODE)"
	priv rm -f "$BIN_DIR/uvcan" "$BIN_DIR/uvcan-open"
	priv rm -f "$APPS_DIR/$DESKTOP_NAME"
	priv rm -f "$MIME_DIR/packages/application-x-uvsys.xml"
	priv rm -f "$ICON_BASE/scalable/apps/application-x-uvsys.svg"
	for s in "${ICON_SIZES[@]}"; do
		priv rm -f "$ICON_BASE/${s}x${s}/apps/application-x-uvsys.png"
	done
	command -v update-mime-database    >/dev/null && priv update-mime-database "$MIME_DIR" || true
	command -v update-desktop-database >/dev/null && priv update-desktop-database "$APPS_DIR" || true
	command -v gtk-update-icon-cache   >/dev/null && priv gtk-update-icon-cache -f -t "$ICON_BASE" >/dev/null 2>&1 || true
	say "Done. The uvcan binary was removed; your .uvsys files are untouched."
	exit 0
fi

# ---- dependencies -----------------------------------------------------------
if [ "$DO_DEPS" -eq 1 ]; then
	if command -v apt-get >/dev/null; then
		say "Installing system dependencies (sudo apt-get)"
		sudo apt-get install -y libncurses-dev pkg-config zenity \
			zip unzip shared-mime-info desktop-file-utils
	else
		warn "apt-get not found; skipping dependency install. Ensure libncurses,"
		warn "pkg-config, zenity, zip/unzip and shared-mime-info are present."
	fi
fi

# ---- obtain the binary ------------------------------------------------------
BIN_SRC="$SCRIPT_DIR/uvcan"
if [ "$DO_BUILD" = "yes" ] || [ ! -x "$BIN_SRC" ]; then
	if [ "$DO_BUILD" = "yes" ] || command -v arm-none-eabi-gcc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; then
		if [ -f "$SCRIPT_DIR/makefile" ] && command -v make >/dev/null; then
			say "Building uvcan (make)"
			make -C "$SCRIPT_DIR"
		fi
	fi
fi
if [ ! -x "$BIN_SRC" ]; then
	echo "error: no uvcan binary found at $BIN_SRC and the build did not produce one." >&2
	echo "       Build it first with 'make', or run a release bundle that ships the binary." >&2
	exit 1
fi

# ---- install files ----------------------------------------------------------
say "Installing into $MODE locations:"
echo "    binary  -> $BIN_DIR/uvcan"
echo "    icon    -> $ICON_BASE/.../application-x-uvsys.*"
echo "    mime    -> $MIME_DIR/packages/application-x-uvsys.xml"
echo "    desktop -> $APPS_DIR/$DESKTOP_NAME"

priv install -d "$BIN_DIR" "$APPS_DIR" "$MIME_DIR/packages" "$ICON_BASE/scalable/apps"

# binary + launcher wrapper
priv install -m 0755 "$BIN_SRC"               "$BIN_DIR/uvcan"
priv install -m 0755 "$PKG_DIR/uvcan-open"    "$BIN_DIR/uvcan-open"

# icon: scalable SVG + pre-rendered PNG sizes
priv install -m 0644 "$PKG_DIR/application-x-uvsys.svg" \
	"$ICON_BASE/scalable/apps/application-x-uvsys.svg"
for s in "${ICON_SIZES[@]}"; do
	src="$PKG_DIR/icons/$s/application-x-uvsys.png"
	[ -f "$src" ] || continue
	priv install -d "$ICON_BASE/${s}x${s}/apps"
	priv install -m 0644 "$src" "$ICON_BASE/${s}x${s}/apps/application-x-uvsys.png"
done

# MIME type
priv install -m 0644 "$PKG_DIR/application-x-uvsys.xml" \
	"$MIME_DIR/packages/application-x-uvsys.xml"

# desktop entry: substitute the wrapper path into the template
tmp_desktop="$(mktemp)"
sed "s|@BIN@|$BIN_DIR/uvcan-open|g" "$PKG_DIR/uvcan-uvsys.desktop.in" > "$tmp_desktop"
priv install -m 0644 "$tmp_desktop" "$APPS_DIR/$DESKTOP_NAME"
rm -f "$tmp_desktop"

# ---- refresh the caches & set default handler ------------------------------
say "Refreshing desktop databases"
command -v update-mime-database    >/dev/null && priv update-mime-database "$MIME_DIR" || warn "update-mime-database missing"
command -v update-desktop-database >/dev/null && priv update-desktop-database "$APPS_DIR" || warn "update-desktop-database missing"
command -v gtk-update-icon-cache   >/dev/null && priv gtk-update-icon-cache -f -t "$ICON_BASE" >/dev/null 2>&1 || true

# associate .uvsys with our entry (per-user setting; honoured in both modes)
if command -v xdg-mime >/dev/null; then
	xdg-mime default "$DESKTOP_NAME" application/x-uvsys || warn "xdg-mime default failed"
fi

# ---- PATH sanity check ------------------------------------------------------
case ":$PATH:" in
	*":$BIN_DIR:"*) ;;
	*) warn "$BIN_DIR is not on your PATH. Add it (e.g. in ~/.profile):"
	   warn "    export PATH=\"$BIN_DIR:\$PATH\""
	   warn "Double-click association still works (it uses an absolute path)." ;;
esac

say "Done. Double-click any .uvsys file to open it in uvcan, or run 'uvcan --ui'."
say "Verify the association with: xdg-mime query default application/x-uvsys"
