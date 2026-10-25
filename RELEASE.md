# Kru — October development snapshot

**Version:** `v0.3.0-alpha.0`  
**Published:** October 25, 2026  
**Source snapshot:** The archive was named October 7; the included development notes also cover October 8.  
**Status:** Experimental alpha. This is an older snapshot, not the newest version I'm working on.

I've still been working on Kru, so I figured I'd put up another version instead of leaving the repository sitting there.

This one is from earlier in October. There's been more work since then, but I haven't sorted all of that out into a public release yet. I mainly wanted to keep an actual record of how the compiler has been changing.

The biggest difference from the previous upload is how much more of the language can run through the native backend. Kru was already generating Linux x86-64 executables directly, but this version goes quite a bit further. I also spent some time on memory handling, tests, and getting the generated code to run better.

## What changed

- Added more native integer types, including `i128`/`u128`, along with `f32`/`f64`, Unicode characters, and checked conversions for the tested cases.
- Expanded the native support for arrays, structs, enums, slices, length-aware strings, methods, modules, and parts of generics and traits.
- Added more support for `Option`/`Result`, compile-time evaluation, and ownership cleanup. The borrow and move checks cover more control-flow cases now, but they're still incomplete.
- Worked on file and string ownership, including owned string reads and explicit cleanup for older allocated string helpers.
- Added some smaller native code-generation optimizations: shorter instructions, direct integer operations, limited register placement, multiplication simplifications, and self-tail-calls for eligible functions.
- Expanded the tests around native execution, invalid programs, heap accounting, arithmetic, and compiler errors.

The compiler itself is still built from C. The difference is that its **direct-native output path** writes machine code and an ELF executable without handing the user's program to GCC, LLVM, an assembler, or a linker. The C-output path is still available too.

## Testing and problems

I checked the source again for this upload on October 25:

- `make` completes successfully.
- `make test` **does not fully pass** on this snapshot. It recorded 220 `PASS` lines before stopping at `tools/test_bootstrap_string_cleanup.sh`.
- The failing case comes from generated C for the string cleanup test. Some `str_concat` results are emitted with an integer type and then passed to string functions, causing compilation errors. This is a real limitation in the archived C-output path, not evidence that every native test failed.
- Later test commands weren't reached by `make test`, so I'm not calling the full suite green.

Some old reports inside the source archive describe results from other runs or environments. They're kept as part of the development record, not new verification for this upload.

## Still not finished

The typed HIR/MIR compiler pipeline, general register allocation, full borrowing and ownership rules, arenas, closures, complete language features, self-hosting, and other CPU targets still need work. Native performance has improved in certain cases, but I wouldn't call Kru generally faster than an optimized C compiler.

That's about it for this upload. It's an older version I'm keeping around while I continue working on the newer compiler. There are still plenty of problems to sort out, and I'm not calling it done anytime soon.
