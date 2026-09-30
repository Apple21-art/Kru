#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$compiler" --emit-c "$root/tests/pt.kru" "$work/pt.c"
cc -std=gnu11 -O2 -Wall -Wextra -Werror "$work/pt.c" -o "$work/pt"

run_case() {
    local name=$1 input=$2 must_have=$3
    printf '%b' "$input" > "$work/$name.in"
    timeout 3 "$work/pt" < "$work/$name.in" > "$work/$name.out"
    grep -Fq "$must_have" "$work/$name.out"
    printf 'PASS adventure/%s\n' "$name"
}

run_case quit '3\n' 'You give up and sit in the dark...'
run_case locked '2\n3\n' 'The door is locked tight. You need to find a key!'
run_case invalid '9\n3\n' 'Invalid choice. Select 1, 2, or 3.'
run_case victory '1\n1\n1\n1\n2\n2\n' 'You escaped the dungeon successfully!'
run_case revisit '1\n1\n1\n1\n1\n1\n2\n2\n' 'The room is empty.'

# EOF must be a clean exit, not an infinite re-prompt loop.
: > "$work/eof.in"
timeout 3 "$work/pt" < "$work/eof.in" > "$work/eof.out"
grep -Fq 'Input closed. Exiting game.' "$work/eof.out"
printf 'PASS adventure/eof\n'
