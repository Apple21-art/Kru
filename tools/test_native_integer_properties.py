#!/usr/bin/env python3
"""Independent modular-integer oracle for leaf registers and strength reduction."""
import pathlib,random,subprocess,sys,tempfile
compiler=str(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'build/kru0').resolve())
rng=random.Random(61008);checked=0
with tempfile.TemporaryDirectory() as directory:
 root=pathlib.Path(directory)
 for bits in [8,16,32,64]:
  for signed in [False,True]:
   kind=('i' if signed else 'u')+str(bits)
   low=-(1<<(bits-1)) if signed else 0
   high=(1<<(bits-(1 if signed else 0)))-1
   def norm(value):
    value&=(1<<bits)-1
    return value-(1<<bits) if signed and value>high else value
   functions=[];calls=[];expected=[]
   for case in range(12):
    mul=rng.choice([0,1,2,3,4,5,8,9,16]);add=rng.randrange(1,32);cut=rng.randrange(0,high+1)
    functions.append(f'fn f{case}(arg:{kind})->{kind} {{var x:{kind}:=arg;var i:i32:=0;while i<31 {{x=x*{mul}{kind}+{add}{kind};if x>{cut}{kind} {{x=x-{add}{kind}}} else {{x=x+1{kind}}};i=i+1}};ret x}}')
    for value in [low,high,0,1,rng.randrange(low,high+1)]:
     calls.append(f'pr(f{case}({value}{kind}))');x=value
     for _ in range(31):
      x=norm(norm(x*mul)+add)
      x=norm(x-add if x>cut else x+1)
     expected.append(str(x));checked+=1
   source=root/'probe.kru';target=root/'probe'
   source.write_text('\n'.join(functions)+'\npub fn main()->i32 {'+';'.join(calls)+';ret 0}\n')
   r=subprocess.run([compiler,'--emit-native',str(source),str(target)],capture_output=True,text=True,timeout=20)
   assert r.returncode==0,r.stderr
   r=subprocess.run([str(target)],capture_output=True,text=True,timeout=10)
   assert r.returncode==0 and r.stdout.splitlines()==expected,(kind,r.returncode,r.stdout,expected)
print(f'PASS native/{checked} independently checked loop/branch cases across all scalar integer widths')
