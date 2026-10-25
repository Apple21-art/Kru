# Kru direct native backend

## Current implementation

`src/native_backend.c` writes Linux x86-64 ELF64 and machine instructions directly.
It invokes no C compiler, assembler, linker, LLVM, or embedded interpreter. The
checked-in runtime uses Linux syscalls without libc. Building the compiler itself
requires a GNU11 C compiler and the host C library.

Execution-tested capabilities:

- Signed/unsigned integers through 128 bits, pointer-sized integers, f32/f64 SSE
  operations, Unicode scalar chars, checked casts including wide integer/float
  conversions, primitive matching and typed aggregate storage.
- Functions, recursion, scalar/aggregate returns, mixed GP/SSE/stack arguments,
  left-to-right evaluation, lexical shadowing and short-circuit logic.
- Fixed arrays, nested structs, payload enums, typed references and dynamic-length
  slices; bounds checks, range slicing, reference mutation and collection loops.
- 16-byte `str` descriptors containing a data pointer and byte length. Embedded
  zero bytes survive calls, aggregate copies, printing, concatenation and file I/O.
  UTF-8 literals and string ranges are checked. String indexing returns an
  unsigned byte as i32; legacy `.len` and length helpers still return i32.
- Source modules, public imports and qualified names; explicit generic
  specialization and argument inference; concrete trait bounds and implementation
  signatures; receiver methods and Option/Result propagation.
- Fuel-bounded compile-time scalar evaluation, value blocks, integer range loops,
  constants and pure function calls. No evaluator is embedded in executables.
- Owned-value transfers, destruction at scope exits and returns, replacement,
  branch alternatives and payload matching. Uninitialized owners have inactive
  destruction flags. Local reference and String-view returns are rejected for
  the structural cases currently tracked.
- Source-distributed String, I/O, memory, environment, Option and Result modules.
  `String.from`, `as_str`, `push_str` and owned `concat` use length-bearing views.
  Checked I/O reports invalid UTF-8 and embedded-zero paths as errors.
- CFG-based direct-reference liveness and reaching-loan analysis. Reference
  copies, assignments, block tails, returned borrowed values, str views and
  slices retain tracked origins across branches and loops. Conflicting direct
  accesses, overlapping mutable aliases and use after source-scope exit are
  rejected for tracked loans. Mutation after the last reference use is allowed.
- Secure struct storage wiping and secure-function frame/register clearing;
  complete SecureString/encryption semantics remain unimplemented.

## Run and measure

In fish:

```fish
make
./build/kru0 --time --run-native tests/stage4.kru
echo $status
./build/kru0 --time --run-native tests/stage5.kru
make bench
```

`--emit-native input.kru output` compiles only. `--run-native` compiles and runs,
forwarding the exit status; default output is `a.out`. `--time` excludes program
runtime. Benchmark process measurements include process startup.

## Verification

`make check` runs release, ASan/UBSan and stress gates. In environments where
LeakSanitizer cannot operate under ptrace, use:

```sh
make check SAN_ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
```

The six former expected-unimplemented fixtures now have mandatory native output
checks. Stages 1–5 and full.kru execute natively. Tests exercise ELF independence
from host tools, file bytes, mixed calls, numeric boundaries, invalid input,
cleanup, UTF-8 and embedded-zero strings. Reviewed native golden output replaces
C comparisons where the C bootstrap's character/float/string semantics differ.

## Runtime maintenance

`include/native_runtime_blob.h` is generated offline from
`tools/native_runtime.S` and `tools/native_numeric_runtime.c` using
`tools/generate_native_runtime.py`. Regeneration needs development-time cc/ld
and binutils; user program compilation does not. Relocations must be PC-relative;
nonempty allocated sections omitted from the blob are rejected. Tests regenerate
an isolated copy and compare bytes.

## Remaining requirements

Full Codex conformance is still pending. Important gaps include the production
resolved HIR/CFG MIR pipeline; complete borrowing and ownership analysis of
partial places and regions; arena allocation and kill rules; full generic/context
inference, generic traits and defaults; closures, destructuring and guarded
patterns; full compile-time aggregate operations; complete core APIs and secure
storage/encryption; labeled flow and full panic diagnostics; object output,
linking, public aggregate ABI, debug information and additional targets.

Aggregate calls currently use a private pointer-to-copy convention with hidden
caller-owned return storage. This is not a public C/System V aggregate ABI.
Unsafe raw-pointer requirements and all reference lifetime guarantees are not yet
enforced. Legacy `str_concat`/file-read helpers allocate backing storage without a
complete ownership API; prefer owned String operations where available.
Implementation limits include 1 MiB code/data, 2048 storage units, 512 function
symbols, 32 source arguments and 8192 fixups. Passing regression tests is not a
claim of complete memory safety or specification coverage.

Reference analysis still lacks complete aggregate-contained provenance,
projected places, nested references/reborrows and arena invalidation. General
borrowed function results conservatively inherit all borrowed arguments;
explicit runtime summaries identify independent allocated/process-owned string
results. Precise function summaries remain pending. Mutable alias copies may be
rejected conservatively where a complete reborrow analysis could allow them.

Whole-owner availability is checked over the emitted control-flow graph with a
fixed-point analysis, including loop back edges, early returns and branch joins.
Discarding an owner, transferring a block tail and propagating an owned payload
with `?` invalidate the source. Partial moves from fields, indices or borrowed
storage remain rejected until per-place cleanup is implemented.

### Runtime regeneration across toolchains

The checked-in runtime is generated offline from assembly and freestanding C.
GCC/binutils versions and flags may produce different valid bytes and symbol
offsets. `make check` verifies a source fingerprint and checks two regenerations
with the current toolchain for byte identity. It always executes the checked-in
runtime. If locally regenerated bytes differ, it also builds an isolated compiler
and runs runtime edge, float and i128 tests against that local runtime.
The checkout is never rewritten by the test.

For a release environment using the original pinned toolchain and flags, set
`KRU_RUNTIME_STRICT_REPRO=1` to additionally require checked-in byte identity.
Source changes require `python3 tools/generate_native_runtime.py`; ordinary Kru
compilation still needs no external assembler, linker or C compiler.
A changed function-alignment setting was used to reproduce the original false
failure and verify the alternate-runtime path. GCC 14.2/binutils 2.44 were not
available in this environment, so this is not a claim of testing those versions.
