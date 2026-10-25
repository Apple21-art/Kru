#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work/probe.kru" "$work/expected" <<'PY'
import pathlib,sys
constants=[0,1,-1,2,3,4,5,8,9,16,128]
values=[-9223372036854775808,-37,0,37,9223372036854775807]
source=[];calls=[];expected=[]
for i,k in enumerate(constants):
 source.append(f'fn scale{i}(x:i64)->i64 {{ret x*({k}i64)}}')
 for x in values:
  calls.append(f'pr(scale{i}({x}i64))')
  v=(x*k)&((1<<64)-1);expected.append(str(v if v<(1<<63) else v-(1<<64)))
source.append('pub fn main()->i32 {'+';'.join(calls)+';ret 0}')
pathlib.Path(sys.argv[1]).write_text('\n'.join(source))
pathlib.Path(sys.argv[2]).write_text('\n'.join(expected)+'\n')
PY
"$compiler" --emit-native "$work/probe.kru" "$work/probe"
"$work/probe" > "$work/actual"
cmp "$work/expected" "$work/actual"
python3 - "$work/probe" <<'PY'
import pathlib,sys
code=pathlib.Path(sys.argv[1]).read_bytes()
assert bytes.fromhex('488d0440') in code,'multiply by three still uses imul'
assert bytes.fromhex('48c1e007') in code,'multiply by 128 still uses imul'
print('PASS native/constant multiplication strength reduction and signed overflow boundaries')
PY
