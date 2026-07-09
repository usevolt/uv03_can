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

ICONS_ROOT="$DATA_DIR/icons"
ICON_BASE="$ICONS_ROOT/hicolor"
MIME_DIR="$DATA_DIR/mime"
APPS_DIR="$DATA_DIR/applications"
DESKTOP_NAME="uvcan-uvsys.desktop"

# run a privileged command (no-op prefix in user mode)
priv() { $SUDO "$@"; }

say()  { printf '\033[1;35m==>\033[0m %s\n' "$*"; }   # purple arrow
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }

# Themes that must also carry the .uvsys mimetype icon.
#
# GTK4 (Nautilus on modern GNOME/Ubuntu) resolves a file's icon from a fallback
# *chain* — [application-x-uvsys, application-x-generic, ...] — and searches it
# THEME-MAJOR: it walks the active theme's inheritance chain (e.g. Yaru ->
# Humanity -> Adwaita -> hicolor) and at each theme tries every name in the
# chain. Because the generic "application-x-generic" exists in Adwaita, which is
# searched before "hicolor", a uvsys icon installed ONLY in hicolor never wins —
# Nautilus shows the plain document icon. To beat the generic we install the
# specific icon into themes searched before hicolor: the user's current icon
# theme (always searched first) and Adwaita (the universal default fallback).
EXTRA_ICON_THEMES=(Adwaita)
if command -v gsettings >/dev/null 2>&1; then
	cur_theme="$(gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null | tr -d "'\"")"
	case "$cur_theme" in
		""|hicolor|Adwaita) ;;                  # already covered / not a real theme
		*) EXTRA_ICON_THEMES+=("$cur_theme") ;;
	esac
fi

# install_mime_icon <theme-root>  — drop the scalable SVG + PNG sizes into a
# theme's mimetypes context. Theme dir definitions come from the theme's own
# index.theme (merged across base dirs by name), so no index.theme is needed here.
install_mime_icon() {
	local root="$1"
	priv install -d "$root/scalable/mimetypes"
	priv install -m 0644 "$PKG_DIR/application-x-uvsys.svg" \
		"$root/scalable/mimetypes/application-x-uvsys.svg"
	for s in "${ICON_SIZES[@]}"; do
		local src="$PKG_DIR/icons/$s/application-x-uvsys.png"
		[ -f "$src" ] || continue
		priv install -d "$root/${s}x${s}/mimetypes"
		priv install -m 0644 "$src" "$root/${s}x${s}/mimetypes/application-x-uvsys.png"
	done
}

# remove_mime_icon <theme-root>  — undo install_mime_icon
remove_mime_icon() {
	local root="$1"
	priv rm -f "$root/scalable/mimetypes/application-x-uvsys.svg"
	for s in "${ICON_SIZES[@]}"; do
		priv rm -f "$root/${s}x${s}/mimetypes/application-x-uvsys.png"
	done
}

# ---- uninstall --------------------------------------------------------------
if [ "$ACTION" = "uninstall" ]; then
	say "Uninstalling uvcan desktop integration ($MODE)"
	priv rm -f "$BIN_DIR/uvcan" "$BIN_DIR/uvcan-open"
	priv rm -f "$APPS_DIR/$DESKTOP_NAME"
	priv rm -f "$MIME_DIR/packages/application-x-uvsys.xml"
	for ctx in mimetypes apps; do
		priv rm -f "$ICON_BASE/scalable/$ctx/application-x-uvsys.svg"
		for s in "${ICON_SIZES[@]}"; do
			priv rm -f "$ICON_BASE/${s}x${s}/$ctx/application-x-uvsys.png"
		done
	done
	for theme in "${EXTRA_ICON_THEMES[@]}"; do
		remove_mime_icon "$ICONS_ROOT/$theme"
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
		# libncurses-dev/pkg-config: build. zenity/zip/unzip/shared-mime-info/
		# desktop-file-utils: installer + .uvsys handling. libglfw3/libglew2.2/
		# libfreetype6/libreadline8: runtime shared libs the binary links against
		# for the OpenGL UI and the interactive terminal (not pulled in otherwise).
		sudo apt-get install -y libncurses-dev pkg-config zenity \
			zip unzip shared-mime-info desktop-file-utils \
			libglfw3 libglew2.2 libfreetype6 libreadline8
	else
		warn "apt-get not found; skipping dependency install. Ensure libncurses,"
		warn "pkg-config, zenity, zip/unzip, shared-mime-info and the runtime libs"
		warn "libglfw3, libglew2.2, libfreetype6 and libreadline8 are present."
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

# icon: scalable SVG + pre-rendered PNG sizes. Installed into the "mimetypes"
# context (where file managers look up a file's icon by its MIME-type name) and
# also "apps" (used by the .desktop entry's Icon=). Without the mimetypes copy a
# .uvsys file falls back to the generic document icon.
for ctx in mimetypes apps; do
	priv install -d "$ICON_BASE/scalable/$ctx"
	priv install -m 0644 "$PKG_DIR/application-x-uvsys.svg" \
		"$ICON_BASE/scalable/$ctx/application-x-uvsys.svg"
	for s in "${ICON_SIZES[@]}"; do
		src="$PKG_DIR/icons/$s/application-x-uvsys.png"
		[ -f "$src" ] || continue
		priv install -d "$ICON_BASE/${s}x${s}/$ctx"
		priv install -m 0644 "$src" \
			"$ICON_BASE/${s}x${s}/$ctx/application-x-uvsys.png"
	done
done

# Also place the mimetype icon into the current theme + Adwaita so GTK4's
# theme-major fallback finds our specific icon before the generic one (see the
# EXTRA_ICON_THEMES comment above). hicolor alone is not enough on GTK4.
for theme in "${EXTRA_ICON_THEMES[@]}"; do
	echo "    icon    -> $ICONS_ROOT/$theme/.../mimetypes/application-x-uvsys.*"
	install_mime_icon "$ICONS_ROOT/$theme"
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
# Refresh the extra theme dirs too. They have no index.theme of their own (the
# definitions are merged from the system theme), so use --ignore-theme-index.
for theme in "${EXTRA_ICON_THEMES[@]}"; do
	command -v gtk-update-icon-cache >/dev/null && \
		priv gtk-update-icon-cache -f -t --ignore-theme-index "$ICONS_ROOT/$theme" >/dev/null 2>&1 || true
done

# Nautilus caches icons in-process; nudge it to re-read so the new icon shows
# without a re-login. Harmless if it is not running (it relaunches on demand).
[ "$MODE" = "user" ] && command -v nautilus >/dev/null && pgrep -x nautilus >/dev/null 2>&1 && \
	(nautilus -q >/dev/null 2>&1 || true)

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
