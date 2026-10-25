#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
registry="$root/include/diagnostics.h"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

grep -RhoE 'K[0-9]{4}' "$root/src" | sort -u > "$work/source_codes"
# K0000 is an internal parser/codegen fallback buffer value, not a public code.
grep -v '^K0000$' "$work/source_codes" > "$work/public_codes"

missing=0
while read -r code; do
    if ! grep -Fq "$code" "$registry"; then
        echo "MISSING diagnostic registry entry: $code" >&2
        missing=1
    fi
done < "$work/public_codes"

if grep -RInE '"K0000:' "$root/src" >/dev/null; then
    echo 'K0000 must never be emitted as a literal public diagnostic' >&2
    missing=1
fi

[[ $missing -eq 0 ]]
echo "PASS diagnostics/audit ($(wc -l < "$work/public_codes") public codes registered)"
