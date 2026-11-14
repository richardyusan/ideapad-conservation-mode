#!/bin/sh
# Install Conservation Mode for the current user (no root needed).
#
#   ./install.sh            install (uses dist/ConservationMode, or ./ConservationMode)
#   ./install.sh uninstall  remove it again
set -e
cd "$(dirname "$0")"

BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/256x256/apps"
DESKTOP="$APP_DIR/io.github.conservationmode.desktop"

if [ "$1" = "uninstall" ]; then
    rm -f "$BIN_DIR/ConservationMode" "$DESKTOP" "$ICON_DIR/conservation-mode.png"
    update-desktop-database "$APP_DIR" 2>/dev/null || true
    echo "Removed. (The boot service, if any, is turned off by switching the mode OFF first.)"
    exit 0
fi

for f in dist/ConservationMode ./ConservationMode; do
    [ -f "$f" ] && BIN="$f" && break
done
if [ -z "$BIN" ]; then
    echo "ConservationMode binary not found. Download it from the Releases page or run ./build.sh" >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$APP_DIR" "$ICON_DIR"
install -m 755 "$BIN" "$BIN_DIR/ConservationMode"
[ -f assets/icon-256.png ] && install -m 644 assets/icon-256.png "$ICON_DIR/conservation-mode.png"

cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Conservation Mode
Comment=Turn Lenovo battery Conservation Mode on or off
Exec=$BIN_DIR/ConservationMode
Icon=conservation-mode
Terminal=false
Categories=Settings;HardwareSettings;
Keywords=battery;lenovo;ideapad;charge;
StartupWMClass=ConservationMode
EOF
update-desktop-database "$APP_DIR" 2>/dev/null || true
gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

echo "Installed. Open \"Conservation Mode\" from your app menu, or run: $BIN_DIR/ConservationMode"
