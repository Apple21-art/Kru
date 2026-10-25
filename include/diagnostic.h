#ifndef KRU_DIAGNOSTIC_H
#define KRU_DIAGNOSTIC_H

#include <stdint.h>


/*
    Task E: shared diagnostic-printing mechanism implementing the
    Codex's required 7-part diagnostic structure (Ch. 77):

        1. severity        ("error", "warning", ...)
        2. code             (e.g. "K1004")
        3. message           (short one-line description)
        4. location           (line:column)
        5. source context       (the offending source line, "NNN | ...")
        6. caret                 (^^^^^ under the offending span)
        7. explanation + suggested fix

    Scoped implementation, matching the overhaul work order: this is
    the mechanism end-to-end, wired up fully for one proof case (the
    immutable-binding-assignment diagnostic, K1004, in sema.c) rather
    than a full rewrite of every diagnostic call site in the codebase.
    Every other call site keeps its old one-line
    "[kru error] CODE: message at LINE:COL" format for now, each
    marked with a TODO-explanation comment so the remaining
    work is visible rather than silently incomplete.

    KNOWN SCOPE LIMITATION (documented, not silently glossed over):
    the Codex's own worked example shows source context spanning
    *two* lines when the error references something declared on a
    different line than where the error itself occurs (the `let`
    declaration on one line, the illegal assignment on the next).
    This implementation only prints the single line the diagnostic's
    own line:column point to. Showing the declaration-site line too
    would require every call site to additionally pass a *second*
    line:column for "where was this thing declared", which none of
    the current call sites track today (sema.c's Symbol struct has
    no line/column field). That's real, separate plumbing work, not
    a diagnostic-printer change -- flagged as a follow-up, not
    attempted here.
*/

typedef struct
{
    const char* severity;          /* e.g. "error"; NULL defaults to "error" */
    const char* code;              /* e.g. "K1004" (required) */
    const char* message;           /* short one-line description (required) */

    uint32_t line;                 /* 1-based source line */
    uint32_t column;               /* 1-based source column */
    uint32_t underline_length;     /* caret span width; 0 defaults to 1 */

    const char* explanation;       /* optional; NULL to omit */
    const char* suggested_fix;     /* optional; NULL to omit (printed as "Help: ...") */

} Diagnostic;


/*
    Called once, early, by main.c right after the source file is
    read into memory -- the same buffer read_file() already fully
    buffers before any compiler stage runs. Every later stage
    (parser, sema, codegen) that wants to print a rich diagnostic
    calls diagnostic_print() without needing to re-read the file
    from disk or thread the buffer through every function signature
    in the codebase.

    Passing NULL disables source-context printing (diagnostic_print
    falls back to just the one-line severity/code/message/location),
    which is also what happens automatically if this is never called.
*/
void diagnostic_set_source(
    const char* source
    );


/*
    Prints a diagnostic in the Codex's 7-part format to stderr.
    Fields left NULL/0 on the Diagnostic are simply omitted from the
    output (this function never invents placeholder text).
*/
void diagnostic_print(
    const Diagnostic* diag
    );


#endif
