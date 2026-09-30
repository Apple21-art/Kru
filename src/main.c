#define _GNU_SOURCE // for memmem()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>


#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/sema.h"
#include "../include/codegen.h"
#include "../include/ast.h"
#include "../include/diagnostic.h"
#include "../include/backend_split.h"
#include "../include/native_backend.h"



static char* read_file(
    const char* path,
    size_t* source_bytes
    )
{
    struct stat st;

    if(stat(path, &st) != 0)
        return NULL;


    if(!S_ISREG(st.st_mode))
    {
        /*
            Refuse directories, FIFOs, devices, sockets, etc. On
            some platforms fseek/ftell report a bogus size (or even
            LONG_MAX) for these, which previously overflowed the
            malloc below into a multi-exabyte request.
        */
        errno = EINVAL;
        return NULL;
    }


    if(st.st_size < 0 || (uintmax_t)st.st_size >= SIZE_MAX)
    {
        errno = EFBIG;
        return NULL;
    }


    FILE* file =
        fopen(
            path,
            "rb"
            );


    if(!file)
        return NULL;


    size_t size =
        (size_t)st.st_size;


    char* buffer =
        malloc(
            size + 1
            );


    if(!buffer)
    {
        fclose(file);
        errno = ENOMEM;
        return NULL;
    }



    errno = 0;
    size_t read =
        fread(
            buffer,
            1,
            size,
            file
            );


    if(read != size)
    {
        int read_error = errno ? errno : EIO;
        free(buffer);
        fclose(file);
        errno = read_error;
        return NULL;
    }


    buffer[size] = '\0';
    *source_bytes = size;

    fclose(file);


    return buffer;
}



/*
    Cheap pre-scan of the generated C source for `__int128` /
    `unsigned __int128` before picking a backend compiler. tcc can't
    build those at all (see the backend-selection comment in main()
    below), so scanning up front means an i128/u128 program goes
    straight to a standard compiler in one shot, instead of paying
    for a doomed tcc invocation and then retrying. Substring match is
    intentionally simple: codegen_c_type() is the only place that
    emits this text (see codegen.c), so there's no other source of
    false positives to worry about here.
*/
static int file_needs_wide_int(
    const char* path
    )
{
    FILE* file = fopen(path, "rb");

    if(!file)
        return 0; // Can't tell -- let the normal compile attempt find out.

    char chunk[4096];
    size_t n;
    int found = 0;

    // Overlap consecutive reads by the needle length minus one so a
    // match straddling a chunk boundary isn't missed.
    const char* needle = "__int128";
    const size_t needle_len = 8;
    char carry[16] = {0};
    size_t carry_len = 0;

    while((n = fread(chunk, 1, sizeof(chunk), file)) > 0)
    {
        char buf[4096 + 16];
        memcpy(buf, carry, carry_len);
        memcpy(buf + carry_len, chunk, n);
        size_t buf_len = carry_len + n;

        if(memmem(buf, buf_len, needle, needle_len) != NULL)
        {
            found = 1;
            break;
        }

        carry_len = (buf_len < needle_len - 1) ? buf_len : needle_len - 1;
        memcpy(carry, buf + (buf_len - carry_len), carry_len);
    }

    fclose(file);

    return found;
}



typedef enum
{
    KRU_MODE_RUN,
    KRU_MODE_EMIT_C,
    KRU_MODE_EMIT_NATIVE,
    KRU_MODE_RUN_NATIVE
} KruDriverMode;

static int native_mode(KruDriverMode mode)
{
    return mode == KRU_MODE_EMIT_NATIVE || mode == KRU_MODE_RUN_NATIVE;
}

static int execute_native(const char* path)
{
    pid_t child = fork();
    if(child < 0)
    {
        perror("[kru] failed to start native program");
        return 1;
    }
    if(child == 0)
    {
        char* target_argv[] = {(char*)path, NULL};
        execv(path, target_argv);
        fprintf(stderr, "[kru] cannot execute '%s': %s\n", path, strerror(errno));
        _exit(126);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while(waited < 0 && errno == EINTR);
    if(waited < 0)
    {
        perror("[kru] failed to wait for native program");
        return 1;
    }
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    if(WIFSIGNALED(status))
    {
        fprintf(stderr, "[kru] native program terminated by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

typedef struct
{
    int enabled;
    size_t source_bytes;
    double started, read_done, parse_done, sema_done, generate_done, cleanup_done;
} KruTiming;

static double monotonic_seconds(int enabled)
{
    if(!enabled) return 0.0;
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        perror("[kru] monotonic clock");
        exit(1);
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void print_timing(const KruTiming* t, KruDriverMode mode, double compile_done)
{
    if(!t->enabled) return;
    double parse_seconds = t->parse_done - t->read_done;
    double total_seconds = compile_done - t->started;
    fprintf(stderr, "\n[kru] Compile timing (wall clock)\n");
    fprintf(stderr, "  Source bytes       %zu\n", t->source_bytes);
    fprintf(stderr, "  Read source        %9.3f ms\n", (t->read_done - t->started) * 1000.0);
    fprintf(stderr, "  Lex + parse        %9.3f ms\n", parse_seconds * 1000.0);
    fprintf(stderr, "  Semantic checks    %9.3f ms\n", (t->sema_done - t->parse_done) * 1000.0);
    fprintf(stderr, "  %-18s %9.3f ms\n", native_mode(mode) ? "Generate native" : "Generate C",
            (t->generate_done - t->sema_done) * 1000.0);
    fprintf(stderr, "  Cleanup            %9.3f ms\n", (t->cleanup_done - t->generate_done) * 1000.0);
    if(mode == KRU_MODE_RUN)
        fprintf(stderr, "  Host C build       %9.3f ms\n", (compile_done - t->cleanup_done) * 1000.0);
    fprintf(stderr, "  Total compile      %9.3f ms\n", total_seconds * 1000.0);
    if(parse_seconds > 0)
        fprintf(stderr, "  Parse throughput   %9.2f MiB/s (%.0f bytes/s)\n",
                (double)t->source_bytes / parse_seconds / 1048576.0,
                (double)t->source_bytes / parse_seconds);
    fprintf(stderr, "  Program runtime excluded. Lexer and parser are measured together.\n\n");
}


static void print_usage(const char* argv0)
{
    fprintf(
        stderr,
        "usage: %s [--time] [--run|--emit-c|--emit-native|--run-native] <input.kru> [output]\n"
        "  --run-native  emit native machine code, then execute it\n"
        "  --time  show compiler stage timings; excludes target execution\n",
        argv0
        );
}


int main(
    int argc,
    char** argv
    )
{
    KruDriverMode mode = KRU_MODE_RUN;
    int argi = 1;
    KruTiming timing = {0};
    while(argi < argc && argv[argi][0] == '-')
    {
        const char* option = argv[argi++];
        if(strcmp(option, "--time") == 0) timing.enabled = 1;
        else if(strcmp(option, "--emit-c") == 0) mode = KRU_MODE_EMIT_C;
        else if(strcmp(option, "--emit-native") == 0) mode = KRU_MODE_EMIT_NATIVE;
        else if(strcmp(option, "--run-native") == 0) mode = KRU_MODE_RUN_NATIVE;
        else if(strcmp(option, "--run") == 0) mode = KRU_MODE_RUN;
        else if(strcmp(option, "--") == 0) break;
        else if(strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "[kru] unknown option: %s\n", option);
            print_usage(argv[0]);
            return 1;
        }
    }

    if(argi >= argc)
    {
        fprintf(stderr, "[kru] missing input file\n");
        print_usage(argv[0]);
        return 1;
    }

    const char* source_path = argv[argi++];
    timing.started = monotonic_seconds(timing.enabled);

    char* source =
        read_file(
            source_path,
            &timing.source_bytes
            );
    int source_error = errno;
    timing.read_done = monotonic_seconds(timing.enabled);


    if(!source)
    {
        fprintf(
            stderr,
            "[kru] cannot read '%s': %s\n",
            source_path,
            strerror(source_error)
            );

        return 1;
    }


    /*
        Task E: hand the already-buffered source to the diagnostic
        module once, here, so any later stage (parser, sema, codegen)
        can print rich diagnostics with source context without
        re-reading the file from disk or threading the buffer through
        every function signature in the codebase.
    */

    diagnostic_set_source(source);





    Lexer lexer;


    lexer_init(
        &lexer,
        source
        );





    ASTNode* root =
        parse_program(
            &lexer
            );
    timing.parse_done = monotonic_seconds(timing.enabled);



    if(!root)
    {
        fprintf(
            stderr,
            "[kru] parsing failed\n"
            );

        free(source);

        return 1;
    }




    if(sema_analyze(root) != 0)
    {
        fprintf(
            stderr,
            "[kru] semantic analysis failed\n"
            );

        ast_free(root);
        free(source);
        return 1;
    }
    timing.sema_done = monotonic_seconds(timing.enabled);





    const char* output_path =
        (argi < argc)
            ? argv[argi++]
            : (native_mode(mode) ? "a.out" : "out.c");

    if(argi < argc)
    {
        fprintf(stderr, "[kru] unexpected argument: %s\n", argv[argi]);
        ast_free(root);
        free(source);
        print_usage(argv[0]);
        return 1;
    }


    if(native_mode(mode))
    {
        int native_status = native_backend_emit_x86_64_linux(root, output_path);
        timing.generate_done = monotonic_seconds(timing.enabled);
        ast_free(root);
        free(source);
        timing.cleanup_done = monotonic_seconds(timing.enabled);
        if(native_status != 0) return 1;
        print_timing(&timing, mode, timing.cleanup_done);
        if(mode == KRU_MODE_RUN_NATIVE) return execute_native(output_path);
        return 0;
    }


    if(codegen_generate(
            root,
            output_path
            ) != 0)
    {
        fprintf(
            stderr,
            "[kru] code generation failed\n"
            );


        ast_free(root);

        free(source);

        return 1;
    }
    timing.generate_done = monotonic_seconds(timing.enabled);

    /*
        Free compiler resources before shelling out to the
        system compiler and running the user program.
    */
    ast_free(root);
    free(source);
    timing.cleanup_done = monotonic_seconds(timing.enabled);

    if(mode == KRU_MODE_EMIT_C)
    {
        print_timing(&timing, mode, timing.cleanup_done);
        return 0;
    }


    /*
        ============================================================
         Auto-Compile & Execute Stage (via Host C Compiler)
        ============================================================
    */

    char bin_path[1024];
    snprintf(
        bin_path,
        sizeof(bin_path),
        "%s.bin",
        output_path
        );


    /*
        Backend compiler selection.

        Per PERFORMANCE.md's backend policy, this "auto-compile & run"
        stage is the fast bootstrap/C dev-loop path (not the optimized
        native build path), so it prefers TinyCC (tcc) here: tcc has
        no real optimizer, but its compile times are on the order of
        10x faster than gcc/clang, which is what this stage is for.

        Known gap: tcc doesn't implement __int128/unsigned __int128,
        which is what codegen_c_type() emits for Kru's i128/u128
        (see codegen.c). Rather than trying tcc and discovering that
        the hard way on every 128-bit program, file_needs_wide_int()
        pre-scans the generated C once and routes straight to a
        standard compiler for those -- one compile invocation either
        way, each program landing on the backend that actually works
        for it. A retry-on-failure fallback still runs underneath in
        case some other, unanticipated construct trips up tcc; that
        path just shouldn't fire for i128/u128 anymore.

        Override with the KRU_CC environment variable (e.g.
        `KRU_CC=cc`, `KRU_CC=gcc`, `KRU_CC=tcc`) to pin one backend
        and skip this selection entirely -- e.g. for an optimized
        release build, since tcc's codegen is not competitive with
        gcc/clang -O2 for anything perf-sensitive.
    */
    const char* forced_cc = getenv("KRU_CC");
    const char* have_tcc_env = getenv("KRU_HAVE_TCC"); // test hook only

    int have_tcc =
        have_tcc_env
            ? (strcmp(have_tcc_env, "1") == 0)
            : (system("command -v tcc >/dev/null 2>&1") == 0);

    int needs_wide_int = file_needs_wide_int(output_path);

    char compile_cmd[2048];
    const char* used_cc = NULL;
    int compile_status = 1;

    /*
        Mixed tcc/cc split.

        When the file both needs wide-int support somewhere AND has
        plenty of ordinary functions alongside it, backend_split_try()
        (backend_split.c) re-splits the generated C into a tcc half
        and a cc half and links them, rather than paying the full
        cc -O2 cost for functions that never touch __int128 at all.
        Skipped entirely when KRU_CC pins a specific backend, or when
        tcc isn't available to begin with.
    */
    if(have_tcc && needs_wide_int && !forced_cc)
    {
        int split_status = backend_split_try(
            output_path,
            bin_path,
            "tcc",
            "cc -O2"
            );

        if(split_status == 0)
        {
            fprintf(
                stderr,
                "[kru] split build: tcc for the plain functions, cc "
                "for the i128/u128 ones\n"
                );

            used_cc = "tcc+cc (split)";
            compile_status = 0;
        }

        // split_status == 1: not attempted (unsafe or no benefit --
        // not an error). split_status == -1: attempted and a step
        // failed. Either way, compile_status is still 1 here, so the
        // single-file path below runs as a normal fallback.
    }

    int try_tcc_first =
        compile_status == 0
            ? 0 // split already succeeded -- nothing left to compile
            : (forced_cc
                ? (strcmp(forced_cc, "tcc") == 0)
                : (have_tcc && !needs_wide_int));

    if(try_tcc_first)
    {
        // tcc: no -O flags -- it doesn't have an optimizer to speak
        // of, and passing -O2 just gets silently ignored anyway.
        snprintf(
            compile_cmd,
            sizeof(compile_cmd),
            "tcc -o \"%s\" \"%s\"",
            bin_path,
            output_path
            );

        used_cc = "tcc";
        compile_status = system(compile_cmd);
    }

    // Fall back to a standard compiler when tcc wasn't tried, or
    // when it was tried and failed for some other reason -- but only
    // when the user didn't explicitly pin the backend to tcc via
    // KRU_CC.
    if(compile_status != 0 &&
       !(forced_cc && strcmp(forced_cc, "tcc") == 0))
    {
        if(used_cc != NULL)
        {
            fprintf(
                stderr,
                "[kru] tcc couldn't build this one; "
                "falling back to a standard compiler\n"
                );
        }
        else if(needs_wide_int)
        {
            fprintf(
                stderr,
                "[kru] this program uses i128/u128, which tcc can't "
                "build -- using a standard compiler\n"
                );
        }

        const char* cc = forced_cc ? forced_cc : "cc";
        snprintf(
            compile_cmd,
            sizeof(compile_cmd),
            "%s -O2 \"%s\" -o \"%s\"",
            cc,
            output_path,
            bin_path
            );

        used_cc = cc;
        compile_status = system(compile_cmd);
    }

    if(compile_status != 0)
    {
        fprintf(
            stderr,
            "[kru] host C compilation failed (%s returned %d)\n",
            used_cc ? used_cc : "cc",
            compile_status
            );

        return 1;
    }
    print_timing(&timing, mode, monotonic_seconds(timing.enabled));


    // 2. Execute the compiled binary directly
    //
    // Bug fix: bin_path can be absolute (e.g. the caller passed
    // "/tmp/foo.c" as the output path), in which case blindly
    // prepending "./" produces "..//tmp/foo.c.bin", which a shell
    // resolves as "./tmp/foo.c.bin" -- i.e. *relative to the
    // current directory*, not the absolute path that was actually
    // compiled. That silently runs the wrong file (usually "not
    // found") whenever kru0 is invoked with an absolute output
    // path. Only add the "./" when bin_path is relative; an
    // absolute path is already unambiguous to the shell as-is.
    char run_cmd[2048];
    snprintf(
        run_cmd,
        sizeof(run_cmd),
        (bin_path[0] == '/') ? "\"%s\"" : "./%s",
        bin_path
        );

    int run_status = system(run_cmd);


    // 3. Return the exit code of the target program seamlessly
#if defined(_WIN32)
    return run_status;
#else
    if (WIFEXITED(run_status)) {
        return WEXITSTATUS(run_status);
    }
    return 1; // Abnormal termination (crashed/killed)
#endif
}
