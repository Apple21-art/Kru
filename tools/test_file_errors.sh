#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if timeout 10 "$compiler" --run-native "$work/missing source.kru" >"$work/out" 2>"$work/error"; then
    echo 'FAIL file-errors/missing source accepted' >&2
    exit 1
fi
grep -Fq "$work/missing source.kru" "$work/error"
grep -Fq 'No such file or directory' "$work/error"
echo 'PASS file-errors/missing input includes exact path and cause'
if timeout 10 "$compiler" --emit-c "$work" >"$work/dir.out" 2>"$work/dir.err"; then
    echo 'FAIL file-errors/directory accepted' >&2
    exit 1
fi
grep -Fq "$work" "$work/dir.err"
grep -Fq 'Invalid argument' "$work/dir.err"
echo 'PASS file-errors/non-regular input has explicit cause'
