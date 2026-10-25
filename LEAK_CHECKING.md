# Leak checking and current limits

`make check` keeps `detect_leaks=1` by default. On this hosted ptrace-based
executor, LeakSanitizer aborts before analysis (cannot inspect /proc tasks).
That is a blocked check, not a clean result. It was not disabled for this pass.
Run the full command on an ordinary Linux host to obtain LSan coverage.

`make check-allocations` independently builds the compiler with UBSan and GNU
linker allocation wrappers. Compiler-owned malloc/calloc/realloc/strdup/strndup
allocations must be freed before process exit. A self-test deliberately leaks
memory to verify detection. A shared findings log prevents negative compilation
tests from accidentally accepting a leak exit status as an expected rejection.
This detects outstanding tracked allocations; it is not a reachability scanner
or a replacement for ASan/LSan, and it does not track libc-internal allocations.

For generated native programs:

```sh
./build/kru0 --check-heap --run-native program.kru
```

The runtime counts successful allocation mappings and successful releases. At
normal main return, a nonzero balance prints a diagnostic and exits with 99;
otherwise the program's exit status is preserved. This covers the native runtime
allocator, not arbitrary unsafe syscalls, abrupt termination, descriptors or all
possible corruption. The entry stub reserves a private stack word; R12 is fixed
in the offline numeric-runtime compilation so C helpers cannot repurpose it.
The count is maintained in ordinary native builds, and reporting is opt-in.
No external runtime library or subprocess is involved.

Tests include a deliberate raw leak, normal raw freeing, 10,000 owned String
iterations, 100 owned file reads and invalid-UTF-8 cleanup. Prefer
`kru_string::concat` (returns String). Both `kru_io::file_read` and
`kru_io::read_to_string` now return Result<String,IoError>, transferring the
buffer without copying. Tests also cover repeated 128 KiB reads, embedded zero
bytes, empty files, ownership transfer after closing the handle and early returns.

Migration from the former library borrowed result:

```kru
let owned := kru_io::file_read(mut ref handle)?
let text: str := owned.as_str()
```

Keep `owned` alive for every use of `text`; String drops at scope exit. This
removes the allocated-borrowed return from the public kru_io library API.

## Bootstrap compatibility ownership

Legacy unqualified `str_concat` and `file_read` return manually owned buffers.
Release the exact returned value once with `str_free(value)`, after every use
of it and all borrowed aliases or slices. Empty concatenations, empty reads and
failed reads also return releasable buffers. Never pass a literal, an args_get
view, a substring, or a String.as_str() view to str_free. Like mem_free, this is
an unmanaged compatibility operation: lifetime misuse is not statically checked.

```kru
let text := str_concat("hello", " world")
pr(text)
str_free(text)
```

Intermediate results require their own releases; a nested concatenation does
not consume its arguments. Prefer kru_string::concat and kru_io::file_read for
scope-based ownership instead. Old callers that omit str_free still leak.
The updated Stage 4, Stage 5 and native string/runtime fixtures release their
buffers. A 10,000-iteration regression checks the native heap balance and uses
allocation wrappers on generated C; Stage 4/5 run under both checks as well.
The former cumulative C leak warning was removed because it counted allocations
without accounting for releases; it was not a leak detector.

## Open work

Automatic ownership migration of the bootstrap signatures, complete borrowed
view provenance and misuse rejection remain unfinished. Match payload method
inference uses concrete variant field types and restores bindings between arms.
No claim is made that arbitrary user programs or the complete implementation are
leak-free. Current passing allocation tests are evidence for their exercised paths.
