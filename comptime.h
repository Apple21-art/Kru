#ifndef KRU_COMPTIME_H
#define KRU_COMPTIME_H
#include "ast.h"
/* Fuel-bounded, pure compile-time evaluation. No I/O, heap, or foreign calls. */
int kru_evaluate_comptime(ASTNode* program);
#endif
