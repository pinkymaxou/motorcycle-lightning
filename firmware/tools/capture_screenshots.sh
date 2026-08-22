#!/usr/bin/env bash
# Screenshots for user_manual/, taken against a running module so the images
# show real data. Usage: tools/capture_screenshots.sh [module-ip]
# Needs chromium; the page selects its tab from the URL hash.
set -euo pipefail
cd "$(dirname "$0")/../.."

IP="${1:-192.168.5.72}"
OUT="user_manual/img"
SHOT="chromium --headless --no-sandbox --disable-gpu --hide-scrollbars
      --force-device-scale-factor=2 --virtual-time-budget=9000"

mkdir -p "$OUT"
shot() {   # shot <tab> <file> <height>
    # shellcheck disable=SC2086
    $SHOT --window-size=1180,"$3" --screenshot="$OUT/$2" \
          "http://$IP/#$1" >/dev/null 2>&1
    echo "  $OUT/$2"
}

echo "capturing from $IP"
shot sim    01-simulate.png 600
# stops after the Colors card on purpose: the WiFi card below it shows
# the home SSID, which has no business in a manual.
shot setup  02-setup.png    1580
shot pinout 03-pinout.png   700
shot system 04-system.png   660
