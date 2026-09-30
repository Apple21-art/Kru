#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for fixture in native_functions native_string_compare native_enum_match stage4 stage5; do
    timeout 10 "$compiler" --emit-native "$root/tests/$fixture.kru" "$work/$fixture.native"
    timeout 10 "$compiler" --emit-c "$root/tests/$fixture.kru" "$work/$fixture.c"
    cc -std=gnu11 -O2 "$work/$fixture.c" -o "$work/$fixture.reference"
    mkdir "$work/$fixture.native-cwd" "$work/$fixture.reference-cwd"
    (cd "$work/$fixture.native-cwd" && timeout 10 "$work/$fixture.native" 'extra argument' >"$work/$fixture.native.out")
    (cd "$work/$fixture.reference-cwd" && timeout 10 "$work/$fixture.reference" 'extra argument' >"$work/$fixture.reference.out")
    diff -u "$work/$fixture.reference.out" "$work/$fixture.native.out"
    if [[ $fixture == stage5 ]]; then
        diff -u "$work/$fixture.reference-cwd/stage5_fixture.txt" "$work/$fixture.native-cwd/stage5_fixture.txt"
        diff -u "$work/$fixture.reference-cwd/output_stage5.txt" "$work/$fixture.native-cwd/output_stage5.txt"
    fi
    readelf -l "$work/$fixture.native" >"$work/elf"
    ! grep -q INTERP "$work/elf"
    echo "PASS native/$fixture execution matches reference; no ELF interpreter"
done

if timeout 10 "$compiler" --emit-native "$root/tests/compile_fail/native_null_string.kru" "$work/null-string" >"$work/null.out" 2>"$work/null.err"; then
    echo 'FAIL native/null accepted as str' >&2
    exit 1
fi
echo 'PASS native/null-to-str rejected'
