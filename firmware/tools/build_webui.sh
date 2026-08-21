#!/usr/bin/env bash
# Gzip the SPA into the firmware-embedded asset. Run after editing webui/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p components/net_services/webui_dist
gzip -9 -c webui/index.html > components/net_services/webui_dist/index.html.gz
ls -l components/net_services/webui_dist/index.html.gz
