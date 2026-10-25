#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/io.kru" <<'SOURCE'
fn text()->str {ret "a\0b\0c"}
pub fn main()->i32 {
 let output:=file_open_write("binary-string.bin")
 file_write(output,text())
 file_close(output)
 let input:=file_open("binary-string.bin")
 let read:=file_read(input)
 file_close(input)
 pr(read.len)
 pr(read==text())
 pr(read[1..4].len)
 pr(read[1..4][1])
 pr("é世界"[2..5])
 pr("é世界"[..].len)
 str_free(read)
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/io.kru" "$work/io"
(cd "$work" && ./io) > "$work/io.out"
printf '5\n1\n3\n98\n世\n8\n' > "$work/io.expected"
cmp "$work/io.expected" "$work/io.out"
printf 'a\0b\0c' > "$work/binary.expected"
cmp "$work/binary.expected" "$work/binary-string.bin"
for range in '1..2' '0..1' '0..3';do
 printf 'pub fn main()->i32 {pr("é"[%s]);ret 0}\n' "$range" > "$work/invalid.kru"
 "$compiler" --emit-native "$work/invalid.kru" "$work/invalid"
 set +e
 "$work/invalid" > "$work/out" 2>/dev/null
 status=$?
 set -e
 [[ $status -eq 132 ]]
done
printf 'pub fn main()->i32 {pr("\\xFF");ret 0}\n' > "$work/invalid-utf8.kru"
if "$compiler" --emit-native "$work/invalid-utf8.kru" "$work/invalid" > "$work/log" 2>&1;then echo 'FAIL invalid UTF-8 accepted';exit 1;fi
rg -q 'invalid string' "$work/log"
echo 'PASS native/length-bearing string file round trip, Unicode slices and invalid UTF-8 rejection'
cat > "$work/checked.kru" <<'SOURCE'
use kru_io
use kru_result
use kru_string
fn read_checked()->kru_result::Result<kru_string::String,kru_io::IoError> {
 var handle:=kru_io::file_open("invalid-utf8.bin")?
 ret kru_io::file_read(mut ref handle)
}
pub fn main()->i32 {
 match read_checked() {Ok(value)=>{pr(0)} Err(error)=>{pr(error.code)}}
 match kru_io::file_open("binary-string.bin\0suffix") {Ok(handle)=>{pr(0)} Err(error)=>{pr(error.code)}}
 ret 0
}
SOURCE
printf '\377' > "$work/invalid-utf8.bin"
"$compiler" --emit-native "$work/checked.kru" "$work/checked"
(cd "$work" && ./checked) > "$work/checked.out"
printf '%s\n' '-84' '-22' > "$work/checked.expected"
cmp "$work/checked.expected" "$work/checked.out"
echo 'PASS native/checked I/O reports invalid UTF-8 and embedded-zero paths'
