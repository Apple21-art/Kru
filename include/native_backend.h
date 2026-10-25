#ifndef KRU_NATIVE_BACKEND_H
#define KRU_NATIVE_BACKEND_H

#include "ast.h"

/*
    Experimental direct native backend.

    Current target: x86-64 Linux ELF executable.
    Current language subset: zero-parameter i32/int main with i32 locals,
    integer/bool expressions, lexical blocks, conditionals and loops.
    Functions/calls, aggregates, pointers and other numeric types remain
    unsupported. See NATIVE_BACKEND.md for the exact tested boundary.

    Unsupported AST shapes are rejected explicitly. This is intentionally
    narrow: the native path must never silently miscompile a Kru program.
*/
int native_backend_emit_x86_64_linux(ASTNode* root, const char* output_path);

#endif
