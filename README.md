# Kru

Kru is a small, statically-typed systems language that transpiles to C.
It's an early-alpha personal project — the compiler (`kru0`) lexes, parses,
does basic semantic analysis, and emits C, which is then compiled to a
native binary by your C compiler of choice.

```
Kru source (.kru) → kru0 (transpiler) → C → native binary
```

## Status

Post-Stage 3. The core pipeline (lex → parse → sema → codegen → binary)
works end to end, and the test suite passes. See
[`SELF_HOSTING_REPORT.md`](./SELF_HOSTING_REPORT.md) for a detailed
breakdown of what's implemented, what's missing, and the roadmap toward a
self-hosted compiler (Kru written in Kru).

**What works today:**
- Full lexer — keywords, operators, literals, comments
- Pratt-parser with full operator precedence and control flow
- Functions, `const`, type aliases, structs (named + tuple), enums
- Block expressions, scoped arena blocks, match expressions
- Diagnostics for parse errors and immutable-assignment errors

**What's not there yet:** arrays/slices, real string handling, file I/O,
modules, pointers/references, dynamic allocation, real generics and
traits. These are tracked as the path to self-hosting.

## Quick start

```bash
git clone https://github.com/Apple21-art/Kru.git
cd Kru
chmod +x kru0

./kru0 tests/hello.kru
```

That builds the compiler (cached after the first run), transpiles
`hello.kru` to C, compiles it, and runs it.

## Example

```kru
pub fn main() -> int {
    ret 42
}
```

A slightly bigger taste, from `tests/core.kru`:

```kru
pub fn math_test() -> int {
    let a: int := 1 + 2 * 3 - 4 / 2
    let b: int := ((10 + 20) * (30 - 10)) / 5
    let c: int := a + b * b - a / 2

    pr(a)
    pr(b)
    pr(c)

    ret c
}
```

## Usage

```bash
./kru0 file.kru              # compile and run
./kru0 build file.kru out.c  # transpile + compile only, don't run
./kru0 test                  # run the full test suite in tests/
```

## Requirements

- `bash`
- `gcc` (or `clang`, if your copy of `kru0` supports `CC=clang`)

## Project layout

```
src/        compiler source (lexer, parser, sema, codegen, ast, main)
include/    corresponding headers
tests/      .kru test/conformance files, run via `./kru0 test`
kru0        build/run/test driver script
```

## Contributing

Contributions are welcome — see [`CONTRIBUTING.md`](./CONTRIBUTING.md) for
the workflow: branch from `main`, add a `.kru` test for any new language
feature, make sure `./kru0 test` still passes, then open a PR.

## License

MIT — see [`LICENSE.md`](./LICENSE.md).
