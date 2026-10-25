#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work/source.kru" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_text('pub fn main() -> i32 {\npr('+ ' + '.join(['1i32']*32)+')\npr('+ ' && '.join(['1i32==1i32']*28)+')\nret 0\n}\n')
PY
timeout 5 "$compiler" --emit-native "$work/source.kru" "$work/program"
"$work/program" > "$work/out"
printf '32\n1\n' > "$work/expected"
cmp "$work/out" "$work/expected"
echo 'PASS native/linear operand type traversal completes chained arithmetic and boolean expressions'
