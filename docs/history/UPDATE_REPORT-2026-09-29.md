# Kru Update Report — September 29, 2026

## Continued update: native Stage4 aggregates and strings

The entire `tests/stage4.kru` fixture now compiles to direct machine code and
executes with stdout identical to the C reference. Stage5 retains stdout/file
parity. Stage4's two incomplete payload matches now explicitly cover their
previously omitted variants; expected output is unchanged.

Added canonical aggregate layouts for padded named structs, nested fixed arrays
and payload enums. Native values support aliases, literals, fields/indexing,
mutation, contextual zero filling, by-value argument snapshots and private
callee copies. Aggregate results use caller-owned storage passed as a hidden
first argument, including calls with stack arguments. This is an internal Kru
ABI, not the public System V aggregate classifier.

Qualified enum constructors/matches and multiple payload bindings now parse and
run. Bootstrap strings gain concat, `.len`/`.len()` and checked unsigned-byte
indexing. The runtime remains checked-in syscall code; emission does not invoke
host compilation or linking. These strings remain NUL-terminated bootstrap
values rather than the Codex str slice/owned String model.

Independent review reproduced and led to fixes for boolean payload reads of
stale stack bytes and skipped side effects in array length receivers. Recursive
layout validation rejects oversized arrays inside structs/enums. Invalid fields,
payloads, arity, duplicate variants and incomplete matches have negative tests;
secure declarations are rejected while wiping/encryption support is absent.

Current verification: `make test` passes 133 checks with six existing expected
failures; release stress passes five checks. The full sanitizer and sanitizer
stress targets pass with leak detection disabled due to this environment's
LeakSanitizer tracing limitation. Tests include odd-size bool structs, empty
arrays, nested zero fill, copy isolation, stack arguments and payload extraction.
`make bench` now measures native Stage4 emission separately as well as Stage5.

This closes the previous Stage4 native gap, not the entire Codex. Full types,
ownership/Copy/move/borrow/drop, modules/generics/traits, safe references, slices,
secure behavior and tooling/targets remain incomplete. The updated full audit
and backend document state those limits explicitly.

## Expanded native execution update

Replaced the main-only emitter with runtime function compilation, SysV
register/stack calls (up to 32 arguments), recursion, bootstrap strings and a
freestanding Linux syscall runtime. Stage5 now compiles and runs natively;
its stdout and generated file bytes are compared with the C reference.
Native emission succeeds with no host tools in PATH, and outputs have no
ELF interpreter/libc dependency.

Added fixed i32/bool arrays with by-value parameter copies, checked indexing,
range and collection loops, i32 reference iteration, block values, scalar
enums and match, constants/aliases, shifts and full scalar compound operators.
Ten-argument/nested calls, runtime memory/file/argv edges, 131,072-byte I/O,
and control transfers with pending expression operands have regressions.
An independent review reproduced and led to fixes for null-handle assignment
and temporary-stack leaks on break/continue.

Frontend fixes include branch definite assignment, conservative loop state,
compound target reads, immutable aggregate-place checks, same-scope shadowing,
C binding names, single-evaluation C match, normative struct field colons and
bitwise precedence. Parameter-mutation fixtures now use explicit local copies.

The full Codex is still incomplete. No complete Stage4/aggregate/payload-enum,
full typing/ownership, module/generic/trait, secure-memory or ecosystem claim
is made. The complete attached Revised3 requirements were audited across all
107 chapters and Appendices A–J in `docs/NATIVE_CODEX_AUDIT.md`; the user-requested
direct backend overrides Revised3's LLVM architecture, not its language rules.
`NATIVE_BACKEND.md` describes the exact supported runtime and limits. Older
entries below describe prior snapshots and are historical, not current limits.

## Current verification

`make test`: 104 PASS checks, six documented XFAIL fixtures, no failures.
`make stress`: five checks pass. The full sanitizer and sanitizer stress targets
also pass with ASan/UBSan; leak detection was disabled because this
container's tracing setup blocks LeakSanitizer's /proc inspection. This is not
verification of language ownership or target-program memory safety.

`make bench` now reports native Stage5 compile latency and direct native fib30
runtime separately. Measurements from this shared environment are variable;
run on the user's machine for useful performance comparisons. The direct
backend currently emits spill-heavy unoptimized code and does not have a
register allocator or optimizer comparable to the host C compiler.

## Packaging and input-error repair

Input read errors now include the exact filename and OS cause. Non-regular
inputs, failed allocation and incomplete reads preserve an explicit error.
Archive entries use a fixed historical timestamp to avoid future-mtime
warnings when extracting on systems with a different clock. Packaging is
verified by building and executing the native fixture from a fresh extraction.

## Follow-up update: timings and native execution

- `--time` reports actual source bytes, individual compiler stages, total
  compile time and combined lexer/parser throughput using a monotonic clock.
- `--run-native` emits the native executable and executes it directly,
  preserving the target exit status. It supports relative/absolute output
  paths and spaces without passing the native path through a shell.
- Compile time excludes target execution. `--run` additionally reports the
  C host compiler build separately; `--emit-native` remains compile-only.
- Repeated `make bench` measurements use a C timing helper, removing the
  former shell/date measurement overhead.
- Added regression tests for timing output, flag order, native execution,
  status propagation, default output and prevention of stale-output execution
  after failed compilation. These do not extend the native language subset.

## Follow-up update: runtime native control flow

The direct x86-64 ELF backend now emits executable code for i32/int locals,
arithmetic, comparisons, Boolean short circuit, lexical blocks, `if`/`else`,
`while`/`loop`, `break` and `continue`. It uses a stable frame pointer so
nested expression temporaries cannot change local addresses. New native tests
cover loops, shadowing, short circuit, rejection of unsupported calls and
types, and definite return. The C bootstrap reference returns the same exit
codes (27 and 17) for the two new runnable native fixtures.

The original benchmark figures below predate this larger backend and should
not be treated as current measurements. The status and limitations are detailed
in `NATIVE_BACKEND.md` and `CONFORMANCE.md`.

Verification in this workspace: release regression suite and stress suite
passed; ASan/UBSan test scripts and stress suite passed with leak detection
disabled. Default `make check` could not complete because LeakSanitizer failed
to read `/proc` under the container's tracing setup; this is an environment
limit, not a reported program leak.

## Original archive update (historical baseline)

The following status and benchmark figures describe the earlier archive before
the runtime native-control-flow follow-up above. This update was performed
against the uploaded Kru bootstrap tree with the Kru Codex as design authority.

## Major changes

- Added explicit driver modes:
  - `--emit-c` — compile-only C bootstrap output
  - `--run` — C bootstrap compile + execute
  - `--emit-native` — direct x86-64 Linux ELF machine-code path
- Added an experimental Kru-owned native backend (`src/native_backend.c`) that writes ELF64 and machine code directly without LLVM, an assembler, or a linker for its supported subset.
- Produced `KRU_CODEX_0_1_ALPHA_REVISED_4.txt`, updating the canonical backend architecture from LLVM to direct native emission while preserving Revised 3 in `docs/history/`.
- Rebuilt the test harness into supported PASS/runtime, compile-fail, XFAIL, stress, sanitizer, and native-backend gates.
- Added full scripted coverage for the Dungeon Escape example, including EOF termination.
- Added conservative semantic checking for explicit primitive literal type mismatches (`K1002`).
- Corrected diagnostic-code meaning collisions:
  - parser expected-expression is `K1005`; `K1002` is type mismatch
  - non-array `for ... in` is `K1042`; `K1041` remains integer-suffix error
- Added a diagnostic registry audit.
- Added release and ASan/UBSan stress tests.
- Split benchmarking into frontend, end-to-end bootstrap, generated-program runtime, and direct-native emission.
- Rewrote README / CONFORMANCE / PERFORMANCE and added `NATIVE_BACKEND.md`.

## Verified supported regression status

The current supported runner reports:

- 18 PASS emit/runtime checks
- 6 expected-unimplemented XFAILs
- 5 compile-fail diagnostic checks
- 6 Dungeon Escape scripted paths
- 4 native-backend checks (including rejection of unsupported and out-of-range programs)
- diagnostic registry audit: 43 public codes registered

## Stress evidence

Release and sanitizer compiler builds pass:

- 1,200 local bindings
- 1,501-term arithmetic expression
- >200 nested expression rejection with K1030
- 200-arm match
- 40 repeated compiler invocations

The 1,200-binding case intentionally reaches the bootstrap compiler's K2050 tracking-capacity warning. The stress passes because it remains a warning and compilation terminates correctly; it is not presented as evidence that the current bounds-tracking table is unlimited.

## Final benchmark sample

Run with `make bench` in this workspace:

- frontend emit-C (`stage4.kru`): median 2.197 ms, p95 3.149 ms, n=50
- end-to-end C bootstrap (`future.kru`): median 39.431 ms, p95 42.945 ms, n=20
- generated runtime (`fib(30)`): median 2.913 ms, p95 3.243 ms, n=30
- direct native emit (`hello.kru`): median 1.658 ms, p95 2.527 ms, n=50

These are machine/session measurements, not universal Kru performance claims.

## Native backend boundary

The direct backend currently supports a zero-parameter `main` whose body is exactly one return of a compile-time integer arithmetic expression. It emits a Linux x86-64 ELF entry stub directly. Unsupported ASTs and i32-out-of-range results are rejected rather than truncated or silently routed through the C backend.

The next native work should follow `NATIVE_BACKEND.md`: locals, branches/control flow, functions/System V ABI, stack frames, aggregates/arrays, references, data/relocations, register allocation, arenas, floating point, and debug information.

## Still intentionally incomplete

This update does not pretend to implement Codex features that are not actually present. Major remaining areas include modules, slices, full `impl`/method semantics, real generics/traits, `Result`/`Option` + `?`, enforced raw-pointer `unsafe`, HIR/MIR as concrete stages, the complete native backend, comptime sandboxing, first-party library/tooling surface, and self-hosting.
