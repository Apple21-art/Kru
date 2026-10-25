#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/match.kru" <<'SOURCE'
struct Small {value:i32}
struct Wide {value:i64}
impl Small {fn get(ref self)->i32 {ret self.value} fn wrap(ref self)->Option<Small> {ret Some(Small{value:self.value})}}
impl Wide {fn get(ref self)->i64 {ret self.value}}
fn identity<T>(value:T)->T {ret value}
fn choose(ok:bool)->Result<Small,Wide> {
 if ok {ret Ok(Small{value:7})}
 ret Err(Wide{value:9i64})
}
pub fn main()->i32 {
 let item:=Small{value:11}
 var i:=0
 while i<2 {
  match choose(i==0) {
   Ok(item)=>{let copy:=identity(item);if copy.get()!=7 {ret 1}}
   Err(item)=>{if item.get()!=9i64 {ret 2}}
  }
  if item.get()!=11 {ret 3}
  i=i+1
 }
 match item.wrap() {Some(inner)=>{if inner.get()!=11 {ret 4}} None=>{ret 5}}
 ret 0
}
SOURCE
"$compiler" --check-heap --emit-native "$work/match.kru" "$work/match"
"$work/match"
cat > "$work/escape.kru" <<'SOURCE'
struct Small {value:i32}
impl Small {fn get(ref self)->i32 {ret self.value}}
pub fn main()->i32 {
 let value:Option<Small>:=Some(Small{value:7})
 match value {Some(payload)=>{} None=>{}}
 ret payload.get()
}
SOURCE
if "$compiler" --emit-native "$work/escape.kru" "$work/escape" > "$work/log" 2>&1; then
 echo 'FAIL match payload escaped its arm' >&2; exit 1
fi
echo 'PASS native/match payload methods, per-arm types, shadow restoration and scope rejection'
