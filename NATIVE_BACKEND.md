# Kru direct native backend

## Current implementation

`src/native_backend.c` emits Linux x86-64 ELF64 and machine instructions directly.
The compiler's native path invokes no assembler, linker, LLVM, host C compiler,
or embedded interpreter. Generated programs use a checked-in freestanding Linux
syscall runtime. The compiler itself is C and uses the host C library.

Supported and execution-tested:

- `i32`/`int`, `bool`, C-string-backed `str`, raw `*i32` pointers, enum
  identities and aliases for supported scalar and aggregate types; constants of
  supported expression kinds.
- Functions, recursion, void and aggregate returns, up to 32 source parameters.
  Scalar arguments use six System V registers plus stack arguments. Arguments
  evaluate left to right; nested calls
  preserve earlier arguments. Integer results normalize to signed 32-bit values.
- Locals, lexical and same-scope shadowing, assignments and compound arithmetic,
  shifts and bitwise operators, signed comparisons, short-circuit Boolean logic.
- `if`/`else`, `while`, `loop`, unlabelled `break`/`continue`, exclusive integer
  range loops, block expressions and enclosing-function returns. Loop transfers
  unwind pending expression temporaries.
- Fixed arrays of supported scalar and aggregate types, including nested arrays:
  literals, declared zero filling, aliases, copying, by-value parameters and
  returns, indexing, mutation, `.len`/`.len()` and value iteration. `ref` iteration is
  supported over i32 arrays. Dynamic bounds violations trap with `ud2`.
- Named structs with declaration-order padded layout, typed literals, nested
  fields, mutation, copying, by-value parameters and returns. Aggregate call
  arguments are snapshotted before later arguments can mutate their source,
  and callees receive private copies.
- Scalar and payload enums, including i32, str, bool, struct and fixed-array payloads,
  aggregate copies/returns and enum arrays; literal/enum match statements,
  qualified constructors and match arms, multiple payload bindings and catch-all
  bindings. Enum matches must cover every variant or have a catch-all. Integer
  and bool matches currently require a catch-all.
- `pr` of one integer/bool/scalar-enum/string, `str_len`, content-based `str_eq` and
  string `==`/`!=`, `str_concat`, string `.len`/`.len()` and checked byte
  indexing returning an unsigned byte as i32; decoded string escapes, raw
  literals and UTF-8 escapes. Negative and out-of-bounds indices trap.
- Bootstrap `file_open`, `file_open_write`, `file_close`, `file_write`,
  `file_read`, `mem_alloc`, `mem_free`, `mem_realloc`, `args_count`, `args_get`.
  File reads grow dynamically; writes handle partial writes. Memory helpers use
  explicit mmap/munmap storage. Invalid file handles use -1 internally.

The entry is a zero-parameter `main` returning i32/int. Functions with results
must satisfy a conservative definite-return analysis. Unix exposes the low
8 bits of a program's exit status. Arithmetic faults use processor traps.
Duplicate struct fields/enum variants and unsupported secure declarations are
rejected; secure wiping/encryption semantics are not implemented.

## Run and measure

In fish:

```fish
make
./build/kru0 --time --run-native tests/stage5.kru
echo $status
make bench
```

`--emit-native input.kru output` compiles only. `--run-native` compiles and
executes, forwarding the program's exit status; the default output is `a.out`.
`--time` excludes program runtime. Benchmark samples include process startup.
The Stage4 and Stage5 execution regressions compare stdout against the C
reference and check the ELF has no interpreter. Stage5 also compares generated
file bytes. Aggregate and string fixtures check exact output and edge cases.

## Runtime maintenance

`include/native_runtime_blob.h` is checked in. `tools/native_runtime.S` contains
its readable assembly; `tools/generate_native_runtime.py` regenerates the header
using development-time assembler/linker tools. Neither regeneration nor those
tools are needed to build kru0 or emit/run native programs. The reproducibility
test regenerates an isolated copy and compares bytes.

## Explicit remaining work

The complete `tests/stage4.kru` fixture and the Stage5 core fixture compile and
execute natively with C-reference parity. This is **not full Codex conformance**.
Missing areas include:

- Other integer widths, floating point, char, full reference types and typing.
- Slices, complete tuple/default struct syntax and destructuring patterns,
  public/external aggregate ABI classification, function pointers and closures.
- Codex str slices and owned strings (current helpers use NUL-terminated bytes),
  library modules and complete I/O methods.
- Typed HIR/MIR, modules, generics, traits/impl, Result/Option propagation,
  comptime, lifetime/borrow/move/drop enforcement, arenas and secure values.
- Full structured backend diagnostics, labeled control flow, pattern guards and
  alternation, exact shift-count/cast policies, complete type checking.
- Object output/linking, debug information, additional targets, PIE, explicit
  stack protection and the rest of the Codex hardening requirements.

Fixed limits include 1 MiB emitted code/data, 2048 storage units, 2048 array
shapes, 512 function symbols (including helpers), 32 source arguments and 8192
fixups. These are implementation limits, not language limits. Aggregate arguments
use an internal Kru pointer-to-copy convention; aggregate results use hidden
first-argument caller-provided return storage, including calls with stack
arguments. This is not the external System V struct classifier or a public C ABI.
Raw pointers and refs currently lack the Codex safety guarantees. No complete
language or memory-safety claim follows from these regression tests.

See `docs/NATIVE_CODEX_AUDIT.md` for the full chapter-by-chapter obligation matrix.
