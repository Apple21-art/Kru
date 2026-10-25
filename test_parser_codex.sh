#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for syntax in colon legacy; do
    timeout 10 "$compiler" --emit-c "$root/tests/parser_struct_$syntax.kru" "$work/$syntax.c" >"$work/$syntax.compiler.out" 2>"$work/$syntax.compiler.err" || {
        cat "$work/$syntax.compiler.err" >&2
        echo "FAIL parser struct literal $syntax" >&2
        exit 1
    }
    "${CC:-cc}" "$work/$syntax.c" -o "$work/$syntax"
    timeout 5 "$work/$syntax" > "$work/$syntax.out"
    printf '42\n' > "$work/expected"
    cmp "$work/expected" "$work/$syntax.out"
    echo "PASS parser struct literal $syntax"
done
# Typed native conditions distinguish a correctly grouped bool comparison
# from the old bitwise operation on a comparison result.
timeout 10 "$compiler" --emit-native "$root/tests/parser_bitwise_precedence.kru" "$work/precedence" >"$work/precedence.compiler.out" 2>"$work/precedence.compiler.err" || {
    cat "$work/precedence.compiler.err" >&2
    echo 'FAIL parser bitwise precedence' >&2
    exit 1
}
timeout 5 "$work/precedence"
echo 'PASS parser bitwise precedence'
cat > "$work/docs.kru" <<'SOURCE'
/// A documented value.
struct Value {
 /// Stored number.
 number:i32
}
/// Documented entry point.
pub fn main()->i32 {
 /// Comments remain trivia inside a block.
 let value:=Value{number:42}
 pr(value.number)
 ret 0
}
/// A trailing doc comment is valid trivia.
SOURCE
for mode in --emit-c --emit-native;do
 "$compiler" "$mode" "$work/docs.kru" "$work/docs-output"
done
"$work/docs-output" > "$work/docs.out"
printf '42\n' > "$work/docs.expected"
cmp "$work/docs.expected" "$work/docs.out"
echo 'PASS parser/doc comments at declarations, fields, statements and EOF'
