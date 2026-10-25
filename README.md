# Kru

Kru is a systems programming language I've been making from scratch. It's partly for school, but mainly just something I started because I wanted to see if I could make my own language. I'm still learning as I go, and there's plenty I want to improve. This repo is where I'm keeping the actual releases, including the rough ones.

This is the **September 30, 2026 native compiler snapshot**. It's the latest *working snapshot in this archive set that I checked*, not a claim that my newer local development stops here.

## What Kru is trying to do

I want Kru to be a systems language that can eventually compile itself and write actual machine code. The plan is a proper compiler with a type system, ownership rules, and a native backend, without quietly handing code generation off to LLVM or a host C compiler.

## What this version can do

The compiler is still bootstrapped in C. It has a C reference backend, plus a separate experimental backend that writes **Linux x86-64 ELF files directly**. That native backend executes a tested subset of functions, control flow, structs, arrays, enums and simple file/memory operations.

This is not full Codex parity, complete memory safety, or self-hosting. Read [the exact native limits](NATIVE_BACKEND.md) and [the conformance status](CONFORMANCE.md) before assuming something works.

## Build / run (Linux x86-64, GCC or Clang)

```sh
make
make test
./build/kru0 --emit-native tests/hello.kru ./hello
./hello
echo $? # 42, the program's expected exit status
```

Or run the tested Stage 4 fixture:

```sh
./build/kru0 --emit-native tests/stage4.kru ./stage4
./stage4
```

`make` builds the **compiler itself** using a host C compiler. `--emit-native` generates the **target program** without LLVM, an assembler or linker. Those are two different things.

For compiler sanitizers and stress suites there are `make debug`, `make stress`, and `make check`. The latter is a larger gate than `make test`.

## Where things stand

- Native Stage 4 fixture: built and ran in a fresh verification.
- `make test`: passed on October 25, 2026.
- Full typed IR, real ownership/moves/borrows, modules, generics, slices, full ABI and self-hosting: still unfinished in this snapshot.
- The Codex describes the goal, **not** a claim that every part already works.

More information: [timeline](docs/RELEASE_HISTORY.md) · [native backend](NATIVE_BACKEND.md) · [Codex audit](docs/NATIVE_CODEX_AUDIT.md) · [performance measurement](PERFORMANCE.md) · [release notes](RELEASE_NOTES.md).

MIT licensed. See [LICENSE](LICENSE).
