# Kru
Stage 0 of the Kru compiler — the earliest surviving build (of ~20 attempts) that lexes, parses, and generates working C for a small subset of the language. Kept as-is for the record.

kru0 — Stage 0

The first surviving build of the Kru compiler. Kru is a from-scratch systems programming language that compiles directly to C (no VM, no managed runtime, no GC), specified in The Kru Codex. This snapshot is Stage 0: roughly twenty earlier attempts were scrapped before this one held together long enough to lex, parse, type-check, and generate working C for a small, real subset of the language.

What this build can do
let immutable bindings and var mutable bindings, both with := initializers
Integer arithmetic (+ - * / %), parenthesized expressions, operator precedence
Functions (fn, pub fn), parameters, return values (ret)
Block scoping
ref / mut ref and postfix @ for read/write through a reference
pr() for printing values
A single int type — no sized integers, floats, bool, char, or str yet (see tests/future.kru for the running TODO list at this stage)
What it does not do yet

This is an early, unaudited snapshot, kept as-is for the record rather than patched retroactively:

No sized integer types (i8..i128, u8..u128), no range checking on literals against a declared type
Integer literals are parsed with strtoll(text, NULL, 0), which does not match Kru's grammar — hex works, but binary (0b) and octal (0o) prefixes are misread as 0, and _ digit separators (1_000_000) truncate the literal at the underscore
No check for reading a var before it's initialized
No compile-time check for constant division/modulo by zero
No test runner — each .kru file is compiled and eyeballed by hand
main() is generated as int64_t main() in the emitted C, which is not a conforming signature

None of this is a defect report on the current state of the language — it's a snapshot of where things stood on day one. Later iterations added sized types, arenas, raw pointers, heap allocation, file I/O with Result/?, strings, arrays/slices, Option, modules, a real build/test harness, and fixes for every gap listed above.

Layout
src/        lexer, parser, semantic analysis, codegen, AST, main driver
include/    corresponding headers
tests/      .kru fixtures exercising what this stage supports
            (future.kru doubles as the roadmap for what's missing)
Building
gcc -std=c11 -Iinclude src/*.c -o kru0
./kru0 path/to/file.kru        # writes out.c in the current directory
gcc out.c -o program && ./program
Why keep it

This is the earliest point in the project's history that still compiles and runs real programs. Everything since has been built on top of it.
