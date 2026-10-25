#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0
check_fail() {
    local name=$1 code=${2:-K1038} status=0
    timeout 10 "$compiler" --emit-c "$root/tests/compile_fail/$name.kru" "$work/$name.c" >"$work/$name.out" 2>"$work/$name.err" || status=$?
    if [[ $status -ne 1 ]] || ! grep -Fq "$code" "$work/$name.err"; then
        echo "FAIL sema/$name: expected rejection with $code" >&2
        cat "$work/$name.err" >&2
        failed=1
    else
        echo "PASS sema/$name ($code)"
    fi
}
check_ok() {
    local name=$1 body=$2 declarations=${3:-}
    printf '%s\npub fn main() -> i32 {\n%s\nret 0\n}\n' "$declarations" "$body" >"$work/$name.kru"
    if timeout 10 "$compiler" --emit-c "$work/$name.kru" "$work/$name.c" >"$work/$name.out" 2>"$work/$name.err"; then
        echo "PASS sema/$name"
    else
        echo "FAIL sema/$name: expected acceptance" >&2
        cat "$work/$name.err" >&2
        failed=1
    fi
}
for name in flow_if_uninitialized flow_else_uninitialized flow_branch_read flow_while_uninitialized flow_for_uninitialized flow_loop_break flow_compound_uninitialized flow_shadow_uninitialized; do
    check_fail "$name"
done
check_ok both_branches $'var value: i32\nif true { value = 1 } else { value = 2 }\npr(value)'
check_ok nested_branches $'var value: i32\nif true { if false { value = 1 } else { value = 2 } } else { value = 3 }\npr(value)'
check_ok plain_assignment $'var value: i32\nvalue = 3\nvalue += 1\npr(value)'
check_ok let_shadow $'let value := 3\nlet value := value + 1\npr(value)'
check_ok var_shadow $'var value := 3\nvar value := value + 1\nvalue = 5\npr(value)'
check_ok preserved_entry $'var value := 3\nif false { value = 4 }\nwhile false { value = 5 }\npr(value)'
check_fail place_immutable_array K1004
check_fail place_immutable_field K1004
check_fail place_immutable_nested K1004
check_fail place_uninitialized_index
check_fail place_uninitialized_pointer
check_fail place_uninitialized_array
check_fail place_uninitialized_field
check_ok mutable_array $'var values := [1, 2]\nvalues[0] = 3\nvalues[1] += 4'
check_ok mutable_field $'var point := Point { x := 1 }\npoint.x = 3' 'struct Point { x: i32 }'
check_ok mutable_nested $'var points: [Point; 1] := [Point { x := 1 }]\npoints[0].x += 3' 'struct Point { x: i32 }'
check_ok mutable_reference $'var value := 1\nlet pointer := mut ref value\npointer@ = 3'
check_ok raw_pointer $'let pointer: *i32 := mem_alloc(4)\npointer@ = 3\nmem_free(pointer)'
exit "$failed"
