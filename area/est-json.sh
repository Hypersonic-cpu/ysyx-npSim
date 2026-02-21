#!/bin/bash
# Estimate area for an npSim output JSON file using CACTI.
# Usage: ./area/est-json.sh simout/NAME.json

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NPSIM_HOME="$(dirname "$SCRIPT_DIR")"
CONF_JSON="$1"

if [ -z "$CONF_JSON" ]; then
    echo "Usage: $0 <simout/NAME.json>"
    exit 1
fi

BASENAME=$(basename "$CONF_JSON" .json)
OUTDIR="$NPSIM_HOME/areaout/$BASENAME"

python3 "$SCRIPT_DIR/area_est.py" \
    --conf-json "$CONF_JSON" \
    --out-dir "$OUTDIR"
