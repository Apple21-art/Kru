# Kru Performance Measurement

Performance numbers in this repository are **measurements of a specific build/environment**, not universal language claims.

The benchmark harness separates compiler and runtime workloads:

1. **Frontend latency** — Kru source -> parser/sema -> emitted bootstrap C (`--emit-c`). Target code is not executed.
2. **End-to-end C bootstrap latency** — Kru frontend + emitted C + host C compilation + target execution (`--run`).
3. **Generated-program runtime** — compile once, then time only the resulting native program.
4. **Direct-native emission latency** — Kru frontend + Kru's own ELF/x86-64 emitter (`--emit-native`), no host C compiler/linker.

The direct-emission class now measures both the small flow fixture and Stage5.
The same fib30 workload also runs through direct native emission for comparison
with the generated C executable. Native compilation is excluded from runtime
samples; neither comparison establishes overall compiler/language superiority.

Run:

```sh
make bench
```

For an individual compiler invocation:

```sh
./build/kru0 --time --run-native tests/native_flow.kru
./build/kru0 --time --emit-c tests/stage4.kru
./build/kru0 --time --run tests/stage4.kru
```

`--time` uses `CLOCK_MONOTONIC` and prints read, lex+parse, semantic,
generation, cleanup and total compile times in milliseconds. C-bootstrap
execution mode also measures host compiler selection/build. Target execution
and process startup are excluded from the internal compile total. Parse
throughput measures buffered source bytes through lexing/parsing together;
it is not a standalone disk-read or Unicode character rate.

The repeated benchmark now uses a C helper to time fork/exec/wait with the
same monotonic clock. It includes process startup and reports the median
(mean of the middle pair for even sample counts) and nearest-rank p95.
The previous shell/date method included a timer subprocess in every sample;
old measurements are historical and are not directly comparable.

## Measurement from this update

Measured September 29, 2026 in the original archive before the runtime native
backend extension. These numbers have not been remeasured for this follow-up:

| Class | Workload | Median | p95 | Samples |
|---|---|---:|---:|---:|
| Frontend emit-C | `stage4.kru` | 2.197 ms | 3.149 ms | 50 |
| End-to-end C bootstrap | `future.kru` | 39.431 ms | 42.945 ms | 20 |
| Generated runtime | recursive `fib(30)` | 2.913 ms | 3.243 ms | 30 |
| Direct native emit | `hello.kru` | 1.658 ms | 2.527 ms | 50 |

Re-run `make bench` on the machine where results will be presented. Do not publish these numbers as cross-machine guarantees.

## Why the split matters

A compiler invocation that also executes the target can return the target's exit code. Before the driver gained `--emit-c`, a program like `hello.kru` (`ret 42`) therefore made a successful compile look like compiler failure status 42, and interactive programs could block the compiler benchmark entirely.

The explicit driver modes remove that ambiguity.

## Current backend policy

- **C bootstrap/reference path:** broad implementation coverage today; useful for conformance comparison and portability during bootstrap.
- **Direct native path:** canonical long-term direction from Codex Revised 4; currently the bounded x86-64 Linux function/array/scalar-enum/syscall runtime documented in NATIVE_BACKEND.md.
- **LLVM:** not required and not the canonical Kru backend.

The goal is eventually to compare equivalent workloads emitted by the direct backend against the C-reference backend and other native compilers. Until the direct backend supports the relevant language surface, such comparisons would not be apples-to-apples.

## Benchmark hygiene for science-fair/public claims

For any external performance claim:

- use equivalent algorithms and inputs
- state whether compilation is included
- separate cold/warm runs
- use release builds
- report sample count, median, and p95 (or fuller distribution)
- record CPU/OS/compiler versions
- prevent dead-code elimination from erasing the workload
- compare native systems-language results separately from interpreter comparisons
- never generalize one microbenchmark into “Kru is N× faster overall”

## Native backend optimization pass (October 7, 2026)

Changes in `src/native_backend.c` (no semantic changes intended):

- Integer binary operations with a literal or plain-local right operand skip the
  push/pop temporary: the right value goes to RCX, or is folded into an imm32
  ALU form (add/sub/and/or/xor/cmp).
- `if`/`while` conditions that are integer comparisons emit `cmp` + `jcc`
  directly instead of `setcc`/`movzx`/`test`/`je`.
- Integer literals use the shortest encoding that yields the same normalized
  register value (`mov eax, imm32` instead of `movabs` + sign-extension).
- Calls with up to six scalar integer arguments stage earlier arguments on
  the stack, move the last result directly, then pop earlier arguments into
  ABI registers. This preserves evaluation order and nested calls.

Measured on one machine, `fib(38)` run time, median of 5 (single machine; not a
cross-machine claim): original native backend 370 ms, this pass 249 ms, C
backend built with `cc -O2` 65 ms. Native code is still about 3.8x slower than
`cc -O2` on this call-heavy workload: locals still live in memory and there is
no register allocation, inlining or tail-call transformation.

Verification: `make check` (release, ASan/UBSan, stress) passes, 498 PASS lines.
A differential mutation fuzz of the previous and new backends (2100 mutants of
tests/*.kru, 715 compiled and executed) found no crashes, hangs or output/exit
divergences.

## Follow-up: compact code and bounded tail recursion (October 8, 2026)

- Incoming integer arguments are stored directly from their ABI registers;
  typed local reads preserve width and signedness.
- Literal multiplication uses the x86-64 immediate form when representable.
- Plain integer local assignment stores directly to its frame slot while retaining
  the borrow-analysis write event.
- Binding-free, non-secure functions with integer parameters/results can lower
  a direct self-call in return position to a jump. New arguments are evaluated
  left to right and captured before any parameter is overwritten. Functions
  with owned locals, references, cleanup scopes or other unsupported shapes
  retain normal calls. This is not general tail-call optimization or register
  allocation.
- Qualified generic constructors such as `model::Box<i32>{value:42}` now parse.

Run `python3 tools/benchmark_native_compare.py --samples 15` for three workload
comparisons against generated C compiled with `cc -O2`. Add
`--previous /path/to/previous/kru0` for an interleaved before/after comparison.
Every timed execution checks its output against independently computed expected
values. Runtime samples exclude compilation but include process startup. The
small tail-sum case is a latency smoke check, not a throughput comparison.
Compilation times are single process measurements, not statistical estimates.
Raw results and platform/compiler metadata are in
`docs/NATIVE_BENCHMARK_2026_10_08.json`. These workloads do not establish general
parity with, or superiority to, optimized C.

The bounded-stack regression executes one million recursive steps under a
256 KiB stack limit, checks simultaneous parameter rotation, and separately
checks destructor order for recursive functions that must retain normal calls.

Final 15-sample interleaved run in this environment (previous = supplied
performance-pass compiler): fib36 204.731 -> 197.254 ms; integer loop
59.605 -> 57.779 ms. These are modest differences in a shared environment,
not evidence of a large or universal speedup. The C -O2 baselines were 44.610
and 34.486 ms respectively. The compiler file grew from 278,552 to 278,632
bytes (80 bytes); corresponding native executables were 9,745 and 9,732 bytes.
The strongest demonstrated improvement is bounded stack use for eligible tail
recursion, not beating optimized C on these computation-heavy workloads.

## Scalar leaf registers and strength reduction

A conservative whole-function check permits four scalar slots in R8-R11 for
integer leaf functions. Calls, references, aggregates, inferred local types,
compound assignment and secure functions use the existing frame path. Higher
slots spill normally. This is static register placement, not a general liveness
allocator. Multiplication by selected constants lowers to equivalent LEA/shift,
negation or zero/identity forms, preserving evaluation and modular integer results.

The 15-sample interleaved results in NATIVE_REGISTER_BENCHMARK_2026_10_08.json
compare against the preceding delivered version. The leaf loop improved from
9.885 to 5.846 ms (about 1.69x); its generated-C -O2 baseline was 6.347 ms.
The division loop measured 26.458 -> 20.313 ms versus C -O2 at 23.652 ms.
Fibonacci remains substantially slower than optimized C. These are specific
microbenchmarks in a shared environment, not general C performance parity.
The new runtime bookkeeping and owned-read helper also change executable layout;
small timing differences should not be attributed solely to register placement.
See LEAK_CHECKING.md for the enabled-but-blocked LSan check and independent audits.
