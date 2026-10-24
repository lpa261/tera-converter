#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
OUTPUT_DIR="$PROJECT_DIR/packages"
VERSION="${TC_PACKAGE_VERSION:-2.2.0}"
ARCH="$(dpkg --print-architecture)"

dpkg --validate-version "$VERSION"
mkdir -p "$OUTPUT_DIR"
PKG_WORKDIR="$(mktemp -d "$OUTPUT_DIR/.teraconvert-deb.XXXXXX")"
PKG_DIR="$PKG_WORKDIR/root"
trap 'rm -rf -- "$PKG_WORKDIR"' EXIT

echo "=== Building TeraConvert-C (Version: $VERSION, Architecture: $ARCH) ==="

# Build the complete desktop application, regardless of previous build options.
SETUP_MODE=()
if [[ -f "$BUILD_DIR/meson-private/coredata.dat" ]]; then SETUP_MODE=(--reconfigure); fi
meson setup "$BUILD_DIR" "$PROJECT_DIR" --prefix=/usr --buildtype=release \
    "${SETUP_MODE[@]}" -Db_sanitize=none -Dclient=true -Ddaemon=true -Dui=enabled \
    -Dengine_vips=enabled -Dengine_ffmpeg=enabled -Dengine_poppler=enabled \
    -Dengine_office=enabled -Dengine_ai=enabled -Dengine_safe_images=enabled
meson compile -C "$BUILD_DIR"

# Stage only in our private directory; retain previous packages for comparison.
meson install -C "$BUILD_DIR" --destdir "$PKG_DIR" --no-rebuild

# Derive ABI/version requirements from the actual binaries built on this machine.
mkdir -p "$PKG_WORKDIR/debian" "$PKG_DIR/DEBIAN"
cat > "$PKG_WORKDIR/debian/control" << 'EOF'
Source: teraconvert-c
Section: utils
Priority: optional
Maintainer: Phuc Anh Le <lephucanh2601@gmail.com>

Package: teraconvert-c
Architecture: any
Description: Native C11 file conversion suite for GNOME Files (Nautilus)
 TeraConvert desktop components.
EOF
EXTENSION_DIR="$(pkg-config --variable=extensiondir libnautilus-extension-4)"
SHLIB_OUTPUT="$(cd "$PKG_WORKDIR" && dpkg-shlibdeps -O \
    -e"$PKG_DIR/usr/bin/teraconvert-daemon" \
    -e"$PKG_DIR/usr/bin/teraconvert-ui" \
    -e"$PKG_DIR/usr/bin/teraconvert-cli" \
    -e"$PKG_DIR/usr/bin/teraconvert-preview" \
    -e"$PKG_DIR$EXTENSION_DIR/libnautilus-teraconvert.so")"
SHLIB_DEPS="$(sed -n 's/^shlibs:Depends=//p' <<< "$SHLIB_OUTPUT")"
if [ -z "$SHLIB_DEPS" ]; then
    echo "Unable to determine runtime library dependencies." >&2
    exit 1
fi
INSTALLED_SIZE="$(du -sk "$PKG_DIR/usr" | cut -f1)"
cat > "$PKG_DIR/DEBIAN/control" << EOF
Package: teraconvert-c
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Installed-Size: $INSTALLED_SIZE
Maintainer: Phuc Anh Le <lephucanh2601@gmail.com>
Depends: $SHLIB_DEPS, nautilus, ffmpeg, img2pdf, ghostscript, poppler-utils, zip, unzip, python3, python3-pil, python3-pikepdf
Recommends: python3-numpy, python3-onnxruntime, gstreamer1.0-plugins-good, gstreamer1.0-libav
Replaces: teraconvert
Conflicts: teraconvert
Provides: teraconvert
Description: Native C11 file conversion suite for GNOME Files (Nautilus)
 TeraConvert-C provides a native Nautilus extension and C11 conversion daemon.
 Includes validated PNG/JPEG, MP4/MOV, PDF, OOXML/ODF and audio preservation
 modes alongside separate lossy presets. Some operations use helper programs
 and Python; AI requires a compatible locally supplied model and runtime.
EOF

# Enable socket activation for login sessions. Reload/restart the current user's
# manager from tools/install-local.sh, rather than running a user bus as root.
cat << 'EOF' > "$PKG_DIR/DEBIAN/postinst"
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    systemctl --global enable teraconvert.socket || true
fi
exit 0
EOF
chmod 755 "$PKG_DIR/DEBIAN/postinst"

# Let dpkg verify installed payloads after the upgrade.
(
    cd "$PKG_DIR"
    find usr -type f -print0 | sort -z | xargs -0 -r md5sum > DEBIAN/md5sums
)

DEB_NAME="teraconvert-c_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$PKG_DIR" "$OUTPUT_DIR/$DEB_NAME"

echo "=== Build Complete! Package created at: $OUTPUT_DIR/$DEB_NAME ==="
ls -lh "$OUTPUT_DIR/$DEB_NAME"
