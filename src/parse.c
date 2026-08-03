#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>


#include "../include/parser.h"
#include "../include/ast.h"
#include "../include/token.h"
#include "../include/lexer.h"



/*
    Maximum expression nesting depth. Recursive-descent parsing uses
    one native C stack frame per nesting level (each '(' re-enters
    parse_expression), so unbounded input like 20000+ nested parens
    will exhaust the OS stack and segfault. This bounds native
    recursion to a depth that's always safe, even on small stacks
    on low-end/old hardware, and turns the crash into a diagnostic.
*/
#define KRU_MAX_EXPR_DEPTH 200

/*
    Stop printing new diagnostics after this many errors. Pathological
    input (e.g. hitting the depth limit above) can otherwise desync
    the parser for the rest of the file and flood the terminal with
    thousands of near-duplicate messages.
*/
#define KRU_MAX_ERRORS 50

typedef struct
{
    Lexer* lexer;

    Token current;
    Token previous;

    bool had_error;

    int depth;
    int error_count;

} Parser;



static void advance(Parser* p)
{
    p->previous = p->current;
    p->current = lexer_next_token(p->lexer);
}



static bool check(
    Parser* p,
    TokenType type
    )
{
    return p->current.type == type;
}



static bool match(
    Parser* p,
    TokenType type
    )
{
    if(check(p,type))
    {
        advance(p);
        return true;
    }

    return false;
}



static void error(
    Parser* p,
    const char* message
    )
{
    p->had_error = true;

    p->error_count++;


    if(p->error_count > KRU_MAX_ERRORS)
    {
        if(p->error_count == KRU_MAX_ERRORS + 1)
        {
            fprintf(
                stderr,
                "[kru error] too many errors, suppressing further diagnostics\n"
                );
        }

        return;
    }


    fprintf(
        stderr,
        "[kru error] %s at %u:%u\n",
        message,
        p->current.line,
        p->current.column
        );
}



/*
    K1xxx diagnostic codes:

    K1001: invalid assignment target
    K1002: expected expression
    K1003: expected variable name
    K1004: expected type name
    K1005: let requires initializer
    K1006: var requires type or initializer
    K1007: expected '}'
    K1008: expected ')'
    K1009: expected '('
    K1010: expected '{'
    K1011: expected 'fn'
    K1012: expected function name
    K1013: expected parameter
    K1014: expected '='
    K1015: expected 'ref'
    K1016: '=' cannot initialize binding, use ':='
    K1017: expected expression
*/



static void expect(
    Parser* p,
    TokenType type,
    const char* message
    )
{
    if(!match(p,type))
    {
        error(
            p,
            message
            );
    }
}



/*
    Parses a Kru integer literal token (as scanned by lex_number) into
    its numeric value.

    This exists instead of a bare strtoll(text, NULL, 0) call because
    strtoll()'s base-0 auto-detection does not match Kru's own literal
    grammar:

      - strtoll treats a leading "0" followed by digits as C-style
        OCTAL ("0123" -> 83), but Kru has no such rule; "0123" is a
        plain decimal literal and must parse as 123.
      - strtoll does not understand Kru's "0b" (binary) or "0o"
        (octal) prefixes at all -- it stops at the 'b'/'o' having
        consumed nothing past the leading zero, silently yielding 0.
      - strtoll stops at the first '_', so underscore digit
        separators ("1_000_000") silently truncate to the digits
        before the first underscore.

    All three previously produced silently wrong values with no
    diagnostic. This parser mirrors the exact character classes
    lex_number/is_hex_digit/is_bin_digit/is_oct_digit/is_dec_digit
    used to scan the literal, strips underscores, and reports
    overflow instead of wrapping/truncating silently.
*/

static int64_t parse_kru_int_literal(
    const char* start,
    uint32_t length,
    bool* out_overflow
    )
{
    const char* cur = start;
    const char* end = start + length;

    int base = 10;

    if(end - cur >= 2 &&
        cur[0] == '0' &&
        (cur[1] == 'x' || cur[1] == 'X'))
    {
        base = 16;
        cur += 2;
    }
    else if(end - cur >= 2 &&
        cur[0] == '0' &&
        (cur[1] == 'b' || cur[1] == 'B'))
    {
        base = 2;
        cur += 2;
    }
    else if(end - cur >= 2 &&
        cur[0] == '0' &&
        (cur[1] == 'o' || cur[1] == 'O'))
    {
        base = 8;
        cur += 2;
    }


    uint64_t value = 0;
    bool overflow = false;


    for(; cur < end; cur++)
    {
        char c = *cur;

        if(c == '_')
            continue;


        int digit;

        if(c >= '0' && c <= '9')
            digit = c - '0';
        else if(c >= 'a' && c <= 'z')
            digit = (c - 'a') + 10;
        else if(c >= 'A' && c <= 'Z')
            digit = (c - 'A') + 10;
        else
            break;


        if(digit >= base)
        {
            /*
                Not a valid digit in this base: this is where a
                type suffix (u8, i64, ...) begins.
            */

            break;
        }


        if(value > (UINT64_MAX - (uint64_t)digit) / (uint64_t)base)
            overflow = true;


        value = (value * (uint64_t)base) + (uint64_t)digit;
    }


    if(value > (uint64_t)INT64_MAX)
        overflow = true;


    if(out_overflow)
        *out_overflow = overflow;


    return (int64_t)value;
}



/*
    AST helpers
*/


static ASTNode* make_node(
    ASTNodeType type
    )
{
    return ast_create(type);
}



static ASTNode* make_type_node(
    Token token
    )
{
    ASTNode* node =
        make_node(AST_TYPE);


    ast_set_name(
        node,
        token.start,
        token.length
        );


    node->line =
        token.line;


    node->column =
        token.column;


    return node;
}



static ASTNode* make_binary(
    TokenType op,
    ASTNode* left,
    ASTNode* right
    )
{
    ASTNode* node =
        make_node(AST_BINARY_EXPR);


    node->op = op;


    ast_add_child(
        node,
        left
        );


    ast_add_child(
        node,
        right
        );


    return node;
}



/*
    Expressions
*/


static ASTNode* parse_expression(Parser* p);
static ASTNode* parse_block(Parser* p);



/*
    Skip generic type arguments: <T, U, ...>
    Called after parsing a type name identifier.
*/
static void skip_generic_args(Parser* p)
{
    if(check(p,TOKEN_LT))
    {
        int depth = 0;
        do
        {
            if(check(p,TOKEN_LT))
                depth++;
            else if(check(p,TOKEN_GT))
                depth--;

            advance(p);
        }
        while(depth > 0 && !check(p,TOKEN_EOF));
    }
}



/*
    Type grammar:

        Type
            -> '[' Type ';' IntLiteral ']'    (fixed-size array)
            -> Identifier ('<' ... '>')?       (named type, generics skipped)

    Array types are represented as an AST_TYPE node with op set to
    TOKEN_LBRACKET, type_node pointing at the element type, and
    int_val holding the declared length. Named types are represented
    as before: an AST_TYPE node carrying just a name.

    Returns NULL (without erroring itself) if the current token
    can't start a type; callers report the specific diagnostic so
    messages stay contextual ("expected type name" vs "expected
    return type", etc.) like they did before this was factored out.
*/
static ASTNode* parse_type(Parser* p)
{
    if(match(p,TOKEN_LBRACKET))
    {
        ASTNode* elem =
            parse_type(p);

        if(!elem)
        {
            error(
                p,
                "K1004: expected type name"
                );

            return NULL;
        }

        expect(
            p,
            TOKEN_SEMICOLON,
            "K1031: expected ';' in array type"
            );

        int64_t length = 0;

        if(check(p,TOKEN_INT_LIT))
        {
            bool overflow = false;

            length =
                parse_kru_int_literal(
                    p->current.start,
                    p->current.length,
                    &overflow
                    );

            if(overflow)
            {
                error(
                    p,
                    "K1034: array length literal does not fit in a 64-bit integer"
                    );
            }

            advance(p);
        }
        else
        {
            error(
                p,
                "K1032: expected array length"
                );
        }

        expect(
            p,
            TOKEN_RBRACKET,
            "K1033: expected ']' in array type"
            );

        ASTNode* node =
            make_node(AST_TYPE);

        node->op = TOKEN_LBRACKET;
        node->type_node = elem;
        node->int_val = length;
        node->line = elem->line;
        node->column = elem->column;

        return node;
    }


    if(check(p,TOKEN_IDENTIFIER))
    {
        ASTNode* node =
            make_type_node(
                p->current
                );

        advance(p);
        skip_generic_args(p);

        return node;
    }


    return NULL;
}



/*
    Declarations
*/


static ASTNode* parse_const(Parser* p)
{
    ASTNode* node =
        make_node(AST_CONST_DECL);


    if(!check(p,TOKEN_IDENTIFIER))
    {
        error(
            p,
            "K1018: expected constant name"
            );
        return node;
    }


    Token name =
        p->current;

    ast_set_name(
        node,
        name.start,
        name.length
        );

    advance(p);


    /*
        Optional type annotation: const X: i32 := ...
    */

    if(match(p,TOKEN_COLON))
    {
        node->type_node =
            parse_type(p);

        if(!node->type_node)
        {
            error(
                p,
                "K1004: expected type name"
                );
        }
    }


    expect(
        p,
        TOKEN_COLON_EQUALS,
        "K1019: expected ':=' for const initializer"
        );


    ast_add_child(
        node,
        parse_expression(p)
        );


    return node;
}



static ASTNode* parse_type_alias(Parser* p)
{
    ASTNode* node =
        make_node(AST_TYPE_ALIAS);


    if(!check(p,TOKEN_IDENTIFIER))
    {
        error(
            p,
            "K1020: expected type alias name"
            );
        return node;
    }


    Token name =
        p->current;

    ast_set_name(
        node,
        name.start,
        name.length
        );

    advance(p);


    expect(
        p,
        TOKEN_COLON_EQUALS,
        "K1021: expected ':=' for type alias"
        );


    node->type_node =
        parse_type(p);

    if(node->type_node)
    {
        /* handled */
    }
    else
    {
        error(
            p,
            "K1004: expected type name"
            );
    }


    return node;
}



static ASTNode* parse_primary(
    Parser* p
    )
{

    if(check(p,TOKEN_INT_LIT))
    {
        Token tok =
            p->current;


        ASTNode* node =
            make_node(AST_INT_LIT);


        bool overflow = false;

        node->int_val =
            parse_kru_int_literal(
                tok.start,
                tok.length,
                &overflow
            );


        if(overflow)
        {
            error(
                p,
                "K1035: integer literal does not fit in a 64-bit integer"
                );
        }


        node->line =
            tok.line;


        node->column =
            tok.column;


        advance(p);

        return node;
    }



    if(check(p,TOKEN_FLOAT_LIT))
    {
        Token tok =
            p->current;


        ASTNode* node =
            make_node(AST_FLOAT_LIT);


        node->float_val =
            strtod(
                tok.start,
                NULL
                );


        node->line =
            tok.line;


        node->column =
            tok.column;


        advance(p);

        return node;
    }



    if(check(p,TOKEN_STRING_LIT))
    {
        Token tok =
            p->current;


        ASTNode* node =
            make_node(AST_STRING_LIT);


        ast_set_string(
            node,
            tok.start,
            tok.length
            );


        node->line =
            tok.line;


        node->column =
            tok.column;


        advance(p);

        return node;
    }



    if(check(p,TOKEN_CHAR_LIT))
    {
        Token tok =
            p->current;


        ASTNode* node =
            make_node(AST_CHAR_LIT);


        /*
            Parse the char literal content.
            tok.start[0] is ', content starts at 1.
            Handle escape sequences.
        */

        if(tok.length >= 2 && tok.start[1] == '\\')
        {
            switch(tok.start[2])
            {
            case 'n':  node->int_val = '\n'; break;
            case 't':  node->int_val = '\t'; break;
            case 'r':  node->int_val = '\r'; break;
            case '0':  node->int_val = '\0'; break;
            case '\\':  node->int_val = '\\'; break;
            case '\'':  node->int_val = '\''; break;
            case '"':  node->int_val = '"'; break;
            default:   node->int_val = tok.start[2]; break;
            }
        }
        else if(tok.length >= 2)
        {
            node->int_val = tok.start[1];
        }


        node->line =
            tok.line;


        node->column =
            tok.column;


        advance(p);

        return node;
    }



    if(check(p,TOKEN_BOOL_LIT))
    {
        Token tok =
            p->current;


        ASTNode* node =
            make_node(AST_BOOL_LIT);


        node->bool_val =
            tok.length == 4 &&
            strncmp(
                tok.start,
                "true",
                4
                ) == 0;

        advance(p);

        return node;
    }



    if(check(p,TOKEN_IDENTIFIER))
    {
        Token tok =
            p->current;


        ASTNode* ident =
            make_node(AST_IDENT);


        ast_set_name(
            ident,
            tok.start,
            tok.length
            );


        ident->line =
            tok.line;


        ident->column =
            tok.column;


        advance(p);


        /*
            Turbofish: identity<Type>(args)
            Need to disambiguate from comparison: a < b
            Use save/restore: try to skip <...>, if followed by '(',
            commit. Otherwise, restore.
        */

        if(check(p,TOKEN_LT))
        {
            const char* saved_cursor =
                p->lexer->cursor;

            uint32_t saved_line =
                p->lexer->line;

            uint32_t saved_column =
                p->lexer->column;

            Token saved_current =
                p->current;

            Token saved_previous =
                p->previous;


            skip_generic_args(p);


            if(check(p,TOKEN_LPAREN))
            {
                /*
                    Turbofish confirmed — the '('< args ')' will be
                    parsed as a function call below.
                */
            }
            else
            {
                /*
                    Not turbofish — restore.

                    Restoring only the cursor (and not line/column)
                    would leave the lexer's line/column counters
                    permanently inflated by however far the failed
                    speculative scan travelled — every subsequent
                    diagnostic in the file would then report the
                    wrong line. Line/column must be rolled back
                    along with the cursor.
                */

                p->lexer->cursor =
                    saved_cursor;

                p->lexer->line =
                    saved_line;

                p->lexer->column =
                    saved_column;

                p->current =
                    saved_current;

                p->previous =
                    saved_previous;
            }
        }


        if(match(p,TOKEN_LPAREN))
        {
            ASTNode* call =
                make_node(AST_CALL_EXPR);


            ast_add_child(
                call,
                ident
                );



            if(!check(p,TOKEN_RPAREN))
            {
                do
                {
                    ast_add_child(
                        call,
                        parse_expression(p)
                        );

                }
                while(match(p,TOKEN_COMMA));
            }



            expect(
                p,
                TOKEN_RPAREN,
                "K1008: expected ')'"
                );


            return call;
        }


        /*
            Struct literal: Identifier { field := value, ... }
            Only parse as struct literal if the identifier starts
            with an uppercase letter (type name convention).
            This avoids ambiguity with 'if flag { ... }' etc.
        */

        if(check(p,TOKEN_LBRACE) &&
           tok.length > 0 &&
           tok.start[0] >= 'A' &&
           tok.start[0] <= 'Z')
        {
            ASTNode* struct_lit =
                make_node(AST_CALL_EXPR);

            /*
                Use AST_CALL_EXPR with the struct name as callee.
                codegen will emit as compound literal.
                We store field assignments as children.
            */

            ast_add_child(
                struct_lit,
                ident
                );


            advance(p); /* consume '{' */


            if(!check(p,TOKEN_RBRACE))
            {
                do
                {
                    /*
                        Each field: name := value
                    */

                    if(!check(p,TOKEN_IDENTIFIER))
                    {
                        error(
                            p,
                            "K1023: expected field name"
                            );
                        break;
                    }


                    Token field_tok =
                        p->current;

                    ASTNode* field_assign =
                        make_node(AST_ASSIGN_STMT);

                    ast_set_name(
                        field_assign,
                        field_tok.start,
                        field_tok.length
                        );

                    advance(p);


                    expect(
                        p,
                        TOKEN_COLON_EQUALS,
                        "K1019: expected ':=' in struct field"
                        );


                    ast_add_child(
                        field_assign,
                        parse_expression(p)
                        );


                    ast_add_child(
                        struct_lit,
                        field_assign
                        );

                }
                while(match(p,TOKEN_COMMA));
            }


            expect(
                p,
                TOKEN_RBRACE,
                "K1007: expected '}'"
                );


            /*
                Mark as struct literal using is_public flag
                (reusing existing field).
            */

            struct_lit->is_struct_lit = true;


            return struct_lit;
        }


        return ident;
    }



    if(match(p,TOKEN_LPAREN))
    {
        ASTNode* expr =
            parse_expression(p);


        expect(
            p,
            TOKEN_RPAREN,
            "K1008: expected ')'"
            );


        return expr;
    }



    /*
        Block expression: { stmts; expr }
        The last expression in the block is the value.
    */

    if(check(p,TOKEN_LBRACE))
    {
        ASTNode* block =
            parse_block(p);


        ASTNode* expr =
            make_node(AST_BLOCK_EXPR);


        ast_add_child(
            expr,
            block
            );


        return expr;
    }



    /*
        Array literal: [ expr, expr, ... ]
    */

    if(match(p,TOKEN_LBRACKET))
    {
        ASTNode* array =
            make_node(AST_ARRAY_LIT);

        array->line =
            p->previous.line;

        array->column =
            p->previous.column;


        if(!check(p,TOKEN_RBRACKET))
        {
            do
            {
                if(check(p,TOKEN_RBRACKET))
                    break;

                ast_add_child(
                    array,
                    parse_expression(p)
                    );
            }
            while(match(p,TOKEN_COMMA));
        }


        expect(
            p,
            TOKEN_RBRACKET,
            "K1034: expected ']' to close array literal"
            );


        return array;
    }



    error(
        p,
        "K1002: expected expression"
        );


    return NULL;
}





static ASTNode* parse_postfix(
    Parser* p
    )
{
    ASTNode* expr =
        parse_primary(p);



    for(;;)
    {
        if(match(p,TOKEN_AT))
        {
            ASTNode* node =
                make_node(AST_DEREF_EXPR);


            ast_add_child(
                node,
                expr
                );


            expr = node;
        }
        else if(match(p,TOKEN_DOT))
        {
            /*
                Field access: expr.field
            */

            if(!check(p,TOKEN_IDENTIFIER))
            {
                error(
                    p,
                    "K1026: expected field name after '.'"
                    );
                break;
            }


            ASTNode* node =
                make_node(AST_FIELD_EXPR);


            Token field_name =
                p->current;

            ast_set_name(
                node,
                field_name.start,
                field_name.length
                );

            advance(p);


            ast_add_child(
                node,
                expr
                );


            expr = node;
        }
        else if(match(p,TOKEN_LBRACKET))
        {
            /*
                Indexing: expr[index]
            */

            ASTNode* node =
                make_node(AST_INDEX_EXPR);

            node->line =
                p->previous.line;

            node->column =
                p->previous.column;


            ast_add_child(
                node,
                expr
                );

            ast_add_child(
                node,
                parse_expression(p)
                );


            expect(
                p,
                TOKEN_RBRACKET,
                "K1035: expected ']' to close index expression"
                );


            expr = node;
        }
        else
        {
            break;
        }
    }


    return expr;
}


static ASTNode* parse_unary(
    Parser* p
    )
{
    /*
        ref x
        mut ref x
    */

    if(
        check(p,TOKEN_KW_REF)
        ||
        check(p,TOKEN_KW_MUT)
        )
    {
        bool is_mut =
            match(
                p,
                TOKEN_KW_MUT
                );


        expect(
            p,
            TOKEN_KW_REF,
            "K1015: expected 'ref'"
            );


        ASTNode* node =
            make_node(AST_REF_EXPR);


        node->is_mut =
            is_mut;


        ast_add_child(
            node,
            parse_postfix(p)
            );


        return node;
    }



    /*
        -x
        !x
        ~x
    */

    if(
        check(p,TOKEN_MINUS)
        ||
        check(p,TOKEN_BANG)
        ||
        check(p,TOKEN_TILDE)
        )
    {
        TokenType op =
            p->current.type;


        advance(p);


        ASTNode* node =
            make_node(AST_UNARY_EXPR);


        node->op =
            op;


        ast_add_child(
            node,
            parse_unary(p)
            );


        return node;
    }



    return parse_postfix(p);
}

static ASTNode* parse_factor(Parser* p)
{
    ASTNode* left =
        parse_unary(p);


    while(
        check(p,TOKEN_STAR) ||
        check(p,TOKEN_SLASH) ||
        check(p,TOKEN_PERCENT)
        )
    {
        TokenType op =
            p->current.type;


        advance(p);


        left =
            make_binary(
                op,
                left,
                parse_unary(p)
                );
    }


    return left;
}



static ASTNode* parse_additive(Parser* p)
{
    ASTNode* left = parse_factor(p);

    while(check(p,TOKEN_PLUS) || check(p,TOKEN_MINUS))
    {
        TokenType op = p->current.type;
        advance(p);
        left = make_binary(op, left, parse_factor(p));
    }

    return left;
}



static ASTNode* parse_shift(Parser* p)
{
    ASTNode* left = parse_additive(p);

    while(check(p,TOKEN_SHL) || check(p,TOKEN_SHR))
    {
        TokenType op = p->current.type;
        advance(p);
        left = make_binary(op, left, parse_additive(p));
    }

    return left;
}



static ASTNode* parse_comparison(Parser* p)
{
    ASTNode* left = parse_shift(p);

    while(
        check(p,TOKEN_LT) ||
        check(p,TOKEN_GT) ||
        check(p,TOKEN_LT_EQUAL) ||
        check(p,TOKEN_GT_EQUAL)
        )
    {
        TokenType op = p->current.type;
        advance(p);
        left = make_binary(op, left, parse_shift(p));
    }

    return left;
}



static ASTNode* parse_equality(Parser* p)
{
    ASTNode* left = parse_comparison(p);

    while(check(p,TOKEN_EQ_EQ) || check(p,TOKEN_BANG_EQUAL))
    {
        TokenType op = p->current.type;
        advance(p);
        left = make_binary(op, left, parse_comparison(p));
    }

    return left;
}



static ASTNode* parse_bitwise_and(Parser* p)
{
    ASTNode* left = parse_equality(p);

    while(check(p,TOKEN_AMP))
    {
        advance(p);
        left = make_binary(TOKEN_AMP, left, parse_equality(p));
    }

    return left;
}



static ASTNode* parse_bitwise_xor(Parser* p)
{
    ASTNode* left = parse_bitwise_and(p);

    while(check(p,TOKEN_CARET))
    {
        advance(p);
        left = make_binary(TOKEN_CARET, left, parse_bitwise_and(p));
    }

    return left;
}



static ASTNode* parse_bitwise_or(Parser* p)
{
    ASTNode* left = parse_bitwise_xor(p);

    while(check(p,TOKEN_PIPE))
    {
        advance(p);
        left = make_binary(TOKEN_PIPE, left, parse_bitwise_xor(p));
    }

    return left;
}



static ASTNode* parse_logical_and(Parser* p)
{
    ASTNode* left = parse_bitwise_or(p);

    while(check(p,TOKEN_AND_AND))
    {
        advance(p);
        left = make_binary(TOKEN_AND_AND, left, parse_bitwise_or(p));
    }

    return left;
}



static ASTNode* parse_logical_or(Parser* p)
{
    ASTNode* left = parse_logical_and(p);

    while(check(p,TOKEN_OR_OR))
    {
        advance(p);
        left = make_binary(TOKEN_OR_OR, left, parse_logical_and(p));
    }

    return left;
}


static ASTNode* parse_expression(Parser* p)
{
    if(p->depth >= KRU_MAX_EXPR_DEPTH)
    {
        error(
            p,
            "K1030: expression nested too deeply"
            );

        return NULL;
    }


    p->depth++;

    ASTNode* result =
        parse_logical_or(p);

    p->depth--;

    return result;
}





/*
    Statements
*/


static ASTNode* parse_variable(
    Parser* p,
    ASTNodeType kind
    )
{
    ASTNode* node =
        make_node(kind);



    if(!check(p,TOKEN_IDENTIFIER))
    {
        error(
            p,
            "K1003: expected variable name"
            );

        return node;
    }



    Token name =
        p->current;


    ast_set_name(
        node,
        name.start,
        name.length
        );


    node->line =
        name.line;


    node->column =
        name.column;


    advance(p);



    bool has_type = false;
    bool has_init = false;



    if(match(p,TOKEN_COLON))
    {
        node->type_node =
            parse_type(p);

        if(node->type_node)
        {
            has_type = true;
        }
        else
        {
            error(
                p,
                "K1004: expected type name"
                );
        }
    }



    if(match(p,TOKEN_COLON_EQUALS))
    {
        has_init = true;
    }
    else if(match(p,TOKEN_EQUALS))
    {
        error(
            p,
            "'=' cannot initialize binding, use ':='"
            );

        has_init = true;
    }



    if(has_init)
    {
        ast_add_child(
            node,
            parse_expression(p)
            );
    }



    if(
        kind == AST_LET_STMT
        &&
        !has_init
        )
    {
        error(
            p,
            "let requires initializer"
            );
    }



    if(
        kind == AST_VAR_STMT
        &&
        !has_init
        &&
        !has_type
        )
    {
        error(
            p,
            "var requires type or initializer"
            );
    }



    return node;
}





static bool is_assignable(
    ASTNode* node
    )
{
    return
        node
        &&
        (
            node->type == AST_IDENT
            ||
            node->type == AST_DEREF_EXPR
            ||
            node->type == AST_INDEX_EXPR
            );
}





static ASTNode* parse_assignment(
    Parser* p,
    ASTNode* left
    )
{
    ASTNode* node =
        make_node(AST_ASSIGN_STMT);


    if(!is_assignable(left))
    {
        error(
            p,
            "K1001: invalid assignment target"
            );
    }


    ast_add_child(
        node,
        left
        );

    expect(
        p,
        TOKEN_EQUALS,
        "K1014: expected '='"
        );

    ast_add_child(
        node,
        parse_expression(p)
        );

    return node;
}



static bool is_compound_assign(
    TokenType type
    )
{
    return
        type == TOKEN_PLUS_EQUALS
        || type == TOKEN_MINUS_EQUALS
        || type == TOKEN_STAR_EQUALS
        || type == TOKEN_SLASH_EQUALS
        || type == TOKEN_PERCENT_EQUALS
        || type == TOKEN_AMP_EQUALS
        || type == TOKEN_PIPE_EQUALS
        || type == TOKEN_CARET_EQUALS
        || type == TOKEN_SHL_EQUALS
        || type == TOKEN_SHR_EQUALS;
}



static ASTNode* parse_compound_assign(
    Parser* p,
    ASTNode* left,
    TokenType op
    )
{
    ASTNode* node =
        make_node(AST_ASSIGN_STMT);


    if(!is_assignable(left))
    {
        error(
            p,
            "K1001: invalid assignment target"
            );
    }


    /*
        Store the compound operator in the node's op field.
        codegen checks this to emit `op=` instead of `=`.
        TOKEN_EOF (default) means plain assignment.
    */

    node->op = op;


    ast_add_child(
        node,
        left
        );


    advance(p);  /* consume the compound assign token */


    ast_add_child(
        node,
        parse_expression(p)
        );


    return node;
}



static ASTNode* parse_expression_statement(
    Parser* p
    )
{
    ASTNode* expr =
        parse_expression(p);



    if(!expr)
        return NULL;



    if(check(p,TOKEN_EQUALS))
    {
        return parse_assignment(
            p,
            expr
            );
    }

    if(is_compound_assign(p->current.type))
    {
        return parse_compound_assign(
            p,
            expr,
            p->current.type
            );
    }



    ASTNode* node =
        make_node(AST_EXPR_STMT);



    ast_add_child(
        node,
        expr
        );


    return node;
}





static ASTNode* parse_return(
    Parser* p
    )
{
    ASTNode* node =
        make_node(AST_RET_STMT);



    if(
        !check(p,TOKEN_RBRACE)
        &&
        !check(p,TOKEN_EOF)
        )
    {
        ast_add_child(
            node,
            parse_expression(p)
            );
    }



    return node;
}





static ASTNode* parse_statement(
    Parser* p
    )
{
    if(match(p,TOKEN_KW_LET))
    {
        return parse_variable(
            p,
            AST_LET_STMT
            );
    }



    if(match(p,TOKEN_KW_VAR))
    {
        return parse_variable(
            p,
            AST_VAR_STMT
            );
    }



    if(match(p,TOKEN_KW_RET))
    {
        return parse_return(p);
    }



    /*
        Bare block expression as a statement.
        Creates a new scope.
    */

    if(check(p,TOKEN_LBRACE))
    {
        return parse_block(p);
    }



    if(match(p,TOKEN_KW_IF))
    {
        ASTNode* node =
            make_node(AST_IF_STMT);


        /*
            Condition: expression after 'if' keyword.
        */

        ast_add_child(
            node,
            parse_expression(p)
            );


        /*
            Then-block.
        */

        ast_add_child(
            node,
            parse_block(p)
            );


        /*
            Optional else-block.
        */

        if(match(p,TOKEN_KW_ELSE))
        {
            if(check(p,TOKEN_KW_IF))
            {
                /*
                    else if  ->  nested if inside else block
                */
                ASTNode* else_block =
                    make_node(AST_BLOCK);

                ast_add_child(
                    else_block,
                    parse_statement(p)
                    );

                ast_add_child(
                    node,
                    else_block
                    );
            }
            else
            {
                ast_add_child(
                    node,
                    parse_block(p)
                    );
            }
        }

        return node;
    }



    if(match(p,TOKEN_KW_WHILE))
    {
        ASTNode* node =
            make_node(AST_WHILE_STMT);


        ast_add_child(
            node,
            parse_expression(p)
            );


        ast_add_child(
            node,
            parse_block(p)
            );

        return node;
    }



    if(match(p,TOKEN_KW_LOOP))
    {
        ASTNode* node =
            make_node(AST_LOOP_STMT);


        ast_add_child(
            node,
            parse_block(p)
            );

        return node;
    }



    if(match(p,TOKEN_KW_BREAK))
    {
        return make_node(AST_BREAK_STMT);
    }



    if(match(p,TOKEN_KW_CONTINUE))
    {
        return make_node(AST_CONTINUE_STMT);
    }



    /*
        Arena block: arena name { ... }
        Parsed as a scoped block. Real arena memory is deferred.
    */

    if(match(p,TOKEN_KW_ARENA))
    {
        ASTNode* node =
            make_node(AST_ARENA_STMT);


        /*
            Arena name.
        */

        if(check(p,TOKEN_IDENTIFIER))
        {
            ast_set_name(
                node,
                p->current.start,
                p->current.length
                );

            advance(p);
        }


        ast_add_child(
            node,
            parse_block(p)
            );


        return node;
    }



    /*
        Match expression: match value { pattern => { ... } ... }
    */

    if(match(p,TOKEN_KW_MATCH))
    {
        ASTNode* node =
            make_node(AST_MATCH_EXPR);


        /*
            Scrutinee expression.
        */

        ast_add_child(
            node,
            parse_expression(p)
            );


        expect(
            p,
            TOKEN_LBRACE,
            "K1010: expected '{' after match expression"
            );


        while(
            !check(p,TOKEN_RBRACE)
            &&
            !check(p,TOKEN_EOF)
            )
        {
            ASTNode* arm =
                make_node(AST_MATCH_ARM);


            /*
                Pattern: literal, identifier, or Type.Variant.
            */

            if(check(p,TOKEN_INT_LIT))
            {
                ASTNode* lit =
                    make_node(AST_INT_LIT);

                bool overflow = false;

                lit->int_val =
                    parse_kru_int_literal(
                        p->current.start,
                        p->current.length,
                        &overflow
                        );

                if(overflow)
                {
                    error(
                        p,
                        "K1035: integer literal does not fit in a 64-bit integer"
                        );
                }

                advance(p);

                ast_add_child(
                    arm,
                    lit
                    );
            }
            else if(check(p,TOKEN_IDENTIFIER))
            {
                ASTNode* ident =
                    make_node(AST_IDENT);

                ast_set_name(
                    ident,
                    p->current.start,
                    p->current.length
                    );

                advance(p);

                /*
                    Check for Type.Variant pattern.
                */

                if(match(p,TOKEN_DOT))
                {
                    if(!check(p,TOKEN_IDENTIFIER))
                    {
                        error(
                            p,
                            "K1026: expected variant name after '.'"
                            );
                        break;
                    }


                    ASTNode* variant =
                        make_node(AST_FIELD_EXPR);

                    ast_set_name(
                        variant,
                        p->current.start,
                        p->current.length
                        );

                    advance(p);

                    ast_add_child(
                        variant,
                        ident
                        );

                    ast_add_child(
                        arm,
                        variant
                        );
                }
                else
                {
                    /*
                        Variable binding pattern.
                    */

                    ast_add_child(
                        arm,
                        ident
                        );
                }
            }
            else
            {
                error(
                    p,
                    "K1027: expected match pattern"
                    );
                break;
            }


            expect(
                p,
                TOKEN_FAT_ARROW,
                "K1028: expected '=>' in match arm"
                );


            /*
                Arm body: block or expression.
            */

            ast_add_child(
                arm,
                parse_block(p)
                );


            ast_add_child(
                node,
                arm
                );


            match(p,TOKEN_COMMA);
        }


        expect(
            p,
            TOKEN_RBRACE,
            "K1007: expected '}'"
            );


        return node;
    }



    return parse_expression_statement(p);
}





static ASTNode* parse_block(
    Parser* p
    )
{
    expect(
        p,
        TOKEN_LBRACE,
        "K1010: expected '{'"
        );


    ASTNode* block =
        make_node(AST_BLOCK);



    while(
        !check(p,TOKEN_RBRACE)
        &&
        !check(p,TOKEN_EOF)
        )
    {
        ASTNode* stmt =
            parse_statement(p);


        if(stmt)
        {
            ast_add_child(
                block,
                stmt
                );
        }
        else
        {
            advance(p);
        }
    }



    expect(
        p,
        TOKEN_RBRACE,
        "K1007: expected '}'"
        );


    return block;
}





/*
    Functions
*/


static ASTNode* parse_struct(Parser* p)
{
    /*
        'struct' keyword already consumed.
    */

    ASTNode* node =
        make_node(AST_STRUCT_DECL);


    if(!check(p,TOKEN_IDENTIFIER))
    {
        error(
            p,
            "K1022: expected struct name"
            );
        return node;
    }


    Token name =
        p->current;

    ast_set_name(
        node,
        name.start,
        name.length
        );

    advance(p);


    /*
        Named struct: struct Name { field: type, ... }
    */

    if(match(p,TOKEN_LBRACE))
    {
        while(
            !check(p,TOKEN_RBRACE)
            &&
            !check(p,TOKEN_EOF)
            )
        {
            if(!check(p,TOKEN_IDENTIFIER))
            {
                error(
                    p,
                    "K1023: expected field name"
                    );
                break;
            }


            Token field_name =
                p->current;

            ASTNode* field =
                make_node(AST_STRUCT_FIELD);

            ast_set_name(
                field,
                field_name.start,
                field_name.length
                );

            advance(p);


            if(match(p,TOKEN_COLON))
            {
                field->type_node =
                    parse_type(p);

                if(!field->type_node)
                {
                    error(
                        p,
                        "K1004: expected type name"
                        );
                }
            }


            ast_add_child(
                node,
                field
                );


            match(p,TOKEN_COMMA);
        }

        expect(
            p,
            TOKEN_RBRACE,
            "K1007: expected '}'"
            );
    }


    /*
        Tuple struct: struct Name(type, type, ...)
    */

    else if(match(p,TOKEN_LPAREN))
    {
        uint32_t anon_index = 0;

        while(
            !check(p,TOKEN_RPAREN)
            &&
            !check(p,TOKEN_EOF)
            )
        {
            if(check(p,TOKEN_IDENTIFIER) || check(p,TOKEN_LBRACKET))
            {
                ASTNode* field =
                    make_node(AST_STRUCT_FIELD);

                /*
                    Generate anonymous field name: _0, _1, ...
                */

                char buf[16];
                snprintf(
                    buf,
                    sizeof(buf),
                    "_%u",
                    anon_index
                    );

                ast_set_name(
                    field,
                    buf,
                    (uint32_t)strlen(buf)
                    );

                field->type_node =
                    parse_type(p);

                ast_add_child(
                    node,
                    field
                    );

                anon_index++;
            }


            if(!match(p,TOKEN_COMMA))
                break;
        }

        expect(
            p,
            TOKEN_RPAREN,
            "K1008: expected ')'"
            );
    }


    return node;
}



static ASTNode* parse_enum(Parser* p)
{
    /*
        'enum' keyword already consumed.
    */

    ASTNode* node =
        make_node(AST_ENUM_DECL);


    if(!check(p,TOKEN_IDENTIFIER))
    {
        error(
            p,
            "K1024: expected enum name"
            );
        return node;
    }


    Token name =
        p->current;

    ast_set_name(
        node,
        name.start,
        name.length
        );

    advance(p);


    expect(
        p,
        TOKEN_LBRACE,
        "K1010: expected '{'"
        );


    while(
        !check(p,TOKEN_RBRACE)
        &&
        !check(p,TOKEN_EOF)
        )
    {
        if(!check(p,TOKEN_IDENTIFIER))
        {
            error(
                p,
                "K1025: expected enum variant"
                );
            break;
        }


        Token variant_name =
            p->current;

        ASTNode* variant =
            make_node(AST_ENUM_VARIANT);

        ast_set_name(
            variant,
            variant_name.start,
            variant_name.length
            );

        advance(p);


        /*
            Optional payload: Variant(type)
        */

        if(match(p,TOKEN_LPAREN))
        {
            while(
                !check(p,TOKEN_RPAREN)
                &&
                !check(p,TOKEN_EOF)
                )
            {
                if(check(p,TOKEN_IDENTIFIER) || check(p,TOKEN_LBRACKET))
                {
                    ASTNode* payload_type =
                        parse_type(p);

                    ast_add_child(
                        variant,
                        payload_type
                        );
                }


                if(!match(p,TOKEN_COMMA))
                    break;
            }

            expect(
                p,
                TOKEN_RPAREN,
                "K1008: expected ')'"
                );
        }


        ast_add_child(
            node,
            variant
            );


        match(p,TOKEN_COMMA);
    }


    expect(
        p,
        TOKEN_RBRACE,
        "K1007: expected '}'"
        );


    return node;
}



/*
    Functions
*/


static ASTNode* parse_function(
    Parser* p
    )
{
    ASTNode* fn =
        make_node(AST_FUNCTION);



    if(match(p,TOKEN_KW_PUB))
    {
        fn->is_public = true;
    }

    if(match(p,TOKEN_KW_UNSAFE))
    {
        fn->is_unsafe = true;
    }

    if(match(p,TOKEN_KW_SECURE))
    {
        fn->is_secure = true;
    }


    if(!match(p,TOKEN_KW_FN))
    {
        error(
            p,
            "K1011: expected fn"
            );

        return NULL;
    }



    if(!check(p,TOKEN_IDENTIFIER))
    {
        error(
            p,
            "K1012: expected function name"
            );

        return NULL;
    }



    Token name =
        p->current;


    ast_set_name(
        fn,
        name.start,
        name.length
        );


    advance(p);


    /*
        Skip generic parameters: <T, U, ...>
    */

    if(check(p,TOKEN_LT))
    {
        int depth = 0;
        do
        {
            if(check(p,TOKEN_LT))
                depth++;
            else if(check(p,TOKEN_GT))
                depth--;

            advance(p);
        }
        while(depth > 0 && !check(p,TOKEN_EOF));
    }


    expect(
        p,
        TOKEN_LPAREN,
        "K1009: expected '('"
        );



    while(
        !check(p,TOKEN_RPAREN)
        &&
        !check(p,TOKEN_EOF)
        )
    {
        if(!check(p,TOKEN_IDENTIFIER))
        {
            error(
                p,
                "K1013: expected parameter"
                );

            break;
        }



        ASTNode* param =
            make_node(AST_PARAM);



        ast_set_name(
            param,
            p->current.start,
            p->current.length
            );


        advance(p);



        if(match(p,TOKEN_COLON))
        {
            param->type_node =
                parse_type(p);

            if(!param->type_node)
            {
                error(
                    p,
                    "K1004: expected type name"
                    );
            }
        }



        ast_add_child(
            fn,
            param
            );



        if(!match(p,TOKEN_COMMA))
            break;
    }



    expect(
        p,
        TOKEN_RPAREN,
        "K1008: expected ')'"
        );



    if(match(p,TOKEN_ARROW))
    {
        fn->type_node =
            parse_type(p);

        if(!fn->type_node)
        {
            error(
                p,
                "K1004: expected type name"
                );
        }
    }



    ast_add_child(
        fn,
        parse_block(p)
        );



    return fn;
}



static ASTNode* parse_declaration(Parser* p)
{
    /*
        Dispatch based on leading keywords.
    */

    bool is_public = false;
    bool is_secure = false;


    if(match(p,TOKEN_KW_PUB))
    {
        is_public = true;
    }

    if(match(p,TOKEN_KW_SECURE))
    {
        is_secure = true;
    }


    if(check(p,TOKEN_KW_FN))
    {
        ASTNode* fn = parse_function(p);

        if(fn)
        {
            if(is_public)
                fn->is_public = true;
            if(is_secure)
                fn->is_secure = true;
        }

        return fn;
    }


    if(check(p,TOKEN_KW_CONST))
    {
        advance(p);
        ASTNode* node = parse_const(p);
        if(node && is_public)
            node->is_public = true;
        return node;
    }

    if(check(p,TOKEN_KW_TYPE))
    {
        advance(p);
        ASTNode* node = parse_type_alias(p);
        if(node && is_public)
            node->is_public = true;
        return node;
    }

    if(check(p,TOKEN_KW_STRUCT))
    {
        advance(p);
        ASTNode* node = parse_struct(p);
        if(node && is_secure)
            node->is_secure = true;
        if(node && is_public)
            node->is_public = true;
        return node;
    }

    if(check(p,TOKEN_KW_ENUM))
    {
        advance(p);
        ASTNode* node = parse_enum(p);
        if(node && is_public)
            node->is_public = true;
        return node;
    }

    if(check(p,TOKEN_KW_COMPTIME))
    {
        /*
            comptime { ... } — skip block for now.
            comptime fn ... — parse as function.
        */
        advance(p);

        if(check(p,TOKEN_LBRACE))
        {
            int depth = 0;
            do
            {
                if(check(p,TOKEN_LBRACE))
                    depth++;
                else if(check(p,TOKEN_RBRACE))
                    depth--;

                advance(p);
            }
            while(depth > 0 && !check(p,TOKEN_EOF));

            ASTNode* placeholder = make_node(AST_EXPR_STMT);
            return placeholder;
        }

        if(check(p,TOKEN_KW_FN))
        {
            return parse_function(p);
        }
    }


    /*
        Handle 'unsafe fn' and 'pub unsafe fn' etc.
    */

    if(check(p,TOKEN_KW_TRAIT))
    {
        /*
            trait Name { ... } — skip entire block.
        */

        advance(p);

        if(check(p,TOKEN_IDENTIFIER))
            advance(p);


        /*
            Skip the trait body.
        */

        if(check(p,TOKEN_LBRACE))
        {
            int depth = 0;
            do
            {
                if(check(p,TOKEN_LBRACE))
                    depth++;
                else if(check(p,TOKEN_RBRACE))
                    depth--;

                advance(p);
            }
            while(depth > 0 && !check(p,TOKEN_EOF));
        }

        ASTNode* placeholder = make_node(AST_EXPR_STMT);
        return placeholder;
    }

    if(check(p,TOKEN_KW_IMPL))
    {
        /*
            impl Trait for Type { ... } — skip for now.
            Functions inside will not be emitted.
        */

        advance(p);


        /*
            Skip to matching closing brace.
        */

        while(!check(p,TOKEN_LBRACE) && !check(p,TOKEN_EOF))
            advance(p);

        if(check(p,TOKEN_LBRACE))
        {
            int depth = 0;
            do
            {
                if(check(p,TOKEN_LBRACE))
                    depth++;
                else if(check(p,TOKEN_RBRACE))
                    depth--;

                advance(p);
            }
            while(depth > 0 && !check(p,TOKEN_EOF));
        }

        ASTNode* placeholder = make_node(AST_EXPR_STMT);
        return placeholder;
    }


    if(check(p,TOKEN_KW_UNSAFE))
    {
        ASTNode* fn = parse_function(p);
        if(fn)
        {
            if(is_public)
                fn->is_public = true;
            if(is_secure)
                fn->is_secure = true;
        }
        return fn;
    }


    error(
        p,
        "K1011: expected declaration"
        );

    return NULL;
}



static ASTNode* parse_program_internal(
    Lexer* lexer,
    bool* had_error
    )
{
    Parser p =
        {
            .lexer = lexer,
            .current = {0},
            .previous = {0},
            .had_error = false
        };



    advance(&p);



    ASTNode* program =
        make_node(AST_PROGRAM);



    while(!check(&p,TOKEN_EOF))
    {
        ASTNode* decl =
            parse_declaration(&p);



        if(!decl)
        {
            break;
        }



        ast_add_child(
            program,
            decl
            );
    }



    *had_error =
        p.had_error;



    if(p.had_error)
    {
        ast_free(program);
        return NULL;
    }



    return program;
}





ASTNode* parse_program(
    Lexer* lexer
    )
{
    Lexer backup =
        *lexer;



    bool error =
        false;



    ASTNode* root =
        parse_program_internal(
            lexer,
            &error
            );



    if(root)
        return root;



    /*
        Restore lexer state.
        Allows old parser fallback.
    */

    *lexer =
        backup;



#ifdef ALPHA_PARSE_FALLBACK

    fprintf(
        stderr,
        "[kru] falling back to ALPHA_PARSE\n"
        );


    return alpha_parse_program(
        lexer
        );

#else

    return NULL;

#endif
}