#!/bin/bash
# Estimate area from npSim config JSON using CACTI.
# Usage: ./area/est-json.sh simout/OUTDIR/conf.json
#   or:  ./area/est-json.sh simout/NAME.json  (legacy)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NPSIM_HOME="$(dirname "$SCRIPT_DIR")"
CONF_JSON="$1"

if [ -z "$CONF_JSON" ]; then
    echo "Usage: $0 <simout/OUTDIR/conf.json | simout/NAME.json>"
    exit 1
fi

# Determine output directory mirroring simout structure into areaout
REL="${CONF_JSON#$NPSIM_HOME/simout/}"
REL="${REL#simout/}"
DIR="$(dirname "$REL")"
if [ "$DIR" = "." ]; then
    # Legacy: simout/NAME.json -> areaout/NAME/
    BASENAME=$(basename "$CONF_JSON" .json)
    OUTDIR="$NPSIM_HOME/areaout/$BASENAME"
else
    # New: simout/sweep_a/tag/conf.json -> areaout/sweep_a/tag/
    OUTDIR="$NPSIM_HOME/areaout/$DIR"
fi

python3 "$SCRIPT_DIR/area_est.py" \
    --conf-json "$CONF_JSON" \
    --outdir "$OUTDIR"
