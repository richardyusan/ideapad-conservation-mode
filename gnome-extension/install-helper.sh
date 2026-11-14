#!/bin/sh
# Install (or remove) the root helper and polkit policy. Run with sudo:
#   sudo ./install-helper.sh
#   sudo ./install-helper.sh uninstall
set -e
cd "$(dirname "$0")"

HELPER=/usr/local/libexec/conservation-mode-helper
POLICY=/usr/share/polkit-1/actions/io.github.conservationmode.policy

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo: sudo $0 $*" >&2
    exit 1
fi

if [ "${1:-}" = "uninstall" ]; then
    rm -f "$HELPER" "$POLICY"
    echo "Removed $HELPER and $POLICY"
    exit 0
fi

install -D -o root -g root -m 755 helper/conservation-mode-helper "$HELPER"
install -D -o root -g root -m 644 helper/io.github.conservationmode.policy "$POLICY"
echo "Installed $HELPER and $POLICY"
