# Kru Self-Hosting Feasibility Report

## Current State (Post-Stage 3)

### Pipeline
```
Kru source → C transpiler (kru0) → GCC → native binary
```

The compiler (`kru0`) is written in C (~3,500 lines across 7 files). It lexes, parses, performs basic semantic analysis, and emits C code. The generated C is compiled with GCC to produce a native binary.

### What Works
- Full lexer: keywords, operators, literals (int, float, char, bool, string), comments
- Parser: 10-layer Pratt precedence, control flow, function params, top-level declarations
- Declarations: const, type aliases, structs (named + tuple), enums (unit + payload variants)
- Codegen: multi-pass (types → prototypes → consts → function bodies), type mapping
- Stage 3 features: block expressions, arena blocks (scoped), enum variant access, match expressions, parse-and-skip for generics/traits/impl
- Diagnostics: K1xxx parser errors, K3001 immutable assignment error
- 8 test files pass (core, memory, future, pt, stage1, stage2, stage3, full)

### What's Missing for Self-Hosting

#### Language Features Still Needed
1. **Arrays and slices** — the compiler source uses arrays extensively (token tables, AST children, function tables). Kru needs array literals, indexing, length, and iteration.
2. **Strings as data** — currently `str` is a type annotation only. Need string buffers, concatenation, comparison, and character access for lexer/parser implementation.
3. **File I/O** — the compiler reads source files (`fopen`, `fread`, `fclose`). Need Kru standard library bindings for file operations.
4. **CLI arguments** — `argv`/`argc` access. The compiler takes a filename argument.
5. **Modules/imports** — the C compiler is split across 7 files. Kru needs an import/module system to structure a compiler across multiple files.
6. **Real struct semantics** — need proper struct field assignment, struct passing/returning, and struct arrays.
7. **Real enum payloads** — `Ok(100)` currently emits just `100`. Need tagged unions for proper variant types.
8. **Pointers and manual memory management** — the AST uses pointers heavily (`ASTNode*`, `children[]`). Kru's `ref T` and `mut ref T` need full implementation.
9. **Dynamic allocation** — `malloc`/`free` or arena allocator. The AST uses `malloc` for every node.
10. **Robust type checker** — current sema is minimal (mutability + const). Need full type inference, overload resolution, and error reporting.
11. **Generics (real)** — currently parse-and-skip. A self-hosting compiler might use generics for data structures.
12. **Traits (real)** — currently parse-and-skip. Could be avoided in compiler source.

#### Infrastructure Needed
- Standard library: file I/O, string manipulation, dynamic arrays, hash maps
- Build system: how to compile multi-file Kru programs
- Error recovery: parser must continue after errors for good diagnostics
- Source location tracking: currently only in error messages, not in AST nodes

## Bootstrap Path

Self-hosting is **not feasible today**, but the path is clear:

### Phase 1: Complete the Language (Stages 4-6)
- **Stage 4**: Arrays, strings, for loops, real struct/enum semantics
- **Stage 5**: Pointers/references, dynamic allocation, file I/O, modules
- **Stage 6**: Real generics, traits, robust type checker, standard library

### Phase 2: Bootstrap (Stage 7)
1. Keep the C transpiler (`kru0`) as the bootstrap compiler
2. Write a minimal Kru lexer in Kru (tokenize a .kru file)
3. Add a parser in Kru (build AST from tokens)
4. Add C codegen in Kru (emit C from AST)
5. Compile the Kru-written compiler with `kru0`
6. Use the generated compiler to compile itself
7. Compare output binaries — if identical, bootstrap is complete

### Phase 3: LLVM Backend (Stage 8+)
- Replace C codegen with LLVM IR emission
- This is optional for self-hosting — the C transpiler is sufficient
- LLVM gives: optimization, better diagnostics, platform independence

### Phase 4: Code Editor (Separate Project)
- VS Code extension with syntax highlighting, build/run button, diagnostics
- Web-based editor can come later
- Not required for self-hosting

## Estimated Timeline

| Phase | Effort | Key Deliverable |
|-------|--------|-----------------|
| Stage 4 | Medium | Arrays, strings, for loops |
| Stage 5 | High | Pointers, file I/O, modules |
| Stage 6 | High | Generics, traits, type checker |
| Stage 7 | High | Bootstrap (Kru compiler in Kru) |
| Stage 8+ | High | LLVM backend |

## Conclusion

The compiler has progressed from ~5% spec coverage (Stage 0) to roughly 25-30% (Stage 3). The core pipeline works: lex → parse → sema → C codegen → binary. full.kru (760+ lines) compiles and runs.

Self-hosting is achievable but requires 3-4 more stages of language development. The C transpiler approach is sound as a bootstrap compiler — it's how many languages start (Rust used OCaml, Zig used C++, etc.). The key missing pieces are data structures (arrays, strings), I/O, and modules, not fundamental architecture.

## Code Editor Recommendation

A Kru code editor should be a **separate project** built as a VS Code extension:

**MVP features:**
- Syntax highlighting for .kru files
- Build and run button (calls `kru0` + GCC)
- Diagnostics from K1xxx/K3xxx error codes mapped to line/column
- Sample file templates

**Later additions:**
- Autocompletion (requires language server)
- Inline type hints
- Go-to-definition
- Debugger integration

The fastest path: write a TextMate grammar file for syntax highlighting, then a simple task runner for build/run. This can be done in a few hours.
