# Kru 0.1 Alpha Conformance Status

## Normative source

The authoritative language/frontend definition in this tree is:

`KRU_CODEX_0_1_ALPHA_REVISED_3.txt`

`KRU_CODEX_0_1_ALPHA_REVISED_4.txt` is treated as backend-direction guidance
for this bootstrap snapshot (direct native emission), not as a replacement for
Revised 3 language semantics.

Where compiler behavior differs from the Codex, the Codex wins.

## Bootstrap implementation profile

`kru0` is currently a plain-C bootstrap compiler. It does not depend on LLVM, libclang, a parser generator, or an external code-generation framework.

It has two backend paths:

1. **C bootstrap/reference backend** — the broadest implemented language surface today. `--emit-c` emits only C; `--run` additionally invokes a host C compiler and executes the result.
2. **Direct native x86-64 Linux experimental backend** — `--emit-native` writes ELF64 + x86-64 instructions directly. It supports functions, padded structs, payload enums, nested fixed arrays, aggregate copies/parameters/returns, bootstrap strings and the tested Stage5 syscall helpers, with explicit limits documented in NATIVE_BACKEND.md.

The direct path is architectural proof, not a claim that the native backend is feature-complete.

## Verified supported regression surface

The current categorized runner verifies successful emit-C and generated-runtime behavior for:

- `hello.kru`
- `future.kru`
- `stage1.kru`
- `stage2.kru`
- `stage3.kru`
- `stage4.kru`
- `stage5.kru`
- `full.kru`
- compiler-only `stage5_math.kru`
- compiler-only / separately scripted `pt.kru`

`tools/run_tests.sh` currently reports 18 supported PASS checks and 6 expected-unimplemented XFAIL fixtures.

### Stage 4

The complete `stage4.kru` fixture compiles and runs through both C reference and direct native paths, with stdout parity. It exercises arrays, field-style `.len`, `for` loops, string helpers, struct behavior and tagged-union enum payload cases. Its Shape matches explicitly cover Label and Square as well as the selected runtime variants.

### Stage 5 core

`stage5.kru` passes through both C reference and direct native paths, exercising the implemented raw-pointer/heap/file/CLI subset with stdout and generated-file parity.

This does **not** mean Stage 5 is complete or fully Codex-conforming. In particular, raw-pointer `unsafe` requirements are not yet enforced.

## Explicit expected-unimplemented fixtures

These are Codex-valid target-shape programs but remain XFAIL in the bootstrap:

| Fixture | Primary gap |
|---|---|
| `stage5_arrays.kru` | slices and slice iteration |
| `stage5_core.kru` | `use`, namespaced first-party modules, arena target syntax |
| `stage5_errors.kru` | modules + real generic Result/Option + `?` |
| `stage5_mod.kru` | module resolver / multi-file build |
| `stage5_mod_main.kru` | module resolver + namespaced paths |
| `stage5_strings.kru` | modules + owned String/method surface |

An XFAIL becoming a pass causes the harness to report XPASS and fail, forcing the classification/documentation to be updated rather than quietly going stale.

## Negative conformance coverage

The compile-fail suite verifies currently implemented error behavior:

- `K1002` — explicit primitive literal type mismatch
- `K1004` — immutable binding modified
- `K1005` — expected expression
- `K1040` — integer literal does not fit declared type
- `K1042` — `for ... in` requires a known fixed-size array in the current bootstrap
- `K1043` — unknown identifier

This update fixed two diagnostic semantic collisions:

- K1002 is now reserved for type mismatch; parser “expected expression” uses Codex-defined K1005.
- K1041 is now only “unrecognized integer literal suffix”; non-array `for ... in` uses K1042.

`tools/audit_diagnostics.sh` ensures every public diagnostic referenced from `src/` is present in `include/diagnostics.h`. The internal fallback buffer value K0000 is excluded and must never appear as a literal public diagnostic.

## Semantic fix added in this update

Before this update, code such as:

```kru
let value: i32 := "not an integer"
```

compiled successfully. `sema.c` now rejects unambiguous explicit primitive-literal mismatches with K1002.

The check is deliberately conservative: aliases and user-defined types are not guessed. Those require the fuller type-resolution system the Codex specifies. This avoids “fixing” a missing type system by introducing false positives.

## Interactive example verification

`tests/pt.kru` now terminates cleanly on input closure and has scripted coverage for quit, invalid choice, locked door, full victory, Armory revisit, and EOF.

## Direct native backend conformance status

The direct native emitter supports, among other bounded i32/int programs:

```kru
pub fn main() -> i32 {
    var x := 6
    while x < 7 { x += 1 }
    ret x
}
```

Verified:

- `hello.kru` becomes a directly emitted x86-64 ELF and exits 42.
- `native_arithmetic.kru` becomes a directly emitted ELF and exits 43.
- `native_unsupported.kru` is rejected rather than silently miscompiled.

The expanded direct backend executes the complete Stage4 fixture with stdout
parity and Stage5 core with identical stdout and generated file bytes to the C
reference. Regressions cover recursion, register/stack scalar arguments, padded
structs/nested fields, nested fixed arrays and aliases, aggregate argument
snapshots/private callee copies and hidden caller return storage. Payload enums
include i32, str, bool, struct and fixed-array cases, arrays, qualified
constructors/match arms and multiple payload bindings.
Array/string `.len()` and bootstrap string concatenation, unsigned byte indexing
and bounds traps, range/i32-ref loops and block values are also covered.
Its runtime uses syscalls and emits without host code-generation tools.

The semantic checker now intersects branch initialization states, conservatively
restores loop entry state, checks compound target reads and aggregate-place
mutability, and permits same-scope shadowing. C emission preserves binding
identity and evaluates match scrutinees once. Struct literal colon syntax and
bitwise precedence now follow the Codex.

The backend resolves canonical supported types and aggregate layouts, but these
fixes do not establish complete frontend typing, borrowing/drop or Codex-wide
conformance. Its aggregate ABI is an internal Kru convention, not the external
System V struct classifier; full public/C ABI classification remains missing.
See `NATIVE_BACKEND.md` and the full chapter matrix in `docs/NATIVE_CODEX_AUDIT.md` for current evidence and remaining gates.

## Major remaining Codex areas

Still missing or incomplete:

1. complete name/type resolution (including aliases in semantic analysis rather than codegen shortcuts)
2. HIR and MIR as actual compiler stages
3. move tracking and Codex region/alias analysis
4. complete method/`impl` model
5. slices
6. modules and `.kh` interfaces
7. real generic constraints/traits/monomorphization
8. closures and explicit capture semantics
9. Result/Option + `?`
10. enforced `unsafe` boundaries
11. deterministic destruction/secure semantics
12. comptime sandbox
13. complete native x86-64 backend, then additional targets
14. first-party library/toolchain surface from the Codex
15. self-hosting

## Stress and sanitizer evidence

Both release and ASan/UBSan builds run the stress suite:

- 1,200 local bindings
- 1,501-term arithmetic expression
- >200 nested expression limit rejection with K1030
- 200 match arms
- 40 repeated compiler invocations

This is evidence for these tested bounds only. It is not a claim of unlimited nesting/file size.

## Self-hosting gate

Kru is not called self-hosting until all of these are true:

- a Kru-written compiler can build itself
- generation N builds generation N+1 reproducibly
- generations pass the same conformance/negative/stress suite
- direct-native output is the normal production path for supported targets
- safe-code memory guarantees from the Codex are actually enforced

That gate is intentionally much stricter than “the parser can parse its own source.”
