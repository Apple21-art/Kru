#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# --emit-c must compile only. hello.kru returns 42 if executed, so a zero
# compiler status proves the target was not run.
timeout 10 "$compiler" --emit-c "$root/tests/hello.kru" "$work/hello.c"
[[ -s "$work/hello.c" ]]
[[ ! -e "$work/hello.c.bin" ]]

echo 'PASS cli emit-c is compile-only'
