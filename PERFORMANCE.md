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
