#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/cleanup.kru" <<'SOURCE'
fn join()->str {ret str_concat("abc","def")}
pub fn main()->i32 {
 var i:=0
 while i<10000 {
  let a:=join()
  let b:=str_concat(a,"ghi")
  if !str_eq(b,"abcdefghi") {ret 2}
  str_free(a)
  str_free(b)
  let empty:=str_concat("","")
  str_free(empty)
  i=i+1
 }
 let missing:=file_read(null)
 if str_len(missing)!=0 {ret 3}
 str_free(missing)
 let out:=file_open_write("empty.txt")
 file_close(out)
 let input:=file_open("empty.txt")
 let empty:=file_read(input)
 file_close(input)
 str_free(empty)
 ret 0
}
SOURCE
"$compiler" --check-heap --emit-native "$work/cleanup.kru" "$work/native"
(cd "$work" && ./native)
"$compiler" --emit-c "$work/cleanup.kru" "$work/generated.c"
"${CC:-cc}" -w "$work/generated.c" "$root/tools/allocation_audit.c" -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup,--wrap=strndup -o "$work/c"
(cd "$work" && ./c)
echo 'PASS bootstrap/explicit string release: 10000 loops, returned allocations, empty concat/read and failed read'
for fixture in stage4 stage5; do
 "$compiler" --check-heap --emit-native "$root/tests/$fixture.kru" "$work/$fixture-native"
 (cd "$work" && "./$fixture-native") > "$work/$fixture-native.out"
 "$compiler" --emit-c "$root/tests/$fixture.kru" "$work/$fixture.c"
 "${CC:-cc}" -w "$work/$fixture.c" "$root/tools/allocation_audit.c" -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup,--wrap=strndup -o "$work/$fixture-c"
 (cd "$work" && "./$fixture-c") > "$work/$fixture-c.out"
 cmp "$work/$fixture-native.out" "$work/$fixture-c.out"
done
echo 'PASS bootstrap/Stage 4 and Stage 5 release allocations in native and generated C'
