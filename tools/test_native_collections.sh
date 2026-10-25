#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
timeout 10 "$compiler" --run-native "$root/tests/native_collections.kru" "$work/native" >"$work/out"
printf '19\n1\n22\n4\n13\n42\n2\n' >"$work/expected"
diff -u "$work/expected" "$work/out"
echo 'PASS native/arrays by value, indexed mutation, refs, ranges, blocks, bool arrays'
set +e
timeout 10 "$compiler" --run-native "$root/tests/native_bounds.kru" "$work/bounds" >"$work/bounds.out" 2>"$work/bounds.err"
status=$?
set -e
[[ $status -eq 132 ]]
echo 'PASS native/dynamic out-of-bounds access traps'

timeout 10 "$compiler" --run-native "$root/tests/native_control_transfer.kru" "$work/control-transfer" >"$work/transfer.out"
printf '2000000\n0\n12\n' >"$work/transfer.expected"
diff -u "$work/transfer.expected" "$work/transfer.out"
echo 'PASS native/break and continue discard pending expression temporaries'
timeout 10 "$compiler" --run-native "$root/tests/native_call_order.kru" "$work/call-order" >"$work/order.out"
printf '388\n10\n' >"$work/order.expected"
diff -u "$work/order.expected" "$work/order.out"
echo 'PASS native/ten arguments evaluated left to right with nested calls'
