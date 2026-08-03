#ifndef KRU_CODEGEN_H
#define KRU_CODEGEN_H


#include <stdio.h>
#include <stdbool.h>

#include "ast.h"



typedef struct
{
    FILE* output;

    int indent_level;

    bool has_error;

} CodegenContext;



int codegen_generate(
    ASTNode* root_node,
    const char* output_filepath
    );



void codegen_context_init(
    CodegenContext* ctx,
    FILE* out_stream
    );



void codegen_context_destroy(
    CodegenContext* ctx
    );



void codegen_visit_node(
    CodegenContext* ctx,
    ASTNode* node
    );



void codegen_emit_indent(
    CodegenContext* ctx
    );



#endif