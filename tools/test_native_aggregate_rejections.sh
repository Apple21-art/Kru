#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
reject() {
    local name=$1 source=$2
    printf '%s\n' "$source" >"$work/$name.kru"
    if timeout 10 "$compiler" --emit-native "$work/$name.kru" "$work/$name" >"$work/out" 2>"$work/err"; then
        echo "FAIL native/$name unexpectedly accepted" >&2
        exit 1
    fi
    [[ ! -e "$work/$name" && -s "$work/err" ]]
    echo "PASS native/$name rejected before output"
}
reject struct_missing 'struct P { x: i32, y: i32 } pub fn main()->i32 { let p := P { x: 1 } ret 0 }'
reject struct_unknown 'struct P { x: i32 } pub fn main()->i32 { let p := P { y: 1 } ret 0 }'
reject struct_duplicate_initializer 'struct P { x: i32, y: i32 } pub fn main()->i32 { let p := P { x: 1, x: 2 } ret 0 }'
reject struct_wrong_field_type 'struct P { x: i32 } pub fn main()->i32 { let p := P { x: true } ret 0 }'
reject array_parameter_shape 'fn f(a:[i32;3])->i32 { ret a[0] } pub fn main()->i32 { ret f([1,2]) }'
reject array_layout_overflow 'pub fn main()->i32 { var a: [i32;16384] ret 0 }'
reject array_count_truncation 'pub fn main()->i32 { var a: [i32;4294967296] ret 0 }'
reject recursive_struct 'struct P { p: P } pub fn main()->i32 { ret 0 }'
reject enum_wrong_payload 'enum E { A(i32), B } pub fn main()->i32 { let e := A("wrong") ret 0 }'
reject enum_incomplete_match 'enum E { A(i32), B } pub fn main()->i32 { let e:=A(2) match e { A(n)=>{ret n} } }'
reject enum_duplicate_arm 'enum E { A(i32), B } pub fn main()->i32 { let e:=A(2) match e { A(n)=>{ret n} E.A=>{ret 0} E.B=>{ret 0} } }'
reject enum_payload_binding_count 'enum E { A(i32,bool), B } pub fn main()->i32 { let e:=A(2,true) match e { A(n)=>{ret n} E.B=>{ret 0} } }'
reject struct_duplicate_field 'struct P { x: i32, x: bool } pub fn main()->i32 { ret 0 }'
reject enum_duplicate_variant 'enum E { A, A } pub fn main()->i32 { ret 0 }'
reject secure_unsupported 'secure struct Secret { x: i32 } pub fn main()->i32 { ret 0 }'

reject nested_array_layout_overflow 'struct Bad { values: [i32; 16384] } pub fn main()->i32 { var b: Bad ret 0 }'
reject enum_array_layout_overflow 'enum Bad { A([i32; 16384]), B } pub fn main()->i32 { ret 0 }'
