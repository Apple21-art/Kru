#!/usr/bin/env python3
"""Compare direct native variants and the generated-C -O2 baseline.
Runtime samples exclude compilation; every run must produce the expected output.
"""
import argparse,json,pathlib,platform,statistics,subprocess,tempfile,time
p=argparse.ArgumentParser()
p.add_argument('compiler',nargs='?',default='build/kru0')
p.add_argument('--previous')
p.add_argument('--samples',type=int,default=5)
p.add_argument('--output')
a=p.parse_args()
if a.samples<1:p.error('--samples must be positive')
compiler=str(pathlib.Path(a.compiler).resolve())
workloads={
 'fib36':('fn fib(n:i32)->i32 {if n<2 {ret n};ret fib(n-1)+fib(n-2)}\npub fn main()->i32 {pr(fib(36));ret 0}\n','14930352\n'),
 'integer_loop':('pub fn main()->i32 {var i:i64:=0i64;var x:i64:=1i64;while i<5000000i64 {x=(x*3i64+7i64)%1000000007i64;i=i+1i64};pr(x);ret 0}\n',None),
 'leaf_loop':('fn run(seed:i64)->i64 {var x:i64:=seed;var i:i64:=0i64;while i<5000000i64 {x=(x*3i64+7i64)&65535i64;i=i+1i64};ret x}\npub fn main()->i32 {pr(run(1i64));ret 0}\n',None),
 'tail_sum':('fn sum(n:i64,acc:i64)->i64 {if n==0i64 {ret acc};ret sum(n-1i64,acc+n)}\npub fn main()->i32 {pr(sum(5000i64,0i64));ret 0}\n','12502500\n')
}
x=1
for _ in range(5000000):x=(x*3+7)%1000000007
source,_=workloads['integer_loop'];workloads['integer_loop']=(source,str(x)+'\n')
x=1
for _ in range(5000000):x=(x*3+7)&65535
source,_=workloads['leaf_loop'];workloads['leaf_loop']=(source,str(x)+'\n')
report={'platform':platform.platform(),'machine':platform.machine(),'cc':subprocess.check_output(['cc','--version'],text=True).splitlines()[0],'samples':a.samples,'compiler_bytes':pathlib.Path(compiler).stat().st_size,'results':[]}
with tempfile.TemporaryDirectory(prefix='kru-bench-') as directory:
 root=pathlib.Path(directory)
 for name,(source,expected) in workloads.items():
  src=root/(name+'.kru');src.write_text(source)
  variants=[('native',compiler)]
  if a.previous:variants.insert(0,('previous',str(pathlib.Path(a.previous).resolve())))
  runs=[]
  for label,exe in variants:
   target=root/(name+'-'+label);start=time.perf_counter_ns()
   subprocess.run([exe,'--emit-native',str(src),str(target)],check=True,capture_output=True)
   compile_ms=(time.perf_counter_ns()-start)/1e6;runs.append((label,target,compile_ms))
  c=root/(name+'.c');target=root/(name+'-c-O2');start=time.perf_counter_ns()
  subprocess.run([compiler,'--emit-c',str(src),str(c)],check=True,capture_output=True)
  subprocess.run(['cc','-std=gnu11','-O2',str(c),'-o',str(target)],check=True,capture_output=True)
  runs.append(('C-O2',target,(time.perf_counter_ns()-start)/1e6))
  samples={label:[] for label,_,_ in runs}
  for _,target,_ in runs:
   r=subprocess.run([str(target)],check=True,capture_output=True,text=True,timeout=20);assert r.stdout==expected,(name,r.stdout,expected)
  for sample in range(a.samples):
   # Rotate order to reduce a fixed before/after bias.
   for label,target,_ in runs[sample%len(runs):]+runs[:sample%len(runs)]:
    start=time.perf_counter_ns();r=subprocess.run([str(target)],check=True,capture_output=True,text=True,timeout=20)
    elapsed=(time.perf_counter_ns()-start)/1e6;assert r.stdout==expected,(name,label,r.stdout)
    samples[label].append(elapsed)
  for label,target,compile_ms in runs:
   row={'workload':name,'backend':label,'median_ms':statistics.median(samples[label]),'samples_ms':samples[label],'executable_bytes':target.stat().st_size,'compile_process_ms':compile_ms}
   report['results'].append(row)
   print(f'{name:16} {label:9} {row["median_ms"]:9.3f} ms  {row["executable_bytes"]:7} bytes',flush=True)
if a.output:pathlib.Path(a.output).write_text(json.dumps(report,indent=2)+'\n')
print('Includes process startup; tiny workloads are latency checks, not throughput claims.')
