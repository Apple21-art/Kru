# Kru Conformance Hardening Design

## Authority and intent

The Kru Codex 0.1 Alpha Revised 3 is the normative language source. Where the bootstrap compiler differs, the implementation is either corrected or the gap is documented; the compiler does not silently redefine Kru.

The current compiler remains a self-contained C bootstrap with no external compiler framework dependencies. LLVM is explicitly not the intended canonical backend. The long-term production path is direct native machine-code emission. The existing C emitter remains a bootstrap/reference backend while that native backend matures.

## Scope of this update

1. Establish a trustworthy compiler-only CLI mode so parsing/sema/codegen tests never accidentally execute user programs.
2. Rebuild the regression harness around PASS, runtime, compile-fail, expected-unimplemented, and stress classes.
3. Add deterministic scripted coverage for the Dungeon Escape example, including victory, locked-door, invalid-choice, revisit, quit, and EOF behavior.
4. Add sanitizer and stress gates for the C bootstrap compiler.
5. Split performance measurement into frontend latency, end-to-end latency, and generated-program runtime.
6. Audit diagnostics and document known Codex gaps honestly.
7. Introduce a clean backend interface and a real experimental direct-native x86-64 Linux path for the smallest useful subset, without LLVM or external codegen libraries.
8. Update documentation so implementation status and measurements match evidence.

## Native backend direction

The canonical architecture is:

Kru source -> lexer -> parser -> semantic analysis -> normalized IR/backend input -> Kru optimizer -> target lowering -> instruction selection -> register allocation -> object/executable emission.

For the bootstrap period, the C backend remains available as a reference backend. This update does not pretend to implement the complete native backend. It establishes an experimental x86-64 Linux ELF emitter for constant-return `main` programs as a proof that Kru can emit executable machine code itself, and it isolates that backend behind an interface that can grow without coupling it to C transpilation.

## Test classes

- PASS: Codex-valid and currently supported; compile-only must succeed.
- RUNTIME: supported programs with deterministic expected output/exit behavior.
- COMPILE-FAIL: intentionally invalid source; compiler must reject it, with diagnostic checks where stable.
- XFAIL: Codex-valid features not implemented by kru0 yet; their failure is tracked but does not masquerade as regression.
- STRESS: generated large/deep/repeated inputs with finite timeouts and sanitizer coverage.

## Completion contract

A release archive is acceptable only when: clean and sanitizer builds succeed; `make test` works; compiler-only tests do not execute programs; Stage 4 is part of regression coverage; XFAILs are explicit; Dungeon Escape has deterministic scripted coverage; stress cases terminate; benchmark classes are separated; docs match verified state; and the returned archive contains the Codex plus the updated source/tests/tooling.
