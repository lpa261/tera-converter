#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 path/to/teraconvert-c_VERSION_ARCH.deb"
    echo "Run from a terminal in your desktop session, without sudo."
}

if [[ $# == 1 && "$1" == --help ]]; then
    usage
    exit 0
fi
if [[ $# != 1 ]]; then
    usage >&2
    exit 2
fi
if (( EUID == 0 )); then
    echo "Run this script as your desktop user; it invokes sudo for apt only." >&2
    exit 1
fi

DEB_PATH="$(realpath -- "$1")"
if [[ ! -f "$DEB_PATH" ]] ||
   [[ "$(dpkg-deb -f "$DEB_PATH" Package)" != teraconvert-c ]]; then
    echo "Expected a teraconvert-c Debian package." >&2
    exit 1
fi
PACKAGE_VERSION="$(dpkg-deb -f "$DEB_PATH" Version)"

# Fail before modifying packages if this is not an accessible desktop session.
systemctl --user show-environment > /dev/null
sudo apt install --reinstall "$DEB_PATH"
INSTALLED_VERSION="$(dpkg-query -W -f='${Version}' teraconvert-c)"
if [[ "$INSTALLED_VERSION" != "$PACKAGE_VERSION" ]]; then
    echo "Installed version differs from the selected package." >&2
    exit 1
fi
(
    cd /
    md5sum --check --quiet /var/lib/dpkg/info/teraconvert-c.md5sums
)

echo "Restarting TeraConvert and reloading the Nautilus extension..."
systemctl --user daemon-reload
systemctl --user stop teraconvert.service teraconvert.socket
systemctl --user enable --now teraconvert.socket
systemctl --user start teraconvert.service
systemctl --user is-active --quiet teraconvert.socket
systemctl --user is-active --quiet teraconvert.service

DAEMON_PID="$(systemctl --user show teraconvert.service --property=MainPID --value)"
if [[ ! "$DAEMON_PID" =~ ^[1-9][0-9]*$ ]] ||
   ! cmp -s "/proc/$DAEMON_PID/exe" /usr/bin/teraconvert-daemon; then
    echo "Unable to confirm that the running daemon uses the installed binary." >&2
    exit 1
fi

if [[ -x /usr/bin/teraconvert-cli ]]; then
    /usr/bin/teraconvert-cli --doctor
fi
nautilus -q
echo "Installed TeraConvert $INSTALLED_VERSION; daemon and socket are active."
echo "Open Files again to load the updated context menu."
