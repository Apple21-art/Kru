#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/diagnostic.h"


static const char* diagnostic_source = NULL;
/* Source buffers outlive module parsing and AST rewriting. ASTs retain stable
   IDs, never borrowed pointers into a module loader's temporary buffer. */
typedef struct {char* path;char* text;} DiagnosticSource;
static DiagnosticSource* sources;
static uint32_t source_count,active_source;
static int cleanup_registered;
static void release_sources(void)
{
    for(uint32_t i=0;i<source_count;i++){free(sources[i].path);free(sources[i].text);}
    free(sources);sources=NULL;source_count=active_source=0;
}
uint32_t diagnostic_add_source(const char* path,const char* source)
{
    char* filename=strdup(path);char* text=strdup(source);
    if(!filename||!text){free(filename);free(text);return 0;}
    DiagnosticSource* grown=realloc(sources,(size_t)(source_count+1)*sizeof(*grown));
    if(!grown){free(filename);free(text);return 0;}
    sources=grown;sources[source_count++]=(DiagnosticSource){filename,text};
    if(!cleanup_registered){atexit(release_sources);cleanup_registered=1;}
    active_source=source_count;return active_source;
}
uint32_t diagnostic_current_source(void){return active_source;}
void diagnostic_use_source(uint32_t id){active_source=id<=source_count?id:0;}



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

    uint32_t id=diag->source_id?diag->source_id:active_source;
    const DiagnosticSource* origin=id&&id<=source_count?&sources[id-1]:NULL;
    if(origin)fprintf(stderr,"  --> %s:%u:%u (line %u, column %u)\n",origin->path,diag->line,diag->column,diag->line,diag->column);
    else fprintf(stderr,"  --> line %u, column %u\n",diag->line,diag->column);


    /*
        Part 5-6: source context line and caret, if we have a source
        buffer and a usable line number.
    */

    uint32_t line_len = 0;

    const char* line_text =
        find_source_line(
            origin?origin->text:diagnostic_source,
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
