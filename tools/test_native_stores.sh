#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/stores.kru" <<'SOURCE'
pub fn main()->i32 {var x:i64:=1i64;x=x*3i64+4i64;pr(x);ret 0}
SOURCE
"$compiler" --emit-native "$work/stores.kru" "$work/stores"
"$work/stores" > "$work/out"
printf '7\n' > "$work/expected"
cmp "$work/out" "$work/expected"
python3 - "$work/stores" <<'PY'
import pathlib,sys
code=pathlib.Path(sys.argv[1]).read_bytes()
assert bytes.fromhex('488d85f8ffffff50') not in code,'plain local assignment still stages its address'
print('PASS native/plain integer assignment omits address staging')
PY
