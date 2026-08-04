# Contributing to Kru

Kru is an early-alpha personal project, so the workflow here is intentionally
lightweight.

## Getting set up

```bash
git clone https://github.com/Apple21-art/Kru.git
cd kru
chmod +x kru0
./kru0 test
```

You'll need `gcc` and `bash`.
Perhaps 'clang' as well, depending if i change too much up.

## Making changes

1. Fork/branch from `main`.
2. Make your change under `src/` (compiler) or `include/` (headers).
3. If you're adding a language feature, add a `.kru` file under `tests/`
   that exercises it.
4. Run `./kru0 test` and make sure you haven't broken any previously-passing
   tests.
5. Open a pull request describing what changed and why.

## Reporting bugs

Open an issue with:

- The `.kru` source that triggers the problem (a minimal repro if possible)
- What you expected vs. what happened
- The exact error output, if any

## Code style

The C source favors explicit, vertically spaced formatting (see `src/*.c`
for examples) — one argument per line in function calls, clear section
comments. Please try to match the existing style rather than reformatting
whole files.
