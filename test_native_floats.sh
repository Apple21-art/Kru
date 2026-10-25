#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_floats.kru "$work/floats"
timeout 10 "$work/floats" > "$work/out"
printf '1\n1\n1\n1\n1\n1\n0\n1\n0\n0\n0\n0\n1\n1\n1\n' > "$work/expected"
diff -u "$work/expected" "$work/out"
echo 'PASS native/f32 f64 arithmetic, mixed and stack float ABI, padded fields, NaNs and signed zero'
"$compiler" --emit-native tests/native_float_print.kru "$work/print"
"$work/print" > "$work/print.out"
printf '1.5\n-2.25\n0\n-0\n1000.25\n1e+20\n9.9999999999999995e-21\ninf\n-inf\nnan\n' > "$work/print.expected"
cmp "$work/print.out" "$work/print.expected"
python3 - "$work" <<'PY'
from pathlib import Path
import math, random, struct, sys
work=Path(sys.argv[1]); random.seed(83401)
values=[float.fromhex('0x1.fffffffffffffp+1023'),float.fromhex('0x0.0000000000001p-1022')]
while len(values)<258:
    value=struct.unpack('<d',random.getrandbits(64).to_bytes(8,'little'))[0]
    if math.isfinite(value):values.append(value)
(work/'roundtrip.kru').write_text('pub fn main() -> i32 {\n'+''.join('pr('+repr(v)+')\n' for v in values)+'ret 0\n}\n')
(work/'roundtrip.expected').write_text(''.join(format(v,'.17g')+'\n' for v in values))
PY
"$compiler" --emit-native "$work/roundtrip.kru" "$work/roundtrip"
"$work/roundtrip" > "$work/roundtrip.out"
diff -u "$work/roundtrip.expected" "$work/roundtrip.out"
echo 'PASS native/freestanding float formatting matches exact 17-digit rounding for extrema and random doubles'
"$compiler" --emit-native tests/native_numeric_casts.kru "$work/casts"
"$work/casts" > "$work/casts.out"
printf '12\n-128\n0\n1\n1\n1\n1\n9223372036854775808\n-9223372036854775808\n' > "$work/casts.expected"
cmp "$work/casts.out" "$work/casts.expected"
echo 'PASS native/checked integer-float conversions, fractional truncation and u64 conversion'
for pair in 'i32 2147483648.0' 'u8 -1.0' 'i64 9223372036854775808.0' 'u64 18446744073709551616.0' 'f32 1e100'; do
    read -r target value <<< "$pair"
    printf 'pub fn main() -> i32 {\n let n := %s\n pr(cast<%s>(n))\n ret 0\n}\n' "$value" "$target" > "$work/bad.kru"
    "$compiler" --emit-native "$work/bad.kru" "$work/bad"
    set +e
    "$work/bad" > "$work/bad.out" 2> "$work/bad.err"
    status=$?
    set -e
    test "$status" -ne 0
    test ! -s "$work/bad.out"
done
echo 'PASS native/out-of-range float conversions trap'
