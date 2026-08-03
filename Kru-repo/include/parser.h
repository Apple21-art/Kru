#ifndef KRU_PARSER_H
#define KRU_PARSER_H

#include "ast.h"
#include "lexer.h"


ASTNode* parse_program(
    Lexer* lexer
    );


ASTNode* alpha_parse_program(
    Lexer* lexer
    );


#endif