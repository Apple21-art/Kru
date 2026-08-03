#ifndef KRU_LEXER_H
#define KRU_LEXER_H


#include <stdint.h>

#include "token.h"



typedef struct
{

    /*
        Original source buffer.

        Lexer does not own this memory.
    */

    const char* source;



    /*
        Current scanning position.
    */

    const char* cursor;



    /*
        Current source location.
    */

    uint32_t line;

    uint32_t column;



} Lexer;



/*
    Initialize lexer.

    Source must remain alive while tokens are being consumed.
*/

void lexer_init(
    Lexer* lexer,
    const char* source
    );



/*
    Consume next token.
*/

Token lexer_next_token(
    Lexer* lexer
    );



#endif