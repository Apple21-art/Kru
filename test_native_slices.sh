#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_slices.kru "$work/program"
"$work/program" > "$work/out"
printf '5\n150\n3\n90\n30\n90\n150\n18446744073709551615\n170141183460469231731687303715884105727\n' > "$work/expected"
cmp "$work/out" "$work/expected"
for body in 'let x:=1; let r:=ref x; r@=2' 'let x:=1; let r:=mut ref x' 'var x:=1u64; let r:ref i32:=ref x'; do
 printf 'pub fn main() -> i32 {\n%s\nret 0\n}\n' "$body" | tr ';' '\n' > "$work/reject.kru"
 if "$compiler" --emit-native "$work/reject.kru" "$work/reject" > "$work/log" 2>&1; then echo 'FAIL reference type/mutability violation accepted';exit 1;fi
 if rg -q 'parsing failed' "$work/log";then cat "$work/log";exit 1;fi
done
for body in 'let a:=[1,2,3]; let s:=a[1..2];pr(s[1])' 'let a:=[1,2,3];let s:=a[2..1];pr(s.len())' 'let a:=[1,2,3];let s:=a[..4];pr(s.len())' 'let a:=[1,2,3];let s:=a[..];pr(s[-1])';do
 printf 'pub fn main() -> i32 {\n%s\nret 0\n}\n' "$body" | tr ';' '\n' > "$work/bounds.kru"
 "$compiler" --emit-native "$work/bounds.kru" "$work/bounds"
 set +e
 "$work/bounds" > /dev/null 2>&1
 status=$?
 set -e
 if [[ $status != 132 ]];then echo "FAIL expected bounds trap, got $status";exit 1;fi
done
echo 'PASS native/typed references, dynamic slices, ranges, width-aware loads, mutability and bounds'
