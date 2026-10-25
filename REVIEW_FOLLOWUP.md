# Review follow-up — 2026-10-07

This records verified changes, not acceptance of every claim in the external
review. The original differential corpus and patched sandbox were not supplied.

## Reproduced and addressed

- `impl O { x }` and `trait T { x }` timed out in the current release compiler.
  Member loops now guarantee progress, reject invalid tokens and never append
  null methods. Failed function parses release the partially allocated node.
- Documentation tokens previously caused K1011 at file scope. The parser now
  accepts `///` as trivia at declarations, fields, statements and EOF. Attaching
  documentation to declarations and generating API documentation remain pending.
- Added timeout-bounded, seeded mutation testing to both release and sanitizer
  make-check gates. An additional 2,000 mutation compilations across emit-C and
  emit-native completed with ASan/UBSan and leak detection disabled. This is a
  reproducible smoke fuzzer, not exhaustive fuzz coverage or a leak-free claim.
- Selected native diagnostics now have codes, help text and source locations:
  literal range, unknown identifier, argument count, unknown field, missing
  return, non-exhaustive match, duplicate functions and function/output capacity.
  Name suggestions use a small spelling heuristic, not complete fuzzy lookup.
- Source IDs survive AST cloning and constant folding. Diagnostic source buffers
  remain available after imported modules are parsed, so selected imported-file
  errors display the correct filename and source line. Literal spans are retained.
- Move/lifetime/alias failures have K3xxx codes. Multi-error semantic recovery,
  complete spans, universal coded limits and moving validation out of the native
  adapter into typed sema remain unfinished. Ordinary unsupported source must not
  be relabeled as an internal compiler error merely because the adapter rejects it.
- Scalar local reads now use a direct frame-memory operand. Width and signedness
  regression tests pass. The seven-width fixture shrank from 10,344 to 10,323 ELF
  bytes; this is not a runtime-speed claim or register-allocation implementation.
- Documented test dependencies including ripgrep. Added .gitignore and a
  deterministic source packager. Generated root executable and I/O scratch files
  were moved under build and are excluded. Archive timestamps are fixed.

## Still open

Closures, destructuring, guards, test/defer/fmt and the
other conformance gaps remain work items. Runtime panic reporting, guard pages,
all capacity removals and the production typed HIR/MIR backend are not complete.
The reported 98 C/native differences cannot be classified without their sources
and expected semantics; the C emitter remains non-authoritative.

Changing literal promotion, overflow policy, short bodies, captures, print
spelling, formatting syntax or compatibility syntax would change language policy.
Those proposals have not been silently substituted for the governing Codex.

## Generic structs and the Codex boundary

Explicit, contextual and nested generic struct construction now lowers to native
layouts. Concrete implementations (including trait implementations), reference
receivers, mutation, Self returns and owned-field destruction have regression
coverage. Field-only type inference remains incomplete.

Chapter 36 explicitly forbids blanket `impl<T>` in 0.1 Alpha. An experimental
implementation was tested and removed from this release; these blocks now receive
an explicit rejection instead of being silently skipped. Concrete implementations
such as `impl Box<i32>` are supported. Chapter 34 specifies bound checking at
monomorphization sites; the review's unused-body checking proposal is not treated
as an additional governing requirement.
