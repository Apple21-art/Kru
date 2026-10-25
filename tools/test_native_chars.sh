#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_chars.kru "$work/chars"
"$work/chars" > "$work/out"
printf 'A\n字\n😀\nÿ\n字\n255\n😀\n23383\n😀\n' > "$work/expected"
cmp "$work/out" "$work/expected"
for literal in "''" "'ab'" "'\\u{D800}'" "'\\u{110000}'" "'\\q'"; do
    printf 'pub fn main() -> i32 {\n pr(%s)\n ret 0\n}\n' "$literal" > "$work/bad.kru"
    if "$compiler" --emit-native "$work/bad.kru" "$work/bad" > "$work/log" 2>&1; then exit 1;fi
done
for value in 55296 57343 1114112; do
    printf 'pub fn main() -> i32 {\n let x := %su32\n pr(cast<char>(x))\n ret 0\n}\n' "$value" > "$work/bad.kru"
    "$compiler" --emit-native "$work/bad.kru" "$work/bad"
    set +e
    "$work/bad" > "$work/bad.out" 2> "$work/bad.err"
    status=$?
    set -e
    test "$status" -ne 0;test ! -s "$work/bad.out"
done
echo 'PASS native/Unicode char literals, UTF-8 printing, padded layout and invalid scalar checks'
