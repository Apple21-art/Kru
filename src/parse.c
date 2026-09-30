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
#include "../include/diagnostic.h"



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


    /*
        Task E: every parser diagnostic call site passes a single
        "K1004: expected type name" style string through this one
        function. Rather than touching all ~55 call sites
        individually, split that string here into code + message and
        route it through the shared diagnostic_print() mechanism, so
        every parser error gets the Codex's 7-part format (source
        line + caret included) "for free". explanation/suggested_fix
        stay NULL for all of these -- writing a good, specific
        explanation for each of the 55 distinct parse errors is real
        per-diagnostic authoring work, not a plumbing change, and is
        out of scope for this pass.

        TODO: explanation / suggested_fix text for each of the ~55
        distinct parser diagnostics that flow through this function.
    */

    const char* colon =
        strstr(message, ": ");

    char code[16] = "K0000";
    const char* msg_text = message;

    if(colon &&
        (size_t)(colon - message) < sizeof(code))
    {
        size_t code_len =
            (size_t)(colon - message);

        memcpy(code, message, code_len);
        code[code_len] = '\0';

        msg_text = colon + 2;
    }


    Diagnostic diag =
    {
        .severity = "error",
        .code = code,
        .message = msg_text,
        .line = p->current.line,
        .column = p->current.column,
        .underline_length = p->current.length ? p->current.length : 1
    };

    diagnostic_print(&diag);
}



/*
    K1xxx diagnostic codes:

    K1001: invalid assignment target
    K1005: expected expression
    K1003: expected variable name
    K1036: expected type name
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

/*
    Task D note (deviation from the overhaul work order, documented
    here rather than silently applied): the work order says "the
    overflow-check logic doesn't need to change" when widening this
    function's storage. That's true for the in-loop multiply-overflow
    check below, but the *post-loop* `if(value > INT64_MAX) overflow
    = true;` line that existed before this patch was not part of that
    in-loop check -- it was a separate artificial cap that exists
    only because the old return type was a signed int64_t. Keeping it
    would silently defeat Task D's entire point: a real u64 literal
    like 18446744073709551615u64 would still be flagged as overflow
    even though it fits fine in the now-uint64_t storage. Removed.
    Per-type-suffix range checking (does *this* value fit *this*
    suffix's type) is Task B's job, wired in via out_suffix below;
    this function's only remaining job is "does it fit in 64 bits at
    all", which the in-loop check already covers correctly.
*/
/*
    Task B: the set of integer type suffixes a literal may carry
    (Codex Lexical Structure: "42u8  100i64  0xFFu16"). Mirrors the
    integer-type branch of codegen_c_type() in codegen.c -- kept as a
    small local table here rather than sharing a header-level list
    across translation units, matching how type-name checks already
    happen independently in each file in this codebase.
*/
static bool is_known_int_suffix(
    const char* text,
    uint32_t len
    )
{
    static const char* const suffixes[] =
    {
        "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128",
        "isize", "usize"
    };

    for(size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
    {
        size_t slen = strlen(suffixes[i]);

        if(slen == len && strncmp(suffixes[i], text, len) == 0)
            return true;
    }

    return false;
}



/*
    Task B item 4: "Without a suffix, integer literals default to
    i32. If the value exceeds i32 range, the compiler promotes to
    i64, then i128, then reports an overflow error." (Codex, Lexical
    Structure). This only decides which type name an *unsuffixed*
    literal implicitly carries; a real "doesn't fit even i128" case
    is already unreachable from this function today since the value
    was already bounded to 64 bits (K1035) upstream in
    parse_kru_int_literal -- 64-bit values always fit in i128's much
    larger range. The cascade is still implemented as a real
    magnitude check (not just "always i32") so it produces the
    correct declared type for codegen and for any future range check
    once literals wider than 64 bits are representable.
*/
static const char* infer_default_int_suffix(
    uint64_t magnitude,
    bool negate
    )
{
    /*
        int32_t/int64_t bounds compared against the literal's
        unsigned magnitude plus its sign, mirroring how
        codegen_check_int_range treats a negated literal.
    */

    if(negate)
    {
        if(magnitude <= 2147483648ULL)       /* -2147483648 fits i32 */
            return "i32";

        if(magnitude <= 9223372036854775808ULL) /* INT64_MIN magnitude */
            return "i64";

        return "i128";
    }

    if(magnitude <= 2147483647ULL)
        return "i32";

    if(magnitude <= 9223372036854775807ULL)
        return "i64";

    return "i128";
}



static uint64_t parse_kru_int_literal(
    const char* start,
    uint32_t length,
    bool* out_overflow,
    const char** out_suffix,
    uint32_t* out_suffix_len
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


    if(out_suffix)
        *out_suffix = cur;

    if(out_suffix_len)
        *out_suffix_len = (uint32_t)(end - cur);


    if(out_overflow)
        *out_overflow = overflow;


    return value;
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
    /*
        Stage 5: raw pointer type, `*T`. Reuses AST_TYPE with
        op == TOKEN_STAR (mirrors the existing `op == TOKEN_LBRACKET`
        convention for array types) and type_node pointing at the
        pointee type, so codegen_c_type can recurse the same way it
        already does for arrays.
    */

    if(match(p,TOKEN_STAR))
    {
        ASTNode* pointee =
            parse_type(p);

        if(!pointee)
        {
            error(
                p,
                "K1036: expected type name after '*'"
                );

            return NULL;
        }

        ASTNode* node =
            make_node(AST_TYPE);

        node->op = TOKEN_STAR;
        node->type_node = pointee;
        node->line = pointee->line;
        node->column = pointee->column;

        return node;
    }


    if(match(p,TOKEN_LBRACKET))
    {
        ASTNode* elem =
            parse_type(p);

        if(!elem)
        {
            error(
                p,
                "K1036: expected type name"
                );

            return NULL;
        }

        expect(
            p,
            TOKEN_SEMICOLON,
            "K1031: expected ';' in array type"
            );

        uint64_t length = 0;

        if(check(p,TOKEN_INT_LIT))
        {
            bool overflow = false;

            length =
                parse_kru_int_literal(
                    p->current.start,
                    p->current.length,
                    &overflow,
                    NULL,
                    NULL
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
                "K1036: expected type name"
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
            "K1036: expected type name"
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
        const char* suffix = NULL;
        uint32_t suffix_len = 0;

        node->int_val =
            parse_kru_int_literal(
                tok.start,
                tok.length,
                &overflow,
                &suffix,
                &suffix_len
            );


        if(overflow)
        {
            error(
                p,
                "K1035: integer literal does not fit in a 64-bit integer"
                );
        }


        /*
            Task B: wire the suffix (if any) through as the literal's
            own type_node, exactly the way a `let`/`var` binding's
            explicit type annotation is represented -- this lets
            codegen's existing codegen_check_int_range() and
            codegen_c_type() operate on a suffixed literal without
            needing a separate code path.
        */

        if(suffix_len > 0)
        {
            if(!is_known_int_suffix(suffix, suffix_len))
            {
                error(
                    p,
                    "K1041: unrecognized integer literal suffix"
                    );
            }
            else
            {
                ASTNode* suffix_type =
                    make_node(AST_TYPE);

                ast_set_name(
                    suffix_type,
                    suffix,
                    suffix_len
                    );

                suffix_type->line = tok.line;
                suffix_type->column = tok.column;

                node->type_node = suffix_type;
                node->has_explicit_suffix = true;
            }
        }
        else
        {
            /*
                No suffix: apply the Codex's default-inference
                cascade (i32 -> i64 -> i128) so codegen sees the
                same "this literal's type is X" information a
                suffix would have given it, instead of falling
                through to codegen's separate untyped fallback.
            */

            bool negate = false;

            /*
                parse_primary only ever sees the literal itself, not
                a leading unary minus (that is a separate AST_UNARY_EXPR
                built by the caller in parse_unary/parse_prefix), so
                the cascade here is evaluated as an unsigned/positive
                magnitude. This still produces the correct declared
                type for the overwhelming majority of literals (any
                literal that isn't the sole operand of a leading `-`);
                a negated literal's type is re-derived from the
                negative range where it matters (codegen_check_int_range
                already special-cases the AST_UNARY_EXPR-wrapping-
                AST_INT_LIT shape for exactly this reason).
            */

            ASTNode* suffix_type =
                make_node(AST_TYPE);

            const char* inferred =
                infer_default_int_suffix(node->int_val, negate);

            ast_set_name(
                suffix_type,
                inferred,
                (uint32_t)strlen(inferred)
                );

            suffix_type->line = tok.line;
            suffix_type->column = tok.column;

            node->type_node = suffix_type;
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



    if(check(p,TOKEN_NULL_LIT))
    {
        ASTNode* node =
            make_node(AST_NULL_LIT);

        node->line =
            p->current.line;

        node->column =
            p->current.column;

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
                        Codex field initializer: name : value.
                        Keep bootstrap name := value source compatible.
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


                    if(!match(p,TOKEN_COLON) && !match(p,TOKEN_COLON_EQUALS))
                    {
                        error(p, "K1019: expected ':' in struct field");
                    }


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
        "K1005: expected expression"
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
        else if(match(p, TOKEN_LPAREN))
        {
            ASTNode* call = make_node(AST_CALL_EXPR);
            ast_add_child(call, expr);
            if(!check(p, TOKEN_RPAREN))
            {
                do
                {
                    ast_add_child(call, parse_expression(p));
                }
                while(match(p, TOKEN_COMMA) && !check(p, TOKEN_RPAREN));
            }
            expect(p, TOKEN_RPAREN, "K1008: expected ')'");
            expr = call;
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



static ASTNode* parse_bitwise_or(Parser* p);

static ASTNode* parse_comparison(Parser* p)
{
    ASTNode* left = parse_bitwise_or(p);

    while(
        check(p,TOKEN_LT) ||
        check(p,TOKEN_GT) ||
        check(p,TOKEN_LT_EQUAL) ||
        check(p,TOKEN_GT_EQUAL)
        )
    {
        TokenType op = p->current.type;
        advance(p);
        left = make_binary(op, left, parse_bitwise_or(p));
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
    ASTNode* left = parse_shift(p);

    while(check(p,TOKEN_AMP))
    {
        advance(p);
        left = make_binary(TOKEN_AMP, left, parse_shift(p));
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
    ASTNode* left = parse_equality(p);

    while(check(p,TOKEN_AND_AND))
    {
        advance(p);
        left = make_binary(TOKEN_AND_AND, left, parse_equality(p));
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
                "K1036: expected type name"
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
            ||
            /*
                Boost pass: field-expr targets (`user.score = 50`,
                `points[1].x = 30`) were rejected as invalid
                assignment targets even though codegen already
                emits AST_FIELD_EXPR correctly as an lvalue for
                reads -- assigning through one just needed this
                gate opened. sema's mutability check only special-
                cases AST_IDENT targets and silently allows anything
                else through, so no sema-side change is needed.
            */
            node->type == AST_FIELD_EXPR
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



    if(match(p,TOKEN_KW_FOR))
    {
        /*
            Two for-loop forms share the 'for IDENT in ...' prefix:

                for IDENT in START..END { block }        (range)
                for IDENT in [ref] COLLECTION { block }   (collection)

            Range-for was the original (and only) form here. Stage 4
            adds collection iteration -- 'for n in nums' and
            'for n in ref nums' -- matching every call site actually
            seen in tests/stage4.kru (a plain array identifier,
            optionally preceded by 'ref'; no arbitrary-iterator
            desugaring, no destructuring). Both forms parse the same
            leading expression and then branch on whether '..'
            follows: if it does, it's the range form (END is parsed
            next, exclusive, as before); if it doesn't, the already-
            parsed expression *is* the collection (bare identifier or
            'ref identifier', since parse_expression already handles
            'ref x' as a unary form).

            node->op distinguishes the two forms for codegen:
            TOKEN_DOT_DOT for range, TOKEN_KW_IN for collection.
        */

        ASTNode* node =
            make_node(AST_FOR_STMT);


        if(!check(p,TOKEN_IDENTIFIER))
        {
            error(
                p,
                "K1029: expected loop variable name after 'for'"
                );
        }
        else
        {
            ast_set_name(
                node,
                p->current.start,
                p->current.length
                );

            advance(p);
        }


        expect(
            p,
            TOKEN_KW_IN,
            "K1030: expected 'in' after for-loop variable"
            );


        ASTNode* first_expr =
            parse_expression(p);


        if(match(p,TOKEN_DOT_DOT))
        {
            node->op = TOKEN_DOT_DOT;

            ast_add_child(
                node,
                first_expr
                );

            ast_add_child(
                node,
                parse_expression(p)
                );

            ast_add_child(
                node,
                parse_block(p)
                );
        }
        else
        {
            node->op = TOKEN_KW_IN;

            ast_add_child(
                node,
                first_expr
                );

            ast_add_child(
                node,
                parse_block(p)
                );
        }

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
                        &overflow,
                        NULL,
                        NULL
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

                ASTNode* callee = ident;
                if(match(p, TOKEN_DOT))
                {
                    if(!check(p, TOKEN_IDENTIFIER))
                    {
                        error(p, "K1026: expected variant name after '.'");
                        break;
                    }
                    ASTNode* variant = make_node(AST_FIELD_EXPR);
                    ast_set_name(variant, p->current.start, p->current.length);
                    advance(p);
                    ast_add_child(variant, ident);
                    callee = variant;
                }
                if(match(p, TOKEN_LPAREN))
                {
                    ASTNode* ctor = make_node(AST_CALL_EXPR);
                    ast_add_child(ctor, callee);
                    if(!check(p, TOKEN_RPAREN))
                    {
                        do
                        {
                            if(!check(p, TOKEN_IDENTIFIER))
                            {
                                error(p, "K1027: expected payload binding name");
                                break;
                            }
                            ASTNode* binding = make_node(AST_IDENT);
                            ast_set_name(binding, p->current.start, p->current.length);
                            advance(p);
                            ast_add_child(ctor, binding);
                        }
                        while(match(p, TOKEN_COMMA) && !check(p, TOKEN_RPAREN));
                    }
                    expect(p, TOKEN_RPAREN, "K1008: expected ')'");
                    ast_add_child(arm, ctor);
                }
                else
                {
                    ast_add_child(arm, callee);
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
                        "K1036: expected type name"
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
                    "K1036: expected type name"
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
                "K1036: expected type name"
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
