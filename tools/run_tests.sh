#!/usr/bin/env bash
set -uo pipefail

compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pass_count=0
fail_count=0
xfail_count=0
xpass_count=0

action_pass() { printf 'PASS   %s\n' "$1"; pass_count=$((pass_count+1)); }
action_fail() { printf 'FAIL   %s%s\n' "$1" "${2:+ — $2}"; fail_count=$((fail_count+1)); }
action_xfail() { printf 'XFAIL  %s\n' "$1"; xfail_count=$((xfail_count+1)); }
action_xpass() { printf 'XPASS  %s — remove from expected-unimplemented list\n' "$1"; xpass_count=$((xpass_count+1)); }

emit_c() {
    local name=$1
    timeout 10 "$compiler" --emit-c "$root/tests/$name.kru" "$work/$name.c" \
        >"$work/$name.compiler.out" 2>"$work/$name.compiler.err"
}

compile_c() {
    local name=$1
    cc -std=gnu11 -O2 -Wall -Wextra -Werror "$work/$name.c" -o "$work/$name" \
        >"$work/$name.cc.out" 2>"$work/$name.cc.err"
}

run_binary() {
    local name=$1 expected_exit=$2 stdin_file=${3:-/dev/null}
    (cd "$work" && timeout 10 "$work/$name" <"$stdin_file" >"$work/$name.run.out" 2>"$work/$name.run.err")
    local status=$?
    [[ $status -eq $expected_exit ]]
}

# Compiler-only conformance: these must emit C without executing the program.
compile_only=(hello future stage1 stage2 stage3 stage4 stage5 stage5_math full pt)
for name in "${compile_only[@]}"; do
    if emit_c "$name"; then action_pass "emit/$name"; else action_fail "emit/$name" "$(tail -1 "$work/$name.compiler.err")"; fi
done

# Generated C must compile cleanly for runnable supported fixtures.
runtime_cases=("hello:42" "future:0" "stage1:0" "stage2:0" "stage3:0" "stage4:0" "stage5:0" "full:0")
for case in "${runtime_cases[@]}"; do
    name=${case%%:*}; expected=${case##*:}
    if [[ ! -s $work/$name.c ]] && ! emit_c "$name"; then
        action_fail "runtime/$name" "Kru emit failed"
        continue
    fi
    if ! compile_c "$name"; then
        action_fail "runtime/$name" "generated C failed to compile"
        continue
    fi
    if run_binary "$name" "$expected"; then
        action_pass "runtime/$name (exit $expected)"
    else
        action_fail "runtime/$name" "unexpected exit/timeout"
    fi
done

# Codex-valid target-shape fixtures that this bootstrap does not implement yet.
xfails=(stage5_arrays stage5_core stage5_errors stage5_mod stage5_mod_main stage5_strings)
for name in "${xfails[@]}"; do
    if emit_c "$name"; then action_xpass "$name"; else action_xfail "$name"; fi
done

printf '\nSummary: PASS=%d FAIL=%d XFAIL=%d XPASS=%d\n' "$pass_count" "$fail_count" "$xfail_count" "$xpass_count"
[[ $fail_count -eq 0 && $xpass_count -eq 0 ]]
