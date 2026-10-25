#!/usr/bin/env bash
set -euo pipefail
compiler=$(realpath "${1:-build/kru0}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/tail.kru" <<'SOURCE'
fn sum(n:i64,acc:i64)->i64 {if n==0i64 {ret acc};ret sum(n-1i64,acc+n)}
fn rotate(n:i32,a:i32,b:i32,c:i32)->i32 {if n==0 {ret a*100+b*10+c};ret rotate(n-1,b,c,a)}
pub fn main()->i32 {pr(sum(1000000i64,0i64));pr(rotate(1000000,1,2,3));ret 0}
SOURCE
"$compiler" --emit-native "$work/tail.kru" "$work/tail"
python3 - "$work/tail" <<'PY'
import resource,subprocess,sys
def limit():
 resource.setrlimit(resource.RLIMIT_STACK,(262144,262144))
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=subprocess.run([sys.argv[1]],capture_output=True,text=True,timeout=10,preexec_fn=limit)
assert r.returncode==0, 'tail recursion did not run with bounded stack: '+str(r.returncode)
assert r.stdout=='500000500000\n231\n',r.stdout
print('PASS native/million tail calls use bounded stack and simultaneous parameter updates')
PY
cat > "$work/cleanup.kru" <<'SOURCE'
struct Owner {value:i32}
impl Owner {fn drop(mut ref self) {pr(self.value)}}
fn recurse(n:i32)->i32 {let owner:=Owner{value:n};if n==0 {ret 7};ret recurse(n-1)}
pub fn main()->i32 {pr(recurse(3));ret 0}
SOURCE
"$compiler" --emit-native "$work/cleanup.kru" "$work/cleanup"
"$work/cleanup" > "$work/actual"
printf '0\n1\n2\n3\n7\n' > "$work/expected"
cmp "$work/expected" "$work/actual"
echo 'PASS native/tail-position calls with owned locals retain recursive cleanup'
