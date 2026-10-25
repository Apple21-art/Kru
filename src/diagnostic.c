#include <stdio.h>
#include <string.h>

#include "../include/diagnostic.h"


static const char* diagnostic_source = NULL;


void diagnostic_set_source(
    const char* source
    )
{
    diagnostic_source = source;
}



/*
    Finds the start of 1-based line `line_number` in `source`, and
    writes its length (not including the trailing newline, if any)
    to *out_len. Returns NULL if the source doesn't have that many
    lines (out_len is left untouched in that case).
*/
static const char* find_source_line(
    const char* source,
    uint32_t line_number,
    uint32_t* out_len
    )
{
    if(!source || line_number == 0)
        return NULL;

    const char* cur = source;
    uint32_t current_line = 1;

    while(current_line < line_number)
    {
        const char* newline =
            strchr(cur, '\n');

        if(!newline)
            return NULL;

        cur = newline + 1;
        current_line++;
    }


    const char* line_end =
        strchr(cur, '\n');

    uint32_t len =
        line_end
            ? (uint32_t)(line_end - cur)
            : (uint32_t)strlen(cur);


    if(out_len)
        *out_len = len;


    return cur;
}



void diagnostic_print(
    const Diagnostic* diag
    )
{
    if(!diag || !diag->code || !diag->message)
        return;

    const char* severity =
        diag->severity
            ? diag->severity
            : "error";


    /*
        Part 1-4: severity, code, message, location.
    */

    fprintf(
        stderr,
        "%s[%s]: %s\n",
        severity,
        diag->code,
        diag->message
        );

    fprintf(
        stderr,
        "  --> line %u, column %u\n",
        diag->line,
        diag->column
        );


    /*
        Part 5-6: source context line and caret, if we have a source
        buffer and a usable line number.
    */

    uint32_t line_len = 0;

    const char* line_text =
        find_source_line(
            diagnostic_source,
            diag->line,
            &line_len
            );

    if(line_text)
    {
        fprintf(
            stderr,
            "\n%5u | %.*s\n",
            diag->line,
            line_len,
            line_text
            );

        fprintf(
            stderr,
            "      | "
            );

        /*
            Column is 1-based; print (column - 1) leading spaces
            before the caret span so it lines up under the offending
            token. A column of 0 (not expected, but defend against
            it) is treated as column 1.
        */

        uint32_t lead =
            (diag->column > 0)
                ? (diag->column - 1)
                : 0;

        for(uint32_t i = 0; i < lead; i++)
            fputc(' ', stderr);


        uint32_t caret_len =
            diag->underline_length
                ? diag->underline_length
                : 1;

        for(uint32_t i = 0; i < caret_len; i++)
            fputc('^', stderr);

        fprintf(stderr, "\n");
    }


    /*
        Part 7: explanation and suggested fix, both optional.
    */

    if(diag->explanation)
        fprintf(stderr, "%s\n", diag->explanation);

    if(diag->suggested_fix)
        fprintf(stderr, "\nHelp: %s\n", diag->suggested_fix);


    fprintf(stderr, "\n");
}
