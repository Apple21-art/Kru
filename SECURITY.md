# Security Model

Kru source, manifests, imports, `.kh` files, and package metadata are untrusted
compiler inputs.

## Required compiler defenses

Each item below is a requirement on the compiler, not a description of what
`kru0` already does. Status notes reflect a direct read of `src/*.c` as of
August 8, 2026; see `CONFORMANCE.md`'s "Known compiler defects to track"
section for the fuller writeup behind the flagged items.

- Checked arithmetic for file sizes, token/AST capacities, offsets, and
  ranges. **Partially implemented** — `main.c`'s file reader rejects
  non-regular files and negative sizes before allocating, which is exactly
  this class of defense done correctly; the same discipline needs auditing
  across the rest of `src/*.c`, not assumed from this one call site.
- Configurable limits for source bytes, tokens, AST nodes, nesting, diagnostics,
  comptime steps, generic instantiations, module depth, and output size.
- No network access during ordinary compilation; dependency fetching is an
  explicit separate command with content-hash verification. **No dependency
  fetcher exists yet** — there is nothing to audit for this yet, positive or
  negative.
- Canonicalized module paths confined to the project and approved package
  roots. **Not yet applicable** — there is no module resolver (`use` is
  lexed but not parsed; see `CONFORMANCE.md`).
- Atomic output replacement; never follow an attacker-controlled output
  symlink. **Not implemented.** `main.c` takes the output path from `argv[2]`
  and writes to it directly with no temp-file-plus-rename step and no check
  for a pre-existing symlink at that path. This is a concrete gap against a
  concrete requirement, not a hypothetical — fix before treating `kru0`'s
  output-writing as trustworthy against an adversarial working directory.
- Deterministic maps/iteration and no timestamps, host paths, or environment
  data in build products unless explicitly requested.
- Fuzzing for lexer, parser, manifest, formatter, `.kh`, and diagnostic
  rendering. **Not implemented.** No fuzz harness exists anywhere in the
  tree today.
- `unsafe` must remain local and explicit; it cannot disable type checking,
  exhaustiveness, or return validation. **Not implemented.** `unsafe` is
  lexed as a keyword and nothing else — it isn't parsed as a block or
  modifier, and nothing in `sema.c` or `codegen.c` checks for it. This
  matters concretely now: Codex Revision 3 (Appendix J.9, item 64) newly
  requires `unsafe` to construct or dereference a raw pointer `*T`, and
  `tests/stage5.kru` already constructs and dereferences raw pointers with
  no `unsafe` anywhere. The Codex's own revision notes flag this as an
  acknowledged, not-yet-closed gap — see `CONFORMANCE.md` for the full list
  of Revision 3 items still unenforced.
- **Diagnostic code integrity** (added as an explicit requirement here,
  since a real violation was found): once a `KNNNN` diagnostic code is
  assigned to a condition, it must never be reused for a different
  condition, and every emitted code must be documented in
  `include/diagnostics.h`. `K1041` is currently emitted for two unrelated
  conditions (`codegen.c:3431` and `parse.c:849`); `K1029` and `K2050` are
  emitted but undocumented. This is a real, present-tense violation of the
  Codex's diagnostic-permanence rule (Ch. 79), not a future risk — see
  `CONFORMANCE.md` for the specific line references and the fix needed.

## Safe-language claim

The Codex's memory-safety claim must not be advertised until MIR implements and
tests region liveness, move state, aliasing-XOR-mutability, initializedness, and
checked indexing on every control-flow path. The current bootstrap does not yet
meet that bar — there is no MIR stage at all yet (HIR/MIR are both still on the
missing-conformance-areas list in `CONFORMANCE.md`), so this claim remains
categorically unadvertisable, not just incomplete.

## Secure values

Secure wiping must use a primitive the optimizer cannot remove, cover every exit
path, and be tested at generated-code level. Compile-time literal encryption is
obfuscation against static string scans, not protection against a debugger or a
hostile process; documentation and diagnostics must say so explicitly.

No secure-wiping primitive exists in `kru0` yet — this section, like the
network/fuzzing/`.kh` items above, describes a requirement the implementation
has not reached, not current behavior.
