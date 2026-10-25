#!/usr/bin/env python3
"""Bounded deterministic malformed-input regression and mutation smoke gate."""
import os
import pathlib
import random
import subprocess
import sys
import tempfile

compiler = str(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'build/kru0').resolve())
rng = random.Random(61006)
seeds = ['impl O { x }', 'trait T { x }', 'impl O { [ }', 'trait T { fn',
         'pub fn main()->i32 {ret 0}', 'struct O {x:i32} impl O {fn read(ref self)->i32 {ret self.x}}',
         'trait T {fn read(ref self)->i32}', 'pub fn main()->i32 {let a:=[1,2];pr(a[0]);ret 0}']
count = int(os.environ.get('KRU_FUZZ_CASES', '80'))
with tempfile.TemporaryDirectory() as directory:
    source = pathlib.Path(directory)/'input.kru'
    output = pathlib.Path(directory)/'output'
    def compile_case(text, mode, reject=False):
        source.write_text(text)
        try:
            r = subprocess.run([compiler, mode, str(source), str(output)], capture_output=True, timeout=2)
        except subprocess.TimeoutExpired:
            raise SystemExit(f'FAIL parser timeout in {mode}: {text!r}')
        if r.returncode not in (0,1) or b'AddressSanitizer' in r.stderr or b'runtime error:' in r.stderr:
            raise SystemExit(f'FAIL compiler crash in {mode}: {text!r}\n{r.stderr.decode(errors="replace")}')
        if reject and (r.returncode != 1 or b'error[K' not in r.stderr):
            raise SystemExit(f'FAIL invalid member not diagnosed: {text!r}')
    for mode in ('--emit-c', '--emit-native'):
        for text in seeds[:4]+['impl O {'+'['*4096, 'trait T {'+'['*4096]:
            compile_case(text, mode, reject=True)
        for _ in range(count):
            text=rng.choice(seeds)
            for _ in range(rng.randint(1,6)):
                at=rng.randrange(len(text)+1)
                if rng.randrange(2) and text:
                    text=text[:at]+text[min(at+rng.randint(1,5),len(text)):]
                else:
                    text=text[:at]+rng.choice(['[', '}', 'fn ', '/// note\n', '@', '"', ';', 'ret ', '\x00'])+text[at:]
            compile_case(text, mode)
print(f'PASS parser/progress regressions and {count*2} seeded mutation compilations')
