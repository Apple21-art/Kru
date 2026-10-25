#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/no-tools"
PATH="$work/no-tools" "$compiler" --check-heap --emit-native "$root/tests/native_string_operations.kru" "$work/native"
timeout 10 "$work/native" >"$work/native.out"
cat >"$work/expected" <<'EXPECTED'
native strings
14
14
110
115
abcd
left
right
0
escaped
bytes	!
EXPECTED
printf 'a\0ignoredb\n10\n' >>"$work/expected"
cmp "$work/expected" "$work/native.out"
echo 'PASS native/string concat, empty strings, nested calls, len, byte indexing preserve exact length-bearing output'
for index in '-1' '3' '2147483647'; do
    cat >"$work/bounds.kru" <<SOURCE
pub fn main() -> i32 {
    let text := str_concat("a", "bc")
    var index: i32 := $index
    pr(text[index])
    ret 0
}
SOURCE
    PATH="$work/no-tools" "$compiler" --emit-native "$work/bounds.kru" "$work/bounds"
    set +e
    timeout 10 "$work/bounds" >"$work/bounds.out" 2>"$work/bounds.err"
    status=$?
    set -e
    [[ $status -eq 132 ]]
    [[ ! -s "$work/bounds.out" ]]
    echo "PASS native/string index $index traps"
done
cat >"$work/empty.kru" <<'SOURCE'
pub fn main() -> i32 {
    let text := str_concat("", "")
    pr(text[0])
    ret 0
}
SOURCE
PATH="$work/no-tools" "$compiler" --emit-native "$work/empty.kru" "$work/empty"
set +e
timeout 10 "$work/empty" >"$work/empty.out" 2>"$work/empty.err"
status=$?
set -e
[[ $status -eq 132 ]]
[[ ! -s "$work/empty.out" ]]
echo 'PASS native/empty string indexing traps'

# Byte indexing zero-extends; the C bootstrap uses host-char signedness here.
cat >"$work/byte.kru" <<'SOURCE'
pub fn main() -> i32 {
    let text: str := "\u{ff}"
    pr(text[0])
    ret 0
}
SOURCE
PATH="$work/no-tools" "$compiler" --emit-native "$work/byte.kru" "$work/byte"
timeout 10 "$work/byte" >"$work/byte.out"
printf '195\n' >"$work/byte.expected"
cmp "$work/byte.expected" "$work/byte.out"
echo 'PASS native/string indexing returns unsigned bytes'
