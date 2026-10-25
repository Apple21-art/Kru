#ifndef KRU_MODULES_H
#define KRU_MODULES_H
#include "ast.h"
/* Loads source imports and resolves public names to deterministic link names. */
int kru_resolve_modules(ASTNode* program,const char* source_path);
#endif
