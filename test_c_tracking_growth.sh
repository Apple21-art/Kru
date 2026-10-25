#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ulimit -c 0
python3 - "$work" <<'PY'
from pathlib import Path
import sys
root=Path(sys.argv[1])
prefix='pub fn main()->i32 {\n'+''.join(f'let v{i}:=0\n' for i in range(1300))
for name,body in {
 'valid':'var arr:[i32;2]:=[7,9]\nvar i:=1\narr[i]=11\nif true {let arr:[i32;1]:=[3];pr(arr[0])}\npr(arr[i])\nret 0',
 'read':'let arr:[i32;2]:=[7,9]\nvar i:=2\npr(arr[i])\nret 0',
 'write':'var arr:[i32;2]:=[7,9]\nvar i:=2\narr[i]=11\nret 0',
 'nested':'let arr:[[i32;2];2]:=[[1,2],[3,4]]\nvar i:=2\npr(arr[0][i])\nret 0'
}.items():
 (root/(name+'.kru')).write_text(prefix+body+'\n}\n')
PY
for name in valid read write nested; do
 "$compiler" --emit-c "$work/$name.kru" "$work/$name.c" 2> "$work/$name.log"
 test ! -s "$work/$name.log"
 rg -Fq '__kru_check_index((int64_t)(i),' "$work/$name.c"
 "${CC:-cc}" -w "$work/$name.c" -o "$work/$name"
done
"$work/valid" > "$work/actual"
printf '3\n11\n' > "$work/expected"
cmp "$work/actual" "$work/expected"
for name in read write nested; do
 set +e
 "$work/$name" > "$work/out" 2> "$work/err"
 status=$?
 set -e
 [[ $status != 0 ]]
 rg -qi 'bounds' "$work/err"
done
python3 - "$work/long-name.kru" <<'PYCODE'
from pathlib import Path
import sys
name='a'*128
Path(sys.argv[1]).write_text(f'pub fn main()->i32 {{let {name}:[i32;2]:=[1,2];ret 0}}')
PYCODE
if "$compiler" --emit-c "$work/long-name.kru" "$work/long-name.c" > "$work/log" 2>&1; then
 echo 'FAIL untrackable binding accepted' >&2; exit 1
fi
rg -q 'K1050' "$work/log"
echo 'PASS C/tracking grows beyond 1300 locals; reads, writes, inner dimensions and shadow restoration stay checked'
