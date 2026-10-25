#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/leaf.kru" <<'SOURCE'
fn loop_value(seed:i64)->i64 {
 var value:i64:=seed
 var i:i64:=0i64
 while i<10000i64 {value=(value*3i64+7i64)%1000000007i64;i=i+1i64}
 ret value
}
fn narrow(x:i8)->i8 {var y:i8:=x;y=y+1i8;ret y}
fn branch(x:i32)->i32 {var y:i32:=3;if x>0 {var y:i32:=7;y=y+x;ret y};y=y-x;ret y}
pub fn main()->i32 {pr(loop_value(1i64));pr(narrow(-128i8));pr(branch(4));pr(branch(-4));ret 0}
SOURCE
"$compiler" --emit-native "$work/leaf.kru" "$work/leaf"
"$work/leaf" > "$work/out"
python3 - "$work/leaf" "$work/out" <<'PY'
import pathlib,sys
x=1
for _ in range(10000):x=(x*3+7)%1000000007
assert pathlib.Path(sys.argv[2]).read_text()==f'{x}\n-127\n11\n7\n'
code=pathlib.Path(sys.argv[1]).read_bytes()
# The loop accumulator (second scalar slot) feeds immediate multiplication.
assert bytes.fromhex('4c89c8488d0440') in code,'leaf accumulator is still memory-resident'
print('PASS native/leaf registers preserve loops, narrow signed values and shadowing')
PY
cat > "$work/boundaries.kru" <<'SOURCE'
fn wrap(x:i8)->i8 {var y:i8:=x;y=y+1i8;ret y}
fn join(x:i32)->i32 {var y:i32:=4;if x>0 {y=y+x} else {y=y-x};ret y}
fn six(a:i64,b:i64,c:i64,d:i64,e:i64,f:i64)->i64 {ret a+10i64*b+100i64*c+1000i64*d+10000i64*e+100000i64*f}
fn called(x:i32)->i32 {pr(99);ret x*3}
fn with_call(x:i32)->i32 {var y:i32:=x;y=called(y);ret y+x}
fn borrowed(x:i32)->i32 {var y:i32:=x;let r:=ref y;ret r@}
pub fn main()->i32 {pr(wrap(127i8));pr(join(3));pr(join(-3));pr(six(1i64,2i64,3i64,4i64,5i64,6i64));pr(with_call(7));pr(borrowed(12));ret 0}
SOURCE
"$compiler" --emit-native "$work/boundaries.kru" "$work/boundaries"
"$work/boundaries" > "$work/out"
printf '%s\n' -128 7 7 654321 99 28 12 > "$work/expected"
cmp "$work/out" "$work/expected"
echo 'PASS native/register overflow, joins, six arguments and call/reference fallbacks'
