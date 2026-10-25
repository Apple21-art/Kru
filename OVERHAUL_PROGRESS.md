# Native overhaul execution record

Authority: Codex Revised 4 and approved native-overhaul architecture.
Plan: docs/superpowers/plans/2026-10-05-native-primitives.md

Baseline: `make -B test` passed on 2026-10-05; six documented expected failures.

Ruling: execute the approved architectural work inline without another permission
round — user explicitly authorized the overhaul and approved its architecture.

Ruling: use an isolated source copy because the supplied project is an archive,
not a git checkout. The original source directory remains a regression reference.

Status: foundation implementation in progress. Full Codex conformance is pending.

## Implemented checkpoint

Shared primitive metadata, full 128-bit literals, width-aware scalar and aggregate
storage, signed/unsigned arithmetic, 128-bit arithmetic helpers, f32/f64 SSE
execution, mixed GP/SSE/stack calls, Unicode character decoding/printing, checked
casts and primitive matches. Runtime remains syscall-only and freestanding.

Independent review verified 35 mixed ABI programs and 69 conversion cases.
Review caught frame sizing for zero-filled storage, wide compound evaluation
ordering, exponential expression typing, and constant type erasure. Reproductions
were added and fixes verified. Runtime regeneration matched the checked-in blob.
Sanitizer verification uses ASan/UBSan with leak detection disabled; no leak-check
claim is made. Six pre-existing XFAIL cases remain expected, not passing tests.

## Remaining approved work

Full Codex completion is pending. The HIR/MIR production pipeline, safe references,
ownership/borrow/drop and regions, str slice model, modules, generics/traits,
closures, Option/Result, comptime, libraries/tooling and other targets are not
implemented. 128-bit integer/float casts are explicitly unsupported. Current
runtime safety failures use traps rather than complete Codex panic diagnostics.
This archive must not be described as the completed overhaul.

## Final gate evidence — 2026-10-05

Release tests, ASan/UBSan tests, release stress and sanitized stress passed.
Sanitized runs use detect_leaks=0:halt_on_error=1 because LeakSanitizer reports
that this environment runs under ptrace. The constant regression fixture also
passed separately against the debug build. Six documented XFAILs remain.
Build outputs are excluded from the source archive.

## Continued overhaul — compound types

Ruling: correct legacy fixtures that contradict Codex typing rather than weaken
language guarantees. Stage1 comparisons/logical values now declare bool; Stage2
integer-to-float constant uses an explicit cast; native collection mutation uses
mut ref. Expected outputs remain unchanged. Accepting the old forms would violate
Codex bool distinction, explicit conversions and read-only references.

Stage5 arrays was observed failing on retained ref/slice syntax, then passed
native execution with exact output after canonical compound types and dynamic
slice lowering. Moved it out of XFAIL into a mandatory native execution gate.
The remaining XFAILs still represent unfinished requirements, not release success.

## Continued overhaul — modules, generic enums, methods and cleanup

The six former expected-unimplemented fixtures now execute natively with golden
outputs and are mandatory gates, with no XFAIL allowance. A source module loader
resolves public imports and deterministic names. Generic declarations are retained
and explicitly instantiated; Option/Result templates, payloads and propagation
execute directly. Core I/O preserves errors, and memory/environment/string modules
are source distributed. General receiver methods are retained and lowered.

Fuel-bounded compile-time evaluation replaces constants; compile-time I/O is
rejected, and blocks are evaluated rather than skipped. Destructor cleanup runs
on scope exit, returns and loop transfers. Secure struct backing storage is wiped
and secure functions clear frame storage and volatile registers. SecureString and
literal encryption remain separate unfinished requirements.

Ownership RED: native_owned_moves printed 5 then segfaulted because copied owners
were destroyed twice; use-after-move also compiled. Runtime ownership flags and
move diagnostics made the transfer execute once and reject the invalid read.
Block-expression RED: native_drop_blocks ran a premature destructor and crashed;
preserving the tail value and transferring its ownership produced 30,30.

Ruling: earlier fixture implicit enum conversions now use explicit casts;
mutable array iteration and mutable-reference call arguments use mut ref. This
preserves intent while enforcing the Codex instead of accepting invalid forms.

Still pending: full typed HIR/MIR pipeline, complete borrowing/region and partial
place tracking, general generic inference/constraints, real str slices, arenas,
full standard library, complete secure storage/encryption, ABI/object/linking and
platform/toolchain requirements. Existing passing tests do not waive these gaps.

## Continued implementation — 2026-10-06

The historical checkpoint sections above describe earlier states. Current source
has no XFAIL allowance. All six former expected failures execute natively.

Ownership regression fixes: replacement evaluates its RHS before dropping the old
value, transfers ownership, and reinitializes moved bindings. Uninitialized owners
start with inactive destruction flags. Match alternatives have independent move
states; bound and ignored owned payloads are destroyed on scope exit and return.
Drop receiver signatures are checked. Complete CFG and partial-place move analysis
are still pending, particularly loop back edges and early-return branch joins.

Compile-time evaluation now supports value blocks and integer range loops, typed
returns, floating +=/-=, decoded string equality (including embedded NUL), and
isolated function scopes. Original constant ASTs remain alive through evaluation
to prevent dangling string aliases. Invalid erased bindings and operators reject.
Long associative expressions no longer trip generic/folding traversal limits.

Native i128/u128 to/from f32/f64 conversions are implemented using checked IEEE
bit operations in the embedded runtime. Tests include 213 randomized/tie checks,
full-width values, negative fractional truncation, and five runtime trap cases.
Runtime generation validates PC-relative relocations and allocated sections;
negative probes reject absolute pointers and omitted data sections.

Generic function calls infer types from concrete scalar/reference arguments and
nested calls. Concrete trait bounds (including multiple bounds) are checked at
specialization. Trait implementations require matching method signatures. Generic
trait implementations, aliases in constraints, default methods and full contextual
inference remain pending.

Native execution gates now cover stages 1–5 and full.kru. Reviewed native golden
outputs are used for stage1/stage2/full because the C bootstrap prints chars as
integers and truncates some float prints. Other stage outputs compare with C.
The source inventory distinguishes executable fixtures, intentional rejection
fixtures, and stage5_math.kru (a module without main).

Still not a completed Codex implementation: production typed HIR/CFG MIR, full
borrowing/regions/partial-place ownership, length-bearing str, arena allocation,
closure support, complete comptime/type inference/traits, full core APIs, secure
storage/encryption, object/linking/ABI and other platform requirements remain.
Passing current tests is necessary but does not establish full conformance.

## String representation migration — 2026-10-06

Native str now uses a 16-byte pointer/byte-length descriptor, including literals,
parameters, returns, struct/enum fields, printing, comparisons and legacy helpers.
Embedded zeros survive concatenation and file round trips. UTF-8 is validated in
literal parsing and checked file reads; string slicing checks bounds and validates
complete Unicode scalars. Checked file open rejects embedded-zero paths.

String.as_str now returns str, push_str accepts str, String.from and owned concat
are implemented. Legacy allocating str helpers still lack an ownership API;
complete borrow/alias analysis remains pending. Structural return checks now
include local owned-String views. Native and C adventure tests both exercise quit,
locked door, invalid choice, victory, revisit and EOF with identical output.

The full release, ASan/UBSan, release stress and sanitized stress gates passed
with leak detection disabled for ptrace. This does not establish leak freedom.
README, NATIVE_BACKEND and CONFORMANCE now describe current capabilities rather
than the obsolete six-XFAIL checkpoint. Complete Codex conformance is still pending.

## Whole-owner control-flow analysis — 2026-10-06

Added move_flow.c: ownership events and resolved branch edges form a control-flow
 graph; a forward may-unavailable fixed point checks reachable owner uses. Local
identities are declaration identities, not reused stack slots. This replaces the
old source-order moved flags and branch snapshots. Loop back edges, continue,
early return, zero-iteration joins, reinitialization, and proven boolean
short-circuit paths have regression coverage. This is a backend-integrated CFG
analysis, not completion of the production typed HIR/MIR architecture.

The follow-up audit reproduced and fixed duplicate destruction of discarded
owners, block tails, and success/error payloads propagated with `?`. Native
regressions check exact destructor output, valid owner return/transfer and invalid
reuse. Returns from fields/indices and transfers through references are rejected
rather than silently duplicating ownership; per-place moves remain unfinished.

The complete make check gate passed with SAN_ASAN_OPTIONS set to
 detect_leaks=0:halt_on_error=1 (release, ASan/UBSan and both stress variants).
LeakSanitizer cannot operate under the execution environment's ptrace; this is
not evidence of leak freedom. Full borrow/alias/region checking remains pending.

## Reference control-flow slice — 2026-10-06

Added reference_flow.c over the same resolved branch graph as owner availability.
Backwards reference liveness and per-loan forward reaching-alias fixed points
track direct reference bindings, copies, reassignment, block results, borrowed
call results, string views and slices. Conflicting direct reads/writes, mutable
alias overlap, repeated mutable call arguments and use after a tracked source
scope ends now produce compile errors. Last-use mutation and early-return paths
remain accepted. No runtime borrowing machinery is emitted.

The reference-flow regression script includes alias copies, shadowing, loops,
branch joins, scope escapes, block-tail escapes, call arguments, returned refs,
field writes to a borrowed owner, String view invalidation and slice reassignment.
The old stage5_core fixture held a shared reference across mutable use contrary
to Chapter 25; its shared borrow now follows mutation, preserving all outputs.

The full make check gate passed for release, ASan/UBSan and both stress variants,
with LeakSanitizer disabled due to ptrace. This does not establish leak freedom.
Complete aggregate-contained provenance, projected-place loans, nested refs and
reborrows, exact function summaries and arena invalidation remain pending.
General borrowed call results retain all borrowed argument origins conservatively;
known independently allocated/process-owned runtime strings have explicit summaries.

## External review hardening — 2026-10-07

Verified and fixed trait/impl parser non-progress and doc-comment rejection.
Added bounded mutation gates, selected coded diagnostics, module source identity
and literal spans, direct typed frame loads, and source-only deterministic
packaging. See REVIEW_FOLLOWUP.md for evidence and explicit remaining work.
The full release/sanitizer/stress gate passed, followed by 2,000 additional
sanitized mutation compilations (LeakSanitizer disabled under ptrace).

## Generic struct construction — 2026-10-07

Added explicit, contextual and nested native generic struct construction and
concrete specialization methods, including Self returns and trait-bound calls.
Regression tests exercise owned container/field destruction and invalid type
arguments, nominal identity, integer ranges and field names. Blanket generic
impls are explicitly rejected as required by Chapter 36.
The complete release, ASan/UBSan and stress make-check gates passed with leak
detection disabled. All 61 executable inventory fixtures emit native successfully;
this inventory check alone is not proof that every emitted program executes.

## Performance continuation — 2026-10-08

Retained the independently reviewed incoming performance pass. Added direct
integer parameter spills, immediate multiplication and conservative scalar
self-tail-call elimination. Added bounded-stack, argument permutation, cleanup
fallback and compact-instruction regression tests. Fixed module-qualified generic
struct construction. Full release, ASan/UBSan and stress checks passed with leak
detection disabled. This remains a bounded AST adapter; typed MIR, general
register allocation, full language conformance and runtime diagnostics are open.

Runtime reproducibility no longer assumes identical GCC/binutils code generation
across versions. Source freshness and same-toolchain regeneration are checked
separately; differing locally generated runtimes receive isolated execution tests.
Strict checked-in byte matching remains opt-in for pinned release environments.
Plain integer local assignment also eliminates address staging while retaining
its reference-analysis write event. No compiler/runtime dependencies were added.

## Register placement and allocation audits — 2026-10-08

Added conservative four-slot integer leaf register placement, constant multiply
strength reduction, and 480 independent modular arithmetic/loop/branch cases.
Added compiler allocation accounting with UBSan and a persistent negative-test
failure log. Added native --check-heap reporting and owned file reading with
successful/error cleanup regressions. The numeric runtime reserves R12 across
its C helpers for entry-stack bookkeeping. LeakSanitizer remains enabled but
cannot analyze processes under this executor's ptrace setup. Legacy allocated
borrowed strings still leak; this checkpoint does not claim complete leak freedom.

Match payloads now contribute their concrete variant field types during generic
and method resolution, with binding cleanup between arms. Regressions cover
nested owned reads, inferred generic calls, a method-returned scrutinee, different
payload types sharing a binding name, outer shadow restoration and scope escape
rejection. Release and stress gates and the complete UBSan allocation-accounting
gate passed. A differently aligned regenerated runtime also passed edge, float,
i128 and owned/raw heap tests; its isolated compiler now includes its lib modules.
ASan/LSan remains blocked by the executor, not recorded as a passing gate.

## C bounds tracking repair — 2026-10-08

Verified reviewer warning K2050 was specific to the C backend's 1024-binding
metadata table; native indexing uses a separate path. Replaced the fixed table
with reusable 128-entry chunks and a growable chunk directory. Existing lookup
pointers stay valid across growth. Storage is freed after successful or failed
code generation. Allocation failure, count overflow and overlong binding names
now reject compilation with K1050 rather than degrading bounds coverage.
Regression programs exceed 1300 locals and exercise valid indexing, shadow
restoration, out-of-bounds reads/writes and nested-dimension traps. Other existing
C backend bounds gaps (e.g. unsupported provenance through fields/pointers) are
not claimed fixed by this change. Native performance and legacy borrowed-string
ownership remain separate work.

## Owned library file reads — 2026-10-08

Aligned kru_io::file_read with Chapter 20: its Result payload is now an owned
kru_string::String, not an allocated borrowed str. read_to_string remains a
forwarding alias. Updated library callers to retain the owner and explicitly
borrow with as_str(). This intentionally changes the former library return type;
the separate unqualified bootstrap file_read/str_concat built-ins are unchanged
and still require ownership migration. Heap regressions cover 100 repeated large
and empty reads, embedded zeros, close-before-use, cross-function ownership
transfer, early returns and invalid UTF-8 cleanup. The stage5_core fixture also
runs cleanly with --check-heap. No general leak-free claim is made.

## Bootstrap string release — 2026-10-08

Added str_free to the native and C bootstrap runtimes. Every legacy file_read
result, including an empty-on-error result, now has releasable backing storage.
Migrated Stage 4/5, native string fixtures and runtime file-read fixtures to
release their results and nested intermediates. Native runtime comparisons now
use --check-heap. A 10,000-iteration test plus Stage 4/5 run against both the
native heap counter and an allocation-wrapped generated C executable. This is
manual ownership compatibility, not automatic str destruction: old callers must
be migrated, and misuse of str_free is not statically checked. Preferred library
String results still provide scope cleanup. Removed the obsolete cumulative C
"leaked bytes" warning, which did not account for releases.
