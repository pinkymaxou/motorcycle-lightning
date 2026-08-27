#!/usr/bin/env bash
# Runs webui/tests/*.js against the real page in a headless browser: the page
# has no bundler and no test runner, and its protobuf codec is hand-written,
# so this is what proves encode/decode and export/import still agree.
set -euo pipefail
cd "$(dirname "$0")/.."

BROWSER="${BROWSER:-$(command -v chromium || command -v chromium-browser \
    || command -v google-chrome || true)}"
if [ -z "$BROWSER" ]; then
    echo "no chromium/chrome found (set BROWSER=/path/to/browser)" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp webui/index.html webui/style.css webui/app.js "$WORK/"

for probe in webui/tests/*.js; do
    cp "$probe" "$WORK/probe.js"
    cp webui/index.html "$WORK/index.html"
    printf '<script src="probe.js"></script>\n' >> "$WORK/index.html"
    title="$("$BROWSER" --headless --no-sandbox --disable-gpu \
        --virtual-time-budget=3000 --dump-dom "file://$WORK/index.html" \
        2>/dev/null | grep -o '<title>[^<]*</title>' | head -1)"
    case "$title" in
        *"PROBE OK"*) echo "$(basename "$probe"): all passed" ;;
        *PROBE*)      echo "$(basename "$probe"): ${title}" >&2; exit 1 ;;
        *)            echo "$(basename "$probe"): the probe never ran" >&2; exit 1 ;;
    esac
done
