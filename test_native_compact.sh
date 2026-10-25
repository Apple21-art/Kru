#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/probe.kru" <<'SOURCE'
fn scale(a:i64,b:i64,c:i64,d:i64,e:i64,f:i64)->i64 {ret a*3+b*8+c*(-7)+d+e+f}
pub fn main()->i32 {pr(scale(11i64,12i64,13i64,14i64,15i64,16i64));ret 0}
SOURCE
"$compiler" --emit-native "$work/probe.kru" "$work/probe"
"$work/probe" > "$work/actual"
printf '83\n' > "$work/expected"
cmp "$work/expected" "$work/actual"
python3 - "$work/probe" <<'PYCODE'
import pathlib,sys
code=pathlib.Path(sys.argv[1]).read_bytes()
for opcode in ['4889bd','4889b5','488995','48898d','4c8985','4c898d','488d0440']:
 assert bytes.fromhex(opcode) in code, 'missing compact instruction '+opcode
print('PASS native/compact six-register spills and immediate multiply')
PYCODE
