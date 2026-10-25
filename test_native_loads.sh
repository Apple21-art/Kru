#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/loads.kru" <<'SOURCE'
fn signed_byte(x:i8)->i8 {ret x}
fn unsigned_byte(x:u8)->u8 {ret x}
fn signed_word(x:i16)->i16 {ret x}
fn unsigned_word(x:u16)->u16 {ret x}
fn signed_dword(x:i32)->i32 {ret x}
fn unsigned_dword(x:u32)->u32 {ret x}
fn qword(x:u64)->u64 {ret x}
pub fn main()->i32 {
 pr(signed_byte(-128i8))
 pr(unsigned_byte(255u8))
 pr(signed_word(-32768i16))
 pr(unsigned_word(65535u16))
 pr(signed_dword(-2147483648i32))
 pr(unsigned_dword(4294967295u32))
 pr(qword(18446744073709551615u64))
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/loads.kru" "$work/loads"
"$work/loads" > "$work/actual"
printf '%s\n' -128 255 -32768 65535 -2147483648 4294967295 18446744073709551615 > "$work/expected"
cmp "$work/expected" "$work/actual"
python3 - "$work/loads" <<'PY'
import pathlib,sys
code=pathlib.Path(sys.argv[1]).read_bytes()
for opcode in ['480fbe85','0fb685','480fbf85','0fb785','486385','8b85','488b85']:
 if bytes.fromhex(opcode) not in code:raise SystemExit('FAIL missing direct stack load '+opcode)
print('PASS native/direct frame loads preserve signedness at every integer width')
PY
