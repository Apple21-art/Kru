#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
reject() {
 local name=$1 body=$2
 printf 'pub fn main()->i32 {%s;ret 0}\n' "$body" > "$work/$name.kru"
 if "$compiler" --emit-native "$work/$name.kru" "$work/$name" > "$work/log" 2>&1;then echo "FAIL reference-flow/$name accepted";exit 1;fi
 rg -q 'reference (alias conflict|outlives its storage)' "$work/log"
 echo "PASS reference-flow/$name rejected"
}
reject shared_write 'var x:=1;let r:=ref x;x=2;pr(r@)'
reject mutable_read 'var x:=1;let r:=mut ref x;pr(x);r@=2'
reject mixed_borrows 'var x:=1;let r:=ref x;let m:=mut ref x;m@=2;pr(r@)'
reject two_mutable 'var x:=1;let a:=mut ref x;let b:=mut ref x;a@=2;b@=3'
reject alias_copy 'var x:=1;let a:=ref x;let b:=a;x=2;pr(b@)'
reject inner_escape 'var x:=1;var r:=ref x;{let inner:=2;r=ref inner};pr(r@)'
reject branch_alias 'var x:=1;var y:=2;var r:=ref y;if x==1 {r=ref x};x=3;pr(r@)'
reject loop_alias 'var x:=1;var y:=2;var r:=ref y;var i:=0;while i<2 {x=3;pr(r@);r=ref x;i+=1}'
cat > "$work/valid.kru" <<'SOURCE'
fn bump(value:mut ref i32) {value@+=1}
pub fn main()->i32 {
 var x:=1
 let a:=ref x
 let b:=a
 pr(a@+b@)
 x=2
 let m:=mut ref x
 m@=3
 pr(m@)
 bump(mut ref x)
 bump(mut ref x)
 pr(x)
 var y:=7
 var r:=ref x
 r=ref y
 x=9
 pr(r@)
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/valid.kru" "$work/valid"
"$work/valid" > "$work/output"
printf '2\n3\n5\n7\n' > "$work/expected"
cmp "$work/expected" "$work/output"
echo 'PASS reference-flow/shared aliases, last use, sequential calls and reassignment'

cat > "$work/call-conflict.kru" <<'SOURCE'
fn pair(a:mut ref i32,b:mut ref i32) {a@=2;b@=3}
pub fn main()->i32 {var x:=1;pair(mut ref x,mut ref x);ret 0}
SOURCE
if "$compiler" --emit-native "$work/call-conflict.kru" "$work/call-conflict" > "$work/log" 2>&1;then
 echo 'FAIL reference-flow/overlapping call arguments accepted';exit 1
fi
rg -q 'reference alias conflict' "$work/log"
echo 'PASS reference-flow/overlapping call arguments rejected'

cat > "$work/early-return.kru" <<'SOURCE'
fn choose(flag:bool)->i32 {
 var x:=1
 let r:=ref x
 if flag {ret r@}
 x=2
 ret x
}
pub fn main()->i32 {pr(choose(true));pr(choose(false));ret 0}
SOURCE
"$compiler" --emit-native "$work/early-return.kru" "$work/early-return"
"$work/early-return" > "$work/output"
printf '1\n2\n' > "$work/expected"
cmp "$work/expected" "$work/output"
echo 'PASS reference-flow/returned branch does not keep a borrow live'
reject shadowed_alias 'var x:=1;let r:=ref x;{let r:=r;x=2;pr(r@)}'

cat > "$work/field-write.kru" <<'SOURCE'
struct Value {number:i32}
fn read(value:ref Value)->i32 {ret (value@).number}
pub fn main()->i32 {
 var value:=Value{number:1}
 let r:=ref value
 value.number=2
 pr(read(r))
 ret 0
}
SOURCE
if "$compiler" --emit-native "$work/field-write.kru" "$work/field-write" > "$work/log" 2>&1;then
 echo 'FAIL reference-flow/field write through borrowed owner accepted';exit 1
fi
rg -q 'reference alias conflict' "$work/log"
echo 'PASS reference-flow/field write through borrowed owner rejected'
reject block_escape 'let r:={let inner:=2;ref inner};pr(r@)'

cat > "$work/returned-reference.kru" <<'SOURCE'
fn identity(r:ref i32)->ref i32 {ret r}
pub fn main()->i32 {var x:=1;let r:=identity(ref x);x=2;pr(r@);ret 0}
SOURCE
if "$compiler" --emit-native "$work/returned-reference.kru" "$work/returned-reference" > "$work/log" 2>&1;then
 echo 'FAIL reference-flow/returned reference accepted';exit 1
fi
rg -q 'reference alias conflict' "$work/log"
echo 'PASS reference-flow/returned reference preserves its loan'
reject slice_write 'var a:=[1,2];let s:=a[..];a[0]=3;pr(s[0])'
cat > "$work/string-view.kru" <<'SOURCE'
use kru_string
pub fn main()->i32 {
 var text:=kru_string::String::from("hello")
 let view:=text.as_str()
 text.push_str(" world")
 pr(view)
 ret 0
}
SOURCE
if "$compiler" --emit-native "$work/string-view.kru" "$work/string-view" > "$work/log" 2>&1;then
 echo 'FAIL reference-flow/invalidated String view accepted';exit 1
fi
rg -q 'reference alias conflict' "$work/log"
echo 'PASS reference-flow/String mutation with a live view rejected'
reject reassigned_slice 'var a:=[1,2];var b:=[3,4];var s:=a[..];s=b[..];b[0]=5;pr(s[0])'
reject borrowed_descriptor_read 'var text:="hello";let r:=mut ref text;pr(text);r@="world"'
reject mutable_copy_overlap 'var x:=1;let a:=mut ref x;let b:=a;a@=2;b@=3'
cat > "$work/repeated-argument.kru" <<'SOURCE'
fn pair(a:mut ref i32,b:mut ref i32) {a@=2;b@=3}
pub fn main()->i32 {var x:=1;let r:=mut ref x;pair(r,r);ret 0}
SOURCE
if "$compiler" --emit-native "$work/repeated-argument.kru" "$work/repeated-argument" > "$work/log" 2>&1;then
 echo 'FAIL reference-flow/repeated mutable argument accepted';exit 1
fi
rg -q 'reference alias conflict' "$work/log"
echo 'PASS reference-flow/repeated mutable argument rejected'
