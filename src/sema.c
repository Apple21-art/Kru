#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "../include/ast.h"
#include "../include/sema.h"
#include "../include/diagnostic.h"



typedef struct Symbol
{
    char* name;
    uint32_t length;

    bool is_function;
    bool is_mutable;
    bool is_initialized;

    struct Symbol* next;

} Symbol;



typedef struct Scope
{
    Symbol* symbols;
    struct Scope* parent;

} Scope;



static Scope* current_scope = NULL;



static void push_scope(void)
{
    Scope* scope =
        calloc(1, sizeof(Scope));


    if(!scope)
        return;


    scope->parent = current_scope;
    current_scope = scope;
}



static void pop_scope(void)
{
    if(!current_scope)
        return;


    Symbol* sym =
        current_scope->symbols;


    while(sym)
    {
        Symbol* next = sym->next;

        free(sym->name);
        free(sym);

        sym = next;
    }


    Scope* parent =
        current_scope->parent;


    free(current_scope);

    current_scope = parent;
}



static void add_symbol(
    const char* name,
    uint32_t length,
    bool function,
    bool mutable,
    bool initialized
    )
{
    if(!current_scope || !name)
        return;


    Symbol* sym =
        calloc(1,sizeof(Symbol));


    if(!sym)
        return;


    sym->name =
        malloc(length + 1);


    if(!sym->name)
    {
        free(sym);
        return;
    }


    memcpy(
        sym->name,
        name,
        length
        );


    sym->name[length] = '\0';

    sym->length = length;
    sym->is_function = function;
    sym->is_mutable = mutable;
    sym->is_initialized = initialized;


    sym->next =
        current_scope->symbols;


    current_scope->symbols = sym;
}



/* Only bindings that exist before a branch participate in its join. */
typedef struct InitState
{
    Symbol* symbol;
    bool entry;
    bool then_initialized;
} InitState;

static int save_initialization(InitState** states, size_t* count)
{
    *states = NULL;
    *count = 0;
    for(Scope* scope = current_scope; scope; scope = scope->parent)
        for(Symbol* sym = scope->symbols; sym; sym = sym->next)
            (*count)++;

    if(!*count)
        return 0;

    *states = calloc(*count, sizeof(**states));
    if(!*states)
        return -1;

    size_t i = 0;
    for(Scope* scope = current_scope; scope; scope = scope->parent)
        for(Symbol* sym = scope->symbols; sym; sym = sym->next)
        {
            (*states)[i].symbol = sym;
            (*states)[i].entry = sym->is_initialized;
            i++;
        }
    return 0;
}

static void restore_initialization(InitState* states, size_t count)
{
    for(size_t i = 0; i < count; i++)
        states[i].symbol->is_initialized = states[i].entry;
}



static Symbol* find_symbol(
    const char* name,
    uint32_t length
    )
{
    Scope* scope =
        current_scope;


    while(scope)
    {
        Symbol* sym =
            scope->symbols;


        while(sym)
        {
            if(sym->length == length &&
                strncmp(sym->name,name,length)==0)
            {
                return sym;
            }


            sym = sym->next;
        }


        scope = scope->parent;
    }


    return NULL;
}



static bool builtin(
    ASTNode* node
    )
{
    if(!node || !node->name)
        return false;


    /*
        Kru builtins
    */

    if(node->name_len == 2 &&
        strncmp(node->name,"pr",2)==0)
    {
        return true;
    }


    return false;
}




static bool type_name_eq(ASTNode* type_node, const char* name)
{
    if(!type_node || !type_node->name || !name)
        return false;

    size_t len = strlen(name);
    return type_node->name_len == len && strncmp(type_node->name, name, len) == 0;
}


static bool type_is_integer(ASTNode* type_node)
{
    static const char* names[] = {
        "int", "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128", "isize", "usize"
    };

    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if(type_name_eq(type_node, names[i]))
            return true;

    return false;
}


static bool type_is_known_primitive(ASTNode* type_node)
{
    return type_is_integer(type_node) ||
           type_name_eq(type_node, "f32") ||
           type_name_eq(type_node, "f64") ||
           type_name_eq(type_node, "bool") ||
           type_name_eq(type_node, "char") ||
           type_name_eq(type_node, "str");
}


static bool initializer_compatible_with_explicit_type(
    ASTNode* type_node,
    ASTNode* init
    )
{
    if(!type_node || !init)
        return true;

    /* Type aliases and user-defined types require alias/type resolution. */
    if(type_node->op != TOKEN_STAR && !type_is_known_primitive(type_node))
        return true;

    /*
        This is deliberately conservative: enforce the obvious primitive
        literal mismatches promised by the Codex, while leaving identifiers,
        calls, structs, references, aliases, and other expressions to the
        fuller type system as it grows. Rejecting only when both sides are
        statically unambiguous avoids turning this bootstrap check into a
        false-positive factory.
    */
    switch(init->type)
    {
    case AST_STRING_LIT:
        return type_name_eq(type_node, "str");

    case AST_BOOL_LIT:
        return type_name_eq(type_node, "bool");

    case AST_CHAR_LIT:
        return type_name_eq(type_node, "char");

    case AST_FLOAT_LIT:
        return type_name_eq(type_node, "f32") || type_name_eq(type_node, "f64");

    case AST_INT_LIT:
        return type_is_integer(type_node) ||
               type_name_eq(type_node, "f32") || type_name_eq(type_node, "f64");

    case AST_UNARY_EXPR:
        if(init->op == TOKEN_MINUS && init->child_count && init->children[0] &&
           (init->children[0]->type == AST_INT_LIT || init->children[0]->type == AST_FLOAT_LIT))
        {
            return initializer_compatible_with_explicit_type(type_node, init->children[0]);
        }
        return true;

    case AST_NULL_LIT:
        return type_node->op == TOKEN_STAR;

    default:
        return true;
    }
}


static int check_binding_initializer_type(ASTNode* node)
{
    if(!node || !node->type_node || node->child_count == 0 || !node->children[0])
        return 0;

    ASTNode* init = node->children[0];
    if(initializer_compatible_with_explicit_type(node->type_node, init))
        return 0;

    char expected[48] = {0};
    if(node->type_node->name)
    {
        size_t n = node->type_node->name_len;
        if(n >= sizeof(expected)) n = sizeof(expected) - 1;
        memcpy(expected, node->type_node->name, n);
        expected[n] = 0;
    }
    else
    {
        snprintf(expected, sizeof(expected), "declared type");
    }

    char message[128];
    snprintf(
        message,
        sizeof(message),
        "initializer is incompatible with declared type '%s'",
        expected
        );

    Diagnostic diag =
    {
        .severity = "error",
        .code = "K1002",
        .message = message,
        .line = init->line ? init->line : node->line,
        .column = init->column ? init->column : node->column,
        .underline_length = 1,
        .explanation = "The initializer's literal kind cannot be assigned to the explicitly declared binding type.",
        .suggested_fix = "change the declared type, or convert the initializer explicitly"
    };

    diagnostic_print(&diag);
    return -1;
}

static int analyze(ASTNode* node)
{
    if(!node)
        return 0;



    switch(node->type)
    {

    case AST_PROGRAM:

        push_scope();


        for(uint32_t i=0;i<node->child_count;i++)
        {
            ASTNode* child =
                node->children[i];


            if(child &&
                child->type == AST_FUNCTION)
            {
                add_symbol(
                    child->name,
                    child->name_len,
                    true,
                    false,
                    true
                    );
            }
            else if(child &&
                    child->type == AST_CONST_DECL)
            {
                /*
                    Register const as an immutable symbol.
                */

                add_symbol(
                    child->name,
                    child->name_len,
                    false,
                    false,
                    true
                    );
            }
        }


        for(uint32_t i=0;i<node->child_count;i++)
        {
            if(analyze(node->children[i]) != 0)
            {
                pop_scope();
                return -1;
            }
        }


        pop_scope();

        return 0;



    case AST_FUNCTION:

        push_scope();


        for(uint32_t i=0;i<node->child_count;i++)
        {
            if(analyze(node->children[i]) != 0)
            {
                pop_scope();
                return -1;
            }
        }


        pop_scope();

        return 0;



    case AST_PARAM:

        /*
            Task C: previously there was no case for AST_PARAM here,
            so parameter names fell through to `default:` (a no-op,
            since AST_PARAM has no children) and were never added to
            the function's scope. A parameter reference inside the
            function body then "resolved" only because AST_IDENT's
            handler treats an unresolved name as fine (see the
            Stage0 comment there) -- i.e. params type-checked by
            accident (by never being checked at all), not by design.

            Params always arrive initialized (the caller supplies the
            value), and are immutable unless is_mut is set on the
            node. The parser does not currently emit `mut` on plain
            by-value AST_PARAM nodes (only `mut ref T` on the type is
            representable per the Codex's function grammar), so
            is_mut reads as false for every parameter today; reading
            it here rather than hardcoding false means this stays
            correct automatically if a future parser change adds
            support for a mutable-by-value parameter form.
        */

        add_symbol(
            node->name,
            node->name_len,
            false,
            node->is_mut,
            true
            );

        return 0;



    case AST_BLOCK:

        push_scope();


        for(uint32_t i=0;i<node->child_count;i++)
        {
            if(analyze(node->children[i]) != 0)
            {
                pop_scope();
                return -1;
            }
        }


        pop_scope();

        return 0;



    case AST_LET_STMT:

        /*
            Analyze initializer first.
        */

        for(uint32_t i=0;i<node->child_count;i++)
            if(analyze(node->children[i]) != 0)
                return -1;

        if(check_binding_initializer_type(node) != 0)
            return -1;


        add_symbol(
            node->name,
            node->name_len,
            false,
            false,
            true
            );


        return 0;



    case AST_VAR_STMT:

        for(uint32_t i=0;i<node->child_count;i++)
            if(analyze(node->children[i]) != 0)
                return -1;

        if(check_binding_initializer_type(node) != 0)
            return -1;


        add_symbol(
            node->name,
            node->name_len,
            false,
            true,
            node->child_count > 0
            );


        return 0;



    case AST_IF_STMT:
    {
        if(node->child_count < 2)
            return -1;
        if(analyze(node->children[0]) != 0)
            return -1;

        InitState* states;
        size_t count;
        if(save_initialization(&states, &count) != 0)
            return -1;

        int result = analyze(node->children[1]);
        if(result == 0)
        {
            for(size_t i = 0; i < count; i++)
                states[i].then_initialized = states[i].symbol->is_initialized;

            /* The else branch starts from the same state as the then branch. */
            restore_initialization(states, count);
            if(node->child_count > 2)
                result = analyze(node->children[2]);

            if(result == 0)
                for(size_t i = 0; i < count; i++)
                    states[i].symbol->is_initialized =
                        states[i].then_initialized && states[i].symbol->is_initialized;
        }
        if(result != 0)
            restore_initialization(states, count);
        free(states);
        return result;
    }

    case AST_WHILE_STMT:
    case AST_FOR_STMT:
    case AST_LOOP_STMT:
    {
        if(node->child_count == 0)
            return -1;

        /* Conditions/range bounds are evaluated before entering the body. */
        for(uint32_t i = 0; i + 1 < node->child_count; i++)
            if(analyze(node->children[i]) != 0)
                return -1;

        InitState* states;
        size_t count;
        if(save_initialization(&states, &count) != 0)
            return -1;

        int result = analyze(node->children[node->child_count - 1]);
        /* A loop can execute zero times, or break before any assignment. */
        restore_initialization(states, count);
        free(states);
        return result;
    }



    case AST_ASSIGN_STMT:

        if(node->child_count >= 2)
        {
            ASTNode* target =
                node->children[0];


            Symbol* target_sym = NULL;


            /* Index/field places mutate the owning value. Dereferencing
               instead mutates pointed-to storage: the pointer/reference
               binding itself may remain immutable. */
            ASTNode* receiver = target;
            while(receiver && (receiver->type == AST_INDEX_EXPR ||
                               receiver->type == AST_FIELD_EXPR))
                receiver = receiver->child_count ? receiver->children[0] : NULL;
            if(!receiver)
                return -1;

            if(receiver->type == AST_IDENT)
            {
                target_sym =
                    find_symbol(
                        receiver->name,
                        receiver->name_len
                        );


                if(target_sym &&
                    !target_sym->is_mutable)
                {
                    /*
                        Task E proof case: the Codex's own worked
                        example (Ch. 77) is specifically this
                        diagnostic. Note on scope: the example's
                        explanation names the exact declaring keyword
                        ("`total` was declared using `let`."), but
                        Symbol (above) doesn't track *how* a binding
                        became immutable -- it could be a `let`, a
                        top-level `const`, or (since Task C) a
                        function parameter, all of which add a symbol
                        with is_mutable=false. Rather than guess or
                        add new plumbing to Symbol to track origin
                        (real but separate work), the explanation
                        here is phrased to be accurate for all three
                        cases instead of falsely specific to one.
                    */

                    char message[96];

                    snprintf(
                        message,
                        sizeof(message),
                        "cannot modify immutable binding '%.*s'",
                        receiver->name_len,
                        receiver->name
                        );

                    Diagnostic diag =
                    {
                        .severity = "error",
                        .code = "K1004",
                        .message = message,
                        .line = receiver->line,
                        .column = receiver->column,
                        .underline_length = receiver->name_len,
                        .explanation =
                            "This binding is immutable -- declared with "
                            "`let`, as a `const`, or as a function "
                            "parameter -- and cannot be assigned to "
                            "after initialization.",
                        .suggested_fix =
                            "if this needs to change, declare it with "
                            "`var` instead of `let` (function parameters "
                            "and `const` bindings cannot be made mutable)"
                    };

                    diagnostic_print(&diag);

                    return -1;
                }
            }


            /* Compound assignment reads the old value; a plain identifier
               assignment only writes it. Other lvalues evaluate their base
               and index/pointer expressions even for plain assignment. */
            if(target->type != AST_IDENT ||
               (node->op != TOKEN_EOF && node->op != TOKEN_EQUALS))
                if(analyze(target) != 0)
                    return -1;

            if(analyze(node->children[1]) != 0)
                return -1;


            /*
                The target only becomes initialized *after* the RHS
                has been evaluated, so a self-reference such as
                `x = x + 1` on a still-uninitialized `x` is still
                caught as a use of an uninitialized binding.
            */

            if(target_sym && target->type == AST_IDENT)
                target_sym->is_initialized = true;
        }


        return 0;



    case AST_IDENT:

        if(builtin(node))
            return 0;


        {
            Symbol* sym =
                find_symbol(
                    node->name,
                    node->name_len
                    );


            if(!sym)
            {
                /*
                    Stage0:
                    allow unresolved names.
                    Parser/codegen are still evolving.
                */

                return 0;
            }


            if(!sym->is_function &&
                !sym->is_initialized)
            {
                char message[96];

                snprintf(
                    message,
                    sizeof(message),
                    "'%.*s' used before it is initialized",
                    node->name_len,
                    node->name
                    );

                Diagnostic diag =
                {
                    .severity = "error",
                    .code = "K1038",
                    .message = message,
                    .line = node->line,
                    .column = node->column,
                    .underline_length = node->name_len
                };

                diagnostic_print(&diag);

                return -1;
            }
        }


        return 0;



    default:

        for(uint32_t i=0;i<node->child_count;i++)
        {
            if(analyze(node->children[i]) != 0)
                return -1;
        }


        return 0;

    }
}



int sema_analyze(
    ASTNode* root_node
    )
{
    if(!root_node)
        return -1;


    current_scope = NULL;


    return analyze(root_node);
}