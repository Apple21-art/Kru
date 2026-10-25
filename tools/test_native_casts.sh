#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_casts.kru "$work/casts"
"$work/casts" > "$work/out"
printf '255\n-128\n127\n9223372036854775807\n4294967295\n18446744073709551615\n' > "$work/expected"
cmp "$work/out" "$work/expected"
echo 'PASS native/checked integer casts preserve valid boundary values'
for pair in 'u8 256u64' 'i8 128u64' 'u64 -1i64' 'i64 9223372036854775808u64'; do
    read -r target value <<< "$pair"
    printf 'pub fn main() -> i32 {\n let n := %s\n pr(cast<%s>(n))\n ret 0\n }\n' "$value" "$target" > "$work/bad.kru"
    "$compiler" --emit-native "$work/bad.kru" "$work/bad"
    set +e
    "$work/bad" > "$work/out" 2> "$work/err"
    status=$?
    set -e
    test "$status" -ne 0
    test ! -s "$work/out"
    printf 'pub fn main() -> i32 {\n pr(cast<%s>(%s))\n ret 0\n }\n' "$target" "$value" > "$work/literal.kru"
    if "$compiler" --emit-native "$work/literal.kru" "$work/literal" > "$work/log" 2>&1; then exit 1; fi
done
echo 'PASS native/computed out-of-range casts trap and literal out-of-range casts reject'
