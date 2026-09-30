#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# 1) Large flat scope: exercises lexer/parser/symbol lookup/codegen growth.
{
  echo 'pub fn main() -> i32 {'
  for i in $(seq 1 1200); do echo "    let v$i := $i"; done
  echo '    ret 0'
  echo '}'
} > "$work/many_bindings.kru"
timeout 20 "$compiler" --emit-c "$work/many_bindings.kru" "$work/many_bindings.c"
echo 'PASS stress/many_bindings(1200)'

# 2) Long left-associative expression. This should not trip the nesting guard.
{
  printf 'pub fn main() -> i32 {\n    ret 1'
  for _ in $(seq 1 1500); do printf ' + 1'; done
  printf '\n}\n'
} > "$work/long_expr.kru"
timeout 20 "$compiler" --emit-c "$work/long_expr.kru" "$work/long_expr.c"
echo 'PASS stress/long_expression(1501 terms)'

# 3) Deliberate nesting overflow: must reject cleanly with K1030, not crash/hang.
{
  printf 'pub fn main() -> i32 {\n    ret '
  for _ in $(seq 1 220); do printf '('; done
  printf '1'
  for _ in $(seq 1 220); do printf ')'; done
  printf '\n}\n'
} > "$work/deep_expr.kru"
set +e
timeout 20 "$compiler" --emit-c "$work/deep_expr.kru" "$work/deep_expr.c" >"$work/deep.out" 2>"$work/deep.err"
st=$?
set -e
[[ $st -ne 0 && $st -ne 124 ]]
grep -Fq 'K1030' "$work/deep.err"
echo 'PASS stress/deep_expression rejects with K1030'

# 4) Large match table.
{
  echo 'pub fn main() -> i32 {'
  echo '    let x := 199'
  echo '    match x {'
  for i in $(seq 0 199); do echo "        $i => { pr($i) }"; done
  echo '        n => { pr(n) }'
  echo '    }'
  echo '    ret 0'
  echo '}'
} > "$work/large_match.kru"
timeout 20 "$compiler" --emit-c "$work/large_match.kru" "$work/large_match.c"
echo 'PASS stress/large_match(200 arms)'

# 5) Repeated frontend invocation catches stale global state / cleanup issues.
for _ in $(seq 1 40); do
a="${RANDOM:-0}"
  timeout 10 "$compiler" --emit-c "$root/tests/stage4.kru" "$work/repeat.c" >/dev/null
  rm -f "$work/repeat.c"
done
echo 'PASS stress/repeated_compile(40)'
