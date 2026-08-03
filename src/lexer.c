#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "../include/lexer.h"



static bool is_alpha_extra(
    char c
    )
{
    return
        isalpha((unsigned char)c)
        ||
        c == '_';
}



static bool is_ident_char(
    char c
    )
{
    return
        is_alpha_extra(c)
        ||
        isdigit((unsigned char)c);
}



static char advance(
    Lexer* lexer
    )
{
    char c = *lexer->cursor++;


    if(c == '\n')
    {
        lexer->line++;
        lexer->column = 1;
    }
    else
    {
        lexer->column++;
    }


    return c;
}



static char peek(
    Lexer* lexer
    )
{
    return *lexer->cursor;
}



static char peek_next(
    Lexer* lexer
    )
{
    if(*lexer->cursor == '\0')
        return '\0';


    return lexer->cursor[1];
}



static Token make_token(
    TokenType type,
    const char* start,
    uint32_t length,
    uint32_t line,
    uint32_t column
    )
{
    Token token;


    token.type = type;

    token.start = start;

    token.length = length;

    token.line = line;

    token.column = column;


    return token;
}



void lexer_init(
    Lexer* lexer,
    const char* source
    )
{
    lexer->source = source;

    lexer->cursor = source;

    lexer->line = 1;

    lexer->column = 1;
}





typedef struct
{

    const char* text;

    TokenType type;

} Keyword;



static const Keyword KEYWORDS[] =
    {

{"fn", TOKEN_KW_FN},

    {"pub", TOKEN_KW_PUB},

    {"ret", TOKEN_KW_RET},


    {"let", TOKEN_KW_LET},

    {"var", TOKEN_KW_VAR},


    {"ref", TOKEN_KW_REF},

    {"mut", TOKEN_KW_MUT},


    {"const", TOKEN_KW_CONST},


    {"if", TOKEN_KW_IF},

    {"else", TOKEN_KW_ELSE},


    {"loop", TOKEN_KW_LOOP},

    {"while", TOKEN_KW_WHILE},


    {"break", TOKEN_KW_BREAK},

    {"continue", TOKEN_KW_CONTINUE},


    {"match", TOKEN_KW_MATCH},


    {"struct", TOKEN_KW_STRUCT},

    {"enum", TOKEN_KW_ENUM},

    {"type", TOKEN_KW_TYPE},


    {"arena", TOKEN_KW_ARENA},


    {"unsafe", TOKEN_KW_UNSAFE},

    {"secure", TOKEN_KW_SECURE},


    {"use", TOKEN_KW_USE},

    {"mod", TOKEN_KW_MOD},


    {"comptime", TOKEN_KW_COMPTIME},

    {"inline", TOKEN_KW_INLINE},
    {"trait", TOKEN_KW_TRAIT},
    {"impl", TOKEN_KW_IMPL},


    {"true", TOKEN_BOOL_LIT},

{"false", TOKEN_BOOL_LIT}

};



static const uint32_t KEYWORD_COUNT =
    sizeof(KEYWORDS) /
    sizeof(KEYWORDS[0]);



static TokenType check_keyword(
    const char* start,
    uint32_t length
    )
{
    for(uint32_t i = 0;
         i < KEYWORD_COUNT;
         i++)
    {
        if(
            strlen(KEYWORDS[i].text) == length
            &&
            strncmp(
                start,
                KEYWORDS[i].text,
                length
                ) == 0
            )
        {
            return KEYWORDS[i].type;
        }
    }


    return TOKEN_IDENTIFIER;
}





static bool skip_trivia(
    Lexer* lexer,
    Token* doc
    )
{
    while(*lexer->cursor)
    {

        if(isspace((unsigned char)peek(lexer)))
        {
            advance(lexer);
            continue;
        }



        /*
            Documentation comment

            ///
        */

        if(
            peek(lexer) == '/'
            &&
            peek_next(lexer) == '/'
            &&
            lexer->cursor[2] == '/'
            )
        {
            uint32_t line = lexer->line;

            uint32_t column = lexer->column;

            const char* start = lexer->cursor;


            while(
                *lexer->cursor
                &&
                peek(lexer) != '\n'
                )
            {
                advance(lexer);
            }


            *doc =
                make_token(
                    TOKEN_DOC_COMMENT,
                    start,
                    (uint32_t)(lexer->cursor - start),
                    line,
                    column
                    );


            return true;
        }



        /*
            Normal comment
        */

        if(
            peek(lexer) == '/'
            &&
            peek_next(lexer) == '/'
            )
        {
            while(
                *lexer->cursor
                &&
                peek(lexer) != '\n'
                )
            {
                advance(lexer);
            }


            continue;
        }



        /*
            Block comment
        */

        if(
            peek(lexer) == '/'
            &&
            peek_next(lexer) == '*'
            )
        {
            advance(lexer);

            advance(lexer);


            while(
                *lexer->cursor
                &&
                !(
                    peek(lexer) == '*'
                    &&
                    peek_next(lexer) == '/'
                    )
                )
            {
                advance(lexer);
            }


            if(*lexer->cursor)
            {
                advance(lexer);
                advance(lexer);
            }


            continue;
        }



        break;
    }


    return false;
}





static bool is_dec_digit(
    char c
    )
{
    return
        isdigit((unsigned char)c)
        ||
        c == '_';
}



static bool is_hex_digit(
    char c
    )
{
    return
        isxdigit((unsigned char)c)
        ||
        c == '_';
}



static bool is_bin_digit(
    char c
    )
{
    return
        c == '0'
        ||
        c == '1'
        ||
        c == '_';
}



static bool is_oct_digit(
    char c
    )
{
    return
        (c >= '0' && c <= '7')
        ||
        c == '_';
}



static void consume_suffix(
    Lexer* lexer
    )
{
    while(is_ident_char(peek(lexer)))
        advance(lexer);
}

static Token lex_number(
    Lexer* lexer,
    const char* start,
    uint32_t line,
    uint32_t column
    )
{
    bool is_float = false;



    if(
        peek(lexer) == '0'
        &&
        (
            peek_next(lexer) == 'x'
            ||
            peek_next(lexer) == 'X'
            )
        )
    {
        advance(lexer);
        advance(lexer);


        while(is_hex_digit(peek(lexer)))
            advance(lexer);
    }


    else if(
        peek(lexer) == '0'
        &&
        (
            peek_next(lexer) == 'b'
            ||
            peek_next(lexer) == 'B'
            )
        )
    {
        advance(lexer);
        advance(lexer);


        while(is_bin_digit(peek(lexer)))
            advance(lexer);
    }


    else if(
        peek(lexer) == '0'
        &&
        (
            peek_next(lexer) == 'o'
            ||
            peek_next(lexer) == 'O'
            )
        )
    {
        advance(lexer);
        advance(lexer);


        while(is_oct_digit(peek(lexer)))
            advance(lexer);
    }


    else
    {
        while(is_dec_digit(peek(lexer)))
            advance(lexer);



        if(
            peek(lexer) == '.'
            &&
            isdigit((unsigned char)peek_next(lexer))
            )
        {
            is_float = true;

            advance(lexer);


            while(is_dec_digit(peek(lexer)))
                advance(lexer);
        }



        if(
            peek(lexer) == 'e'
            ||
            peek(lexer) == 'E'
            )
        {
            is_float = true;


            advance(lexer);


            if(
                peek(lexer) == '+'
                ||
                peek(lexer) == '-'
                )
            {
                advance(lexer);
            }


            while(is_dec_digit(peek(lexer)))
                advance(lexer);
        }
    }


    consume_suffix(lexer);



    return make_token(
        is_float
            ?
            TOKEN_FLOAT_LIT
            :
            TOKEN_INT_LIT,

        start,

        (uint32_t)(lexer->cursor - start),

        line,

        column
        );
}





static Token lex_string(
    Lexer* lexer,
    const char* start,
    uint32_t line,
    uint32_t column,
    bool raw
    )
{
    if(raw)
        advance(lexer);



    advance(lexer);



    while(
        *lexer->cursor
        &&
        peek(lexer) != '"'
        )
    {
        if(
            !raw
            &&
            peek(lexer) == '\\'
            &&
            peek_next(lexer)
            )
        {
            advance(lexer);
            advance(lexer);
        }
        else
        {
            advance(lexer);
        }
    }



    if(peek(lexer) == '"')
    {
        advance(lexer);
    }
    else
    {
        return make_token(
            TOKEN_ERROR,
            start,
            (uint32_t)(lexer->cursor - start),
            line,
            column
            );
    }



    return make_token(
        TOKEN_STRING_LIT,
        start,
        (uint32_t)(lexer->cursor - start),
        line,
        column
        );
}





static Token lex_char(
    Lexer* lexer,
    const char* start,
    uint32_t line,
    uint32_t column
    )
{
    advance(lexer);



    while(
        *lexer->cursor
        &&
        peek(lexer) != '\''
        )
    {
        if(
            peek(lexer) == '\\'
            &&
            peek_next(lexer)
            )
        {
            advance(lexer);
            advance(lexer);
        }
        else
        {
            advance(lexer);
        }
    }



    if(peek(lexer) == '\'')
    {
        advance(lexer);
    }
    else
    {
        return make_token(
            TOKEN_ERROR,
            start,
            (uint32_t)(lexer->cursor - start),
            line,
            column
            );
    }



    return make_token(
        TOKEN_CHAR_LIT,
        start,
        (uint32_t)(lexer->cursor - start),
        line,
        column
        );
}





static Token simple_token(
    TokenType type,
    const char* start,
    uint32_t line,
    uint32_t column
    )
{
    return make_token(
        type,
        start,
        1,
        line,
        column
        );
}





static Token two_token(
    Lexer* lexer,
    TokenType type,
    const char* start,
    uint32_t line,
    uint32_t column
    )
{
    advance(lexer);


    return make_token(
        type,
        start,
        2,
        line,
        column
        );
}

Token lexer_next_token(
    Lexer* lexer
    )
{
    Token doc;


    if(skip_trivia(lexer, &doc))
        return doc;



    uint32_t line = lexer->line;

    uint32_t column = lexer->column;

    const char* start = lexer->cursor;



    if(*lexer->cursor == '\0')
    {
        return make_token(
            TOKEN_EOF,
            start,
            0,
            line,
            column
            );
    }



    if(isdigit((unsigned char)peek(lexer)))
    {
        return lex_number(
            lexer,
            start,
            line,
            column
            );
    }



    if(
        peek(lexer) == 'r'
        &&
        peek_next(lexer) == '"'
        )
    {
        return lex_string(
            lexer,
            start,
            line,
            column,
            true
            );
    }



    if(peek(lexer) == '"')
    {
        return lex_string(
            lexer,
            start,
            line,
            column,
            false
            );
    }



    if(peek(lexer) == '\'')
    {
        return lex_char(
            lexer,
            start,
            line,
            column
            );
    }



    if(is_alpha_extra(peek(lexer)))
    {
        while(is_ident_char(peek(lexer)))
            advance(lexer);


        return make_token(
            check_keyword(
                start,
                (uint32_t)(lexer->cursor - start)
                ),

            start,

            (uint32_t)(lexer->cursor - start),

            line,

            column
            );
    }



    char c = advance(lexer);



    switch(c)
    {

    case '(':
        return simple_token(TOKEN_LPAREN,start,line,column);

    case ')':
        return simple_token(TOKEN_RPAREN,start,line,column);


    case '{':
        return simple_token(TOKEN_LBRACE,start,line,column);

    case '}':
        return simple_token(TOKEN_RBRACE,start,line,column);


    case '[':
        return simple_token(TOKEN_LBRACKET,start,line,column);

    case ']':
        return simple_token(TOKEN_RBRACKET,start,line,column);



    case ',':
        return simple_token(TOKEN_COMMA,start,line,column);


    case ';':
        return simple_token(TOKEN_SEMICOLON,start,line,column);



    case '.':
        if(peek(lexer) == '.')
            return two_token(
                lexer,
                TOKEN_DOT_DOT,
                start,
                line,
                column
                );

        return simple_token(
            TOKEN_DOT,
            start,
            line,
            column
            );



    case '@':
        return simple_token(
            TOKEN_AT,
            start,
            line,
            column
            );



    case ':':
        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_COLON_EQUALS,
                start,
                line,
                column
                );

        return simple_token(
            TOKEN_COLON,
            start,
            line,
            column
            );



    case '-':

        if(peek(lexer) == '>')
            return two_token(
                lexer,
                TOKEN_ARROW,
                start,
                line,
                column
                );


        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_MINUS_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_MINUS,
            start,
            line,
            column
            );



    case '+':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_PLUS_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_PLUS,
            start,
            line,
            column
            );



    case '*':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_STAR_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_STAR,
            start,
            line,
            column
            );



    case '/':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_SLASH_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_SLASH,
            start,
            line,
            column
            );



    case '%':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_PERCENT_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_PERCENT,
            start,
            line,
            column
            );



    case '=':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_EQ_EQ,
                start,
                line,
                column
                );


        if(peek(lexer) == '>')
            return two_token(
                lexer,
                TOKEN_FAT_ARROW,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_EQUALS,
            start,
            line,
            column
            );



    case '!':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_BANG_EQUAL,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_BANG,
            start,
            line,
            column
            );



    case '<':

        if(peek(lexer) == '<')
        {
            advance(lexer);

            if(peek(lexer) == '=')
                return two_token(
                    lexer,
                    TOKEN_SHL_EQUALS,
                    start,
                    line,
                    column
                    );


            return make_token(
                TOKEN_SHL,
                start,
                2,
                line,
                column
                );
        }


        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_LT_EQUAL,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_LT,
            start,
            line,
            column
            );



    case '>':

        if(peek(lexer) == '>')
        {
            advance(lexer);

            if(peek(lexer) == '=')
                return two_token(
                    lexer,
                    TOKEN_SHR_EQUALS,
                    start,
                    line,
                    column
                    );


            return make_token(
                TOKEN_SHR,
                start,
                2,
                line,
                column
                );
        }


        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_GT_EQUAL,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_GT,
            start,
            line,
            column
            );



    case '&':

        if(peek(lexer) == '&')
            return two_token(
                lexer,
                TOKEN_AND_AND,
                start,
                line,
                column
                );


        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_AMP_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_AMP,
            start,
            line,
            column
            );



    case '|':

        if(peek(lexer) == '|')
            return two_token(
                lexer,
                TOKEN_OR_OR,
                start,
                line,
                column
                );


        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_PIPE_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_PIPE,
            start,
            line,
            column
            );



    case '^':

        if(peek(lexer) == '=')
            return two_token(
                lexer,
                TOKEN_CARET_EQUALS,
                start,
                line,
                column
                );


        return simple_token(
            TOKEN_CARET,
            start,
            line,
            column
            );



    case '~':

        return simple_token(
            TOKEN_TILDE,
            start,
            line,
            column
            );

    }



    return make_token(
        TOKEN_ERROR,
        start,
        1,
        line,
        column
        );
}