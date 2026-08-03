#ifndef KRU_SEMA_H
#define KRU_SEMA_H


#include "ast.h"



/*
    Semantic analysis.

    Checks:

        - identifier resolution
        - scope rules
        - mutability rules
        - function visibility
        - invalid references

    Returns:

        0  -> valid
        -1 -> semantic error

*/

int sema_analyze(
    ASTNode* root
    );



#endif