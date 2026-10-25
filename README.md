# Kru

Kru is a systems programming language I've been making from scratch. It's partly for school, but mainly a personal project I started because I wanted to see if I could make my own language. I'm still learning as I go, and there's a lot I want to improve.

## About this version

This is **v0.3.0-alpha.0**, a historical October 7–8, 2026 snapshot published on October 25. It's not my newest development build, and I don't consider Kru finished or ready for production use. See [the release notes](RELEASE.md) for what changed and what failed during this archive's recheck.

The compiler is still built with C, but it can also compile supported Kru programs straight into Linux x86-64 machine code and write ELF64 executables itself. That native code-generation path doesn't call GCC, LLVM, an assembler, or a linker. The C-output path is kept for comparison and bootstrap work.

## Build and try it

You need Linux x86-64, GNU Make, and a GNU11 C compiler to **build the Kru compiler**:

```sh
make
./build/kru0 --emit-native tests/hello.kru /tmp/kru-hello
/tmp/kru-hello
echo $?
# 42
```

To generate C instead:

```sh
./build/kru0 --emit-c tests/hello.kru /tmp/kru-hello.c
```

## Testing

```sh
make test
```

**Known issue:** `make test` does not pass completely in this archived snapshot. During recheck on October 25, the suite reached 220 PASS lines and then failed in `tools/test_bootstrap_string_cleanup.sh` because the generated C assigned a string-concatenation result to an integer variable. I didn't change the compiler source to hide this in an old release. See [release notes](RELEASE.md) and [the verification note](docs/SNAPSHOT_VERIFICATION_2026-10-25.md).

Some tests require Bash, Python 3, ripgrep, binutils, and other common Linux development utilities. See the Makefile and scripts for dependencies.

## Where things are

- `src/`, `include/` — compiler and native runtime interfaces
- `lib/` — early Kru library modules
- `tests/` — compiler, runtime, and invalid-program test cases
- `tools/` — build, testing, runtime regeneration, and benchmarks
- `KRU_CODEX_0_1_ALPHA_REVISED_4.txt` — intended language specification for this snapshot
- `CONFORMANCE.md`, `NATIVE_BACKEND.md` — feature coverage and limitations
- `docs/OVERHAUL_PROGRESS.md` — historical implementation notes
- `PERFORMANCE.md` — what was measured, when, and the limits of the numbers

The Codex describes what I want Kru to become. The compiler doesn't implement all of it yet.

**Status:** experimental, still being worked on.
