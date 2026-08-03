#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "../include/ast.h"
#include "../include/sema.h"



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



static Symbol* find_symbol_current_scope(
    const char* name,
    uint32_t length
    )
{
    if(!current_scope || !name)
        return NULL;


    Symbol* sym =
        current_scope->symbols;


    while(sym)
    {
        if(sym->length == length &&
            strncmp(sym->name,name,length)==0)
        {
            return sym;
        }


        sym = sym->next;
    }


    return NULL;
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
            analyze(node->children[i]);


        if(find_symbol_current_scope(
                node->name,
                node->name_len))
        {
            fprintf(
                stderr,
                "[kru error] K3002: '%.*s' is already declared in this scope at %u:%u\n",
                node->name_len,
                node->name,
                node->line,
                node->column
                );

            return -1;
        }


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
            analyze(node->children[i]);


        if(find_symbol_current_scope(
                node->name,
                node->name_len))
        {
            fprintf(
                stderr,
                "[kru error] K3002: '%.*s' is already declared in this scope at %u:%u\n",
                node->name_len,
                node->name,
                node->line,
                node->column
                );

            return -1;
        }


        add_symbol(
            node->name,
            node->name_len,
            false,
            true,
            node->child_count > 0
            );


        return 0;



    case AST_ASSIGN_STMT:

        if(node->child_count >= 2)
        {
            ASTNode* target =
                node->children[0];


            Symbol* target_sym = NULL;


            if(target->type == AST_IDENT)
            {
                target_sym =
                    find_symbol(
                        target->name,
                        target->name_len
                        );


                if(target_sym &&
                    !target_sym->is_mutable)
                {
                    fprintf(
                        stderr,
                        "[kru error] K3001: immutable assignment '%.*s' at %u:%u\n",
                        target->name_len,
                        target->name,
                        target->line,
                        target->column
                        );

                    return -1;
                }
            }


            if(analyze(node->children[1]) != 0)
                return -1;


            /*
                The target only becomes initialized *after* the RHS
                has been evaluated, so a self-reference such as
                `x = x + 1` on a still-uninitialized `x` is still
                caught as a use of an uninitialized binding.
            */

            if(target_sym)
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
                fprintf(
                    stderr,
                    "[kru error] K3003: '%.*s' used before it is initialized at %u:%u\n",
                    node->name_len,
                    node->name,
                    node->line,
                    node->column
                    );

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