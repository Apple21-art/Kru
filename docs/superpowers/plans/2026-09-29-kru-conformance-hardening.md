# Kru Conformance Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the current C bootstrap into a trustworthy, Codex-grounded compiler/test package and establish a real direct-native backend seam without LLVM.

**Architecture:** Preserve the existing parser/sema/C codegen. Add explicit CLI modes, a categorized test runner, sanitizer/stress/benchmark tooling, and a small x86-64 Linux ELF native backend behind a separate interface. Treat the Codex as normative and unimplemented Codex features as explicit XFAILs.

**Tech Stack:** C (GNU11), POSIX shell, system C compiler only for the bootstrap C backend, no external compiler/codegen libraries.

**Spec:** `docs/superpowers/specs/2026-09-29-kru-conformance-hardening-design.md`

## Global Constraints

- The Codex is normative.
- No LLVM, libclang, parser generator, libffi, or third-party codegen framework.
- Keep the compiler in self-contained C and preserve the C backend as bootstrap/reference infrastructure.
- Every test/stress command must terminate under a finite timeout.
- Do not count Codex-valid but unimplemented features as passing.

## Review Focus

- Compiler-only mode must never execute generated code.
- Runtime tests must preserve target exit codes instead of confusing them with compiler failure.
- EOF in interactive examples must terminate rather than busy-loop.
- XFAIL classification must not hide a regression in an already-supported feature.
- Experimental native output must be valid ELF x86-64 and reject unsupported ASTs rather than silently miscompile them.

### Task 1: CLI execution boundary
- Test: add shell regression proving explicit emit-C mode returns compiler status 0 for `hello.kru` and does not run it.
- Implement explicit `--emit-c`, `--run`, and backward-compatible CLI handling in `src/main.c`.
- Verify full build and CLI regression.

### Task 2: Categorized regression harness
- Replace stale fixture list with PASS/RUNTIME/XFAIL/COMPILE-FAIL classes.
- Include Stage 4.
- Add deterministic result summary and timeout handling.
- Wire `make test`, `make test-debug`, `make stress`, `make check`.

### Task 3: Dungeon Escape hardening
- Add scripted routes for quit, locked door, invalid input, victory, revisit.
- Make EOF terminate cleanly in generated runtime input helpers or example control flow.
- Verify no route hangs.

### Task 4: Negative and diagnostic coverage
- Add compile-fail fixtures for immutable assignment, bad type, out-of-range integer, and malformed syntax using currently implemented diagnostics.
- Add a diagnostic-code audit script for malformed/duplicate referenced codes.

### Task 5: Stress suite
- Add generator for many bindings, deep arithmetic, large control flow, and repeated compilation.
- Run release and sanitizer compiler with timeouts and record discovered supported limits without claiming unlimited depth.

### Task 6: Benchmark separation
- Replace benchmark script with frontend (`--emit-c`), end-to-end (`--run`), and generated-native runtime classes.
- Report median/p95 using shell/Python-free tooling where practical; otherwise use a small C helper or awk.

### Task 7: Direct-native backend proof
- Add `include/native_backend.h` and `src/native_backend.c`.
- First RED test: `hello.kru` emitted with `--emit-native` must produce an x86-64 Linux ELF executable whose exit status is 42.
- Backend supports only `pub fn main()` returning a compile-time integer expression initially; unsupported ASTs fail explicitly.
- No LLVM or external assembler/linker for this path; write ELF + machine code directly.

### Task 8: Documentation and package verification
- Update README/CONFORMANCE/PERFORMANCE to reflect verified behavior and native-backend status.
- Copy the Codex into the project archive.
- Run clean build, release tests, sanitizer tests, stress, benchmark smoke, and native-backend test.
- Package updated project as `Kru-updated.zip`.
