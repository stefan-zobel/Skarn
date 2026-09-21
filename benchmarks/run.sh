#!/usr/bin/env bash
# Skarn benchmark runner — quick shell wrapper.
# Measures wall time (from VM-internal nanoTime), user/sys CPU, and peak RSS
# using /usr/bin/time.  For JSON output and richer analysis use run.py.
#
# Usage:
#   bash benchmarks/run.sh
#   bash benchmarks/run.sh --vm /path/to/skarnvm
#
# Env override: VM_PATH=/path/to/skarnvm bash benchmarks/run.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VM="${VM_PATH:-$REPO_ROOT/build/skarnvm}"
PROGS="$SCRIPT_DIR/programs"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vm) VM="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ ! -x "$VM" ]]; then
    echo "error: skarnvm not found at: $VM" >&2
    echo "       Build with: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release" >&2
    echo "                   cmake --build build --target skarnvm" >&2
    exit 1
fi

IS_MAC=0; [[ "$(uname)" == "Darwin" ]] && IS_MAC=1

echo "=== Skarn Benchmark Runner ==="
echo "VM:  $VM"
if [[ $IS_MAC -eq 1 ]]; then
    echo "CPU: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
else
    echo "CPU: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo unknown)"
fi
echo "Date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo ""

printf "%-18s  %-12s  %-10s  %-8s  %-10s\n" "benchmark" "wall(VM)" "user_cpu" "sys_cpu" "peak_rss"
printf "%-18s  %-12s  %-10s  %-8s  %-10s\n" "------------------" "------------" "----------" "--------" "----------"

for prog in "$PROGS"/skarn/*.skn; do
    name="$(basename "$prog" .skn)"
    tmp="$(mktemp)"

    if [[ $IS_MAC -eq 1 ]]; then
        vm_out="$( /usr/bin/time -l "$VM" "$prog" 2>"$tmp" )" || true
    else
        vm_out="$( /usr/bin/time -v "$VM" "$prog" 2>"$tmp" )" || true
    fi
    time_out="$(cat "$tmp")"
    rm -f "$tmp"

    wall_ns="$(echo "$vm_out" | grep -oE 'wall_ns=[0-9]+' | head -1 | cut -d= -f2 || true)"
    if [[ -n "$wall_ns" && "$wall_ns" -gt 0 ]]; then
        wall_ms=$(( wall_ns / 1000000 ))
        if (( wall_ms >= 1000 )); then
            wall_fmt="$(echo "scale=2; $wall_ms/1000" | bc)s"
        else
            wall_fmt="${wall_ms}ms"
        fi
    else
        wall_fmt="N/A"
    fi

    if [[ $IS_MAC -eq 1 ]]; then
        user="$(echo "$time_out" | grep -oE '[0-9]+\.[0-9]+ user' | awk '{print $1}' || true)"
        sys="$(echo  "$time_out" | grep -oE '[0-9]+\.[0-9]+ sys'  | awk '{print $1}' || true)"
        rss_b="$(echo "$time_out" | grep -i 'maximum resident set size' | awk '{print $1}' || true)"
        rss_fmt="$( [[ -n "${rss_b:-}" ]] && echo "$(( rss_b / 1024 ))K" || echo "N/A" )"
    else
        user="$(echo "$time_out" | grep 'User time'    | grep -oE '[0-9]+\.[0-9]+' || true)"
        sys="$(echo  "$time_out" | grep 'System time'  | grep -oE '[0-9]+\.[0-9]+' || true)"
        rss_k="$(echo "$time_out" | grep 'Maximum resident set size' | awk '{print $NF}' || true)"
        rss_fmt="$( [[ -n "${rss_k:-}" ]] && echo "${rss_k}K" || echo "N/A" )"
    fi

    printf "%-18s  %-12s  %-10s  %-8s  %-10s\n" \
        "$name" "$wall_fmt" "${user:-N/A}s" "${sys:-N/A}s" "$rss_fmt"
done

echo ""
if command -v python3 >/dev/null 2>&1; then
    echo "Tip: python3 benchmarks/run.py  — JSON output, richer metrics, CSV export"
fi
