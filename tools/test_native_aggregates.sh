#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
compiler=${1:-build/kru0}
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native "$root/tests/native_aggregates.kru" "$work/native"
timeout 10 "$work/native" >"$work/native.out"
printf '12\n1\npacket\n34\n199\n3\n2\n12\n37\n15\n309\n9\n84\n120\n2\n1\n' >"$work/expected"
diff -u "$work/expected" "$work/native.out"
echo 'PASS native/aggregate layout, nested fields/arrays, copies and return storage'

"$compiler" --run-native "$root/tests/native_payload_enums.kru" "$work/enums" >"$work/enums.out"
printf '0\n42\n6\n7\n1\n13\n14\n' >"$work/enums.expected"
diff -u "$work/enums.expected" "$work/enums.out"
echo 'PASS native/payload enums, return copies, matching and arrays'

"$compiler" --run-native "$root/tests/native_payload_bool.kru" "$work/bool" >"$work/bool.out"
printf '2147483647\n0\n0\n' >"$work/bool.expected"
diff -u "$work/bool.expected" "$work/bool.out"
echo 'PASS native/boolean payload reads ignore stale stack bytes'

"$compiler" --run-native "$root/tests/native_payload_tuple.kru" "$work/tuple" >"$work/tuple.out"
printf '0\n7\n13\ntuple\n42\n3\n3\n' >"$work/tuple.expected"
diff -u "$work/tuple.expected" "$work/tuple.out"
echo 'PASS native/qualified tuple constructors, multiple payload bindings, method lengths'

"$compiler" --run-native "$root/tests/native_aggregate_layout_edges.kru" "$work/layout-edges" >"$work/layout.out"
printf '0\n1\n1\n9\n8\n42\n22\n' >"$work/layout.expected"
diff -u "$work/layout.expected" "$work/layout.out"
echo 'PASS native/odd-size bool structs, nested zero fill, empty arrays, pointer arrays'
