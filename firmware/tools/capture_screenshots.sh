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

# Heights are generous on purpose: the page paints its own dark ground over
# the whole viewport, so a window taller than the content costs a little
# background, while one that is too short silently cuts a card in half.
mkdir -p "$OUT"
shot() {   # shot <tab> <file> <height>
    # shellcheck disable=SC2086
    $SHOT --window-size=1180,"$3" --screenshot="$OUT/$2" \
          "http://$IP/#$1" >/dev/null 2>&1
    echo "  $OUT/$2"
}

echo "capturing from $IP"
shot sim    01-simulate.png 830
shot setup  02-setup.png    1560
shot wifi   05-wifi.png     700
shot pinout 03-pinout.png   570
shot system 04-system.png   1000

# The WiFi tab carries two network names — the module's access point and the
# home one. Blur both fields. The boxes are in screenshot pixels (device
# pixels, i.e. CSS x2), so re-check them if that tab's layout moves.
AP_SSID_BOX="350:62:340:340"     # w:h:x:y
STA_SSID_BOX="350:62:340:725"
ffmpeg -loglevel error -y -i "$OUT/05-wifi.png" -filter_complex \
    "[0:v]crop=$AP_SSID_BOX,boxblur=12[a];\
     [0:v][a]overlay=340:340[m];\
     [m]crop=$STA_SSID_BOX,boxblur=12[b];\
     [m][b]overlay=340:725" \
    "$OUT/05-wifi-blur.png"
mv "$OUT/05-wifi-blur.png" "$OUT/05-wifi.png"
echo "  $OUT/05-wifi.png (both SSIDs blurred)"
