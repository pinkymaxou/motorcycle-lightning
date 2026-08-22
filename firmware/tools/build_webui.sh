#!/usr/bin/env bash
# Inline webui/{style.css,app.js} into index.html and gzip the result into the
# firmware-embedded asset. The sources stay split for editing; the module still
# serves one request — it has a single httpd task and a WebSocket stream to
# feed, so extra round-trips and extra embedded blobs buy nothing there.
# Run after editing anything in webui/.
set -euo pipefail
cd "$(dirname "$0")/.."

# A symbol deleted during a refactor otherwise ships as a blank page.
python3 tools/check_webui.py webui

mkdir -p components/net_services/webui_dist
python3 - <<'PYEOF' | gzip -9 -c > components/net_services/webui_dist/index.html.gz
import pathlib
import sys

d = pathlib.Path('webui')
html = (d / 'index.html').read_text(encoding='utf-8')
for tag, wrap in (('<link rel="stylesheet" href="style.css">',
                   ('style.css', '<style>\n%s</style>')),
                  ('<script src="app.js"></script>',
                   ('app.js', '<script>\n%s</script>'))):
    name, fmt = wrap
    if tag not in html:
        sys.exit('index.html no longer references %s — nothing to inline' % name)
    html = html.replace(tag, fmt % (d / name).read_text(encoding='utf-8'))
sys.stdout.write(html)
PYEOF
ls -l components/net_services/webui_dist/index.html.gz
