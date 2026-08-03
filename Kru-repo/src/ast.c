#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/ast.h"



ASTNode* ast_create(
    ASTNodeType type
    )
{
    ASTNode* node =
        calloc(
            1,
            sizeof(ASTNode)
            );


    if(!node)
        return NULL;


    node->type = type;

    node->op = TOKEN_EOF;


    return node;
}





void ast_add_child(
    ASTNode* parent,
    ASTNode* child
    )
{
    if(!parent || !child)
        return;


    if(parent->child_count >= parent->child_capacity)
    {
        uint32_t new_capacity =
            parent->child_capacity == 0
                ? 4
                : parent->child_capacity * 2;


        ASTNode** children =
            realloc(
                parent->children,
                sizeof(ASTNode*) * new_capacity
                );


        if(!children)
            return;


        parent->children = children;
        parent->child_capacity = new_capacity;
    }


    parent->children[
        parent->child_count++
    ] = child;
}





static char* ast_copy_string(
    const char* source,
    uint32_t length
    )
{
    if(!source)
        return NULL;


    char* copy =
        malloc(
            length + 1
            );


    if(!copy)
        return NULL;


    memcpy(
        copy,
        source,
        length
        );


    copy[length] = '\0';


    return copy;
}





void ast_set_name(
    ASTNode* node,
    const char* name,
    uint32_t length
    )
{
    if(!node || !name)
        return;


    char* copy =
        ast_copy_string(
            name,
            length
            );


    if(!copy)
        return;


    free(
        (void*)node->name
        );


    node->name = copy;

    node->name_len = length;
}





void ast_set_string(
    ASTNode* node,
    const char* value,
    uint32_t length
    )
{
    if(!node || !value)
        return;


    char* copy =
        ast_copy_string(
            value,
            length
            );


    if(!copy)
        return;


    free(
        (void*)node->string_val
        );


    node->string_val = copy;

    node->string_len = length;
}





static const char* ast_type_name(
    ASTNodeType type
    )
{
    switch(type)
    {
    case AST_PROGRAM:
        return "PROGRAM";

    case AST_FUNCTION:
        return "FUNCTION";

    case AST_PARAM:
        return "PARAM";


    case AST_BLOCK:
        return "BLOCK";


    case AST_LET_STMT:
        return "LET";

    case AST_VAR_STMT:
        return "VAR";

    case AST_ASSIGN_STMT:
        return "ASSIGN";


    case AST_RET_STMT:
        return "RET";


    case AST_IF_STMT:
        return "IF";

    case AST_ELSE_STMT:
        return "ELSE";

    case AST_WHILE_STMT:
        return "WHILE";

    case AST_LOOP_STMT:
        return "LOOP";


    case AST_BREAK_STMT:
        return "BREAK";

    case AST_CONTINUE_STMT:
        return "CONTINUE";


    case AST_EXPR_STMT:
        return "EXPR";


    case AST_INT_LIT:
        return "INT";

    case AST_FLOAT_LIT:
        return "FLOAT";

    case AST_STRING_LIT:
        return "STRING";

    case AST_BOOL_LIT:
        return "BOOL";

    case AST_CHAR_LIT:
        return "CHAR";


    case AST_IDENT:
        return "IDENT";


    case AST_BINARY_EXPR:
        return "BINARY";

    case AST_UNARY_EXPR:
        return "UNARY";


    case AST_CALL_EXPR:
        return "CALL";


    case AST_REF_EXPR:
        return "REF";

    case AST_DEREF_EXPR:
        return "DEREF";


    case AST_FIELD_EXPR:
        return "FIELD";


    case AST_CONST_DECL:
        return "CONST";

    case AST_TYPE_ALIAS:
        return "TYPE_ALIAS";

    case AST_STRUCT_DECL:
        return "STRUCT";

    case AST_STRUCT_FIELD:
        return "FIELD_DECL";

    case AST_ENUM_DECL:
        return "ENUM";

    case AST_ENUM_VARIANT:
        return "VARIANT";


    case AST_TYPE:
        return "TYPE";

    case AST_BLOCK_EXPR:
        return "BLOCK_EXPR";

    case AST_MATCH_EXPR:
        return "MATCH";

    case AST_MATCH_ARM:
        return "MATCH_ARM";

    case AST_ARENA_STMT:
        return "ARENA";


    default:
        return "UNKNOWN";
    }
}





void ast_print(
    ASTNode* node,
    int indent
    )
{
    if(!node)
        return;


    for(int i = 0; i < indent; i++)
        printf("  ");


    printf(
        "%s",
        ast_type_name(node->type)
        );


    if(node->name)
    {
        printf(
            " %s",
            node->name
            );
    }


    printf("\n");


    for(uint32_t i = 0;
         i < node->child_count;
         i++)
    {
        ast_print(
            node->children[i],
            indent + 1
            );
    }
}





void ast_free(
    ASTNode* node
    )
{
    if(!node)
        return;

    /*
        Iterative free using an explicit heap worklist instead of
        recursion. A recursive walk uses one native stack frame per
        AST depth level, so a deeply nested tree (matching parser
        nesting, or a struct-of-arrays-of-structs style program)
        can exhaust the stack the same way unbounded parsing can.
        This keeps native stack usage constant regardless of tree
        depth or size.
    */

    uint32_t capacity = 256;
    uint32_t count = 0;

    ASTNode** stack =
        malloc(
            sizeof(ASTNode*) * capacity
            );

    if(!stack)
    {
        /*
            Extremely low memory: fall back to leaking rather than
            risking a recursive crash. Freeing the AST is a one-shot
            operation right before process exit in every current
            caller, so this only matters under real OOM pressure.
        */
        return;
    }

    stack[count++] = node;

    while(count > 0)
    {
        ASTNode* current =
            stack[--count];

        if(!current)
            continue;

        /*
            Push children and type_node onto the worklist before
            freeing this node's own buffers.
        */

        uint32_t needed =
            count + current->child_count + 1;

        if(needed > capacity)
        {
            while(capacity < needed)
                capacity *= 2;

            ASTNode** grown =
                realloc(
                    stack,
                    sizeof(ASTNode*) * capacity
                    );

            if(!grown)
            {
                /*
                    Can't grow the worklist further; stop freeing
                    rather than risk corrupting memory. Leaks the
                    remainder, but that's strictly safer than UB.
                */
                free(stack);
                return;
            }

            stack = grown;
        }

        for(uint32_t i = 0;
             i < current->child_count;
             i++)
        {
            stack[count++] = current->children[i];
        }

        stack[count++] = current->type_node;

        free(
            current->children
            );

        free(
            (void*)current->name
            );

        free(
            (void*)current->string_val
            );

        free(
            current
            );
    }

    free(stack);
}