#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_i128.kru "$work/wide"
timeout 10 "$work/wide" > "$work/out"
cat > "$work/expected" <<'OUT'
340282366920938463463374607431768211455
-170141183460469231731687303715884105728
18446744073709551658
113427455640312821154458202477256070485
0
36893488147419103232
1
-1
1267650600228229401496703205376
1
1
18446744073709551617
42
18446744073709551631
1
-1
-9223372036854775808
18446744073709551615
OUT
diff -u "$work/expected" "$work/out"
python3 - "$work" <<'PY'
from pathlib import Path
import random,sys
work=Path(sys.argv[1]);random.seed(9881)
source=['pub fn main() -> i32 {'];expected=[];mask=(1<<128)-1
for i in range(60):
    a=random.getrandbits(128);b=random.getrandbits(128) or 1
    for op,value in [('+',(a+b)&mask),('-',(a-b)&mask),('*',(a*b)&mask),('/',a//b),('%',a%b),('&',a&b),('|',a|b),('^',a^b)]:
        source.append(f'pr({a}u128 {op} {b}u128)');expected.append(str(value))
source+=['ret 0','}']
(work/'random.kru').write_text('\n'.join(source)+'\n');(work/'random.expected').write_text('\n'.join(expected)+'\n')
PY
"$compiler" --emit-native "$work/random.kru" "$work/random"
timeout 10 "$work/random" > "$work/random.out"
diff -u "$work/random.expected" "$work/random.out"
echo 'PASS native/full-range i128 u128 literals, operations, printing, storage and random arithmetic'
for source in \
    'let x := 340282366920938463463374607431768211456u128' \
    'let x := 170141183460469231731687303715884105728i128' \
    'let x: u64 := 18446744073709551616' \
    'let x: u128 := -1'; do
    printf 'pub fn main() -> i32 {\n %s\n ret 0\n}\n' "$source" > "$work/bad.kru"
    if "$compiler" --emit-native "$work/bad.kru" "$work/bad" > "$work/log" 2>&1; then cat "$work/log";exit 1;fi
done
echo 'PASS native/full-width out-of-range literals reject'
