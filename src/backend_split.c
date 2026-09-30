#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/backend_split.h"

/*
    ============================================================
     Top-level chunker
    ============================================================

    Splits a C translation unit into top-level "chunks": each chunk
    is either a bodiless declaration (typedef, struct/enum, global
    var, #include, forward declaration, function prototype -- ends
    at a top-level ';') or a function definition (signature followed
    by a brace-matched { ... } body).

    For a function-definition chunk, sig_len marks where the body
    starts within the chunk (text[0..sig_len) is the signature,
    text[sig_len..len) is "{ ... }"). For a bodiless declaration,
    sig_len == len -- the whole chunk is "signature".

    Comments and pure whitespace between chunks are dropped rather
    than preserved -- they don't affect what compiles, and the
    original, unsplit output_path remains on disk as the
    human-readable artifact regardless of whether a split happens.
*/

typedef struct
{
    size_t start;
    size_t len;
    size_t sig_len;
    int is_definition;
} chunk_t;

// Skip whitespace and comments sitting between chunks.
static size_t skip_trivia(
    const char* s,
    size_t i,
    size_t n
    )
{
    for(;;)
    {
        while(i < n && (s[i] == ' ' || s[i] == '\t' ||
                        s[i] == '\r' || s[i] == '\n'))
            i++;

        if(i + 1 < n && s[i] == '/' && s[i + 1] == '/')
        {
            i += 2;
            while(i < n && s[i] != '\n')
                i++;
            continue;
        }

        if(i + 1 < n && s[i] == '/' && s[i + 1] == '*')
        {
            i += 2;
            while(i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
                i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }

        return i;
    }
}

/*
    Parses the file into top-level chunks. Returns the chunk array
    (caller frees) and sets *out_count, or returns NULL if the file
    doesn't brace-match cleanly (defensive -- shouldn't happen for
    codegen's own output, but a scanner bug or unexpected construct
    should fail safe rather than emit something silently wrong).
*/
static chunk_t* chunk_file(
    const char* s,
    size_t n,
    size_t* out_count
    )
{
    size_t cap = 64;
    size_t count = 0;
    chunk_t* chunks = malloc(cap * sizeof(chunk_t));

    if(!chunks)
        return NULL;

    size_t i = skip_trivia(s, 0, n);

    while(i < n)
    {
        size_t chunk_start = i;

        // Preprocessor directive: consume the whole line (with
        // backslash-newline continuation support).
        if(s[i] == '#')
        {
            while(i < n)
            {
                if(s[i] == '\n')
                {
                    if(i > 0 && s[i - 1] == '\\')
                    {
                        i++;
                        continue;
                    }
                    i++;
                    break;
                }
                i++;
            }

            if(count == cap)
            {
                cap *= 2;
                chunk_t* grown = realloc(chunks, cap * sizeof(chunk_t));
                if(!grown) { free(chunks); return NULL; }
                chunks = grown;
            }

            chunks[count].start = chunk_start;
            chunks[count].len = i - chunk_start;
            chunks[count].sig_len = i - chunk_start;
            chunks[count].is_definition = 0;
            count++;

            i = skip_trivia(s, i, n);
            continue;
        }

        int depth = 0;
        int found_body = 0;
        int is_function_body = 0;
        size_t sig_len = 0;
        int ok = 1;

        while(i < n)
        {
            char c = s[i];

            if(c == '/' && i + 1 < n && s[i + 1] == '/')
            {
                i += 2;
                while(i < n && s[i] != '\n')
                    i++;
                continue;
            }

            if(c == '/' && i + 1 < n && s[i + 1] == '*')
            {
                i += 2;
                while(i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
                    i++;
                i = (i + 1 < n) ? i + 2 : n;
                continue;
            }

            if(c == '"')
            {
                i++;
                while(i < n && s[i] != '"')
                {
                    if(s[i] == '\\' && i + 1 < n)
                        i++;
                    i++;
                }
                if(i < n) i++;
                continue;
            }

            if(c == '\'')
            {
                i++;
                while(i < n && s[i] != '\'')
                {
                    if(s[i] == '\\' && i + 1 < n)
                        i++;
                    i++;
                }
                if(i < n) i++;
                continue;
            }

            if(c == '{')
            {
                if(depth == 0 && !found_body)
                {
                    sig_len = i - chunk_start;
                    found_body = 1;

                    // A function signature's parameter list always
                    // ends in ')' immediately before the body brace;
                    // a struct/union/enum body never does (it's just
                    // the tag/name, or nothing). That's a reliable
                    // way to tell "this brace opens a function" from
                    // "this brace opens a type" without needing to
                    // recognize every possible type keyword.
                    size_t k = i;
                    while(k > chunk_start)
                    {
                        k--;
                        if(!(s[k] == ' ' || s[k] == '\t' ||
                             s[k] == '\n' || s[k] == '\r'))
                            break;
                    }
                    is_function_body = (k < i && s[k] == ')');
                }
                depth++;
                i++;
                continue;
            }

            if(c == '}')
            {
                depth--;
                i++;

                if(depth < 0) { ok = 0; break; }

                if(depth == 0 && found_body)
                {
                    if(is_function_body)
                        break; // function ends right at its '}'

                    // Type body (struct/union/enum, possibly
                    // typedef'd): C requires a top-level ';' to
                    // terminate the declaration, optionally preceded
                    // by a declarator list (e.g. "} User;"). Don't
                    // end the chunk here -- keep scanning; the
                    // existing depth==0 ';' rule below will close it
                    // out correctly, trailing declarator and all.
                }

                continue;
            }

            if(c == ';' && depth == 0)
            {
                i++;
                break;
            }

            i++;
        }

        if(!ok)
        {
            free(chunks);
            return NULL;
        }

        size_t chunk_end = i;

        // sig_len covers the whole chunk for anything that isn't a
        // true function body (plain declarations, and struct/union
        // /enum/typedef type bodies) -- those are always shared, so
        // their entire text is "signature" for safety-check purposes.
        if(!found_body || !is_function_body)
            sig_len = chunk_end - chunk_start;

        if(chunk_end == chunk_start)
        {
            // No progress made (malformed trailing content) --
            // bail rather than loop forever.
            free(chunks);
            return NULL;
        }

        if(count == cap)
        {
            cap *= 2;
            chunk_t* grown = realloc(chunks, cap * sizeof(chunk_t));
            if(!grown) { free(chunks); return NULL; }
            chunks = grown;
        }

        chunks[count].start = chunk_start;
        chunks[count].len = chunk_end - chunk_start;
        chunks[count].sig_len = sig_len;
        chunks[count].is_definition = found_body && is_function_body;
        count++;

        i = skip_trivia(s, chunk_end, n);
    }

    *out_count = count;
    return chunks;
}

static int contains_wide(
    const char* s,
    size_t len
    )
{
    return memmem(s, len, "__int128", 8) != NULL;
}

static char* read_whole_file(
    const char* path,
    size_t* out_len
    )
{
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;

    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if(size < 0) { fclose(f); return NULL; }
    rewind(f);

    char* buf = malloc((size_t)size + 1);
    if(!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if(got != (size_t)size) { free(buf); return NULL; }

    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

static int write_chunks(
    const char* path,
    const char* src,
    chunk_t* chunks,
    size_t count,
    const int* include_mask
    )
{
    FILE* f = fopen(path, "wb");
    if(!f) return -1;

    for(size_t idx = 0; idx < count; idx++)
    {
        if(!include_mask[idx])
            continue;

        fwrite(src + chunks[idx].start, 1, chunks[idx].len, f);
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

static int run(const char* cmd)
{
    return system(cmd);
}

int backend_split_try(
    const char* output_path,
    const char* bin_path,
    const char* tcc_cmd,
    const char* cc_cmd
    )
{
    size_t len = 0;
    char* src = read_whole_file(output_path, &len);
    if(!src)
        return 1;

    size_t count = 0;
    chunk_t* chunks = chunk_file(src, len, &count);
    if(!chunks)
    {
        free(src);
        return 1;
    }

    // Safety precondition: __int128 must never appear in any
    // chunk's *signature* portion (a bodiless declaration's whole
    // text counts as its signature). If it only ever shows up
    // inside function bodies, no narrow-classified function can
    // possibly be handed a __int128 value through a parameter,
    // return, global, or type -- those are exactly the places a
    // signature would have to mention it. That makes the split
    // trivially safe: no call-graph analysis needed.
    for(size_t idx = 0; idx < count; idx++)
    {
        if(contains_wide(src + chunks[idx].start, chunks[idx].sig_len))
        {
            free(chunks);
            free(src);
            return 1;
        }
    }

    int* is_wide = calloc(count, sizeof(int));
    if(!is_wide) { free(chunks); free(src); return 1; }

    size_t wide_fn_count = 0;
    size_t narrow_fn_count = 0;

    for(size_t idx = 0; idx < count; idx++)
    {
        if(!chunks[idx].is_definition)
            continue; // shared, goes in both files

        size_t body_off = chunks[idx].sig_len;
        size_t body_len = chunks[idx].len - body_off;

        if(contains_wide(src + chunks[idx].start + body_off, body_len))
        {
            is_wide[idx] = 1;
            wide_fn_count++;
        }
        else
        {
            narrow_fn_count++;
        }
    }

    // No benefit to splitting if everything (or nothing) is wide --
    // let the caller's existing single-file logic handle it.
    if(wide_fn_count == 0 || narrow_fn_count == 0)
    {
        free(is_wide);
        free(chunks);
        free(src);
        return 1;
    }

    int* in_narrow = malloc(count * sizeof(int));
    int* in_wide = malloc(count * sizeof(int));
    if(!in_narrow || !in_wide)
    {
        free(in_narrow); free(in_wide);
        free(is_wide); free(chunks); free(src);
        return 1;
    }

    for(size_t idx = 0; idx < count; idx++)
    {
        if(!chunks[idx].is_definition)
        {
            in_narrow[idx] = 1;
            in_wide[idx] = 1;
        }
        else if(is_wide[idx])
        {
            in_narrow[idx] = 0;
            in_wide[idx] = 1;
        }
        else
        {
            in_narrow[idx] = 1;
            in_wide[idx] = 0;
        }
    }

    char narrow_path[1200];
    char wide_path[1200];
    char narrow_obj[1200];
    char wide_obj[1200];

    snprintf(narrow_path, sizeof(narrow_path), "%s.narrow.c", output_path);
    snprintf(wide_path, sizeof(wide_path), "%s.wide.c", output_path);
    snprintf(narrow_obj, sizeof(narrow_obj), "%s.narrow.o", output_path);
    snprintf(wide_obj, sizeof(wide_obj), "%s.wide.o", output_path);

    int write_ok =
        (write_chunks(narrow_path, src, chunks, count, in_narrow) == 0) &&
        (write_chunks(wide_path, src, chunks, count, in_wide) == 0);

    free(in_narrow);
    free(in_wide);
    free(is_wide);
    free(chunks);
    free(src);

    if(!write_ok)
        return -1;

    char cmd[4096];

    snprintf(
        cmd, sizeof(cmd),
        "%s -c \"%s\" -o \"%s\"",
        tcc_cmd, narrow_path, narrow_obj
        );
    if(run(cmd) != 0)
        return -1;

    snprintf(
        cmd, sizeof(cmd),
        "%s -c \"%s\" -o \"%s\"",
        cc_cmd, wide_path, wide_obj
        );
    if(run(cmd) != 0)
        return -1;

    // Link with cc (not tcc): the wide object may need libgcc's
    // 128-bit divide/modulo helpers (__divti3 etc.), which cc's
    // driver pulls in automatically and tcc doesn't know about.
    snprintf(
        cmd, sizeof(cmd),
        "%s -Wl,-z,noexecstack \"%s\" \"%s\" -o \"%s\"",
        cc_cmd, narrow_obj, wide_obj, bin_path
        );
    if(run(cmd) != 0)
        return -1;

    return 0;
}
