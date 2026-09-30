#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-build/kru0}
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $compiler = /* ]] || compiler="$root/$compiler"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0
check() {
    local name=$1 expected=$2 source=$3
    printf '%s\n' "$source" >"$work/$name.kru"
    if ! timeout 10 "$compiler" --emit-c "$work/$name.kru" "$work/$name.c" >"$work/$name.log" 2>&1 ||
       ! "${CC:-cc}" -std=gnu11 -O2 "$work/$name.c" -o "$work/$name" >>"$work/$name.log" 2>&1; then
        echo "FAIL shadow/$name: compilation failed" >&2
        cat "$work/$name.log" >&2
        failed=1
        return
    fi
    local status=0
    timeout 10 "$work/$name" >"$work/$name.out" 2>"$work/$name.err" || status=$?
    if [[ $status -ne 0 || $(cat "$work/$name.out") != "$expected" ]]; then
        echo "FAIL shadow/$name: wrong runtime result (status=$status)" >&2
        cat "$work/$name.out" "$work/$name.err" >&2
        failed=1
    else
        echo "PASS shadow/$name"
    fi
}
check same_scope 4 $'pub fn main() -> i32 {\nlet x := 3\nlet x := x + 1\npr(x)\nret 0\n}'
check nested $'4\n5\n4\n3' $'pub fn main() -> i32 {\nlet x := 3\n{ let x := x + 1\npr(x)\n{ let x := x + 1\npr(x) }\npr(x) }\npr(x)\nret 0\n}'
check parameter 4 $'fn bump(x: i32) -> i32 {\nlet x := x + 1\nret x\n}\npub fn main() -> i32 {\npr(bump(3))\nret 0\n}'
check mutable 6 $'pub fn main() -> i32 {\nvar x := 3\nvar x := x + 1\nx += 2\npr(x)\nret 0\n}'
check captured_reference $'3\n4' $'pub fn main() -> i32 {\nvar x := 3\nlet old := ref x\nlet x := x + 1\npr(old@)\npr(x)\nret 0\n}'
check arrays $'3\n9\n3' $'pub fn main() -> i32 {\nlet xs := [3, 4]\n{ let xs := [xs[0], 9]\npr(xs[0])\npr(xs[1]) }\npr(xs[0])\nret 0\n}'
check initializer_block 5 $'pub fn main() -> i32 {\nlet x := 3\nlet x := { let offset := 2\nx + offset }\npr(x)\nret 0\n}'
check match_once $'called\n2' $'fn value() -> i32 { pr("called")\nret 2 }\npub fn main() -> i32 {\nmatch value() { 1 => { pr(1) }, 2 => { pr(2) }, _ => { pr(3) } }\nret 0\n}'
check match_binding $'5\n4' $'pub fn main() -> i32 {\nlet x := 3\nlet x := x + 1\nmatch 5 { x => { pr(x) } }\npr(x)\nret 0\n}'
check loop_restore $'0\n1\n4' $'pub fn main() -> i32 {\nlet x := 3\nlet x := x + 1\nfor x in 0..2 { pr(x) }\npr(x)\nret 0\n}'
check generated_name_collision $'6\n7' $'pub fn main() -> i32 {\nlet __kru_shadow_0 := 2\nlet __kru_match_0 := 7\nlet x := 3\nlet x := x + 1\npr(x + __kru_shadow_0)\nmatch 1 { 1 => { pr(__kru_match_0) }, _ => { pr(0) } }\nret 0\n}'
exit "$failed"
