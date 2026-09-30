# Native compiler Codex requirements audit

Audit date: 2026-09-29. Basis: the complete attached **Kru Codex 0.1 Alpha Revised 3**, 5,108 lines, 107 chapters and Appendices A–J. Code evidence: `include/ast.h`, `include/token.h`, `src/lexer.c`, `src/parse.c`, `src/sema.c`, `src/codegen.c`, `src/native_backend.c`, and stage fixtures. This is a requirements inventory, not certification. It records the integrated direct-native implementation and regression evidence at the end of this work, with remaining requirements explicitly identified.

Revision 3 Chapter 47 explicitly prescribes LLVM and rejects compiler-owned instruction selection/emission. The user expressly requests direct machine emission. That backend architecture is an intentional user-authorized deviation, reflected by the local Revision 4. The language, memory, safety and ecosystem requirements below remain requirements; choosing a direct emitter does not waive them. Revision 3 is the reference for this audit, rather than silently substituting implementation behavior for specification.

## Reading the matrix

**FE partial** means the frontend can represent part of the syntax, without all semantic guarantees. **FE missing** means syntax/data is absent or discarded, so backend work alone cannot implement it. **C partial** identifies reference-backend behavior, not native support. **Native current** includes recursive typed functions with up to 32 arguments (six System V registers plus stack arguments), i32/bool/NUL-terminated str/raw *i32 values, signed arithmetic/shifts/all compounds, constants/aliases, padded named structs, nested fixed arrays and aliases, scalar/payload enum aggregates, by-value aggregate snapshots/private callee copies and hidden caller return storage, checked indexing/mutation, exclusive integer range/collection loops, block values, and scalar/payload enum identity/exhaustive basic match with qualified constructors/match arms and multiple payload bindings. Native strings support content equality, concatenation, `.len`/`.len()` length and checked unsigned byte indexing; null-to-str is rejected. A freestanding syscall runtime provides printing, string helpers, file operations, heap allocation/reallocation/free, and argv access. This is substantially beyond the original main-only emitter, and still not full language coverage. **Policy** is a design/convention requirement rather than a source construct. Features explicitly deferred by the Codex are separated below.

## Complete chapter requirements matrix

| Chapters | Required behavior | Existing evidence / gap | Acceptance gate |
|---|---|---|---|
| 1–3 | Natural systems programming, explicit behavior, native execution, determinism, safety, first-party ecosystem; no GC | C bootstrap plus the current direct native implementation exist; mature language/ecosystem remains incomplete | Native compilation must preserve language behavior and reject unsupported code clearly; do not call the bounded subset the complete compiler |
| 4 | `.kru`, `.krux`, `.kh` units; filename defines module; public i32 main | Single source pipeline with native public i32 entry/calls; no component/header/module model | Separate-unit compile/link, public API visibility, generated `.kh`, Kru-X mode |
| 5 | UTF-8 strings/comments, ASCII identifiers; all escapes including byte/Unicode; raw strings; decimal/hex/bin/oct, separators, suffixes; i32→i64→i128 inference; floats, Unicode char | Lexer/parser implement substantial literal syntax; AST integer magnitude is only uint64; true i128/u128 full-range values cannot be retained | Boundary suite for every type, malformed escapes/scalars, embedded NUL with preserved length, invalid UTF-8 policy |
| 6 | Active and future keywords; contextual self/Self/component; shadowable builtin names; null only raw pointer in unsafe | Some keywords tokenize but have no implementation; contextual treatment and builtin shadowing need review; null AST exists with only literal-binding compatibility check | Reserved names fail; legal contextual names succeed; shadowed pr calls user function; null misuse rejected |
| 7 | Identifier grammar; naming conventions are style, not semantics | Identifier lexer exists; no complete naming lint | Valid lowercase type name must parse as type/literal, regardless of stylistic convention |
| 8 | let/var `:=`, assignment `=`, same-scope shadowing, explicit typing, uninitialized var with definite assignment | Same-scope shadowing implemented with distinct C binding names and native slots. sema intersects if branches, restores state after potentially zero-iteration loops, and checks compound reads; broader CFG analysis remains incomplete | Shadowing succeeds; read before assignment or assignment only on some paths fails; typed assignment/calls preserve type |
| 9 | All signed/unsigned widths through 128, pointer-sized integers, f32/f64, bool/char/str; size/alignment rules | C types partially mapped; native i32/bool/str/raw *i32 supported, fixed arrays correctly use 4-byte i32 / 1-byte bool elements; other widths/floats/char absent; AST lacks full 128-bit literal storage | Signedness/width/layout edge cases; float instructions/ABI; real str slice layout |
| 10 | ref and mut ref creation/type, explicit postfix load/store; correct method precedence; raw pointers distinct | AST_REF_EXPR/DEREF_EXPR lowered natively for scalar i32 references/raw pointers and fixed-array collection references; ref/mut ref type syntax and safe provenance/alias checks remain missing | Typed refs, mutable stores, rejection of writes through immutable ref, same-width load/store, raw deref unsafe-only |
| 11 | All arithmetic/bitwise/logical operators and compounds, specified precedence, assignments void/nonchainable | Codex bitwise precedence fixed; native signed i32 arithmetic/shifts/all compounds and bool logic typed; broader frontend operand typing missing | Shift and compound suite; bool/integer distinctions; evaluate lvalue and RHS once; checked cast policy |
| 12 | Functions, explicit returns, void early return, by-value move/Copy and reference arguments; no overload/defaults; optional inline hint | Native functions enforce supported parameter/call/return types and arity, recursive calls, up to 32 args with registers/stack; supported aggregates returned through hidden caller-provided storage; broader frontend typing incomplete | Nested and recursive calls, arity/type rejection, >6 scalar args, early return, all-path return, preserved stack alignment |
| 13 | Typed closures, explicit captures (value/mutable/reference), capture environment in type, no hidden heap; capture-free function pointer; K3003 escape rejection | FE missing: no closure/capture/fn-type node | Capture-mode behavior; value closure escape works; local reference escape fails; indirect native call and environment layout |
| 14 | if/else, while/loop, break/continue with labels; blocks are values of tail expressions; ret always enclosing function | Unlabelled control flow and block values lowered natively, including cleanup of pending expression temporaries on break/continue; labels absent in parser | Block-expression early ret; nested labels; no-value tail rejected when value required |
| 15 | Exhaustive match; literal/enum/payload/struct/wildcard/binding/alternation; pure guards and false-guard warning | Native scalar/payload enum identity/basic match evaluates scrutinee once, binds multiple typed payload fields and checks enum exhaustion; primitive catch-all required. C match now evaluates scrutinee once. Guards, struct patterns, alternation and complete bool exhaustion remain absent | Scrutinee evaluated once; ordered fallthrough; correct binding scopes; K1006/K2003; exhaustive bool/enums and required primitive wildcard |
| 16 | Named/tuple/unit structs, field access, explicit value/ref/mut-ref self methods, defaults | Named/tuple/unit declarations and normative colon literals (legacy := accepted) exist in frontend/C; native named structs have declaration-order padded layout, nested field access/mutation and by-value copies/parameters/returns; tuple fields synthesized `_0` while `.0` parser rejects numeric field; default fields and impl methods missing; struct-literal parsing depends on uppercase initial | Layout and field type checks, defaults, numeric tuple access, mutation rules; method receiver dispatch |
| 17 | Payload enums with multiple typed fields; optional backing type/discriminants; construction and match; cast discriminant | Native scalar enums have exact type identity, construction, arguments, printing and basic exhaustive match. FE/C bare-type payload subset exists; native i32/str/bool/struct/fixed-array payload construction, copies, returns, arrays and qualified multiple-binding match implemented; backing types/custom discriminants absent; normative named payload parameters (`Jump(addr: u64)`) do not parse | Distinct enum identity; correct tag/payload alignment; qualified multi-payload construction/match; checked discriminant cast |
| 18 | Real Option/Result types and constructors; compatible `?` propagation; immediate non-unwinding panic; Error trait | FE missing generic identity/propagation; token `?` alone is insufficient; builtin bootstrap calls are not generic library semantics | Ok/Err/Some/None identity/type inference; evaluate operand once; both propagation branches; incompatible enclosing return fails |
| 19 | Fixed arrays with comptime size, fill/list, bounds checks; pointer+length slices, all ranges, whole-array ref decay; len method; value/ref/mut-ref/range/custom Iterable loops | Native fixed arrays of supported scalar/aggregate types, nested arrays, aliases, parameter copies/returns, indexing/assignment, bounds traps, field-style and `.len()` length, exclusive integer range and collection loops supported. Slice/ref types/slicing absent; literal size only; alternate `for item in ref array` supported, required `for ref item in array` absent | Nested-array stride and copy/layout; negative/upper index panic; bounds-checked slice ranges; mutation, empty arrays/slices, dynamic length; Iterable direct calls |
| 20 | UTF-8 str slice, explicit owned String construction/as_str/concat; no string +; byte len, char count, char conversion; SecureString separation | C/native str currently uses NUL-terminated pointers plus bootstrap helpers for content equality, concatenation and field-style length. Native arrays/strings also support `.len()`. Native string indexing checks bounds and returns an unsigned byte; the C bootstrap still uses host-char indexing. No length-bearing UTF-8 str/owned String type/method model; embedded NUL truncates library operations; general method postfix call absent | Embedded NUL preservation, Unicode byte/scalar counts, move/drop, explicit conversion, secure print rejection |
| 21 | Actual generics, inference, bounds and deterministic deduplicated monomorphization; no vtables/boxing | Parser skips generic parameters/arguments, losing identity and constraints; no generic IR | identity<i32>/identity<f64> produce separate typed functions; inferred specialization deduplicates; mismatched type args fail |
| 22 | Deterministic comptime blocks/functions, constants without runtime storage, aliases incl function types | Const/alias AST and native constant inlining and scalar/aggregate type alias resolution exist; no full comptime-evaluable validation; top-level comptime blocks skipped and comptime fn loses distinct restriction; no sandbox | Fold recursive fib, comptime array size, aliases resolved before typing, runtime comptime-fn call fails, forbidden I/O fails |
| 23 | Explicit stack/heap/arena; real O(1) bump arena scope release; no hidden promotion/boxing | Arena AST exists but C emits ordinary scoped block; bootstrap heap helpers active/native work | Aligned bump allocation and exhaustion; scoped release; no references escaping region; heap free contract |
| 24–26 | Resource moves vs opt-in Copy; scope-bound references; aliasing XOR mutability; validity proven or unsafe; raw type legal everywhere but raw production/load unsafe; nullable raw only | sema now performs conservative initialization flow checks, compound reads and immutable index/field owner checks; ownership/provenance/liveness/move/borrow analyses remain absent; safe-reference guarantees not established | K3001/K3002/K3010; branch/loop borrow checks; use-after-move; Copy field validity; raw pointer restrictions; no memory-corruption UB in safe subset |
| 27 | unsafe block suspends only memory/alias checks; FFI/raw arithmetic/unchecked access permitted; type/match/return checks remain | Token/function flag exist, but unsafe block AST/parser missing | Raw/FFI operations rejected outside block; type/match errors still rejected inside |
| 28 | Reverse-order exactly-once stack drop on every scope exit; ordinary arena drop elided; secure arena values wiped | No cleanup/drop IR; impl skipped; secure flags carry no destructor behavior | Cleanup order across fallthrough/ret/break/continue, moved owners not double-dropped, secure wipe even arena release |
| 29 | Safe subset prevents memory-corruption UB; unsafe leaves use-after-free/races/alignment/uninitialized responsibility | Guarantee not implemented; bounds checks alone do not establish it | Negative memory/ownership suite and sanitizer stress; reject invalid safe code before emission |
| 30 | Declaration-order padded layout; packed/align attributes; comptime size_of/align_of | C delegates default layout; attribute parser and layout IR absent; native supported named structs have declaration-order padding and nested aggregate layout; layout attributes/intrinsics remain absent | Known Header offsets/size, nested padding, over-alignment, packed accesses, size/align intrinsics |
| 31 | Summary of memory guarantees | Incomplete as above | Each guarantee mapped to explicit positive/negative tests, never inferred from Stage 5 smoke tests |
| 32–35 | Traits, builtins including Iterable, Self, concrete impl/methods, derives, bounded monomorphization and static dispatch | Trait/impl bodies skipped; no derive or trait model | Missing impl/bad signatures rejected; derived Copy validity; static direct method calls, no trait runtime objects |
| 36 | Concrete impl lookup; blanket impl/specialization deferred | Parser skips whole impl; thus cannot enforce concrete restriction | Reject blanket/specialized impl; deterministic duplicate/concrete impl resolution |
| 37 | Concrete trait method usable during comptime after specialization | Both trait and comptime systems missing | Fold comptime concrete trait call |
| 38–39 | Reflection/metaprogramming reserved, no active macros | Future, not implementation debt for Alpha | Do not add speculative syntax |
| 40–44 | Deterministic pure pipeline; lexer, recursive descent/Pratt-equivalent precedence, AST complete spans; semantic order name/type/move/traits/lifetime | Lexer/parser/AST/sema plus richer native type checks exist; AST stores line/column only, no file/end-span identity; general sema still allows unresolved names and lacks most type/ownership checks | Every Codex example parsed; every expression resolved and typed before emission; source spans retained through backend |
| 45–46 | HIR desugaring/monomorphization then MIR CFG with precise dataflow safety | No HIR/MIR stages; C/direct emitter consumes raw AST | Typed lowered IR with values/places/blocks, explicit calls/moves/drop/unsafe regions; branch/loop analyses |
| 47 | Native backend, arena bump lowering; Revision3 LLVM architecture intentionally overridden | Direct x86-64 executable emitter supports typed functions, arrays, enums, branch/call/data fixups and embedded syscall runtime. C remains reference. Object files/separate-unit linking and general public aggregate/float ABI still missing | Standalone emitted ELF runs inputs at runtime, no embedded-source interpreter or host compilation; object/separate-unit linking; later target support |
| 48 | Folding, dedup, hints, dead drop; general optimization | No real typed optimization pipeline | Optimize without changed behavior; no erased secure wipes; deterministic specialization ordering |
| 49 | Sandboxed deterministic bytecode comptime execution, no syscalls, bounded steps | Missing | Infinite comptime loop diagnosed; filesystem/network/clock unavailable; host-independent results |
| 50 | Ordered maps/sets, no path/time/env leaks, deterministic mono order | Direct emitter uses ordered source traversal and fixed runtime bytes, with byte-identity regression coverage; no full package identity/parallel modules | Byte-identical binaries across output paths, process repeats, directories, cache states; seed explicit |
| 51 | Content/identity-keyed HIR module incremental cache including specialization dependency | Missing | Dependency changes invalidate exactly affected modules; cache never changes output |
| 52 | Self-hosting long-term, explicitly non-normative | C compiler, no self-hosting | Do not label self-hosting an Alpha obligation |
| 53–54 | First-party io/mem/fs/net/str/collections/thread/sync/crypto/db; Heap alloc/free and Region checked access | Bootstrap/native helper subset includes syscall heap, file, io, strings and argv; namespaces/use/library modules missing; arenas/Region absent | Actual shipped modules resolve without external packages; Result-based failures and type-safe Region access |
| 55 | stdout/stdin methods, library pr space-separated args+newline, fmt indexed placeholders | Native print/string helpers and syscall file I/O verified against C output and bytes; pr single argument only; no actual io module, fmt, stdin API or general multiargument print | Input/output/EOF/errors, multiple print args, {N} formatting and Unicode/NUL correctness; shadowing legal |
| 56 | Filesystem file structs with read/close and exactly-once drop | Native syscall file helpers implement read/write/open/close, including 131072-byte read growth parity; real module/method/Result/drop absent | Open failure Result; reads/writes/EOF; explicit close then scope exit never double closes |
| 57 | TCP/UDP/socket APIs and Server/Response | Missing | Loopback network tests and Result errors; no accidental capability access |
| 58 | Native threads, join, atomics, mutexes/channels | Missing; detailed concurrency memory model explicitly deferred | Thread lifecycle and synchronized sharing; define implemented atomic ordering instead of claiming unspecified model |
| 59 | SHA256/key generation, secure key material | Missing | Known crypto vectors, OS entropy explicit runtime API, secure ownership/wiping; no custom unreviewed substitute |
| 60 | NativeStore embedded key/value/document persistence | Missing | Persistence across processes, errors/integrity, ownership and resource cleanup |
| 61–64 | secure structs every exit wipe; SecureString unprintable constant-time equality; deterministic literal encryption/seed; secure slots/registers cleaned on return | Parser flags exist; native secure declarations are explicitly rejected; no wiping/encryption implementation | Inspect emitted binary for encrypted literals, exit-path wipe instructions and constant-time comparison; reject printable/conversion misuse |
| 65 | Default stack protector, PIE, full RELRO, non-executable stack/heap, release stripped; explicit opt-outs | Native emits fixed-address ET_EXEC with one RX PT_LOAD containing code/runtime/data; explicit GNU_STACK, PIE, stack protector and complete applicable hardening/debug profile model absent | readelf/security tests; build/debug/off flags; implement applicable hardening explicitly for freestanding static output |
| 66–67 | Compile-time manifest capabilities; distinction default vs opt-in mechanisms | No manifest/capability resolver | Denied import rejected; dependencies cannot bypass capability grants; ordinary code avoids secure overhead |
| 68–70 | Kru-X shared syntax/ABI, component contextual construct; mount/unmount arena, persistent state/rerender, render requirement, nested lifecycle | FE missing component/extension-aware mode and UI model | Ordinary .krux structs unchanged; component render validation; state/nested lifecycle tests |
| 71 | Generate/import public `.kh` ABI surface, native Kru↔Kru-X calls | Missing | Separate module/header builds with layout/signature agreement and no marshaling |
| 72–73 | Linux Wayland/X11, macOS Cocoa, Windows Win32 desktop UI; WebAssembly target | Missing; primary native target only x86-64 Linux | Per-platform source/ABI/widget tests; wasm reproducibility; backend architecture adapted per user direction |
| 74 | Mobile platforms explicitly deferred | Future | No Alpha obligation |
| 75 | Kru-X capabilities default empty grant | Missing | Every privileged import needs manifest grant |
| 76–80 | Actionable severity/code/message/location/context/why/fix, stable codes and related spans, no leaked compiler internals | Structured frontend diagnostics partial; source spans incomplete; native rejection includes source node information but lacks full standardized Diagnostic payload/code/related spans | Native unsupported/type/memory errors include same structured diagnostic fields as FE; warnings configurable |
| 81 | Optional correct performance suggestions, never automatic behavior/security changes | No substantive lints; suggestions optional | Suggestions verified, deterministic; never add unsafe or hidden allocation |
| 82 | Recovery at statements/declarations/braces/match arms; manifest lints | Parser recovery partial; manifest lints missing | Multiple independent errors without cascades, malformed-source termination |
| 83 | Distinct ICE K5001 including compiler/target/reproduction/backtrace option | No complete ICE metadata path | Injected internal failure distinguishes compiler bug from bad source |
| 84–86 | Deterministic four-space formatter, doc comments attach to next declaration, naming lint | Formatter/doc attachment/naming lint missing | Idempotency and semantic preservation; docs retained by lexer/AST |
| 87 | Source-tree modules, use paths/.kh, public/private boundaries, unused imports | use/module-path/header AST missing; single-file pipeline | Multi-file call, private visibility failure, import cycles/resolution identity, unused-import warning |
| 88–91 | Explicit Result error style, visually minimal unsafe, secure data guidance, review conventions | Policy plus unimplemented underlying constructs | Apply as style/lints/review; do not reinterpret SHOULD as mandatory syntax |
| 92 | Manifest/package identity/compatibility; explicit fetch; local/registry/git hashes; lock file, no silent upgrades/download | Missing | Offline locked builds, source hash tamper error, deterministic resolution; no compile-time auto-fetch |
| 93 | Project build/link targets/profiles/flags; pub i32 main exit code; flags enter identity | CLI subset exists; no complete manifest/build manager | Debug/release/library/target builds; public entry contract; all native compiler flags deterministic |
| 94–95 | Shared-frontend incremental LSP; native debug info and debugger with typed memory/unsafe mode | Missing | Editor and compile diagnostics agree; native source stepping/variables/regions |
| 96 | test declarations, assert/equal/error, compiled native tests, separate unsafe failures, optional parallelism | AST/test syntax absent; shell compiler regressions are not language test framework | Native test binary, failing assertions with expected/received, Err semantics, unsafe categorization |
| 97–98 | Idempotent configured formatter and public API docs/examples/source links | Missing | Same semantic source after repeated format; correct public-only docs with traits/types |
| 99–100 | Reproduce records compiler/target/flags/dependency/source hashes; toolchain semver | Missing reproducibility command/metadata | recreate recorded build and compare hash; versioned compatible rules |
| 101 | Alpha/beta/stable source-of-truth/stability policy | Policy | Incomplete compiler remains labelled incomplete |
| 102–105 | Active match guards/alternation/for remain obligations; other patterns/adapters/async/reflection/macros/library expansion/platform expansion are future; rejected designs remain rejected | See active rows above and reserved list below | No VM/GC/implicit allocations masquerading as native implementation; no future-feature claims |
| 106–107 | Proposal costs/alternatives/compatibility and simplicity final rule | Policy | Design choices documented without inventing unspecified language semantics |

## All appendices

| Appendix | Requirement / classification | Implementation consequence |
|---|---|---|
| A.1–A.10 | Complete translation-unit/declaration/type/expression/control/pattern grammar; resolved-name generic disambiguation and incremental enum/module path resolution | Current parser fixes normative struct colon and bitwise precedence, but still deliberately skips generics/traits/impl/comptime, lacks ref/slice/function types, closure/?/unsafe/use/extern/test/attributes/guards/alternation/struct patterns/labels and general postfix calls. Grammar completion is frontend work, not emitter work |
| B.1–B.4 | Type identity includes declared and generic identity; local inference; no implicit narrowing; cast checked for literals and runtime range, fractional truncation; monomorphized generic types | Extend the canonical supported native types/layouts into a complete shared TypeId model with target size/alignment and conversion rules. Preserve full 128-bit literals; checked runtime cast must branch to panic rather than silently wrap |
| C.1–C.5 | Deterministic symbols/layout; platform ABI; external C only unsafe; stable qualified/mangled public/generic symbols; `.kh` exports | Supported scalar calls have SysV registers/stack and alignment (up to 32 source arguments). Supported aggregate parameters use snapshots and private callee copies; aggregate/array returns use hidden first-argument caller storage, including stack-argument calls. This internal Kru convention is not the external SysV struct classifier. Public aggregate/C ABI and floats still require classification |
| D.1–D.4 | Shipped Option/Result, str/String/SecureString, Array/Slice/Vector/Map/HashSet with explicit ownership/allocation/mutation | Implement real types and shipped modules; name-based builtin shortcuts cannot replace generic identity or resource rules |
| E.1–E.3 | Actionable structured diagnostics, levels, permanent class/range codes | Retain file/full spans throughout lowering and use existing Diagnostic printer for native rejection |
| F.1–F.5 | Illustrated stack/arena/scope reference/move/Copy semantics | Positive and compile-fail tests should implement the described semantics; no extra language construct |
| G | Future keywords/namespaces reserved, not required to function | Reserve them deliberately; unsupported future syntax must not silently execute |
| H.1–H.5 | Explicit placement/lifetime/cost, native performance, safe subset, final rule | Review criterion; no evidence that a hello-world emitter meets these guarantees |
| I.1–I.10 | Recommended phased implementation, every Codex example as regression, Linux SysV and native pr; LLVM advice intentionally overridden | Use phase dependency order, adapting codegen to direct emission. Phase completion requires stated test goal, not one fixture |
| J.1–J.9 | 66 revision changes; newest correction wins historical notes | Particularly enforce generic args before call, known-name disambiguation, path vs field distinction, raw unsafe/null rules, whole-array slice decay. Earlier J.3 “as” mention was superseded by J.8 cast correction |

## Reserved/non-normative work, kept separate

Not required as functional Alpha features: async/await/actors/interfaces/where/yield/defer/operator/macros; dynamic trait objects/dispatch; blanket impl and specialization; reflection (`kru::meta`) and metaprogramming; arena annotation to run ordinary destructors; nondeterministic comptime intrinsic; detailed concurrency memory model; self-hosting; mobile UI; future range patterns; lazy/parallel iterator adapters; compression/image/audio/GPU/distributed library expansion; future platform expansion beyond the target being implemented. Reserved namespaces also include `kru::compiler`, `kru::target`, `kru::experimental`.

Already active, **not** future: match guards, pattern alternation, basic for/Iterable, all Volumes II–III safe-memory rules, secure data rules, and the core-library types in Appendix D. Kru-X desktop and wasm are specified active elsewhere despite broad future platform discussion.

## Highest-impact remaining gaps

1. **Complete typed semantic model is missing.** General `sema.c` Symbol records name/function/mutability/initialization, permits unresolved identifiers and performs limited literal compatibility checks. Native supported types now have specific call/return/operator checks, but that does not establish complete type identity, field semantics, generics, casts, or frontend conformance.
2. **Memory safety requires actual ownership/borrow/region/drop machinery.** Current raw/reference operations lack Codex provenance/lifetime/alias proof and unsafe-block enforcement. Conservative initialization and immutable aggregate-owner checks are useful fixes, but safe code cannot yet claim the promised no-memory-corruption guarantee.
3. **Source syntax is discarded.** Generic parameters/arguments, trait/impl bodies and comptime placeholders lose required meaning. Retain and resolve them or reject explicitly; parse acceptance is not feature implementation.
4. **Foundational aggregate/primitive support is incomplete.** Named structs, supported payload enums, nested arrays and aggregate returns now lower natively with canonical supported types and target layout. Wider signed/unsigned values, floats and char remain unsupported. Full i128/u128 literal storage cannot use the current uint64 AST magnitude. General primitive/layout coverage and public ABI classification remain incomplete.
5. **str is still a NUL-terminated pointer.** String content equality, concatenation, checked unsigned byte indexing, field-style length and null-to-str rejection are implemented, but the specified pointer/length UTF-8 slice and owned String/drop semantics are absent. Embedded NUL operations lose suffix bytes. Runtime file failures use bootstrap conventions rather than typed Result; returned allocated strings lack a complete ownership API.
6. **Remaining patterns and frontend guarantees are absent.** Named payload declaration fields, guards/purity, alternation, struct destructuring, complete bool exhaustion, required ref-loop syntax, slices, method receivers and typed refs need frontend and lowering work. Unknown C payload patterns still merit explicit validation instead of unreachable-code fallback.
7. **Toolchain, modules/security and hardening remain incomplete.** No full HIR/MIR, module/header/object linking, shipped generic libraries, comptime sandbox, secure data/capabilities, PIE/stack protector or complete ecosystem tooling exists.

Fixed during this work and retained as regressions: branch/loop initialization leakage; unread initialized target on compound assignment; same-scope shadow rejection and C binding-name collisions; immutable field/index mutation and unread receiver/index checking; repeated C match scrutinee evaluation; normative struct colon syntax; bitwise precedence; native string pointer equality and null-to-str compatibility; supported scalar stack arguments; pending expression temporaries on loop transfers.

## Dependency order after the native Stage 5 runtime

1. Share and extend the canonical supported native types/layouts with frontend resolved types/bindings and lvalues; complete frontend name/call/return/operator/assignment typing. Make native diagnostics span-aware. This removes the risk of growing a machine emitter on guesses.
2. Add a typed HIR/MIR CFG, consolidating the current conservative branch/loop initialization and native control-transfer work with all-path returns, evaluate-once places/scrutinees, cleanup edges and explicit unsafe regions.
3. Extend implemented structs, payload enums, nested aggregates, block values/constants/range loops and checked indexing to wider primitives and complete target layout/address lowering. Add external/public ABI classification beyond the internal Kru aggregate convention. Bounds traps already exist for supported arrays; add checked casts and precise diagnostics.
4. Complete ref/slice/function types and slice ranges/whole-array decay, then borrow/region/ownership/drop checks. Only now make safe reference and arena guarantees.
5. Complete enum patterns, guards/purity/exhaustiveness, methods/traits/generics/monomorphization, Option/Result and propagation; closures and comptime follow the typed IR foundation.
6. Implement modules/ABI objects/headers and shipped library types, then security, remaining libraries, package/build/LSP/debug/format/doc/test/reproduce toolchain. Some module/type work can begin earlier, but certification still requires all gates.

This is a multi-stage compiler/ecosystem implementation. A short native Stage 5 pass can deliver useful real runtime behavior and close particular gaps; it cannot honestly complete the entire Codex.

## Exact native-lowering acceptance programs

These programs use constructs represented by the current AST (apart from the separately marked future frontend cases). Struct colon syntax is now fixed. The nested-array and named-struct examples below are now implemented acceptance cases; separately marked frontend completion cases remain future gates. Add helper calls to test the expanded function emitter. Verify emitted binaries with exact exit status and stdout; compiler acceptance alone is insufficient.

### Nested arrays: implemented acceptance case (expected exit 24)

```kru
pub fn main() -> i32 {
    var data: [i32; 4] := [3, 5, 7, 9]
    data[1] += 2
    var grid: [[i32; 2]; 2] := [[1, 2], [3, 4]]
    grid[1][0] = data[1]
    ret data[0] + grid[1][0] + grid[0][1] + grid[1][1] + data[3] - 1
}
```

Bounds-negative companions: change last expression to `data[4]`, `data[-1]`, or `grid[1][2]`; each must deterministically panic, rather than load adjacent slots. Test dynamic index as a function parameter too. Preserve the full rank/stride at every index.

### Struct layout: implemented acceptance case (expected exit 42)

```kru
struct Pair { left: i32, right: i32 }
fn sum(p: Pair) -> i32 { ret p.left + p.right }
pub fn main() -> i32 {
    var p := Pair { left: 10, right: 20 }
    p.left += 12
    ret sum(p)
}
```

Negative: change `var p` to `let p` and retain mutation: K1004. Add typed fields u8/u64 in a layout test once primitive backend widths exist; do not allocate every field in identical i32 slots.

### Scalar reference places (expected exit 42; safety gate also required)

```kru
pub fn main() -> i32 {
    var value: i32 := 40
    let handle := mut ref value
    handle@ += 2
    ret handle@
}
```

Native lowering implements supported i32 creation/store/load, but the semantic checker cannot establish mutable reference validity. Negative: use `ref value`, then write `handle@`; reject. Add simultaneous immutable/mutable references and alias-based direct assignment negative tests. Typed ref parameters require frontend work first.

### Literal and enum match (expected exit 42)

```kru
enum Mode { Off, On }
fn classify(value: i32) -> i32 {
    match value {
        3 => { ret 40 }
        _ => { ret 0 }
    }
    ret 0
}
pub fn main() -> i32 {
    let mode := Mode.On
    match mode {
        Mode.Off => { ret 0 }
        Mode.On => { ret classify(3) + 2 }
    }
    ret -1
}
```

Remove `Mode.Off`: reject nonexhaustive match. For the evaluate-once test, call a function that prints one marker and returns a non-first-arm value as scrutinee; stdout must contain exactly one marker. Qualified constructors/match patterns and multiple typed payload bindings are covered by `native_payload_tuple.kru`; named payload declaration fields and general destructuring remain frontend gates.

### Exclusive integer range loop and block value (expected exit 42)

```kru
pub fn main() -> i32 {
    var total := 0
    for i in 0..4 { total += i }
    let remainder := {
        let factor := 6
        factor * factor
    }
    ret total + remainder
}
```

`continue` must advance the range index; nested loops must patch only their own break edges. Test an early `ret` inside the expression block to ensure it exits main/function.

### Definite-initialization negative regressions (fixed)

```kru
pub fn main() -> i32 {
    var value: i32
    if false { value = 42 }
    ret value
}
```

```kru
pub fn main() -> i32 {
    var value: i32
    while false { value = 42 }
    ret value
}
```

```kru
pub fn main() -> i32 {
    var value: i32
    value += 1
    ret value
}
```

All three are now rejected before emission. Further requirements: reject `fn f() -> bool { ret 7 }`, wrong arity, undefined names, string arithmetic, char/int implicit conversion, and mutation through immutable refs/aggregates. Same-scope `let value := 1; let value := value + 1; ret value` is a positive test (expected 2).

### Frontend completion gates, not existing-AST native tasks

```kru
fn sum(values: ref [i32]) -> i32 {
    var total := 0
    for ref item in values { total += item@ }
    ret total
}
pub fn main() -> i32 {
    let values: [i32; 4] := [10, 20, 30, 40]
    let view: ref [i32] := values[1..3]
    ret sum(view)
}
```

Expected exit 50; whole-array `sum(ref values)` must return 100. Complete the required type grammar, slice AST/lowering, general method calls and borrowing before claiming this passes. Struct defaults/tuple `.0`, guarded alternation/destructuring, explicit closure captures, typed methods/impl, `?`, unsafe blocks, extern C, attributes and modules likewise require retained frontend representations first.

## Current verification evidence

The integrated suite includes `tools/test_parser_codex.sh` (normative/legacy struct literals and eight typed-native precedence checks), `tools/test_native_stage5.sh` (including complete Stage4 and Stage5 core parity), `tools/test_native_aggregates.sh`, `tools/test_native_aggregate_rejections.sh`, `tools/test_native_string_operations.sh`, `tools/test_native_runtime.sh`, `tools/test_native_collections.sh`, `tools/test_shadow_runtime.sh`, native enum/string regressions and expanded compile-fail cases. Read the scripts/fixtures alongside this matrix for exact scope.

Native collection fixtures verify i32/bool storage, by-value parameter copying, indexed mutation, scalar-reference collection mutation, field-style len, range break/continue, block values and same-scope shadowing; dynamic bounds failure terminates without timeout. Control-transfer regression executes two million iterations to detect unbalanced temporary-stack growth. Ten-argument nested calls verify left-to-right evaluation and register/stack argument handling. Scalar enum fixture exercises exact enum arguments, constants/aliases and exhaustive variant match.

Native runtime parity checks compare stdout, exit outcomes and filesystem bytes with the C reference backend while hiding host compiler/codegen tools from native invocation, and inspect ELF for absence of an interpreter/external library dependencies. Signed integer/argv edge fixtures and a 131072-byte file exercise runtime read growth and complete write preservation. These establish the implemented bootstrap runtime, not Result/owned String/first-party module conformance.

Current initialization negatives (branch-only assignment, zero-iteration-loop assignment and compound read before assignment) fail with K1038. Struct colon parsing and precedence now pass. Native string-content equality and null-to-str misuse are covered by dedicated positive/negative fixtures. Aggregate regressions verify nested padded structs/arrays, aliases, argument snapshots/private copies, aggregate returns with stack arguments, value iteration and i32/str/bool/struct/fixed-array payload enums with qualified multiple-binding patterns. Native array/string `.len()` calls evaluate their receiver once. String regressions verify exact output/C parity, nested/empty concatenation, bootstrap NUL truncation, unsigned bytes and negative/upper/empty index traps. These do not establish slices, owned String/drop, generics, ownership, security, modules, external aggregate ABI or tooling.

Aggregate rejection regressions cover invalid field/variant declarations, construction/call types, shape/layout limits, exhaustiveness and unsupported secure declarations. Rejecting secure syntax does not implement wiping or encryption.

Backend limits are 1 MiB emitted code/data, 2048 storage units, 2048 array shapes, 512 function symbols including helpers, 32 source arguments and 8192 fixups; they are implementation limits, not Codex language limits.
