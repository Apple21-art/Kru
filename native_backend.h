#ifndef KRU_NATIVE_BACKEND_H
#define KRU_NATIVE_BACKEND_H

#include "ast.h"

/*
    Direct native migration backend.

    Current target: x86-64 Linux ELF executable.
    Implements primitive integer widths through 128, f32/f64 and char,
    typed functions, control flow and supported aggregate values.
    This AST adapter precedes the planned typed HIR/MIR migration.
    See NATIVE_BACKEND.md for the exact tested boundary and Codex gaps.

    Unsupported AST shapes are rejected explicitly. This is intentionally
    narrow: the native path must never silently miscompile a Kru program.
*/
int native_backend_emit_x86_64_linux(ASTNode* root, const char* output_path, int check_heap);

#endif
