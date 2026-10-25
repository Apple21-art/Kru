#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" --emit-native tests/native_move_flow.kru "$work/flow"
"$work/flow" > "$work/flow.out"
printf '7\n7\n7\n7\n10\n10\n20\n20\n21\n' > "$work/flow.expected"
cmp "$work/flow.expected" "$work/flow.out"
prefix='struct Owner {value:i32} impl Owner {fn drop(mut ref self) {pr(self.value)}} fn consume(value:Owner) {}'
reject() {
 local name=$1 body=$2
 printf '%s\npub fn main()->i32 {%s}\n' "$prefix" "$body" > "$work/$name.kru"
 if "$compiler" --emit-native "$work/$name.kru" "$work/$name" > "$work/log" 2>&1;then echo "FAIL accepted $name";exit 1;fi
 rg -q 'moved owning value.*reachable control-flow' "$work/log"
 echo "PASS move-flow/$name rejected"
}
reject loop_back_edge 'let owner:=Owner{value:1};var i:=0;while i<2 {consume(owner);i+=1};ret 0'
reject continue_edge 'var owner:=Owner{value:1};var i:=0;while i<2 {consume(owner);if i==0 {i+=1;continue};owner=Owner{value:2};i+=1};ret 0'
reject zero_iteration_join 'var owner:=Owner{value:1};consume(owner);while false {owner=Owner{value:2}};consume(owner);ret 0'
reject branch_join 'let owner:=Owner{value:1};if true {consume(owner)};consume(owner);ret 0'
reject for_back_edge 'let owner:=Owner{value:1};for i in 0..2 {consume(owner)};ret 0'
printf '%s\npub fn main()->i32 {let owner:=Owner{value:1};loop {consume(owner);break};ret 0}\n' "$prefix" > "$work/break.kru"
"$compiler" --emit-native "$work/break.kru" "$work/break"
"$work/break" > "$work/break.out"
printf '1\n' > "$work/break.expected"
cmp "$work/break.expected" "$work/break.out"
echo 'PASS move-flow/early returns, reinitialization and breaking transfers'

cat > "$work/short-circuit.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
fn consume(value:Owner)->bool {ret true}
pub fn main()->i32 {
 let a:=Owner {value:31}
 let ignored:=false && consume(a)
 if false {consume(a)}
 consume(a)
 let b:=Owner {value:32}
 let skipped:=true || consume(b)
 consume(b)
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/short-circuit.kru" "$work/short-circuit"
"$work/short-circuit" > "$work/short-circuit.out"
printf '31\n32\n' > "$work/short-circuit.expected"
cmp "$work/short-circuit.expected" "$work/short-circuit.out"
echo 'PASS move-flow/proven short-circuit paths do not consume owners'

# Returning a place must obey the same ownership rules as initialization.
# Until per-place cleanup is implemented, reject these instead of emitting
# two owners whose destructors release the same resource.
for place in 'box.item' 'items[0]' 'borrowed@'; do
 cat > "$work/return-place.kru" <<SOURCE
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
struct Box {item:Owner}
fn extract()->Owner {
 let box:=Box{item:Owner{value:41}}
 let items:[Owner;1]:=[Owner{value:42}]
 let value:=Owner{value:43}
 let borrowed:=ref value
 ret $place
}
pub fn main()->i32 {let result:=extract();ret 0}
SOURCE
 if "$compiler" --emit-native "$work/return-place.kru" "$work/return-place" > "$work/log" 2>&1;then
  echo "FAIL accepted owning return $place";exit 1
 fi
 rg -q 'moving owning (fields|values through references)' "$work/log"
 echo "PASS move-flow/owning return $place rejected"
done

cat > "$work/discard.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
pub fn main()->i32 {let owner:=Owner{value:51};owner;ret 0}
SOURCE
"$compiler" --emit-native "$work/discard.kru" "$work/discard"
"$work/discard" > "$work/discard.out"
printf '51\n' > "$work/discard.expected"
cmp "$work/discard.expected" "$work/discard.out"
echo 'PASS move-flow/discard destroys an owner exactly once'
reject discard_then_use 'let owner:=Owner{value:1};owner;consume(owner);ret 0'
reject block_tail_then_use 'let owner:=Owner{value:1};let taken:={owner};consume(owner);ret 0'

cat > "$work/propagation.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
enum Result {Ok(Owner),Err(Owner)}
fn forward(ok:bool)->Result {
 let source:Result:=Result::Err(Owner{value:61})
 if ok {
  let good:Result:=Result::Ok(Owner{value:62})
  let value:=good?
  ret Result::Ok(value)
 }
 let value:=source?
 ret Result::Ok(value)
}
pub fn main()->i32 {
 let good:=forward(true)
 let bad:=forward(false)
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/propagation.kru" "$work/propagation"
"$work/propagation" > "$work/propagation.out"
printf '61\n61\n62\n' > "$work/propagation.expected"
cmp "$work/propagation.expected" "$work/propagation.out"
echo 'PASS move-flow/propagation transfers success and error owners exactly once'

cat > "$work/propagation-reuse.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
enum Result {Ok(Owner),Err(Owner)}
fn invalid()->Result {
 let source:Result:=Result::Ok(Owner{value:71})
 let value:=source?
 ret source
}
pub fn main()->i32 {let result:=invalid();ret 0}
SOURCE
if "$compiler" --emit-native "$work/propagation-reuse.kru" "$work/propagation-reuse" > "$work/log" 2>&1;then
 echo 'FAIL accepted reuse after propagation';exit 1
fi
rg -q 'moved owning value.*reachable control-flow' "$work/log"
echo 'PASS move-flow/propagation invalidates the source owner'

cat > "$work/block-transfer.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
fn produce()->Owner {let value:=Owner{value:81};ret value}
pub fn main()->i32 {
 let outer:=produce()
 let transferred:={outer}
 let inner:={let local:=Owner{value:82};local}
 ret 0
}
SOURCE
"$compiler" --emit-native "$work/block-transfer.kru" "$work/block-transfer"
"$work/block-transfer" > "$work/block-transfer.out"
printf '82\n81\n' > "$work/block-transfer.expected"
cmp "$work/block-transfer.expected" "$work/block-transfer.out"
echo 'PASS move-flow/return and block transfers destroy each owner exactly once'
