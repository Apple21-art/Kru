#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
check() {
 local source=$1 expected=$2
 "$compiler" --emit-native "tests/$source.kru" "$work/$source"
 "$work/$source" > "$work/$source.out"
 printf '%s\n' "$expected" > "$work/$source.expected"
 cmp "$work/$source.out" "$work/$source.expected"
 echo "PASS native/$source"
}
check native_string_slices $'3\n98\n0\n1\n3\n42\n5\n99\n5\n5\n1\n8\nfirst second'
check native_trait_constraints 42
check native_generic_inference $'42\n7\n9\n12'
check native_methods $'21\n42\n21'
check native_result_option $'100\n5\nzero\n200\n8\n-1'
check native_comptime_blocks $'12\n10'
check native_comptime_scalars $'42\n3.25\n0\n1\n1\n0\ntext'
check native_comptime $'45\n46'
check native_drop $'20\n10\n42\n0\n1\n99'
check native_drop_blocks $'30\n30'
check native_owned_moves 5
check native_match_owners $'7\n7\n11\n11\n12\n7\n7\n11\n11\n12'
check native_owner_reassign $'1\n3\n4\n2\n3'
printf 'use kru_string\npub fn main()->i32 {let a:=kru_string::String::new();let b:=a;pr(a.len());ret 0}\n' > "$work/moved.kru"
if "$compiler" --emit-native "$work/moved.kru" "$work/moved" > "$work/log" 2>&1;then echo 'FAIL accepted moved owner';exit 1;fi
rg -q 'moved owning value' "$work/log"
printf 'fn bad()->ref i32 {let x:=42;ret ref x}\npub fn main()->i32 {ret 0}\n' > "$work/escape.kru"
if "$compiler" --emit-native "$work/escape.kru" "$work/escape" > "$work/log" 2>&1;then echo 'FAIL accepted dangling reference';exit 1;fi
rg -q 'reference to local storage escapes' "$work/log"
printf 'comptime fn bad()->i32 {pr(1);ret 0}\nconst X:=bad()\npub fn main()->i32 {ret 0}\n' > "$work/sandbox.kru"
if "$compiler" --emit-native "$work/sandbox.kru" "$work/sandbox" > "$work/log" 2>&1;then echo 'FAIL accepted comptime I/O';exit 1;fi
rg -q 'sandbox forbids' "$work/log"
echo 'PASS native/ownership, reference escape and compile-time sandbox rejections'

reject_comptime() {
 local name=$1 source=$2 diagnostic=$3
 printf '%s\n' "$source" > "$work/$name.kru"
 if "$compiler" --emit-native "$work/$name.kru" "$work/$name" > "$work/log" 2>&1;then echo "FAIL accepted $name";exit 1;fi
 rg -q "$diagnostic" "$work/log"
 echo "PASS comptime/$name rejected"
}
reject_comptime caller_scope 'comptime fn inner()->i32 {ret secret} comptime fn outer()->i32 {let secret:=7;ret inner()} const A:=outer() pub fn main()->i32 {ret 0}' 'not available at compile time'
reject_comptime float_bitwise 'const A:=1.5 & 2.0 pub fn main()->i32 {ret 0}' 'invalid for operand type'
reject_comptime bool_arithmetic 'const A:=true + false pub fn main()->i32 {ret 0}' 'invalid for operand type'
reject_comptime erased_type_error 'const A:=comptime {let bad:i32:="wrong";42} pub fn main()->i32 {ret 0}' 'binding type mismatch'
reject_comptime integer_condition 'const A:=comptime {while 0 {};42} pub fn main()->i32 {ret 0}' 'condition requires bool'
reject_comptime missing_trait 'trait Named {fn name(ref self)->i32} struct Thing {x:i32} fn accept<T:Named>(x:T)->i32 {ret 1} pub fn main()->i32 {ret accept(Thing{x:1})}' 'does not implement required trait'
reject_comptime missing_trait_method 'trait Named {fn name(ref self)->i32} struct Thing {x:i32} impl Named for Thing {} pub fn main()->i32 {ret 0}' 'missing a required method'
reject_comptime wrong_trait_method 'trait Named {fn name(ref self)->i32} struct Thing {x:i32} impl Named for Thing {fn name(self)->i32 {ret 1}} pub fn main()->i32 {ret 0}' 'signature does not match'
reject_comptime inconsistent_inference 'fn same<T>(a:T,b:T)->T {ret a} pub fn main()->i32 {let x:=same(1,true);ret 0}' 'cannot infer consistent generic arguments'
reject_comptime dangling_string_view 'use kru_string
fn bad()->str {let owner:=kru_string::String::from("gone");ret owner.as_str()} pub fn main()->i32 {ret 0}' 'reference to local storage escapes'
