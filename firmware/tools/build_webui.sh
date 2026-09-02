#!/usr/bin/env bash
# Inline webui/{style.css,app.js} into index.html and gzip the result into the
# firmware-embedded asset. The sources stay split for editing; the module still
# serves one request — it has a single httpd task and a WebSocket stream to
# feed, so extra round-trips and extra embedded blobs buy nothing there.
#
#   build_webui.sh          regenerate the asset (run after editing webui/)
#   build_webui.sh --check  fail if the committed asset is stale — compares the
#                           inlined page by content, not gzip bytes, so a
#                           different gzip build cannot fake a difference
set -euo pipefail
cd "$(dirname "$0")/.."

ASSET=components/net_services/webui_dist/index.html.gz

# A symbol deleted during a refactor otherwise ships as a blank page.
python3 tools/check_webui.py webui

inline() {
    python3 - <<'PYEOF'
import pathlib
import sys

d = pathlib.Path('webui')
html = (d / 'index.html').read_text(encoding='utf-8')
for tag, name, fmt in (('<link rel="stylesheet" href="style.css">', 'style.css', '<style>\n%s</style>'),
                       ('<script src="app.js"></script>', 'app.js', '<script>\n%s</script>')):
    if tag not in html:
        sys.exit('index.html no longer references %s — nothing to inline' % name)
    html = html.replace(tag, fmt % (d / name).read_text(encoding='utf-8'))
sys.stdout.write(html)
PYEOF
}

if [ "${1:-}" = "--check" ]; then
    if ! cmp -s <(inline) <(gzip -dc "$ASSET"); then
        echo "$ASSET is stale — run tools/build_webui.sh and commit it" >&2
        exit 1
    fi
    echo "$ASSET matches webui/"
    exit 0
fi

mkdir -p "$(dirname "$ASSET")"
inline | gzip -9 -n -c > "$ASSET"
ls -l "$ASSET"
