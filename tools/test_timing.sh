#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

timeout 10 "$compiler" --time --emit-c "$root/tests/hello.kru" "$work/hello.c" >"$work/stdout" 2>"$work/timing"
[[ ! -s "$work/stdout" && -s "$work/hello.c" && ! -e "$work/hello.c.bin" ]]
for label in 'Read source' 'Lex + parse' 'Semantic checks' 'Generate C' 'Total compile' 'Parse throughput' 'Source bytes'; do
    grep -Fq "$label" "$work/timing"
done
grep -Eq 'Total compile[[:space:]]+[0-9]+\.[0-9]+ ms' "$work/timing"
echo 'PASS timing/emit-C stage breakdown without execution'

timeout 10 "$compiler" --emit-native --time "$root/tests/native_flow.kru" "$work/native" 2>"$work/native.time"
grep -Fq 'Generate native' "$work/native.time"
set +e
"$work/native"
status=$?
set -e
[[ $status -eq 27 ]]
echo 'PASS timing/native flags in either order and correct executable'

set +e
timeout 10 "$compiler" --time --run "$root/tests/hello.kru" "$work/run.c" >"$work/run.out" 2>"$work/run.time"
status=$?
set -e
[[ $status -eq 42 ]]
grep -Fq 'Host C build' "$work/run.time"
grep -Fq 'Program runtime excluded' "$work/run.time"
echo 'PASS timing/run preserves target status and excludes runtime'

timeout 10 "$compiler" --emit-c "$root/tests/hello.kru" "$work/quiet.c" 2>"$work/quiet.err"
[[ ! -s "$work/quiet.err" ]]
echo 'PASS timing/disabled by default'
