# Snapshot check — October 25, 2026

This document describes what was verified when the older archive `Kru-native-perf-2026-10-07(8).zip` was prepared for public upload. Files in the source, include, library, test, and tool directories are not modified.

**Build:** `make` succeeded with a GNU11 compiler on Linux x86-64.

**Test result:** `make test` stopped with exit status 2. The log contained 220 lines starting with `PASS` before the failure. These are individual log messages, not necessarily 220 unique tests. No full test-suite or sanitizer-suite pass is claimed.

The failure occurs in `tools/test_bootstrap_string_cleanup.sh`, where the `--emit-c` path generates code similar to:

```c
int64_t b = __kru_str_concat(a, "ghi");
str_free(b);
```

`__kru_str_concat` returns a pointer, so the generated program's compiler complains about incompatible conversions and pointer arguments. The native portion of that script ran before the C compilation failed.

Some individual native test scripts completed before this failure. This is useful evidence, but it does not prove full Codex compliance, absence of leaks, or universal correctness.

**Archival note:** Although the original ZIP filename mentions October 7, documents inside include October 8 updates and measurements. The exact creation instant of the source snapshot cannot be independently established from that filename.
