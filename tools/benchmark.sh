#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
timer=${2:-build/kru-bench-timer}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
[[ $timer = /* ]] || timer="$root/$timer"
[[ -x $timer ]] || { echo 'Build the benchmark timer with make bench.' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

summarize() {
    local label=$1 file=$2
    sort -n "$file" > "$file.sorted"
    local n median p95_idx
    n=$(wc -l < "$file.sorted")
    median=$(awk -v n="$n" 'NR == int((n+1)/2) { a=$1 } NR == int((n+2)/2) { printf "%.6f", (a+$1)/2 }' "$file.sorted")
    p95_idx=$(( (95 * n + 99) / 100 ))
    printf '%-24s median=%8.3f ms  p95=%8.3f ms  n=%d\n' \
        "$label" \
        "$median" \
        "$(sed -n "${p95_idx}p" "$file.sorted")" "$n"
}

measure() {
    local times=$1 count=$2
    shift 2
    timeout 120 "$timer" "$count" "$@" > "$times"
}

echo 'Kru benchmark: repeated process wall-clock measurements (monotonic clock)'
echo 'Each sample includes process startup; compiler --time reports internal stages.'

# Warm compiler/page cache once before collecting each class.
"$compiler" --emit-c "$root/tests/stage4.kru" "$work/warm.c" >/dev/null 2>&1
measure "$work/frontend.times" 50 "$compiler" --emit-c "$root/tests/stage4.kru" "$work/frontend.c"
summarize 'frontend emit-C' "$work/frontend.times"

# End-to-end includes Kru frontend + generated-C host compile + target execution.
"$compiler" --run "$root/tests/future.kru" "$work/e2e.c" >/dev/null 2>&1
measure "$work/e2e.times" 20 "$compiler" --run "$root/tests/future.kru" "$work/e2e.c"
summarize 'end-to-end C bootstrap' "$work/e2e.times"

# Generated-program runtime: compile once, then time only the executable.
"$compiler" --emit-c "$root/tests/bench_runtime.kru" "$work/runtime.c" >/dev/null
cc -std=gnu11 -O2 "$work/runtime.c" -o "$work/runtime"
"$work/runtime"
measure "$work/runtime.times" 30 "$work/runtime"
summarize 'generated runtime fib30' "$work/runtime.times"

# Direct-native backend latency: no C compiler/linker in this path.
if [[ $(uname -s) == Linux && $(uname -m) == x86_64 ]]; then
    "$compiler" --emit-native "$root/tests/native_flow.kru" "$work/native" >/dev/null
    measure "$work/native.times" 50 "$compiler" --emit-native "$root/tests/native_flow.kru" "$work/native"
    summarize 'direct native emit' "$work/native.times"
    "$compiler" --emit-native "$root/tests/stage4.kru" "$work/stage4.native" >/dev/null
    measure "$work/stage4.times" 50 "$compiler" --emit-native "$root/tests/stage4.kru" "$work/stage4.native"
    summarize 'native emit Stage4' "$work/stage4.times"
    "$compiler" --emit-native "$root/tests/stage5.kru" "$work/stage5.native" >/dev/null
    measure "$work/stage5.times" 50 "$compiler" --emit-native "$root/tests/stage5.kru" "$work/stage5.native"
    summarize 'native emit Stage5' "$work/stage5.times"
    "$compiler" --emit-native "$root/tests/bench_runtime.kru" "$work/runtime.native"
    "$work/runtime.native"
    measure "$work/native-runtime.times" 30 "$work/runtime.native"
    summarize 'direct runtime fib30' "$work/native-runtime.times"
fi
