#ifndef KRU_TOKEN_H
#define KRU_TOKEN_H


#include <stdint.h>



typedef enum
{

    TOKEN_EOF,

    TOKEN_ERROR,


    /*
        Keywords
    */

    TOKEN_KW_FN,

    TOKEN_KW_PUB,

    TOKEN_KW_RET,


    TOKEN_KW_LET,

    TOKEN_KW_VAR,


    TOKEN_KW_REF,

    TOKEN_KW_MUT,


    TOKEN_KW_CONST,


    TOKEN_KW_IF,

    TOKEN_KW_ELSE,


    TOKEN_KW_LOOP,

    TOKEN_KW_WHILE,


    TOKEN_KW_BREAK,

    TOKEN_KW_CONTINUE,


    TOKEN_KW_MATCH,


    TOKEN_KW_STRUCT,

    TOKEN_KW_ENUM,

    TOKEN_KW_TYPE,


    TOKEN_KW_ARENA,


    TOKEN_KW_UNSAFE,

    TOKEN_KW_SECURE,


    TOKEN_KW_USE,

    TOKEN_KW_MOD,


    TOKEN_KW_COMPTIME,

    TOKEN_KW_INLINE,
    TOKEN_KW_TRAIT,
    TOKEN_KW_IMPL,



    /*
        Identifiers / literals
    */

    TOKEN_IDENTIFIER,


    TOKEN_INT_LIT,

    TOKEN_FLOAT_LIT,

    TOKEN_STRING_LIT,

    TOKEN_CHAR_LIT,

    TOKEN_BOOL_LIT,


    TOKEN_DOC_COMMENT,



    /*
        Delimiters
    */

    TOKEN_LPAREN,

    TOKEN_RPAREN,


    TOKEN_LBRACE,

    TOKEN_RBRACE,


    TOKEN_LBRACKET,

    TOKEN_RBRACKET,


    TOKEN_COMMA,


    TOKEN_DOT,

    TOKEN_DOT_DOT,


    TOKEN_COLON,

    TOKEN_SEMICOLON,


    TOKEN_ARROW,

    TOKEN_FAT_ARROW,


    TOKEN_QUESTION,



    /*
        Binding / assignment
    */

    TOKEN_COLON_EQUALS,

    TOKEN_EQUALS,



    /*
        Memory

        ptr@
    */

    TOKEN_AT,



    /*
        Arithmetic
    */

    TOKEN_PLUS,

    TOKEN_MINUS,

    TOKEN_STAR,

    TOKEN_SLASH,

    TOKEN_PERCENT,



    /*
        Bitwise
    */

    TOKEN_AMP,

    TOKEN_PIPE,

    TOKEN_CARET,

    TOKEN_TILDE,


    TOKEN_SHL,

    TOKEN_SHR,



    /*
        Comparison
    */

    TOKEN_EQ_EQ,

    TOKEN_BANG_EQUAL,


    TOKEN_LT,

    TOKEN_GT,


    TOKEN_LT_EQUAL,

    TOKEN_GT_EQUAL,



    /*
        Logical
    */

    TOKEN_BANG,

    TOKEN_AND_AND,

    TOKEN_OR_OR,



    /*
        Compound assignment
    */

    TOKEN_PLUS_EQUALS,

    TOKEN_MINUS_EQUALS,

    TOKEN_STAR_EQUALS,

    TOKEN_SLASH_EQUALS,

    TOKEN_PERCENT_EQUALS,


    TOKEN_AMP_EQUALS,

    TOKEN_PIPE_EQUALS,

    TOKEN_CARET_EQUALS,


    TOKEN_SHL_EQUALS,

    TOKEN_SHR_EQUALS



} TokenType;



typedef struct
{

    TokenType type;


    const char* start;


    uint32_t length;


    uint32_t line;

    uint32_t column;


} Token;



#endif