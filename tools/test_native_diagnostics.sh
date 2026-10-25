#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
reject() {
 local name=$1 code=$2 message=$3 source=$4
 printf '%s\n' "$source" > "$work/$name.kru"
 if "$compiler" --emit-native "$work/$name.kru" "$work/$name" > "$work/log" 2>&1;then echo "FAIL diagnostics/$name accepted";exit 1;fi
 grep -Fq "error[$code]" "$work/log"
 grep -Fq "$message" "$work/log"
 grep -Fq 'Help:' "$work/log"
 echo "PASS diagnostics/$name"
}
reject literal K1040 'literal out of range' 'pub fn main()->i32 {let value:=300u8;ret 0}'
reject typed_literal K1040 'literal out of range' 'pub fn main()->i32 {let value:u8:=300;ret 0}'
reject unknown K1044 'cuont' 'pub fn main()->i32 {let count:=1;pr(cuont);ret 0}'
grep -Fq 'count' "$work/log"
reject arity K1045 'expects 1 argument' 'fn take(x:i32) {} pub fn main()->i32 {take();ret 0}'
reject field K1046 'missing' 'struct A {value:i32} pub fn main()->i32 {let a:=A{value:1};pr(a.missing);ret 0}'
reject missing_return K1047 'without ret' 'fn missing()->i32 {} pub fn main()->i32 {ret 0}'
reject match K1048 'non-exhaustive' 'pub fn main()->i32 {match true {true=>pr(1)};ret 0}'
python3 - "$work/limit.kru" <<'PY'
import sys
open(sys.argv[1],'w').write('\n'.join('fn f%d() {}'%i for i in range(512))+'\npub fn main()->i32 {ret 0}\n')
PY
if "$compiler" --emit-native "$work/limit.kru" "$work/limit" > "$work/log" 2>&1;then echo 'FAIL function limit not enforced';exit 1;fi
grep -Fq 'error[K5002]' "$work/log"
grep -Fq '512' "$work/log"
if grep -Fq 'duplicate/reserved' "$work/log";then echo 'FAIL ambiguous limit diagnostic';exit 1;fi
echo 'PASS diagnostics/function capacity distinct from duplicates'
cat > "$work/diagnostic_module.kru" <<'SOURCE'
pub fn broken()->i32 {ret misspelled}
SOURCE
printf 'use diagnostic_module\npub fn main()->i32 {ret 0}\n' > "$work/import.kru"
if "$compiler" --emit-native "$work/import.kru" "$work/import" > "$work/log" 2>&1;then echo 'FAIL invalid imported function accepted';exit 1;fi
grep -Fq "$work/diagnostic_module.kru:1:" "$work/log"
grep -Fq 'pub fn broken()->i32 {ret misspelled}' "$work/log"
echo 'PASS diagnostics/imported source filename and context'
reject string_span K1002 'initializer is incompatible' 'pub fn main()->i32 {let x:i32:="not an integer";ret 0}'
grep -Fq '^^^^^^^^^^^^^^^^' "$work/log"
