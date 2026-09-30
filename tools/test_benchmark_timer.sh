#!/usr/bin/env bash
set -euo pipefail
timer=${1:-build/kru-bench-timer}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
timeout 10 "$timer" 3 true >"$work/times"
[[ $(wc -l < "$work/times") -eq 3 ]]
awk 'NF != 1 || $1 !~ /^[0-9]+\.[0-9]+$/ { exit 1 }' "$work/times"
if timeout 10 "$timer" 3 false >"$work/fail" 2>"$work/error"; then
    echo 'FAIL benchmark accepted unsuccessful command' >&2
    exit 1
fi
[[ ! -s "$work/fail" ]]
echo 'PASS benchmark/monotonic samples and failure handling'
