#!/bin/sh
# Build dist/conservation-mode_<version>_amd64.deb from dist/ConservationMode.
# Run ./build.sh (or at least the Linux part of it) first.
#
# The package installs:
#   /usr/bin/ConservationMode                        the GTK app
#   /usr/share/applications, /usr/share/icons        app menu entry and icon
#   /usr/libexec/conservation-mode-helper            root helper for the toggle
#   /usr/share/polkit-1/actions/...policy            lets the active user run the helper without a password
#   /usr/share/gnome-shell/extensions/<uuid>/        GNOME Quick Settings toggle
set -e
umask 022
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' src/conservation_mode_linux.c)
ARCH=amd64
PKG=conservation-mode
UUID=conservation-mode@conservationmode.github.io
OUT="dist/${PKG}_${VERSION}_${ARCH}.deb"
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
chmod 755 "$ROOT"

[ -n "$VERSION" ] || { echo "Could not read APP_VERSION" >&2; exit 1; }
[ -x dist/ConservationMode ] || { echo "dist/ConservationMode not found. Run ./build.sh first." >&2; exit 1; }

install -D -m 755 dist/ConservationMode "$ROOT/usr/bin/ConservationMode"
install -D -m 644 assets/icon-256.png "$ROOT/usr/share/icons/hicolor/256x256/apps/conservation-mode.png"
install -D -m 755 gnome-extension/helper/conservation-mode-helper "$ROOT/usr/libexec/conservation-mode-helper"
mkdir -p "$ROOT/usr/share/polkit-1/actions"
sed 's#/usr/local/libexec/#/usr/libexec/#' gnome-extension/helper/io.github.conservationmode.policy \
    > "$ROOT/usr/share/polkit-1/actions/io.github.conservationmode.policy"
chmod 644 "$ROOT/usr/share/polkit-1/actions/io.github.conservationmode.policy"
install -D -m 644 "gnome-extension/$UUID/metadata.json" "$ROOT/usr/share/gnome-shell/extensions/$UUID/metadata.json"
install -D -m 644 "gnome-extension/$UUID/extension.js" "$ROOT/usr/share/gnome-shell/extensions/$UUID/extension.js"
install -D -m 644 LICENSE "$ROOT/usr/share/doc/$PKG/copyright"

mkdir -p "$ROOT/usr/share/applications"
cat > "$ROOT/usr/share/applications/io.github.conservationmode.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Conservation Mode
Comment=Turn Lenovo battery Conservation Mode on or off
Exec=ConservationMode
Icon=conservation-mode
Terminal=false
Categories=Settings;HardwareSettings;
Keywords=battery;lenovo;ideapad;charge;
StartupWMClass=ConservationMode
EOF

SIZE=$(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
mkdir -p "$ROOT/DEBIAN"
cat > "$ROOT/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: libc6, libgtk-3-0, pkexec | policykit-1
Recommends: gnome-shell
Installed-Size: $SIZE
Maintainer: richardyusan <37256826+richardyusan@users.noreply.github.com>
Homepage: https://github.com/richardyusan/ideapad-conservation-mode
Description: Lenovo IdeaPad battery Conservation Mode switch
 Turns battery Conservation Mode (charge limit) on or off on Lenovo
 IdeaPad laptops through the ideapad_laptop kernel driver. Includes a
 GTK app and a GNOME Shell Quick Settings toggle that works without a
 password prompt for the active local user.
EOF

cat > "$ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
if [ "\$1" = "configure" ] && [ -z "\$2" ]; then
    echo "To show the toggle in GNOME Quick Settings, log out and back in, then run:"
    echo "  gnome-extensions enable $UUID"
fi
exit 0
EOF
chmod 755 "$ROOT/DEBIAN/postinst"

mkdir -p dist
dpkg-deb --build --root-owner-group -Zxz "$ROOT" "$OUT" >/dev/null
echo "$OUT"
