#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_i128_float_casts.kru "$work/basic"
"$work/basic" > "$work/out"
printf '1\n1\n1\n18446744073709551616\n-18446744073709551616\n0\n-170141183460469231731687303715884105728\n' > "$work/expected"
diff -u "$work/expected" "$work/out"
python3 - "$compiler" "$work" <<'PY'
from pathlib import Path
import math, random, struct, subprocess, sys
compiler, directory=sys.argv[1:];work=Path(directory)
rng=random.Random(12853)
source=['pub fn main()->i32 {'];expected=[]
for i in range(70):
    value=rng.getrandbits(127)*(-1 if i%2 else 1)
    for width in (32,64):
        floating=float(value)
        if width==32:floating=struct.unpack('f',struct.pack('f',floating))[0]
        source.append(f'pr(cast<f{width}>({value}i128)=={floating!r}f{width})')
        expected.append('1')
    floating=math.ldexp(rng.random(),rng.randrange(1,127))*(-1 if i%2 else 1)
    source.append(f'pr(cast<i128>({floating!r}f64))');expected.append(str(int(floating)))
# Exact halfway cases test round-to-nearest, ties-to-even, including carry.
for value,want in [(2**53+1,2**53),(2**53+3,2**53+4),(2**54-1,2**54)]:
    source.append(f'pr(cast<f64>({value}u128)=={float(want)!r})');expected.append('1')
source+=['ret 0','}']
(work/'random.kru').write_text('\n'.join(source))
subprocess.run([compiler,'--emit-native',str(work/'random.kru'),str(work/'random')],check=True)
actual=subprocess.check_output([str(work/'random')],text=True).splitlines()
assert actual==expected,(actual,expected)
for index,(parameter,result,argument) in enumerate([
    ('f64','i128','170141183460469231731687303715884105728.0'),
    ('f64','u128','340282366920938463463374607431768211456.0'),
    ('f64','u128','-1.0'),('f64','i128','0.0/0.0'),
    ('u128','f32','340282366920938463463374607431768211455u128')]):
    text=f'fn convert(x:{parameter})->{result} {{ret cast<{result}>(x)}} pub fn main()->i32 {{pr(convert({argument}));ret 0}}'
    path=work/f'trap{index}.kru';exe=work/f'trap{index}';path.write_text(text)
    subprocess.run([compiler,'--emit-native',str(path),str(exe)],check=True)
    result=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert result.returncode==-4,(index,result.returncode)
print('PASS native/wide float casts, randomized conversions, ties and checked traps')
PY
