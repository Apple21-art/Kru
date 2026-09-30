#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

check_fail() {
    local name=$1 code=$2
    set +e
    timeout 10 "$compiler" --emit-c "$root/tests/compile_fail/$name.kru" "$work/$name.c" >"$work/$name.out" 2>"$work/$name.err"
    local status=$?
    set -e
    if [[ $status -eq 0 ]]; then
        echo "FAIL compile-fail/$name: unexpectedly compiled" >&2
        return 1
    fi
    grep -Fq "$code" "$work/$name.err"
    echo "PASS compile-fail/$name ($code)"
}

check_fail immutable_assignment K1004
check_fail type_mismatch K1002
check_fail integer_range K1040
check_fail malformed_expression K1005
check_fail for_non_array K1042
