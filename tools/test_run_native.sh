#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

set +e
timeout 10 "$compiler" --time --run-native "$root/tests/native_flow.kru" "$work/native program" >"$work/output" 2>"$work/timing"
status=$?
set -e
[[ $status -eq 27 && -x "$work/native program" ]]
grep -Fq 'Generate native' "$work/timing"
grep -Fq 'Program runtime excluded' "$work/timing"
! grep -Fq 'Host C build' "$work/timing"
echo 'PASS run-native/compile execute and propagate exit with spaced path'

set +e
(cd "$work" && timeout 10 "$compiler" --run-native "$root/tests/hello.kru") >"$work/default.out" 2>"$work/default.err"
status=$?
set -e
[[ $status -eq 42 && -x "$work/a.out" && ! -s "$work/default.err" ]]
echo 'PASS run-native/default relative output and timing opt-in'

# A compile failure must not execute a stale executable at the output path.
printf '#!/bin/sh\necho stale > "%s"\n' "$work/stale_marker" >"$work/previous"
chmod +x "$work/previous"
if timeout 10 "$compiler" --run-native "$root/tests/native_bad_type.kru" "$work/previous" >"$work/fail.out" 2>"$work/fail.err"; then
    echo 'FAIL run-native accepted unsupported input' >&2
    exit 1
fi
[[ ! -e "$work/stale_marker" ]]
echo 'PASS run-native/failed compile never executes stale output'
