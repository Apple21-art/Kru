#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../include/ast.h"
#include "../include/codegen.h"


/*
    Set when codegen detects something that would be unsafe or
    undefined to hand to the C compiler as-is (e.g. a constant
    division by zero). codegen_generate() checks this after walking
    the tree and reports failure instead of silently emitting a
    C file that compiles clean but crashes (or is UB) at runtime.
*/
static bool codegen_had_error = false;


static void codegen_error(
    const char* message,
    uint32_t line,
    uint32_t column
    )
{
    codegen_had_error = true;

    fprintf(
        stderr,
        "[kru error] %s at %u:%u\n",
        message,
        line,
        column
        );
}


/*
    Forward declarations for type registries.
*/

static bool is_known_type(
    const char* name,
    uint32_t name_len
    );


/*
    Map Kru type names to C types.
*/
static const char* codegen_c_type(
    ASTNode* type_node
    )
{
    if(!type_node || !type_node->name)
        return "int64_t";

    if(strncmp(type_node->name, "i8", 2) == 0 && type_node->name_len == 2)
        return "int8_t";
    if(strncmp(type_node->name, "i16", 3) == 0 && type_node->name_len == 3)
        return "int16_t";
    if(strncmp(type_node->name, "i32", 3) == 0 && type_node->name_len == 3)
        return "int32_t";
    if(strncmp(type_node->name, "i64", 3) == 0 && type_node->name_len == 3)
        return "int64_t";
    if(strncmp(type_node->name, "i128", 4) == 0 && type_node->name_len == 4)
        return "__int128";
    if(strncmp(type_node->name, "u8", 2) == 0 && type_node->name_len == 2)
        return "uint8_t";
    if(strncmp(type_node->name, "u16", 3) == 0 && type_node->name_len == 3)
        return "uint16_t";
    if(strncmp(type_node->name, "u32", 3) == 0 && type_node->name_len == 3)
        return "uint32_t";
    if(strncmp(type_node->name, "u64", 3) == 0 && type_node->name_len == 3)
        return "uint64_t";
    if(strncmp(type_node->name, "u128", 4) == 0 && type_node->name_len == 4)
        return "unsigned __int128";
    if(strncmp(type_node->name, "isize", 5) == 0 && type_node->name_len == 5)
        return "intptr_t";
    if(strncmp(type_node->name, "usize", 5) == 0 && type_node->name_len == 5)
        return "uintptr_t";
    if(strncmp(type_node->name, "f32", 3) == 0 && type_node->name_len == 3)
        return "float";
    if(strncmp(type_node->name, "f64", 3) == 0 && type_node->name_len == 3)
        return "double";
    if(strncmp(type_node->name, "bool", 4) == 0 && type_node->name_len == 4)
        return "bool";
    if(strncmp(type_node->name, "char", 4) == 0 && type_node->name_len == 4)
        return "uint32_t";
    if(strncmp(type_node->name, "str", 3) == 0 && type_node->name_len == 3)
        return "const char*";
    if(strncmp(type_node->name, "int", 3) == 0 && type_node->name_len == 3)
        return "int64_t";
    if(strncmp(type_node->name, "void", 4) == 0 && type_node->name_len == 4)
        return "void";

    /*
        If it's a known type (struct, enum, type alias),
        emit the name as-is so C typedefs resolve.
    */

    if(is_known_type(type_node->name, type_node->name_len))
        return type_node->name;

    /*
        Unknown type name — could be a generic type parameter
        (like T) or an unregistered type.
        Default to int64_t for safety.
    */

    return "int64_t";
}



/*
    Codex Ch.8: "The compiler verifies that 8080 fits in u16." This
    checks a literal initializer against its declared fixed-width
    integer type and reports an error instead of silently truncating
    (C itself will happily wrap uint16_t port = 99999 down to 34463
    with no diagnostic, which is not what the language promises).

    Returns false (and reports an error) only when type_node names a
    known fixed-width integer type AND the literal provably doesn't
    fit. Anything else (no type, non-literal initializer, unknown
    type name, 64-bit types) is left alone.
*/
static bool codegen_check_int_range(
    ASTNode* type_node,
    ASTNode* init_node,
    uint32_t line,
    uint32_t column
    )
{
    if(!type_node || !type_node->name || !init_node)
        return true;

    bool negate = false;
    ASTNode* lit = init_node;

    if(lit->type == AST_UNARY_EXPR &&
        lit->op == TOKEN_MINUS &&
        lit->child_count &&
        lit->children[0])
    {
        negate = true;
        lit = lit->children[0];
    }

    if(lit->type != AST_INT_LIT)
        return true;


    const char* name = type_node->name;
    uint32_t len = type_node->name_len;

    int64_t v = negate ? -(lit->int_val) : lit->int_val;

    int64_t min = 0;
    int64_t max = 0;
    bool checked = true;

    if(len == 2 && strncmp(name, "i8", 2) == 0)
    {
        min = INT8_MIN; max = INT8_MAX;
    }
    else if(len == 3 && strncmp(name, "i16", 3) == 0)
    {
        min = INT16_MIN; max = INT16_MAX;
    }
    else if(len == 3 && strncmp(name, "i32", 3) == 0)
    {
        min = INT32_MIN; max = INT32_MAX;
    }
    else if(len == 2 && strncmp(name, "u8", 2) == 0)
    {
        min = 0; max = UINT8_MAX;
    }
    else if(len == 3 && strncmp(name, "u16", 3) == 0)
    {
        min = 0; max = UINT16_MAX;
    }
    else if(len == 3 && strncmp(name, "u32", 3) == 0)
    {
        min = 0; max = UINT32_MAX;
    }
    else if((len == 3 && strncmp(name, "u64", 3) == 0) ||
            (len == 5 && strncmp(name, "usize", 5) == 0))
    {
        /*
            int_val is a signed int64_t, so any value that made it
            this far is already <= INT64_MAX; a negative literal is
            the only thing worth flagging for an unsigned type.
        */

        min = 0; max = INT64_MAX;
    }
    else
    {
        checked = false;
    }


    if(checked && (v < min || v > max))
    {
        char msg[96];

        snprintf(
            msg,
            sizeof(msg),
            "K4002: literal %lld does not fit in declared type '%.*s'",
            (long long)v,
            len,
            name
            );

        codegen_error(msg, line, column);

        return false;
    }


    return true;
}



/*
    Emit a C array declarator for a Kru array type: "elem_t name[N]"
    (no trailing ';' or initializer -- caller adds those). Handles
    nested array types like [[i32; 4]; 8] by walking down through
    each TOKEN_LBRACKET-marked AST_TYPE level and printing one [N]
    per dimension, ending in the base scalar/named type.
*/
static void codegen_emit_array_decl(
    FILE* out,
    ASTNode* type_node,
    const char* name,
    uint32_t name_len
    )
{
    ASTNode* dims[8];
    int dim_count = 0;

    ASTNode* cursor = type_node;

    while(cursor &&
          cursor->op == TOKEN_LBRACKET &&
          dim_count < 8)
    {
        dims[dim_count++] = cursor;
        cursor = cursor->type_node;
    }

    const char* base_type =
        codegen_c_type(cursor);

    fprintf(
        out,
        "%s %.*s",
        base_type,
        name_len,
        name
        );

    for(int i = 0; i < dim_count; i++)
    {
        fprintf(
            out,
            "[%lld]",
            (long long)dims[i]->int_val
            );
    }
}



/*
    Function signature table for type inference.
    Populated during codegen pre-pass.
*/

#define MAX_FUNCS 256


typedef struct {
    const char* name;
    uint32_t name_len;
    const char* ret_type;
} FuncSig;

static FuncSig func_table[MAX_FUNCS];
static uint32_t func_count = 0;


/*
    Enum type registry for Type.Variant access.
    Also used to distinguish known types from unknown ones.
*/

#define MAX_ENUMS 64
#define MAX_TYPES 128

typedef struct {
    const char* name;
    uint32_t name_len;
} TypeEntry;

static TypeEntry enum_table[MAX_ENUMS];
static uint32_t enum_count = 0;

static TypeEntry known_types[MAX_TYPES];
static uint32_t type_count = 0;


static void register_enum(
    const char* name,
    uint32_t name_len
    )
{
    if(enum_count >= MAX_ENUMS)
        return;

    enum_table[enum_count].name = name;
    enum_table[enum_count].name_len = name_len;
    enum_count++;
}


static void register_type(
    const char* name,
    uint32_t name_len
    )
{
    if(type_count >= MAX_TYPES)
        return;

    known_types[type_count].name = name;
    known_types[type_count].name_len = name_len;
    type_count++;
}


static bool is_known_type(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < type_count; i++)
    {
        if(known_types[i].name_len == name_len &&
           strncmp(known_types[i].name, name, name_len) == 0)
        {
            return true;
        }
    }

    return false;
}


static bool is_enum_type(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < enum_count; i++)
    {
        if(enum_table[i].name_len == name_len &&
           strncmp(enum_table[i].name, name, name_len) == 0)
        {
            return true;
        }
    }

    return false;
}


static void register_function(
    const char* name,
    uint32_t name_len,
    const char* ret_type
    )
{
    if(func_count >= MAX_FUNCS)
        return;

    func_table[func_count].name = name;
    func_table[func_count].name_len = name_len;
    func_table[func_count].ret_type = ret_type;
    func_count++;
}


static const char* lookup_function_return(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < func_count; i++)
    {
        if(func_table[i].name_len == name_len &&
           strncmp(func_table[i].name, name, name_len) == 0)
        {
            return func_table[i].ret_type;
        }
    }

    return NULL;
}


static void collect_function_signatures(
    ASTNode* program
    )
{
    if(!program || program->type != AST_PROGRAM)
        return;

    func_count = 0;
    enum_count = 0;
    type_count = 0;

    /*
        Pass 1: register all type declarations
        (structs, enums, type aliases) so that function
        signatures can reference forward-declared types.
    */

    for(uint32_t i = 0;
         i < program->child_count;
         i++)
    {
        ASTNode* child =
            program->children[i];

        if(!child)
            continue;

        if(child->type == AST_ENUM_DECL)
        {
            register_enum(
                child->name,
                child->name_len
                );

            register_type(
                child->name,
                child->name_len
                );
        }
        else if(child->type == AST_STRUCT_DECL)
        {
            register_type(
                child->name,
                child->name_len
                );
        }
        else if(child->type == AST_TYPE_ALIAS)
        {
            register_type(
                child->name,
                child->name_len
                );
        }
    }

    /*
        Pass 2: register function signatures.
        Now all types are known so return types resolve correctly.
    */

    for(uint32_t i = 0;
         i < program->child_count;
         i++)
    {
        ASTNode* child =
            program->children[i];

        if(!child || child->type != AST_FUNCTION)
            continue;


        const char* ret_type = "int64_t";

        if(child->name && child->name_len == 4 &&
           strncmp(child->name, "main", 4) == 0)
        {
            ret_type = "int";
        }
        else if(child->type_node)
        {
            ret_type = codegen_c_type(child->type_node);
        }


        register_function(
            child->name,
            child->name_len,
            ret_type
            );
    }
}


/*
    Emit forward declarations for all functions
    so that forward references work in C.
*/
static void emit_function_prototypes(
    ASTNode* program,
    FILE* out
    )
{
    if(!program || program->type != AST_PROGRAM)
        return;

    for(uint32_t i = 0;
         i < program->child_count;
         i++)
    {
        ASTNode* fn =
            program->children[i];

        if(!fn || fn->type != AST_FUNCTION)
            continue;


        const char* ret_type = "int64_t";

        if(fn->name && fn->name_len == 4 &&
           strncmp(fn->name, "main", 4) == 0)
        {
            ret_type = "int";
        }
        else if(fn->type_node)
        {
            ret_type = codegen_c_type(fn->type_node);
        }


        fprintf(
            out,
            "%s %.*s(",
            ret_type,
            fn->name_len,
            fn->name
            );


        bool first_param = true;

        for(uint32_t j = 0;
             j < fn->child_count;
             j++)
        {
            ASTNode* child =
                fn->children[j];

            if(!child || child->type != AST_PARAM)
                continue;


            if(!first_param)
                fprintf(out, ", ");

            first_param = false;


            const char* param_type = "int64_t";

            if(child->type_node)
                param_type = codegen_c_type(child->type_node);


            fprintf(
                out,
                "%s",
                param_type
                );
        }


        fprintf(
            out,
            ");\n"
            );
    }


    fprintf(
        out,
        "\n"
        );
}


static const char* operator_string(
    TokenType op
    )
{
    switch(op)
    {
    case TOKEN_PLUS:
        return "+";

    case TOKEN_MINUS:
        return "-";

    case TOKEN_STAR:
        return "*";

    case TOKEN_SLASH:
        return "/";

    case TOKEN_PERCENT:
        return "%";

    case TOKEN_AMP:
        return "&";

    case TOKEN_PIPE:
        return "|";

    case TOKEN_CARET:
        return "^";

    case TOKEN_SHL:
        return "<<";

    case TOKEN_SHR:
        return ">>";

    case TOKEN_EQ_EQ:
        return "==";

    case TOKEN_BANG_EQUAL:
        return "!=";

    case TOKEN_LT:
        return "<";

    case TOKEN_GT:
        return ">";

    case TOKEN_LT_EQUAL:
        return "<=";

    case TOKEN_GT_EQUAL:
        return ">=";

    case TOKEN_AND_AND:
        return "&&";

    case TOKEN_OR_OR:
        return "||";

    default:
        return "?";
    }
}



static void codegen_node(
    ASTNode* node,
    FILE* out
    )
{
    if(!node)
        return;



    switch(node->type)
    {

    case AST_PROGRAM:
    {
        /*
            Pre-pass: collect function signatures for type inference.
        */

        collect_function_signatures(node);


        fprintf(
            out,
            "#include <stdio.h>\n"
            "#include <stdint.h>\n"
            "#include <stdbool.h>\n\n"
            );


        /*
            Pass 1: emit type declarations (typedefs, structs, enums)
            so they're visible to function prototypes and consts.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(!child)
                continue;

            if(child->type == AST_TYPE_ALIAS ||
               child->type == AST_STRUCT_DECL ||
               child->type == AST_ENUM_DECL)
            {
                codegen_node(child, out);
            }
        }


        /*
            Pass 2: emit function prototypes.
        */

        emit_function_prototypes(node, out);


        /*
            Pass 3: emit const declarations.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(child && child->type == AST_CONST_DECL)
            {
                codegen_node(child, out);
            }
        }


        /*
            Pass 4: emit function bodies.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(child && child->type == AST_FUNCTION)
            {
                codegen_node(child, out);
            }
        }

        break;
    }



    case AST_CONST_DECL:
    {
        /*
            const NAME: type := value
            → static const type NAME = value;
        */

        const char* c_type = "int64_t";

        if(node->type_node)
            c_type = codegen_c_type(node->type_node);

        /*
            Avoid double-const: if c_type already starts with 'const ',
            don't add another 'static const' prefix.
            Also, if the initializer is a function call (non-constant),
            emit as 'static' instead of 'static const'.
        */

        bool is_call_init =
            (node->child_count &&
             node->children[0] &&
             node->children[0]->type == AST_CALL_EXPR);


        if(is_call_init)
        {
            /*
                Non-constant initializer (function call).
                C requires constant expressions for file-scope
                initializers. Defer to comptime evaluation (Stage 3+).
                Emit as a comment for now.
            */

            fprintf(
                out,
                "/* const %.*s := <runtime initializer> (deferred) */\n\n",
                node->name_len,
                node->name
                );

            break;
        }


        if(strncmp(c_type, "const ", 6) == 0)
        {
            fprintf(
                out,
                "static %s %.*s = ",
                c_type,
                node->name_len,
                node->name
                );
        }
        else
        {
            fprintf(
                out,
                "static const %s %.*s = ",
                c_type,
                node->name_len,
                node->name
                );
        }


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }
        else
        {
            fprintf(out, "0");
        }

        fprintf(
            out,
            ";\n\n"
            );

        break;
    }



    case AST_TYPE_ALIAS:
    {
        /*
            type Name := Target
            → typedef Target Name;
        */

        const char* c_type = "int64_t";

        if(node->type_node)
            c_type = codegen_c_type(node->type_node);

        fprintf(
            out,
            "typedef %s %.*s;\n\n",
            c_type,
            node->name_len,
            node->name
            );

        break;
    }



    case AST_STRUCT_DECL:
    {
        /*
            struct Name { field: type, ... }
            → typedef struct Name { type field; ... } Name;
        */

        fprintf(
            out,
            "typedef struct %.*s {\n",
            node->name_len,
            node->name
            );


        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* field =
                node->children[i];

            if(!field || field->type != AST_STRUCT_FIELD)
                continue;


            const char* field_type = "int64_t";

            if(field->type_node)
                field_type = codegen_c_type(field->type_node);


            fprintf(
                out,
                "    %s %.*s;\n",
                field_type,
                field->name_len,
                field->name
                );
        }


        fprintf(
            out,
            "} %.*s;\n\n",
            node->name_len,
            node->name
            );

        break;
    }



    case AST_ENUM_DECL:
    {
        /*
            enum Name { Variant1, Variant2, ... }
            → typedef enum { Variant1, Variant2, ... } Name;
        */

        fprintf(
            out,
            "typedef enum {\n"
            );


        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* variant =
                node->children[i];

            if(!variant || variant->type != AST_ENUM_VARIANT)
                continue;


            fprintf(
                out,
                "    %.*s,\n",
                variant->name_len,
                variant->name
                );
        }


        fprintf(
            out,
            "} %.*s;\n\n",
            node->name_len,
            node->name
            );

        break;
    }



    case AST_FUNCTION:
    {
        /*
            Emit return type using codegen_c_type.
            Special-case 'main' to emit 'int main()'.
        */

        const char* ret_type = "int64_t";

        if(node->name && node->name_len == 4 &&
           strncmp(node->name, "main", 4) == 0)
        {
            ret_type = "int";
        }
        else if(node->type_node)
        {
            ret_type = codegen_c_type(node->type_node);
        }


        fprintf(
            out,
            "%s %.*s(",
            ret_type,
            node->name_len,
            node->name
            );


        /*
            Separate parameters from the block.
            AST_PARAM nodes are children before the AST_BLOCK child.
        */

        bool first_param = true;

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(!child || child->type != AST_PARAM)
                continue;


            if(!first_param)
                fprintf(out, ", ");

            first_param = false;


            /*
                Use codegen_c_type for param types.
            */

            const char* param_type = "int64_t";

            if(child->type_node)
                param_type = codegen_c_type(child->type_node);

            fprintf(
                out,
                "%s %.*s",
                param_type,
                child->name_len,
                child->name
                );
        }

        fprintf(out, ") ");


        /*
            Emit the body (AST_BLOCK) and any other children.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(child && child->type == AST_PARAM)
                continue;

            codegen_node(
                child,
                out
                );
        }


        fprintf(
            out,
            "\n\n"
            );

        break;
    }



    case AST_BLOCK:
    {
        fprintf(
            out,
            "{\n"
            );


        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            fprintf(
                out,
                "    "
                );


            codegen_node(
                node->children[i],
                out
                );


            fprintf(
                out,
                ";\n"
                );
        }


        fprintf(
            out,
            "}\n"
            );


        break;
    }



    case AST_LET_STMT:
    case AST_VAR_STMT:
    {
        if(node->type_node &&
            node->type_node->op == TOKEN_LBRACKET)
        {
            /*
                Array declaration: let nums: [i32; 4] := [...]
                → int32_t nums[4] = {...};
                C's array declarator puts the size after the name,
                so this can't reuse the plain "TYPE NAME = " path
                below.
            */

            codegen_emit_array_decl(
                out,
                node->type_node,
                node->name,
                node->name_len
                );

            fprintf(out, " = ");

            if(node->child_count &&
                node->children[0])
            {
                codegen_node(
                    node->children[0],
                    out
                    );
            }
            else
            {
                fprintf(out, "{0}");
            }

            break;
        }


        int is_reference = 0;


        if(node->child_count &&
            node->children[0] &&
            node->children[0]->type == AST_REF_EXPR)
        {
            is_reference = 1;
        }



        if(is_reference)
        {
            fprintf(
                out,
                "%s* %.*s = ",
                node->type_node ? codegen_c_type(node->type_node) : "int64_t",
                node->name_len,
                node->name
                );
        }
        else
        {
            const char* var_type = "int64_t";

            bool type_emitted = false;


            if(node->type_node)
            {
                var_type = codegen_c_type(node->type_node);

                if(node->child_count &&
                    node->children[0])
                {
                    codegen_check_int_range(
                        node->type_node,
                        node->children[0],
                        node->line,
                        node->column
                        );
                }
            }
            else if(node->child_count &&
                    node->children[0] &&
                    node->children[0]->type == AST_CALL_EXPR &&
                    !node->children[0]->is_struct_lit &&
                    node->children[0]->child_count &&
                    node->children[0]->children[0] &&
                    node->children[0]->children[0]->type == AST_IDENT)
            {
                /*
                    Infer type from function call return type:
                    let x := foo(...) → RetType x = ...
                */

                ASTNode* callee =
                    node->children[0]->children[0];

                const char* ret_type =
                    lookup_function_return(
                        callee->name,
                        callee->name_len
                        );

                if(ret_type)
                {
                    fprintf(
                        out,
                        "%s %.*s = ",
                        ret_type,
                        node->name_len,
                        node->name
                        );

                    type_emitted = true;
                }
            }
            else if(node->child_count &&
                    node->children[0] &&
                    node->children[0]->type == AST_CALL_EXPR &&
                    node->children[0]->is_struct_lit &&
                    node->children[0]->child_count &&
                    node->children[0]->children[0])
            {
                /*
                    Infer type from struct literal:
                    let x := Foo { ... } → Foo x = ...
                */

                ASTNode* callee =
                    node->children[0]->children[0];

                if(callee->type == AST_IDENT && callee->name)
                {
                    fprintf(
                        out,
                        "%.*s %.*s = ",
                        callee->name_len,
                        callee->name,
                        node->name_len,
                        node->name
                        );

                    type_emitted = true;
                }
            }


            if(!type_emitted)
            {
                fprintf(
                    out,
                    "%s %.*s = ",
                    var_type,
                    node->name_len,
                    node->name
                    );
            }
        }



        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }
        else
        {
            fprintf(
                out,
                "0"
                );
        }


        break;
    }



    case AST_ASSIGN_STMT:
    {
        if(node->child_count < 2)
            break;


        codegen_node(
            node->children[0],
            out
            );


        /*
            Check if this is a compound assignment.
            node->op holds the compound operator token
            (TOKEN_EOF means plain assignment).
        */

        if(node->op != TOKEN_EOF)
        {
            /*
                Map compound token to C operator suffix.
            */

            const char* suffix = "";

            switch(node->op)
            {
            case TOKEN_PLUS_EQUALS:    suffix = "+=";  break;
            case TOKEN_MINUS_EQUALS:   suffix = "-=";  break;
            case TOKEN_STAR_EQUALS:    suffix = "*=";  break;
            case TOKEN_SLASH_EQUALS:   suffix = "/=";  break;
            case TOKEN_PERCENT_EQUALS: suffix = "%=";  break;
            case TOKEN_AMP_EQUALS:     suffix = "&=";  break;
            case TOKEN_PIPE_EQUALS:    suffix = "|=";  break;
            case TOKEN_CARET_EQUALS:   suffix = "^=";  break;
            case TOKEN_SHL_EQUALS:     suffix = "<<="; break;
            case TOKEN_SHR_EQUALS:     suffix = ">>="; break;
            default: suffix = "="; break;
            }

            fprintf(
                out,
                " %s ",
                suffix
                );
        }
        else
        {
            fprintf(
                out,
                " = "
                );
        }


        codegen_node(
            node->children[1],
            out
            );


        break;
    }



    case AST_RET_STMT:
    {
        fprintf(
            out,
            "return "
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }
        else
        {
            fprintf(
                out,
                "0"
                );
        }


        break;
    }



    case AST_IF_STMT:
    {
        /*
            children[0] = condition
            children[1] = then-block
            children[2] = else-block (optional)
        */

        fprintf(out, "if (");

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);

        fprintf(out, ") ");

        if(node->child_count > 1 && node->children[1])
            codegen_node(node->children[1], out);
        else
            fprintf(out, "{}\n");


        if(node->child_count > 2 && node->children[2])
        {
            fprintf(out, " else ");
            codegen_node(node->children[2], out);
        }

        break;
    }



    case AST_WHILE_STMT:
    {
        fprintf(out, "while (");

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);

        fprintf(out, ") ");

        if(node->child_count > 1 && node->children[1])
            codegen_node(node->children[1], out);
        else
            fprintf(out, "{}\n");

        break;
    }



    case AST_LOOP_STMT:
    {
        fprintf(out, "while (1) ");

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);
        else
            fprintf(out, "{}\n");

        break;
    }



    case AST_BREAK_STMT:
    {
        fprintf(out, "break");
        break;
    }



    case AST_CONTINUE_STMT:
    {
        fprintf(out, "continue");
        break;
    }



    case AST_EXPR_STMT:
    {
        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }

        break;
    }



    case AST_ARENA_STMT:
    {
        /*
            arena name { ... }
            Emit as a regular scoped block with a comment.
            Real arena memory is deferred.
        */

        fprintf(
            out,
            "/* arena %.*s */ {\n",
            node->name_len,
            node->name ? node->name : (const char*)""
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(out, "}\n");

        break;
    }



    case AST_BLOCK_EXPR:
    {
        /*
            Block expression: { stmts; expr }
            Emit as GNU C statement-expression: ({ ...; expr; })
            The last expression statement is the value.
        */

        fprintf(out, "({\n");


        /*
            children[0] is an AST_BLOCK.
            Emit its children directly (not the block wrapper).
        */

        if(node->child_count &&
            node->children[0] &&
            node->children[0]->type == AST_BLOCK)
        {
            ASTNode* block =
                node->children[0];

            for(uint32_t i = 0;
                 i < block->child_count;
                 i++)
            {
                fprintf(out, "    ");

                codegen_node(
                    block->children[i],
                    out
                    );

                fprintf(out, ";\n");
            }
        }


        fprintf(out, "})");

        break;
    }



    case AST_MATCH_EXPR:
    {
        /*
            match scrutinee { pattern => block ... }
            Lower to if/else if chain.
        */

        if(node->child_count < 2)
            break;


        ASTNode* scrutinee =
            node->children[0];


        /*
            Emit each arm as: if (scrutinee == pattern) { block }
            except the last arm which may be a catch-all.
        */

        for(uint32_t i = 1;
             i < node->child_count;
             i++)
        {
            ASTNode* arm =
                node->children[i];

            if(!arm || arm->type != AST_MATCH_ARM)
                continue;


            if(i > 1)
                fprintf(out, "else ");


            ASTNode* pattern =
                arm->child_count > 0 ? arm->children[0] : NULL;

            ASTNode* body =
                arm->child_count > 1 ? arm->children[1] : NULL;


            /*
                Check for catch-all (variable binding pattern).
            */

            bool is_catchall =
                (pattern &&
                 pattern->type == AST_IDENT);


            if(is_catchall)
            {
                /*
                    Variable binding: bind scrutinee to variable.
                */

                fprintf(out, "{\n");

                fprintf(
                    out,
                    "int64_t %.*s = ",
                    pattern->name_len,
                    pattern->name
                    );

                codegen_node(scrutinee, out);

                fprintf(out, ";\n");


                if(body)
                    codegen_node(body, out);


                fprintf(out, "}\n");
            }
            else
            {
                fprintf(out, "if (");

                codegen_node(scrutinee, out);

                fprintf(out, " == ");

                codegen_node(pattern, out);

                fprintf(out, ") {\n");


                if(body)
                    codegen_node(body, out);


                fprintf(out, "}\n");
            }
        }


        break;
    }



    case AST_INT_LIT:
    {
        fprintf(
            out,
            "%lld",
            (long long)node->int_val
            );

        break;
    }



    case AST_CHAR_LIT:
    {
        fprintf(
            out,
            "%lld",
            (long long)node->int_val
            );

        break;
    }



    case AST_BOOL_LIT:
    {
        fprintf(
            out,
            "%lld",
            (long long)(node->bool_val ? 1 : 0)
            );

        break;
    }



    case AST_FLOAT_LIT:
    {
        fprintf(
            out,
            "%lf",
            node->float_val
            );

        break;
    }



    case AST_STRING_LIT:
    {
        /*
            Emit the raw string literal token.
            The token includes surrounding quotes.
        */

        fprintf(
            out,
            "%.*s",
            node->string_len,
            node->string_val
            );

        break;
    }



    case AST_IDENT:
    {
        fprintf(
            out,
            "%.*s",
            node->name_len,
            node->name
            );

        break;
    }



    case AST_REF_EXPR:
    {
        fprintf(
            out,
            "&"
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        break;
    }



    case AST_DEREF_EXPR:
    {
        fprintf(
            out,
            "*("
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(
            out,
            ")"
            );


        break;
    }



    case AST_FIELD_EXPR:
    {
        /*
            expr.field → (expr).field
            But if expr is an enum type name, emit just
            the variant name (C enums are unscoped).
        */

        if(node->child_count &&
            node->children[0] &&
            node->children[0]->type == AST_IDENT &&
            node->children[0]->name)
        {
            if(is_enum_type(
                node->children[0]->name,
                node->children[0]->name_len
                ))
            {
                fprintf(
                    out,
                    "%.*s",
                    node->name_len,
                    node->name
                    );

                break;
            }
        }


        fprintf(
            out,
            "("
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(
            out,
            ").%.*s",
            node->name_len,
            node->name
            );


        break;
    }



    case AST_ARRAY_LIT:
    {
        /*
            [e1, e2, ...] → { e1, e2, ... }
            Valid as a C array initializer (used on the RHS of an
            array let/var declaration). Not valid as a standalone
            C expression elsewhere -- Kru doesn't support array
            literals outside of a declaration's initializer yet.
        */

        fprintf(out, "{ ");

        for(uint32_t i = 0; i < node->child_count; i++)
        {
            if(i > 0)
                fprintf(out, ", ");

            codegen_node(
                node->children[i],
                out
                );
        }

        fprintf(out, " }");

        break;
    }



    case AST_INDEX_EXPR:
    {
        /*
            expr[index] → (expr)[index]
        */

        fprintf(out, "(");

        if(node->child_count > 0)
        {
            codegen_node(
                node->children[0],
                out
                );
        }

        fprintf(out, ")[");

        if(node->child_count > 1)
        {
            codegen_node(
                node->children[1],
                out
                );
        }

        fprintf(out, "]");

        break;
    }



    case AST_BINARY_EXPR:
    {
        if((node->op == TOKEN_SLASH || node->op == TOKEN_PERCENT) &&
            node->child_count > 1 &&
            node->children[1] &&
            node->children[1]->type == AST_INT_LIT &&
            node->children[1]->int_val == 0)
        {
            codegen_error(
                node->op == TOKEN_SLASH
                    ? "K4001: division by constant zero"
                    : "K4001: modulo by constant zero",
                node->children[1]->line,
                node->children[1]->column
                );
        }


        fprintf(
            out,
            "("
            );


        if(node->child_count > 0)
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(
            out,
            " %s ",
            operator_string(node->op)
            );


        if(node->child_count > 1)
        {
            codegen_node(
                node->children[1],
                out
                );
        }


        fprintf(
            out,
            ")"
            );


        break;
    }



    case AST_UNARY_EXPR:
    {
        const char* op_str = "?";

        switch(node->op)
        {
        case TOKEN_MINUS: op_str = "-"; break;
        case TOKEN_BANG:   op_str = "!"; break;
        case TOKEN_TILDE:  op_str = "~"; break;
        default: break;
        }

        fprintf(out, "%s", op_str);

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);

        break;
    }



    case AST_CALL_EXPR:
    {
        if(!node->child_count)
            break;


        ASTNode* callee =
            node->children[0];


        /*
            Struct literal: TypeName { field := value, ... }
            → (TypeName){ .field = value, ... }
        */

        if(node->is_struct_lit)
        {
            fprintf(
                out,
                "(%.*s){",
                callee->name_len,
                callee->name
                );


            for(uint32_t i = 1;
                 i < node->child_count;
                 i++)
            {
                ASTNode* field =
                    node->children[i];

                if(!field || field->type != AST_ASSIGN_STMT)
                    continue;


                fprintf(
                    out,
                    ".%.*s = ",
                    field->name_len,
                    field->name
                    );


                if(field->child_count &&
                    field->children[0])
                {
                    codegen_node(
                        field->children[0],
                        out
                        );
                }


                if(i + 1 < node->child_count)
                {
                    fprintf(
                        out,
                        ", "
                        );
                }
            }


            fprintf(
                out,
                "}"
                );

            break;
        }


        if(callee &&
            callee->type == AST_IDENT &&
            callee->name_len == 2 &&
            strncmp(
                callee->name,
                "pr",
                2
                ) == 0)
        {
            fprintf(
                out,
                "printf(\"%%lld\\n\", (long long)("
                );


            if(node->child_count > 1)
            {
                codegen_node(
                    node->children[1],
                    out
                    );
            }


            fprintf(
                out,
                "))"
                );

            break;
        }



        /*
            Check if callee is a known function.
            If not, and it starts with uppercase, treat as
            enum variant constructor: Ok(100) → 100.
        */

        bool is_known_func = false;

        if(callee && callee->type == AST_IDENT && callee->name)
        {
            for(uint32_t i = 0; i < func_count; i++)
            {
                if(func_table[i].name_len == callee->name_len &&
                   strncmp(func_table[i].name, callee->name,
                           callee->name_len) == 0)
                {
                    is_known_func = true;
                    break;
                }
            }
        }


        if(!is_known_func &&
           callee && callee->type == AST_IDENT &&
           callee->name_len > 0 &&
           callee->name[0] >= 'A' &&
           callee->name[0] <= 'Z')
        {
            /*
                Enum variant constructor with payload.
                Emit just the first argument as the value.
            */

            if(node->child_count > 1)
            {
                codegen_node(
                    node->children[1],
                    out
                    );
            }
            else
            {
                fprintf(out, "0");
            }

            break;
        }



        codegen_node(
            callee,
            out
            );


        fprintf(
            out,
            "("
            );


        for(uint32_t i = 1;
             i < node->child_count;
             i++)
        {
            codegen_node(
                node->children[i],
                out
                );


            if(i + 1 < node->child_count)
            {
                fprintf(
                    out,
                    ", "
                    );
            }
        }


        fprintf(
            out,
            ")"
            );


        break;
    }



    default:
    {
        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            codegen_node(
                node->children[i],
                out
                );
        }

        break;
    }

    }
}



int codegen_generate(
    ASTNode* root_node,
    const char* output_filepath
    )
{
    codegen_had_error = false;


    FILE* out =
        fopen(
            output_filepath,
            "w"
            );


    if(!out)
        return -1;



    codegen_node(
        root_node,
        out
        );


    fclose(out);


    if(codegen_had_error)
        return -1;


    return 0;
}