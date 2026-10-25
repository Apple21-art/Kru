#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_integer_widths.kru "$work/widths"
timeout 10 "$work/widths" > "$work/output"
cat > "$work/expected" <<'OUT'
-128
255
-32768
65535
-9223372036854775808
18446744073709551615
6148914691236517205
0
1
1
-1
1
-8000000000
18446744073709551615
1
-16384
1
1
1
21845
7
OUT
diff -u "$work/expected" "$work/output"
"$compiler" --emit-native tests/native_integer_widths.kru "$work/again"
cmp "$work/widths" "$work/again"
if readelf -l "$work/widths" | rg -q INTERP; then exit 1; fi
echo 'PASS native/integer widths, signedness, layout, nested and stack calls, deterministic ELF'
for source in \
    'pub fn main() -> i32 { let x: u8 := 256; ret 0 }' \
    'pub fn main() -> i32 { let x: i8 := -129; ret 0 }' \
    'pub fn main() -> i32 { let x: i64 := 1u64; ret 0 }' \
    'pub fn main() -> i32 { let x := 1u64; let y := 2i64; pr(x + y); ret 0 }'; do
    printf '%s\n' "$source" | tr ';' '\n' > "$work/bad.kru"
    if "$compiler" --emit-native "$work/bad.kru" "$work/bad" > "$work/log" 2>&1; then
        cat "$work/log"; echo "FAIL invalid numeric input: $source"; exit 1
    fi
    if rg -q 'parsing failed' "$work/log"; then cat "$work/log";exit 1;fi
    test ! -e "$work/bad"
done
echo 'PASS native/invalid integer ranges and explicit type mismatches rejected'
"$compiler" --emit-native tests/native_primitive_match.kru "$work/matches"
"$work/matches" > "$work/matches.out"
printf '1\n1\n1\n1\n1\n2\n' > "$work/matches.expected"
cmp "$work/matches.out" "$work/matches.expected"
for arms in 'true => {pr(1)}' 'true => {pr(1)} true => {pr(2)} false => {pr(3)}'; do
    printf 'pub fn main() -> i32 {\n let value := true\n match value {\n %s\n}\n ret 0\n}\n' "$arms" > "$work/bool.kru"
    if "$compiler" --emit-native "$work/bool.kru" "$work/bool" > "$work/log" 2>&1; then exit 1;fi
done
echo 'PASS native/wide integer and char matching, exhaustive bool matches and rejection checks'
"$compiler" --emit-native tests/native_primitive_storage.kru "$work/storage"
"$work/storage" > "$work/storage.out"
printf '2\n11\n' > "$work/storage.expected"
diff -u "$work/storage.expected" "$work/storage.out"
echo 'PASS native/compound evaluation snapshots and zero-fill stack frame sizing'

cat > "$work/constants.kru" <<'KRU'
const X: u8 := 255
const F: f32 := 1.5
pub fn main() -> i32 {
let y: u8 := X
pr(y)
pr(F+1.0f32)
ret 0
}
KRU
"$compiler" --emit-native "$work/constants.kru" "$work/constants"
"$work/constants" > "$work/constants.out"
printf '255\n2.5\n' > "$work/constants.expected"
cmp "$work/constants.out" "$work/constants.expected"
printf 'const X: u8 := 1u64\npub fn main() -> i32 { ret 0 }\n' > "$work/bad-constant.kru"
if "$compiler" --emit-native "$work/bad-constant.kru" "$work/bad-constant" > "$work/log" 2>&1; then exit 1; fi
rg -q 'constant initializer does not fit' "$work/log"
echo 'PASS native/constant type identity and initializer validation'
