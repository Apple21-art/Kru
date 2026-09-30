#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/no-tools" "$work/regen/tools" "$work/regen/include"

# Regenerate outside the checkout: the regression must never rewrite its input.
cp "$root/tools/generate_native_runtime.py" "$root/tools/native_runtime.S" "$work/regen/tools/"
python3 "$work/regen/tools/generate_native_runtime.py" >"$work/regeneration.out"
cmp "$root/include/native_runtime_blob.h" "$work/regen/include/native_runtime_blob.h"
echo 'PASS native-runtime/blob reproducible from assembly'

compare_program() {
    local source=$1 name=$2
    "$compiler" --emit-c "$source" "$work/$name.c"
    "${CC:-cc}" -w "$work/$name.c" -o "$work/$name-reference"
    # No assembler, linker, compiler, or other child program is available here.
    PATH="$work/no-tools" "$compiler" --emit-native "$source" "$work/$name-native"
    if readelf -l "$work/$name-native" | grep -q INTERP; then
        echo "FAIL native-runtime/$name has dynamic interpreter" >&2
        exit 1
    fi
    if readelf -d "$work/$name-native" | grep -q NEEDED; then
        echo "FAIL native-runtime/$name has external library dependencies" >&2
        exit 1
    fi
    mkdir "$work/$name-reference-dir" "$work/$name-native-dir"
    (cd "$work/$name-reference-dir" && timeout 15 "$work/$name-reference" first 'second value') >"$work/$name-reference.out"
    (cd "$work/$name-native-dir" && timeout 15 "$work/$name-native" first 'second value') >"$work/$name-native.out"
    cmp "$work/$name-reference.out" "$work/$name-native.out"
    diff -r "$work/$name-reference-dir" "$work/$name-native-dir"
    echo "PASS native-runtime/$name matches C output and file bytes without host tools"
}

compare_program "$root/tests/native_runtime_edges.kru" edges
cat >"$work/edges.expected" <<'EXPECTED'
123456789
-654321
-2147483648
2147483647
0
3
first
second value
runtime-edges-ok
EXPECTED
cmp "$work/edges.expected" "$work/edges-native.out"
echo 'PASS native-runtime/explicit signed and argv output'

# 131072 bytes exercises five capacity-growth boundaries and detects truncation.
python3 - "$work/large.kru" <<'PY'
from pathlib import Path
import sys
payload = '0123456789abcdef' * 64
Path(sys.argv[1]).write_text('''pub fn main() -> i32 {
    let output := file_open_write("large.txt")
    if output == null { ret 1 }
    var i: i32 := 0
    while i < 128 {
        file_write(output, "''' + payload + '''")
        i = i + 1
    }
    file_close(output)
    let input := file_open("large.txt")
    if input == null { ret 2 }
    let content := file_read(input)
    if str_len(content) != 131072 { ret 3 }
    if !str_eq(file_read(input), "") { ret 4 }
    file_close(input)
    pr(str_len(content))
    pr(content)
    ret 0
}
''')
PY
compare_program "$work/large.kru" large
python3 - "$work/large-native.out" "$work/large-native-dir/large.txt" <<'PY'
from pathlib import Path
import sys
payload = b'0123456789abcdef' * 8192
assert Path(sys.argv[1]).read_bytes() == b'131072\n' + payload + b'\n'
assert Path(sys.argv[2]).read_bytes() == payload
PY
echo 'PASS native-runtime/131072-byte read and write preserve all bytes'
