#!/bin/sh
# Install (or remove) the GNOME Shell extension for the current user.
#   ./install.sh
#   ./install.sh uninstall
# The root helper is installed separately: sudo ./install-helper.sh
set -e
cd "$(dirname "$0")"

UUID=conservation-mode@conservationmode.github.io
DEST="$HOME/.local/share/gnome-shell/extensions/$UUID"

if [ "${1:-}" = "uninstall" ]; then
    gnome-extensions disable "$UUID" 2>/dev/null || true
    rm -rf "$DEST"
    echo "Removed $DEST"
    exit 0
fi

mkdir -p "$DEST"
cp "$UUID/metadata.json" "$UUID/extension.js" "$DEST/"
echo "Installed to $DEST"

if [ ! -x /usr/local/libexec/conservation-mode-helper ]; then
    echo "Note: the helper is not installed yet. Run: sudo $(pwd)/install-helper.sh"
fi

if gnome-extensions enable "$UUID" 2>/dev/null; then
    echo "Enabled. Open the Quick Settings menu (top-right) to find the toggle."
else
    echo "GNOME Shell only sees new extensions after you log out and back in (Wayland)."
    echo "After logging back in, run: gnome-extensions enable $UUID"
fi
