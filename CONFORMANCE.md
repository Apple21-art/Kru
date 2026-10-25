# Kru conformance status

The normative source is `KRU_CODEX_0_1_ALPHA_REVISED_4.txt`. Where implementation
and Codex differ, the Codex wins. The overhaul remains in progress.

## Verified regression surface

The direct Linux x86-64 backend emits machine code and ELF without invoking
another compiler, assembler, linker, LLVM or interpreter. The C backend remains
available for bootstrap/reference use; it is no longer a complete behavioral
oracle for native Unicode, floating-point and length-bearing string behavior.

Stages 1–5 and `full.kru` have native execution gates. All six former XFAIL cases
are mandatory native tests: stage5_arrays, stage5_core, stage5_errors,
stage5_mod, stage5_mod_main and stage5_strings. There is no expected-unimplemented
allowance in the categorized runner. `stage5_math.kru` is a source module and is
compiled through its importers; it is not a standalone executable with main.

Dedicated gates cover numeric widths/casts, mixed ABI arguments, aggregate
layouts and snapshots, enums, slices, strings and UTF-8, modules, traits,
compile-time evaluation, ownership cleanup, invalid programs and processor traps.
`docs/NATIVE_TEST_INVENTORY.json` records top-level fixture emission separately
from execution evidence. An emitted executable alone is not a passing test.

Run `make check` for release/sanitizer/stress gates. Leak detection may be disabled
under ptrace using `SAN_ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`; that does not
establish leak freedom. Runtime checks require no installed host compiler tools
and verify no ELF interpreter or external library dependency.

## Unfinished specification requirements

See `NATIVE_BACKEND.md` for current capabilities and concrete restrictions, and
`docs/OVERHAUL_PROGRESS.md` for implementation and regression history.

The production typed HIR/CFG MIR pipeline is not complete. Full borrow/alias,
region/arena and partial-place guarantees remain. Whole-owner move checking now
uses a control-flow fixed point for loops, early returns and branch joins; this
backend-integrated analysis does not replace the planned typed HIR/MIR pipeline.
Direct-reference liveness and reaching-loan analysis now covers tracked bindings,
copies, reassignment, block tails, borrowed call results, string views and slices.
Aggregate-contained origins, projected places, complete reborrowing and arena
invalidation remain outside that implemented slice. General call-result origin
summaries are conservative and may reject otherwise valid programs.
Raw-pointer unsafe-context rules remain incomplete. Generic inference/traits,
compile-time aggregates and the standard library cover tested subsets. Closures,
full patterns, secure string/encryption behavior, public aggregate ABI,
object/linking/debug-info and other targets still require implementation.

Strings now have pointer-and-length layout, preserve embedded zero, and validate
UTF-8. Their complete borrow/lifetime analysis and ownership of legacy allocating
string helpers remain unfinished. Source String methods return explicit owned
values or borrowed str views, with structural local-view escape checks.

Some fixture syntax was corrected to follow the Codex: comparisons bind bool,
conversions use explicit casts, mutation uses mut ref, and String.as_str returns
str. These corrections preserve the test's intent and expected behavior; they do
not remove unsupported requirements from the specification.

Older chapter audit documents describe historical checkpoints. They must not be
read as proof of current completeness. No full Codex or memory-safety claim is
made by this snapshot, even when all current regression gates pass.
