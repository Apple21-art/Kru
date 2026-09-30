#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$compiler" --emit-native "$root/tests/hello.kru" "$work/hello"
file "$work/hello" | grep -Eq 'ELF 64-bit.*x86-64'
set +e
"$work/hello"
st=$?
set -e
[[ $st -eq 42 ]]
echo 'PASS native/hello exit=42'

"$compiler" --emit-native "$root/tests/native_arithmetic.kru" "$work/arithmetic"
set +e
"$work/arithmetic"
st=$?
set -e
[[ $st -eq 43 ]]
echo 'PASS native/arithmetic exit=43'

for spec in flow:27 short_circuit:17; do
  name=${spec%%:*}
  expected=${spec#*:}
  "$compiler" --emit-native "$root/tests/native_$name.kru" "$work/$name"
  set +e
  "$work/$name"
  st=$?
  set -e
  [[ $st -eq $expected ]] || { echo "FAIL native/$name expected $expected got $st"; exit 1; }
  echo "PASS native/$name exit=$st"
done

if "$compiler" --emit-native "$root/tests/native_bad_type.kru" "$work/bad_type" >"$work/b.out" 2>"$work/b.err"; then
  echo 'FAIL native accepted string local' >&2
  exit 1
fi
[[ ! -e "$work/bad_type" ]]
echo 'PASS native/integer binding from string rejected'

if "$compiler" --emit-native "$root/tests/native_fallthrough.kru" "$work/fallthrough" >"$work/f.out" 2>"$work/f.err"; then
  echo 'FAIL native accepted reachable main fallthrough' >&2
  exit 1
fi
[[ ! -e "$work/fallthrough" ]]
echo 'PASS native/reachable fallthrough rejected'

if "$compiler" --emit-native "$root/tests/native_suffix_mismatch.kru" "$work/suffix" >"$work/s.out" 2>"$work/s.err"; then
  echo 'FAIL native accepted explicit u64 literal as i32' >&2
  exit 1
fi
[[ ! -e "$work/suffix" ]]
echo 'PASS native/explicit wider literal rejected'

set +e
"$compiler" --emit-native "$root/tests/native_unsupported.kru" "$work/unsupported" >"$work/u.out" 2>"$work/u.err"
st=$?
set -e
[[ $st -ne 0 ]]
grep -Fq 'native backend' "$work/u.err"
echo 'PASS native/unsupported rejects instead of miscompiling'

set +e
"$compiler" --emit-native "$root/tests/native_out_of_range.kru" "$work/out_of_range" >"$work/r.out" 2>"$work/r.err"
st=$?
set -e
[[ $st -ne 0 ]]
grep -Fq 'native backend' "$work/r.err"
echo 'PASS native/out-of-range return rejects instead of truncating'
