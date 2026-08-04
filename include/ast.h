#ifndef KRU_AST_H
#define KRU_AST_H

#include <stdint.h>
#include <stdbool.h>

#include "token.h"


typedef enum
{
    AST_PROGRAM,

    AST_FUNCTION,
    AST_PARAM,

    AST_BLOCK,

    AST_LET_STMT,
    AST_VAR_STMT,

    AST_ASSIGN_STMT,

    AST_RET_STMT,

    AST_IF_STMT,
    AST_ELSE_STMT,
    AST_WHILE_STMT,
    AST_LOOP_STMT,

    AST_BREAK_STMT,
    AST_CONTINUE_STMT,

    AST_EXPR_STMT,

    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_STRING_LIT,
    AST_BOOL_LIT,
    AST_CHAR_LIT,

    AST_IDENT,

    AST_BINARY_EXPR,
    AST_UNARY_EXPR,

    AST_CALL_EXPR,

    AST_REF_EXPR,
    AST_DEREF_EXPR,

    AST_FIELD_EXPR,

    AST_ARRAY_LIT,
    AST_INDEX_EXPR,

    /*
        Top-level declarations
    */

    AST_CONST_DECL,
    AST_TYPE_ALIAS,
    AST_STRUCT_DECL,
    AST_STRUCT_FIELD,
    AST_ENUM_DECL,
    AST_ENUM_VARIANT,

    AST_TYPE,

    /*
        Stage 3: expressions
    */

    AST_BLOCK_EXPR,
    AST_MATCH_EXPR,
    AST_MATCH_ARM,
    AST_ARENA_STMT

} ASTNodeType;



typedef struct ASTNode ASTNode;


struct ASTNode
{
    ASTNodeType type;


    uint32_t line;
    uint32_t column;


    char* name;
    uint32_t name_len;


    uint64_t int_val; /* widened from int64_t: u64 literals can exceed INT64_MAX */

    double float_val;

    bool bool_val;


    char* string_val;
    uint32_t string_len;


    TokenType op;


    ASTNode* type_node;


    bool is_public;
    bool is_unsafe;
    bool is_secure;


    bool is_mut;
    bool is_ref;

    bool is_struct_lit;


    ASTNode** children;

    uint32_t child_count;

    uint32_t child_capacity;
};



ASTNode* ast_create(ASTNodeType type);


void ast_add_child(
    ASTNode* parent,
    ASTNode* child
    );


void ast_set_name(
    ASTNode* node,
    const char* name,
    uint32_t length
    );


void ast_set_string(
    ASTNode* node,
    const char* value,
    uint32_t length
    );


void ast_print(
    ASTNode* node,
    int indent
    );


void ast_free(
    ASTNode* node
    );

void ast_set_string(
    ASTNode* node,
    const char* value,
    uint32_t length
    );

#endif