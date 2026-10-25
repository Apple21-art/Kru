#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/clean.kru" <<'SOURCE'
pub fn main()->i32 {let p:=mem_alloc(64);mem_free(p);ret 7}
SOURCE
"$compiler" --check-heap --emit-native "$work/clean.kru" "$work/clean"
set +e
"$work/clean" > "$work/out" 2> "$work/log"
status=$?
set -e
[[ $status == 7 && ! -s "$work/log" ]]
cat > "$work/leak.kru" <<'SOURCE'
pub fn main()->i32 {let p:=mem_alloc(64);ret 0}
SOURCE
"$compiler" --check-heap --emit-native "$work/leak.kru" "$work/leak"
set +e
"$work/leak" > "$work/out" 2> "$work/log"
status=$?
set -e
[[ $status == 99 ]]
rg -q 'heap allocations remain live at exit' "$work/log"
cat > "$work/owners.kru" <<'SOURCE'
use kru_string
pub fn main()->i32 {
 var i:i32:=0
 while i<10000 {var text:=kru_string::String::from("hello");text.push_str(" world");i=i+1}
 ret 0
}
SOURCE
"$compiler" --check-heap --emit-native "$work/owners.kru" "$work/owners"
"$work/owners"
echo 'PASS native/heap audit detects raw leaks, preserves exit status and verifies 10000 String drops'
printf 'hello owned I/O' > "$work/owned-read.txt"
cat > "$work/owned-read.kru" <<'SOURCE'
use kru_io
pub fn main()->i32 {
 var i:i32:=0
 while i<100 {
  match kru_io::file_open("owned-read.txt") {
   Ok(handle)=>{var input:=handle;match kru_io::file_read(mut ref input) {Ok(text)=>{if text.len()!=15usize {ret 2}} Err(error)=>{ret 3}}}
   Err(error)=>{ret 4}
  }
  i=i+1
 }
 ret 0
}
SOURCE
"$compiler" --check-heap --emit-native "$work/owned-read.kru" "$work/owned-read"
(cd "$work" && ./owned-read)
echo 'PASS native/owned file reads release buffers across 100 iterations'
printf '\377' > "$work/owned-read.txt"
cat > "$work/owned-error.kru" <<'SOURCE'
use kru_io
pub fn main()->i32 {
 match kru_io::file_open("owned-read.txt") {
  Ok(handle)=>{var input:=handle;match kru_io::file_read(mut ref input) {Ok(text)=>{ret 2} Err(error)=>{if error.code != -84i64 {ret 3}}}}
  Err(error)=>{ret 4}
 }
 ret 0
}
SOURCE
"$compiler" --check-heap --emit-native "$work/owned-error.kru" "$work/owned-error"
(cd "$work" && ./owned-error)
echo 'PASS native/failed UTF-8 owned reads release their buffer'
python3 - "$work/large-owned.txt" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b'a\0b\n'*32768)
PY
: > "$work/empty-owned.txt"
cat > "$work/owned-transfer.kru" <<'SOURCE'
use kru_io
use kru_string
use kru_result
fn read_owned(path:str)->kru_result::Result<kru_string::String,kru_io::IoError> {
 var input:=kru_io::file_open(path)?
 let text:=kru_io::read_to_string(mut ref input)?
 kru_io::file_close(mut ref input)?
 ret Ok(text)
}
fn probe()->i32 {
 let prefix:=kru_string::String::from("temporary")
 match read_owned("large-owned.txt") {
  Ok(text)=>{
   if text.len()!=131072usize {ret 2}
   var grown:=text
   grown.push_str("!")
   if grown.len()!=131073usize {ret 8}
   let view:=grown.as_str()
   if view[0]!=97 || view[1]!=0 || view[131071]!=10 || view[131072]!=33 {ret 3}
   match read_owned("empty-owned.txt") {Ok(empty)=>{if empty.len()!=0usize {ret 4}} Err(error)=>{ret 5}}
   ret 0
  }
  Err(error)=>{ret 6}
 }
 ret 7
}
pub fn main()->i32 {
 var i:=0
 while i<100 {let status:=probe();if status!=0 {ret status};i=i+1}
 ret 0
}
SOURCE
"$compiler" --check-heap --emit-native "$work/owned-transfer.kru" "$work/owned-transfer"
(cd "$work" && ./owned-transfer)
echo 'PASS native/owned read transfer, closed handles, embedded zeros, growth, empty files and early-return cleanup'
