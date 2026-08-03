**Kru**

Kru is a small, statically-typed systems programming language that compiles to C.
It's a personal project where I'm exploring how compilers actually work - lexing,
parsing, semantic analysis, code generation, all of it built from scratch in C
with no external dependencies besides a C compiler.

> **Status: ALPHA-1.** The core language (functions, control flow, structs,
> enums, generics, traits, references, arenas, modules) works and is tested.
> Some newer features (multi-value returns, a few `stage5` standard library
> paths) are still in progress - see [Known Issues](#known-issues) below.

```kru
pub fn main() -> int
{
    ret 42
}
```

Why Kru?:

I wanted to actually understand what it takes to go from raw source text to a
running native binary, not just read about it. Kru is that project - a
hand-written lexer, a recursive-descent parser, a semantic analysis pass, and
a C code generator, all wired together by a small build driver (`kru0`). Also,
the name Kru is cool and I wanted an excuse to use it.

Features:

- **Static typing** with primitive types (`int`, `i8`-`i64`, `u8`-`u64`, `f32`,
  `f64`, `bool`, `str`, `char`) and type aliases
- **Structs and enums**, including struct literals and field access
- **Generics and traits**, monomorphized at each call site, with `impl` blocks
  and `#[derive(...)]` attributes (`Copy`, `Clone`, `PartialEq`, `Debug`)
- **References and mutability**: `ref`, `mut ref`, and explicit dereference (`@`)
- **Arena allocation** via `arena { ... }` blocks, plus `unsafe` raw pointers
  and manual `mem_alloc` / `mem_free` / `mem_realloc` if you need them
- **`Option<T>` / `Result<T, E>`** with `?`-operator error propagation
- **Modules** (`use kru_io`, `use kru_env`, `use kru_mem`, ...) for I/O,
  environment access, and memory management
- **Full operator set** - arithmetic, comparison, logical, bitwise, shifts -
  with C-compatible precedence
- **Control flow**: `if` / `else if` / `else`, `while`, `loop`, `break`,
  `continue`, `for ... in a..b`
- Compiles down to plain C, then to a native binary via `gcc`

Quick Start:

You'll need `gcc` and `bash` (Linux/macOS, or WSL on Windows).

```bash
git clone https://github.com/Apple21-art/Kru.git
cd Kru
chmod +x kru0

# Compile and run a Kru program in one step
./kru0 tests/hello.kru
```

The first run builds the Kru compiler itself (`kru0_bin`) from the sources in
`src/`, caches it, and reuses that cached binary on later runs unless the
compiler sources change.

### Other commands

```bash
# Transpile only, don't compile/run (writes out.c by default)
./kru0 build tests/hello.kru

# Transpile to a specific C file
./kru0 tests/hello.kru my_output.c

# Run the full test suite
./kru0 test
```

How it works:

Kru doesn't generate machine code directly. It transpiles to C first, and
that gets compiled with `gcc`. The pipeline looks like this:

```
source.kru -> [lexer] -> tokens -> [parser] -> AST -> [sema] -> checked AST
            -> [codegen] -> generated C -> gcc -> native binary
```

| Stage | File | Responsibility |
|---|---|---|
| Lexer | `src/lexer.c` | Source text -> token stream |
| Parser | `src/parse.c` | Tokens -> AST (recursive descent) |
| Semantic analysis | `src/sema.c` | Type checking, name resolution |
| Codegen | `src/codegen.c` | AST -> C source |
| Driver | `kru0` | Orchestrates build/transpile/compile/run/test |

Project layout:

```
Kru/
├── kru0            # Build/run driver (bash)
├── include/        # Compiler headers (lexer, parser, ast, sema, codegen, token)
├── src/             # Compiler implementation (C)
└── tests/          # .kru conformance tests, run via `./kru0 test`
```

Language Rundown:

A bigger example, showing structs, generics, and traits:

```kru
#[derive(Copy, Clone, PartialEq, Debug)]
struct Point {
    x: i32
    y: i32
}

trait Shape {
    fn area(self) -> i32
    fn name(ref self) -> str
}

impl Shape for Point {
    fn area(self) -> i32 { ret 0 }
    fn name(ref self) -> str { ret "Point" }
}

struct Rectangle {
    width: i32
    height: i32
}

impl Shape for Rectangle {
    fn area(self) -> i32 { ret self.width * self.height }
    fn name(ref self) -> str { ret "Rectangle" }
}
```

References, arenas, and unsafe pointers:

```kru
fn ref_test() -> i32 {
    var x: i32 := 10
    let rx: ref i32 := ref x
    let mr: mut ref i32 := mut ref x

    mr@ = 20
    pr(rx@) // 20
    ret 0
}

fn arena_test() -> i32 {
    arena {
        var v: i32 := 42
        let rv: ref i32 := ref v
        pr(rv@) // 42
    }
    ret 0
}
```

More examples live in [`tests/`](./tests). They double as the conformance
suite and as a rough guide to what's currently supported.

Testing:

```bash
./kru0 test
```

This compiles and runs every `.kru` file in `tests/` and prints a pass/fail
summary. As of ALPHA-1, most core-language tests pass. Some `stage5` tests
that target newer standard library and multi-value-return features are still
failing (see below), and I'm actively working through them.

Known Issues:
This is an alpha release, so there are gaps I'm still closing:

- A handful of `stage5_*` tests (arrays, mod, strings, errors, core, math)
  fail to parse. These exercise standard library modules (`kru_io`,
  `kru_env`, `kru_mem`, `kru_result`, `kru_option`) and some syntax that's
  still being finalized.
- No formal language spec yet. Right now the `tests/` directory is the
  closest thing to one.
- Error messages work but aren't polished yet.

Bug reports and issues are welcome. This is still very much in progress.

Roadmap:

- [ ] Finish standard library module support (`kru_io`, `kru_env`, `kru_mem`)
- [ ] Multi-value returns
- [ ] Better diagnostics (clearer error spans, suggestions)
- [ ] Self-hosting: rewrite the compiler in Kru itself
- [ ] Formal grammar / language spec

License:

See [LICENSE](./LICENSE).

Acknowledgments:

Built as an independent project and science fair submission, exploring
compiler construction from the ground up.
