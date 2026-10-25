#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/structs.kru" <<'SOURCE'
struct Box<T> {value:T}
struct Pair<A,B> {first:A,second:B}
fn unwrap(value:Box<i32>)->i32 {ret value.value}
pub fn main()->i32 {
 let explicit:=Box<i32>{value:42}
 let contextual:Box<u64>:=Box{value:18446744073709551615u64}
 let nested:=Box<Box<i32>>{value:Box<i32>{value:7}}
 let mixed:=Pair<i16,str>{first:-12i16,second:"generic"}
 pr(unwrap(explicit))
 pr(contextual.value)
 pr(nested.value.value)
 pr(mixed.first)
 pr(mixed.second)
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/structs.kru" "$work/structs"
"$work/structs" > "$work/actual"
printf '%s\n' 42 18446744073709551615 7 -12 generic > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/generic structs: explicit, contextual, nested and mixed layouts'
cat > "$work/methods.kru" <<'SOURCE'
struct Box<T> {value:T}
impl Box<i16> {
 fn get(ref self)->i16 {ret self.value}
 fn set(mut ref self,value:i16) {self.value=value}
 fn replaced(self,value:i16)->Self {ret Self{value:value}}
}
impl Box<u64> {fn get(ref self)->u64 {ret self.value}}
pub fn main()->i32 {
 var small:=Box<i16>{value:-7i16}
 pr(small.get())
 small.set(12i16)
 pr(small.get())
 let large:=Box<u64>{value:18446744073709551615u64}
 pr(large.get())
 let replacement:=small.replaced(23i16)
 pr(replacement.get())
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/methods.kru" "$work/methods"
"$work/methods" > "$work/actual"
printf '%s\n' -7 12 18446744073709551615 23 > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/concrete impl receivers, mutation and Self returns'
cat > "$work/trait.kru" <<'SOURCE'
trait Readable {fn read(ref self)->i32}
struct Box<T> {value:T}
impl Readable for Box<u64> {fn read(ref self)->i32 {ret 42}}
fn read_value<T:Readable>(value:T)->i32 {ret value.read()}
pub fn main()->i32 {pr(read_value(Box<u64>{value:99u64}));ret 0}
SOURCE
"$compiler" --emit-native "$work/trait.kru" "$work/trait"
"$work/trait" > "$work/actual"
printf '42\n' > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/concrete impl satisfies a concrete trait bound'
for body in 'let x:=Box<i32,u64>{value:1}' 'let x:Box<i32>:=Box<u64>{value:1u64}' 'let x:=Box<i8>{value:300}' 'let x:=Box<i32>{wrong:1}';do
 printf 'struct Box<T>{value:T}\npub fn main()->i32 {%s;ret 0}\n' "$body" > "$work/reject.kru"
 if "$compiler" --emit-native "$work/reject.kru" "$work/reject" > "$work/log" 2>&1;then echo "FAIL generic struct accepted $body";exit 1;fi
done
echo 'PASS native/generic struct arity, identity, range and field rejection'
cat > "$work/drop.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
struct Bag<T> {value:T}
impl Bag<Owner> {fn drop(mut ref self) {pr(90)}}
pub fn main()->i32 {let bag:=Bag<Owner>{value:Owner{value:91}};ret 0}
SOURCE
"$compiler" --emit-native "$work/drop.kru" "$work/drop"
"$work/drop" > "$work/actual"
printf '90\n91\n' > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/generic container and owned field drop exactly once'
cat > "$work/concrete-impl.kru" <<'SOURCE'
struct Box<T> {value:T}
impl Box<i32> {fn value(ref self)->i32 {ret self.value}}
pub fn main()->i32 {let x:=Box<i32>{value:17};pr(x.value());ret 0}
SOURCE
"$compiler" --emit-native "$work/concrete-impl.kru" "$work/concrete-impl"
"$work/concrete-impl" > "$work/actual"
printf '17\n' > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/impl for a concrete generic specialization'

cat > "$work/blanket.kru" <<'SOURCE'
struct Box<T> {value:T}
impl<T> Box<T> {fn get(ref self)->T {ret self.value}}
pub fn main()->i32 {ret 0}
SOURCE
if "$compiler" --emit-native "$work/blanket.kru" "$work/blanket" > "$work/log" 2>&1;then
 echo 'FAIL blanket impl accepted';exit 1
fi
rg -q 'Codex Chapter 36' "$work/log"
echo 'PASS native/blanket impl rejected per Codex Chapter 36'
cat > "$work/model.kru" <<'SOURCE'
pub struct Box<T> {value:T}
SOURCE
cat > "$work/qualified.kru" <<'SOURCE'
use model
pub fn main()->i32 {let box:=model::Box<i32>{value:42};pr(box.value);ret 0}
SOURCE
"$compiler" --emit-native "$work/qualified.kru" "$work/qualified"
"$work/qualified" > "$work/actual"
printf '42\n' > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/module-qualified generic struct construction'
