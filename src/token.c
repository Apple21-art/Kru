#include "../include/token.h"



const char* token_type_name(
    TokenType type
    )
{
    switch(type)
    {

    case TOKEN_EOF:
        return "EOF";

    case TOKEN_ERROR:
        return "ERROR";


    case TOKEN_KW_FN:
        return "fn";

    case TOKEN_KW_PUB:
        return "pub";

    case TOKEN_KW_RET:
        return "ret";


    case TOKEN_KW_LET:
        return "let";

    case TOKEN_KW_VAR:
        return "var";


    case TOKEN_KW_REF:
        return "ref";

    case TOKEN_KW_MUT:
        return "mut";


    case TOKEN_KW_CONST:
        return "const";


    case TOKEN_KW_IF:
        return "if";

    case TOKEN_KW_ELSE:
        return "else";

    case TOKEN_KW_WHILE:
        return "while";

    case TOKEN_KW_LOOP:
        return "loop";


    case TOKEN_KW_BREAK:
        return "break";

    case TOKEN_KW_CONTINUE:
        return "continue";


    case TOKEN_KW_MATCH:
        return "match";


    case TOKEN_KW_STRUCT:
        return "struct";

    case TOKEN_KW_ENUM:
        return "enum";

    case TOKEN_KW_TYPE:
        return "type";


    case TOKEN_KW_ARENA:
        return "arena";

    case TOKEN_KW_UNSAFE:
        return "unsafe";

    case TOKEN_KW_SECURE:
        return "secure";


    case TOKEN_KW_USE:
        return "use";

    case TOKEN_KW_MOD:
        return "mod";


    case TOKEN_KW_COMPTIME:
        return "comptime";

    case TOKEN_KW_INLINE:
        return "inline";

    case TOKEN_KW_TRAIT:
        return "trait";

    case TOKEN_KW_IMPL:
        return "impl";


    case TOKEN_IDENTIFIER:
        return "identifier";


    case TOKEN_INT_LIT:
        return "integer";

    case TOKEN_FLOAT_LIT:
        return "float";

    case TOKEN_STRING_LIT:
        return "string";

    case TOKEN_CHAR_LIT:
        return "char";

    case TOKEN_BOOL_LIT:
        return "bool";


    case TOKEN_LPAREN:
        return "(";

    case TOKEN_RPAREN:
        return ")";


    case TOKEN_LBRACE:
        return "{";

    case TOKEN_RBRACE:
        return "}";


    case TOKEN_LBRACKET:
        return "[";

    case TOKEN_RBRACKET:
        return "]";


    case TOKEN_COMMA:
        return ",";

    case TOKEN_DOT:
        return ".";

    case TOKEN_DOT_DOT:
        return "..";


    case TOKEN_COLON:
        return ":";

    case TOKEN_SEMICOLON:
        return ";";


    case TOKEN_ARROW:
        return "->";

    case TOKEN_FAT_ARROW:
        return "=>";


    case TOKEN_COLON_EQUALS:
        return ":=";

    case TOKEN_EQUALS:
        return "=";


    case TOKEN_AT:
        return "@";


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

    case TOKEN_TILDE:
        return "~";


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


    case TOKEN_BANG:
        return "!";


    case TOKEN_AND_AND:
        return "&&";

    case TOKEN_OR_OR:
        return "||";


    case TOKEN_PLUS_EQUALS:
        return "+=";

    case TOKEN_MINUS_EQUALS:
        return "-=";

    case TOKEN_STAR_EQUALS:
        return "*=";

    case TOKEN_SLASH_EQUALS:
        return "/=";

    case TOKEN_PERCENT_EQUALS:
        return "%=";


    case TOKEN_AMP_EQUALS:
        return "&=";

    case TOKEN_PIPE_EQUALS:
        return "|=";

    case TOKEN_CARET_EQUALS:
        return "^=";


    case TOKEN_SHL_EQUALS:
        return "<<=";

    case TOKEN_SHR_EQUALS:
        return ">>=";


    default:
        return "unknown";
    }
}