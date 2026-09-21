#!/usr/bin/env bash
set -euo pipefail

RESULTS_DIR="benchmarks/results"
JSON="$RESULTS_DIR/results.json"
CSV="$RESULTS_DIR/results.csv"
CHARTS="$RESULTS_DIR/charts"

mkdir -p "$RESULTS_DIR" "$CHARTS"

echo "=== Running benchmarks ==="
python3 benchmarks/run.py \
    --out "$JSON" \
    --csv "$CSV" \
    "$@"

echo ""
echo "=== Generating charts ==="
python3 benchmarks/visualize.py \
    --csv "$CSV" \
    --out "$CHARTS"

echo ""
echo "Done. Results in $RESULTS_DIR/"
