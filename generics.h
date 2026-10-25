#ifndef KRU_GENERICS_H
#define KRU_GENERICS_H
#include "ast.h"
/* Deterministic structural instantiation; template declarations stay retained. */
int kru_instantiate_generics(ASTNode* program);
#endif
