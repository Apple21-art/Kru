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
    AST_FOR_STMT,

    AST_BREAK_STMT,
    AST_CONTINUE_STMT,

    AST_EXPR_STMT,

    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_STRING_LIT,
    AST_BOOL_LIT,
    AST_CHAR_LIT,
    AST_NULL_LIT,

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


    /*
        Widened to uint64_t (Codex Task D): a signed int64_t could not
        represent a u64/u128 literal above INT64_MAX (e.g. the actual
        u64 max, 18446744073709551615u64), which degraded the range
        check for unsigned types to "just check it's non-negative".
        Signed literal values are still read out of this field via an
        explicit (int64_t) cast at each use site; this is a mechanical
        widening of storage, not a semantic change, for every existing
        signed use case.
    */
    uint64_t int_val;

    /*
        Task B: true only when this AST_INT_LIT carries an explicit
        source-level type suffix (42u8), as opposed to type_node
        being set from the Codex's unsuffixed default-inference
        cascade (i32/i64/i128 by magnitude). Codegen uses this to
        decide whether a suffix may narrow an untyped var/let
        binding's underlying C storage type -- an *explicit* suffix
        is a deliberate programmer choice; the cascade default is
        not, and letting it silently change storage width was found
        to interact badly with a separate, pre-existing codegen path
        (`ref` bindings) that independently assumes int64_t for any
        untyped variable's pointee type. See Task B notes in the
        overhaul report for the full explanation; that interaction
        is flagged as a real bug but is out of scope to fix here.
    */
    bool has_explicit_suffix;

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