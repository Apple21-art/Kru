# Kru Stage 5 — Test Suite & Stub Modules

## File listing

### Test files
| File | Tests |
|------|-------|
| `stage5_core.kru` | ref/mut ref, arena, raw pointers + null, heap alloc/free/realloc, file I/O with Result/?, CLI args |
| `stage5_strings.kru` | str literals, String buffer, push_str, as_str, len, str_eq |
| `stage5_arrays.kru` | [T; N] arrays, indexing, .len(), ref [T] slices |
| `stage5_errors.kru` | Option<T>, Result<T, E>, ? operator, error propagation chains |
| `stage5_math.kru` | Module with pub functions (imported by stage5_mod_main) |
| `stage5_mod_main.kru` | `use` import, calling imported pub functions |

### Stub module interfaces (wire these to your C builtins)
| File | Maps to |
|------|---------|
| `kru_io.kru` | fopen/fread/fwrite/fclose + str_len/str_eq |
| `kru_env.kru` | argc/argv |
| `kru_mem.kru` | malloc/free/realloc |
| `kru_string.kru` | owned string buffer (String type) |
| `kru_result.kru` | Result<T, E> enum |
| `kru_option.kru` | Option<T> enum |

## Usage

1. Drop all `.kru` files into one folder.
2. Wire the stub modules to your C runtime builtins.
3. Compile and run each test file independently:
   - `stage5_core.kru` — refs, arena, pointers, heap, file I/O, args
   - `stage5_strings.kru` — string operations
   - `stage5_arrays.kru` — arrays and slices
   - `stage5_errors.kru` — Option/Result/error propagation
   - `stage5_mod_main.kru` — module imports (requires stage5_math.kru)

## Expected output

- `stage5_core.kru`: 20, 42, 45, 123, 123, 0, file length, arg count, 0
- `stage5_strings.kru`: 18, 18, 1
- `stage5_arrays.kru`: 4, 10, 40, 4, 20
- `stage5_errors.kru`: 5, 5, -2
- `stage5_mod_main.kru`: 42, 100, 100
