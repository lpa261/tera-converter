#!/usr/bin/env bash
set -euo pipefail
if (( EUID == 0 )); then echo "Run as your desktop user, without sudo." >&2; exit 1; fi
TARGET="${XDG_DATA_HOME:-$HOME/.local/share}/dbus-1/services/org.gnome.NautilusPreviewer.service"
MARKER='# Managed by TeraConvert Quick Look'
case "${1:---enable}" in
  --disable)
    if [[ -f "$TARGET" ]] && grep -Fxq "$MARKER" "$TARGET"; then rm -- "$TARGET"; fi
    echo 'TeraConvert Space integration disabled. Sign out and back in to reload your previous previewer.'
    ;;
  --enable)
    if [[ ! -x /usr/bin/teraconvert-preview ]]; then echo 'Install the TeraConvert 2.2 desktop package first.' >&2; exit 1; fi
    if [[ -e "$TARGET" ]] && ! grep -Fxq "$MARKER" "$TARGET"; then echo "Existing user service retained: $TARGET" >&2; exit 1; fi
    mkdir -p -- "$(dirname -- "$TARGET")"
    TEMP_SERVICE="$(mktemp "$(dirname -- "$TARGET")/.teraconvert-preview.XXXXXX")"
    trap 'rm -f -- "$TEMP_SERVICE"' EXIT
    cat > "$TEMP_SERVICE" <<SERVICE
$MARKER
[D-BUS Service]
Name=org.gnome.NautilusPreviewer
Exec=/usr/bin/teraconvert-preview --sushi-service --gapplication-service
SERVICE
    chmod 644 "$TEMP_SERVICE"
    mv -- "$TEMP_SERVICE" "$TARGET"
    echo 'Space integration enabled for your account. Sign out and back in to release any running Sushi service.'
    ;;
  *) echo 'Usage: enable-quicklook.sh [--enable|--disable]' >&2; exit 2;;
esac
