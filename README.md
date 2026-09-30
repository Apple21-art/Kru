# Kru Bootstrap Compiler

Kru is a systems programming language whose design is governed by the **Kru Codex**. The authoritative specification in this tree is:

- `KRU_CODEX_0_1_ALPHA_REVISED_3.txt`

`KRU_CODEX_0_1_ALPHA_REVISED_4.txt` is kept as supplemental backend-direction guidance. For language/frontend semantics and conformance expectations, Revised 3 is the normative source of truth in this repository update.

## What this snapshot contains

The bootstrap compiler is written in plain C (GNU11) with no external compiler framework dependency. It currently provides:

- lexer + parser + AST
- limited semantic analysis
- C bootstrap/reference code generation
- direct x86-64 Linux ELF emission with functions, arrays, scalar enums and syscall runtime
- staged language fixtures through the implemented Stage-5 core subset
- categorized PASS / runtime / compile-fail / XFAIL regression testing
- ASan/UBSan gates
- generated stress tests
- split performance measurements
- Codex-style diagnostics with a registry audit

The direct-native path does **not** call an assembler, linker, LLVM, libclang, or other code-generation framework. `kru0` writes the ELF executable and x86-64 machine code itself.

## Build

```sh
make
```

Produces:

```text
build/kru0
```

Sanitizer build:

```sh
make debug
```

Produces `build/kru0-debug` with AddressSanitizer + UndefinedBehaviorSanitizer.

## Driver modes

### Compile Kru to C only

```sh
build/kru0 --emit-c tests/stage4.kru /tmp/stage4.c
```

This mode **never executes the target program**. It exists specifically so conformance testing and frontend benchmarking can measure the compiler rather than accidentally folding target execution into compiler status.

### C-bootstrap compile and run

```sh
build/kru0 --run tests/stage4.kru /tmp/stage4.c
```

The legacy shorthand remains supported:

```sh
build/kru0 tests/stage4.kru /tmp/stage4.c
```

This emits C, compiles it with the bootstrap host-C path, runs the resulting executable, and returns the target program's exit status.

### Direct native machine-code emission

```sh
build/kru0 --emit-native tests/hello.kru /tmp/hello
/tmp/hello
echo $?
# 42
```

Current native target: Linux x86-64 ELF64. Functions and recursion, i32/bool,
bootstrap strings with concatenation and checked byte indexing, padded structs,
payload enums/match, nested fixed arrays, aggregate copies/parameters/returns,
block values, range/collection loops, raw i32 pointers, and file/memory/argv
helpers execute directly. Stage4 has native stdout parity and Stage5 core has
stdout/file parity against the C reference. Calls support up to 32 source
arguments; scalars use System V registers/stack, while aggregates use an internal
Kru copy/return-storage convention.

The complete Codex is not implemented. Full semantic typing, ownership/drop,
slices and owned strings, wider primitives, modules, external aggregate ABI
classification and other major requirements remain. See `NATIVE_BACKEND.md` for precise limits and
`docs/NATIVE_CODEX_AUDIT.md` for the complete requirements inventory.

### Direct native compile and run

```sh
./build/kru0 --run-native tests/stage5.kru
```

This writes `a.out`, executes it directly and returns its exit status.
An optional output path can follow the input filename. This mode uses the
same native language subset as `--emit-native`. Compile failures stop before
execution. Program input/output goes to the terminal normally.

## Verification

### See compilation speed

Add `--time` before the input filename:

```sh
./build/kru0 --time --emit-native tests/native_flow.kru ./native_flow
./build/kru0 --time --run-native tests/native_flow.kru
./build/kru0 --time --emit-c tests/stage4.kru ./stage4.c
./build/kru0 --time --run tests/stage4.kru
```

The report shows source bytes, reading, combined lexing/parsing, semantic
checks, code generation, cleanup, total compile time and parse throughput.
`--run` also shows the host C build time. Program execution is excluded from
the compile total; the report goes to stderr before the program starts.

For repeated measurements with median and p95:

```sh
make bench
```

The benchmark uses a small C monotonic-clock helper. Its samples include
process startup. Internal `--time` measurements start at source reading and
end after compilation/cleanup, so they will differ from shell `time`.
Throughput is bytes per second, not Unicode characters per second.

In fish:

```fish
make
./build/kru0 --time --run-native tests/native_flow.kru
echo $status
# 27
```

Inspect the exit status immediately after running the command. Bash uses
`echo $?` instead.

### Test commands

Primary release gate:

```sh
make check
```

It runs:

- release conformance/runtime suite
- compile-fail diagnostics
- six deterministic Dungeon Escape routes
- direct-native ELF tests
- diagnostic registry audit
- ASan/UBSan conformance suite
- release stress suite
- ASan/UBSan stress suite

Individual commands:

```sh
make test
make test-debug
make stress
make stress-debug
make bench
```

### Current categorized regression status

Supported regression runner:

- 18 PASS checks across emit-C + generated-runtime behavior
- Stage 4 is part of the real regression gate
- 6 Codex-valid target-shape fixtures are XFAIL rather than being disguised as regressions:
  - `stage5_arrays`
  - `stage5_core`
  - `stage5_errors`
  - `stage5_mod`
  - `stage5_mod_main`
  - `stage5_strings`

Compile-fail coverage currently pins:

- K1002 explicit primitive literal type mismatch
- K1004 mutation of immutable binding
- K1005 malformed/missing expression
- K1040 literal out of declared integer range
- K1042 `for ... in` on a non-array target

The diagnostic audit verifies every public diagnostic referenced by `src/` has a registry entry in `include/diagnostics.h`.

## Dungeon Escape example

`tests/pt.kru` is both a playable example and a deterministic runtime regression fixture.

The test suite exercises:

- immediate quit
- locked exit without key
- invalid numeric choice
- full key/victory route
- returning to the Armory after the key is gone
- EOF/input closure without an infinite re-prompt loop

## Stress coverage

`tools/stress_tests.sh` currently checks:

- 1,200 local bindings in one function
- a 1,501-term arithmetic expression
- >200 nested expression depth rejecting cleanly with K1030 instead of crashing/hanging
- 200-arm match
- 40 repeated compiler invocations to catch stale global state/cleanup problems

All stress invocations have finite timeouts.

## Current implemented language boundary

Verified implemented in the bootstrap/reference path includes substantial support for:

- primitive values and numeric suffix/range checking
- `let` / `var`
- functions and recursion
- `if`, `while`, `loop`, `for`, `break`, `continue`
- references and postfix `@`
- structs and enums, including tagged enum payloads used by Stage 4
- block expressions and `match`
- fixed arrays, indexing, field-style `.len`, and bounds checks
- strings as data and current Stage-4 string helpers
- raw pointers / heap builtins / file and CLI primitives exercised by `stage5.kru`

Important Codex-valid areas that are still not implemented include:

- module `use` and multi-file resolution
- slices (`ref [T]`) and slice iteration
- complete `impl`/method receiver model
- complete arena allocator syntax/model from the Codex
- real generics/traits/derive/monomorphization
- `Result`/`Option` target surface and `?`
- enforced `unsafe` semantics for raw pointers
- HIR/MIR as actual compiler stages
- the full direct native backend
- deterministic comptime interpreter
- self-hosting

See `CONFORMANCE.md` for the detailed boundary.

## Design rule

The compiler implementation does not define the language. The Codex does.

When `kru0` and the Codex disagree, that is an implementation gap to fix or document—not permission for the bootstrap to silently redefine Kru.
