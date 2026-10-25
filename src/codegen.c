#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../include/ast.h"
#include "../include/codegen.h"
#include "../include/diagnostic.h"


/*
    Forward declaration: codegen_expr_is_string (used by pr()'s
    codegen) needs to check a call expression's registered return
    type, but lookup_function_return's real definition sits much
    later in this file, in the function-signature-table section.
*/
static const char* lookup_function_return(
    const char* name,
    uint32_t name_len
    );


/*
    Set when codegen detects something that would be unsafe or
    undefined to hand to the C compiler as-is (e.g. a constant
    division by zero). codegen_generate() checks this after walking
    the tree and reports failure instead of silently emitting a
    C file that compiles clean but crashes (or is UB) at runtime.
*/
static bool codegen_had_error = false;

/*
    Stage 5: set by AST_FUNCTION right before it emits main's block,
    so AST_BLOCK (which owns emitting the opening '{') can inject the
    argc/argv capture as the very first statement inside main's body.
    Cleared by AST_BLOCK once consumed, so it never fires for a
    nested block inside main.
*/
static bool codegen_pending_argv_capture = false;

/*
    Unique suffix generator for the collection `for` loop's index
    variable (__kru_idxN), so nested/sibling collection for-loops in
    the same function never collide on variable name.
*/
static unsigned codegen_for_in_counter = 0;


/*
    Task B follow-up fix: `ref` bindings previously hardcoded the
    pointee's C type to "int64_t" regardless of what the target
    variable actually was declared as. That was silently safe only
    because every untyped variable happened to *also* default to
    int64_t; once literal suffixes (42u8) or the default-inference
    cascade can produce narrower types (uint8_t, int32_t, ...), that
    coincidence breaks and produces a mismatched pointer type -- e.g.
    `int64_t* reference = &value;` where `value` is actually
    `uint8_t`, which is undefined behavior and reads garbage through
    the pointer. This is a real, demonstrated bug (see overhaul
    report), not a hypothetical one.

    Minimal fix: track each declared variable's resolved C type by
    name as codegen emits `let`/`var` declarations, so a `ref`
    binding's pointee type can be looked up instead of assumed. This
    is intentionally a flat, function-scoped table (reset at the
    start of each AST_FUNCTION), not a full lexical-scope-aware
    symbol table like sema.c's -- shadowing a name in a nested block
    with a *different* type is a pre-existing gap this table doesn't
    close (falls back to "int64_t" like before in that case), but it
    fixes the common case this bug pass actually introduced.
*/
#define CODEGEN_VAR_TYPE_TABLE_SIZE 1024

typedef struct
{
    char name[128];
    char c_name[160];
    char c_type[32];

    /*
        Stage 4: arrays need more than a C type string to support
        `.len` and `for x in <array>` -- the declared element count
        isn't recoverable from "int32_t" the way it is from a type
        string like "int32_t*". Tracked alongside c_type in the same
        table (same name-keyed slots) rather than a separate table,
        since every array variable already needs a c_type entry too
        (its element type, used when a param/local array decays to a
        pointer expression elsewhere).
    */
    bool is_array;
    uint64_t array_len;
    char elem_c_type[32];

    /*
        Per-dimension lengths for multi-dimensional arrays (e.g.
        [[i32; 2]; 2] stores dims = {2, 2}, dim_count = 2), separate
        from array_len above (which stays the outer/dims[0] length,
        kept for every existing caller that only ever needed the
        outer size -- `.len`, `for x in arr`, decaying a param to a
        pointer). Added so AST_INDEX_EXPR can bounds-check every
        level of a chained index (grid[i][j]), not just the
        outermost: previously only dims[0] was tracked at all, so
        grid[i] was checked but grid[i][j]'s inner [j] silently
        compiled to unchecked C indexing.
    */
    uint64_t dims[8];
    int dim_count;

} CodegenVarType;

static CodegenVarType codegen_var_types[CODEGEN_VAR_TYPE_TABLE_SIZE];
static int codegen_var_type_count = 0;
static unsigned codegen_shadow_counter = 0;
static unsigned codegen_match_counter = 0;
static ASTNode* codegen_source_root = NULL;

/*
    Set once by codegen_var_type_slot the first time the table fills
    up, so the "table is full, further tracking for this function is
    silently degraded" state gets reported exactly once per function
    (via codegen_warning, from the AST_FUNCTION case) instead of
    either staying invisible or spamming a warning per overflow
    attempt.
*/
static bool codegen_var_type_table_full = false;


static void codegen_reset_var_types(void)
{
    codegen_var_type_count = 0;
    codegen_var_type_table_full = false;
    codegen_shadow_counter = 0;
    codegen_match_counter = 0;
}


/*
    Scope boundary support for shadowing correctness.

    The table used to be purely name-keyed: codegen_var_type_slot
    searched for an existing entry with the same name and reused
    (overwrote) it if found. That is correct for the common case
    (recording a variable's type once, maybe refining it later at
    the same scope) but wrong across nested scopes: a `let arr := ..`
    inside an `if` block that shadows an outer `arr` would overwrite
    the *outer* variable's slot, and nothing ever restored it when
    the block exited -- so code after the block kept seeing the
    inner shadow's (wrong) type/array-length for the outer name.
    Concretely, this made bounds-checked indexing (__kru_check_index)
    abort on valid, in-bounds accesses to the outer array once its
    name had been shadowed and un-shadowed.

    Fix: never overwrite in place. Every record call appends a new
    slot (codegen_var_type_slot below no longer searches for an
    existing name to reuse). Lookups scan backward (most recently
    appended match wins), so a shadow's slot naturally takes priority
    over an outer slot with the same name while both exist. Block
    scoping is handled by two calls bracketing each AST_BLOCK's
    children: codegen_var_type_scope_mark() before, which records the
    current table height, and codegen_var_type_scope_pop() after,
    which truncates the table back to that height -- dropping every
    slot (shadows and ordinary locals alike) declared inside the
    block that's ending, so lookups after the block naturally find
    whatever slot was visible before it (an outer declaration of the
    same name, now un-shadowed, or nothing, if the name didn't exist
    before the block). This relies on slots for a block's own
    contents being contiguous at the tail of the table, which holds
    because codegen is a single pre-order tree walk: a block's
    children are fully processed (and any deeper nested blocks fully
    entered and exited, popping their own slots) before control
    returns to pop this block's own mark.
*/

static int codegen_var_type_scope_mark(void)
{
    return codegen_var_type_count;
}


static void codegen_var_type_scope_pop(int mark)
{
    if(mark < 0 || mark > codegen_var_type_count)
        return;

    codegen_var_type_count = mark;
}


/*
    Always appends a new slot -- see the scoping note above for why
    this no longer searches for and reuses an existing same-name
    slot. Returns NULL (and sets codegen_var_type_table_full) if the
    table is exhausted; every caller already treats a NULL slot as
    "give up recording this one" rather than crashing.
*/
static CodegenVarType* codegen_var_type_slot(
    const char* name,
    uint32_t name_len
    )
{
    if(!name || name_len == 0)
        return NULL;

    if(name_len >= sizeof(codegen_var_types[0].name))
        return NULL;

    if(codegen_var_type_count >= CODEGEN_VAR_TYPE_TABLE_SIZE)
    {
        codegen_var_type_table_full = true;
        return NULL;
    }

    CodegenVarType* slot =
        &codegen_var_types[codegen_var_type_count++];

    memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';
    memcpy(slot->c_name, slot->name, name_len + 1);

    slot->c_type[0] = '\0';
    slot->is_array = false;
    slot->array_len = 0;
    slot->elem_c_type[0] = '\0';
    slot->dim_count = 0;

    return slot;
}


static void codegen_record_var_type(
    const char* name,
    uint32_t name_len,
    const char* c_type
    )
{
    if(!c_type || strlen(c_type) >= 32)
        return;

    CodegenVarType* slot =
        codegen_var_type_slot(name, name_len);

    if(!slot)
        return;

    memcpy(slot->c_type, c_type, strlen(c_type) + 1);

    /*
        A plain scalar/pointer recording always means "not an array"
        -- clears whatever a previous occupant of this slot may have
        left behind (see codegen_var_type_slot's comment).
    */
    slot->is_array = false;
    slot->array_len = 0;
    slot->elem_c_type[0] = '\0';
    slot->dim_count = 0;
}


/*
    Record an array variable: name, its element C type, and its full
    per-dimension shape. Kru arrays are always fixed-size, so every
    dimension's length is known at every declaration/parameter site
    -- this is what lets `.len` become a plain integer constant
    instead of a runtime field lookup (Chapter 19: array length is
    comptime-known), and what lets AST_INDEX_EXPR bounds-check every
    level of a chained index (grid[i][j]), not just the outermost.

    dims[0] is the outer/first dimension -- callers that only ever
    dealt with a single length (array_len, still set here as
    dims[0]) are unaffected; dims[1..dim_count-1] are the nested
    dimensions, absent (dim_count == 1) for a flat array.
*/
static void codegen_record_array_var_dims(
    const char* name,
    uint32_t name_len,
    const char* elem_c_type,
    const uint64_t* dims,
    int dim_count
    )
{
    if(!elem_c_type || strlen(elem_c_type) >= 32)
        elem_c_type = "int64_t";

    if(dim_count < 1)
        dim_count = 1;

    if(dim_count > 8)
        dim_count = 8;

    CodegenVarType* slot =
        codegen_var_type_slot(name, name_len);

    if(!slot)
        return;

    memcpy(slot->elem_c_type, elem_c_type, strlen(elem_c_type) + 1);
    slot->is_array = true;
    slot->array_len = dims ? dims[0] : 0;

    slot->dim_count = dim_count;

    for(int i = 0; i < dim_count; i++)
        slot->dims[i] = dims ? dims[i] : 0;

    /* c_type reflects the decayed pointer form, for any caller that
       looks up a plain type string for this array (e.g. a `ref`
       binding taking its address). */
    snprintf(slot->c_type, sizeof(slot->c_type), "%s*", elem_c_type);
}


/*
    Single-dimension convenience wrapper -- kept so every existing
    call site (flat arrays: `let nums := [4, 5, 6]`, and any future
    caller that only has one length) doesn't need to build a
    one-element dims array itself.
*/
static void codegen_record_array_var(
    const char* name,
    uint32_t name_len,
    const char* elem_c_type,
    uint64_t array_len
    )
{
    uint64_t dims[1] = { array_len };

    codegen_record_array_var_dims(
        name,
        name_len,
        elem_c_type,
        dims,
        1
        );
}


static bool codegen_source_uses_name(ASTNode* node, const char* name)
{
    if(!node)
        return false;
    size_t length = strlen(name);
    if(node->name && node->name_len == length &&
       strncmp(node->name, name, length) == 0)
        return true;
    for(uint32_t i = 0; i < node->child_count; i++)
        if(codegen_source_uses_name(node->children[i], name))
            return true;
    return false;
}

static void codegen_unique_name(char* name, size_t size, const char* prefix,
                                unsigned* counter)
{
    do {
        snprintf(name, size, "%s%u", prefix, (*counter)++);
    } while(codegen_source_uses_name(codegen_source_root, name));
}

static const char* codegen_lookup_var_name(const char* name, uint32_t length)
{
    for(int i = codegen_var_type_count - 1; i >= 0; i--)
        if(strlen(codegen_var_types[i].name) == length &&
           strncmp(codegen_var_types[i].name, name, length) == 0)
            return codegen_var_types[i].c_name;
    return NULL;
}

/* Register the new emitted name only after its initializer has been emitted. */
static void codegen_finish_binding(int mark, const char* emitted_name)
{
    for(int i = mark; i < codegen_var_type_count; i++)
        snprintf(codegen_var_types[i].c_name,
                 sizeof(codegen_var_types[i].c_name), "%s", emitted_name);
}

static const char* codegen_lookup_var_type(
    const char* name,
    uint32_t name_len
    )
{
    if(!name || name_len == 0)
        return NULL;

    /*
        Backward scan: with shadowing now represented as a newer
        slot coexisting alongside an outer one (see the scoping note
        above codegen_var_type_slot), the most recently appended
        match is the innermost-in-scope one and must win.
    */
    for(int i = codegen_var_type_count - 1; i >= 0; i--)
    {
        if((uint32_t)strlen(codegen_var_types[i].name) == name_len &&
            strncmp(codegen_var_types[i].name, name, name_len) == 0)
        {
            return codegen_var_types[i].c_type;
        }
    }

    return NULL;
}


/*
    Look up array info for a variable by name. Returns true (and
    fills the out params, either of which may be NULL) only if the
    variable is a known array -- e.g. an [i32; 4] let/var/param seen
    earlier in the current function.
*/
static bool codegen_lookup_array_info(
    const char* name,
    uint32_t name_len,
    const char** out_elem_c_type,
    uint64_t* out_array_len
    )
{
    if(!name || name_len == 0)
        return false;

    /* Backward scan -- see codegen_lookup_var_type above. */
    for(int i = codegen_var_type_count - 1; i >= 0; i--)
    {
        if((uint32_t)strlen(codegen_var_types[i].name) == name_len &&
            strncmp(codegen_var_types[i].name, name, name_len) == 0)
        {
            if(!codegen_var_types[i].is_array)
                return false;

            if(out_elem_c_type)
                *out_elem_c_type = codegen_var_types[i].elem_c_type;

            if(out_array_len)
                *out_array_len = codegen_var_types[i].array_len;

            return true;
        }
    }

    return false;
}


/*
    Per-dimension counterpart to codegen_lookup_array_info, for
    bounds-checking one level of a chained index (grid[i][j]).
    `depth` is 0-based from the outermost dimension (dims[0]).
    Returns true (and fills out_len) only if the variable is a known
    array AND has a tracked dimension at that depth -- false for an
    unknown variable, a non-array variable, or a depth beyond what
    was tracked at the declaration site (dim_count is capped at 8;
    see codegen_record_array_var_dims).
*/
static bool codegen_lookup_array_dim(
    const char* name,
    uint32_t name_len,
    int depth,
    uint64_t* out_len
    )
{
    if(!name || name_len == 0 || depth < 0)
        return false;

    /* Backward scan -- see codegen_lookup_var_type above. */
    for(int i = codegen_var_type_count - 1; i >= 0; i--)
    {
        if((uint32_t)strlen(codegen_var_types[i].name) == name_len &&
            strncmp(codegen_var_types[i].name, name, name_len) == 0)
        {
            if(!codegen_var_types[i].is_array)
                return false;

            if(depth >= codegen_var_types[i].dim_count)
                return false;

            if(out_len)
                *out_len = codegen_var_types[i].dims[depth];

            return true;
        }
    }

    return false;
}


/*
    Walks down an index-chain base expression (the children[0] of an
    AST_INDEX_EXPR being codegen'd) to find two things: the root
    identifier the chain is ultimately indexing into, and how many
    index operations were already applied to reach `base` from that
    root -- which is exactly the dimension depth the *current* index
    (the one this base belongs to) is checking. E.g. for `grid[i][j]`,
    when codegen reaches the outer AST_INDEX_EXPR (the `[j]`), its
    base is the inner AST_INDEX_EXPR (`grid[i]`); walking that base
    finds root `grid` at depth 1 (one AST_INDEX_EXPR -- the `[i]` --
    was already consumed getting there), so `[j]` is checked against
    grid's dims[1]. For the inner AST_INDEX_EXPR itself (`grid[i]`),
    its base is plain `grid` (AST_IDENT, depth 0 immediately), so
    `[i]` is checked against dims[0].

    Returns false (root/depth unset) if the chain doesn't bottom out
    at a plain identifier -- e.g. indexing through a deref, a struct
    field, or a call's return value, none of which this bootstrap
    compiler tracks a shape for. That's an existing, documented
    coverage gap (see the AST_INDEX_EXPR case), not something this
    walk is meant to close.
*/
static bool codegen_index_chain_depth(
    ASTNode* base,
    ASTNode** out_root,
    int* out_depth
    )
{
    int depth = 0;

    while(base && base->type == AST_INDEX_EXPR)
    {
        depth++;

        base =
            base->child_count > 0 ? base->children[0] : NULL;
    }

    if(!base || base->type != AST_IDENT)
        return false;

    if(out_root)
        *out_root = base;

    if(out_depth)
        *out_depth = depth;

    return true;
}


/*
    Best-effort check for whether an expression evaluates to a
    string (`str` / const char*), so pr() can pick %s instead of
    always formatting its argument as an integer. Deliberately scoped
    to the shapes pr() is actually called with in the test suite --
    a string literal, a string-typed variable, or a call to a
    function (including the str_concat builtin) whose return type is
    str. Anything else falls through to the existing numeric path,
    preserving current behavior for every non-string caller.
*/
static bool codegen_expr_is_string(
    ASTNode* expr
    )
{
    if(!expr)
        return false;

    if(expr->type == AST_STRING_LIT)
        return true;

    if(expr->type == AST_IDENT)
    {
        const char* t =
            codegen_lookup_var_type(expr->name, expr->name_len);

        return t && strcmp(t, "const char*") == 0;
    }

    if(expr->type == AST_CALL_EXPR &&
        !expr->is_struct_lit &&
        expr->child_count &&
        expr->children[0] &&
        expr->children[0]->type == AST_IDENT)
    {
        ASTNode* callee = expr->children[0];

        if(callee->name_len == 10 &&
            strncmp(callee->name, "str_concat", 10) == 0)
        {
            return true;
        }

        const char* ret =
            lookup_function_return(callee->name, callee->name_len);

        return ret && strcmp(ret, "const char*") == 0;
    }

    return false;
}


static void codegen_error(
    const char* message,
    uint32_t line,
    uint32_t column
    )
{
    codegen_had_error = true;

    /*
        Task E: same treatment as parse.c's error() -- split the
        "K1039: division by constant zero" style string into
        code + message and route through the shared diagnostic_print()
        mechanism instead of a bare one-line fprintf. See parse.c's
        error() for the fuller explanation of this choice.

        TODO: explanation / suggested_fix text for each of codegen's
        distinct diagnostics.
    */

    const char* colon =
        strstr(message, ": ");

    char code[16] = "K0000";
    const char* msg_text = message;

    if(colon &&
        (size_t)(colon - message) < sizeof(code))
    {
        size_t code_len =
            (size_t)(colon - message);

        memcpy(code, message, code_len);
        code[code_len] = '\0';

        msg_text = colon + 2;
    }


    Diagnostic diag =
        {
            .severity = "error",
            .code = code,
            .message = msg_text,
            .line = line,
            .column = column,
            .underline_length = 1
        };

    diagnostic_print(&diag);
}


/*
    Non-fatal counterpart to codegen_error: reports something worth
    the developer's attention without failing the build or setting
    codegen_had_error. Currently used only for K2050 (the var-type
    table filling up mid-function -- see codegen_var_type_slot),
    which degrades bounds-check coverage for the rest of that
    function rather than breaking anything; a hard error would be
    the wrong severity for that.
*/
static void codegen_warning(
    const char* message,
    uint32_t line,
    uint32_t column
    )
{
    const char* colon =
        strstr(message, ": ");

    char code[16] = "K0000";
    const char* msg_text = message;

    if(colon &&
        (size_t)(colon - message) < sizeof(code))
    {
        size_t code_len =
            (size_t)(colon - message);

        memcpy(code, message, code_len);
        code[code_len] = '\0';

        msg_text = colon + 2;
    }


    Diagnostic diag =
        {
            .severity = "warning",
            .code = code,
            .message = msg_text,
            .line = line,
            .column = column,
            .underline_length = 1
        };

    diagnostic_print(&diag);
}


/*
    Forward declarations for type registries.
*/

static bool is_known_type(
    const char* name,
    uint32_t name_len
    );


/*
    Map Kru type names to C types.
*/
static const char* codegen_c_type(
    ASTNode* type_node
    )
{
    if(!type_node)
        return "int64_t";

    /*
        Stage 5: raw pointer type, `*T` -> C's `T*`. codegen_c_type
        returns a plain (non-owned) const char*, so a pointer type's
        stringified form can't just be built on the stack and
        returned -- it has to outlive this call. A small rotating
        pool of static buffers (same trick as the array elem_c_type
        buffers above) lets nested/sequential calls in one statement
        (e.g. a param list) each get a distinct, valid buffer without
        a real allocator.
    */

    if(type_node->op == TOKEN_STAR)
    {
        static char ptr_buf[8][80];
        static int ptr_buf_idx = 0;

        const char* inner =
            codegen_c_type(type_node->type_node);

        char* buf = ptr_buf[ptr_buf_idx];
        ptr_buf_idx = (ptr_buf_idx + 1) % 8;

        snprintf(buf, sizeof(ptr_buf[0]), "%s*", inner ? inner : "int64_t");

        return buf;
    }

    if(!type_node->name)
        return "int64_t";

    if(strncmp(type_node->name, "i8", 2) == 0 && type_node->name_len == 2)
        return "int8_t";
    if(strncmp(type_node->name, "i16", 3) == 0 && type_node->name_len == 3)
        return "int16_t";
    if(strncmp(type_node->name, "i32", 3) == 0 && type_node->name_len == 3)
        return "int32_t";
    if(strncmp(type_node->name, "i64", 3) == 0 && type_node->name_len == 3)
        return "int64_t";
    if(strncmp(type_node->name, "i128", 4) == 0 && type_node->name_len == 4)
        return "__int128";
    if(strncmp(type_node->name, "u8", 2) == 0 && type_node->name_len == 2)
        return "uint8_t";
    if(strncmp(type_node->name, "u16", 3) == 0 && type_node->name_len == 3)
        return "uint16_t";
    if(strncmp(type_node->name, "u32", 3) == 0 && type_node->name_len == 3)
        return "uint32_t";
    if(strncmp(type_node->name, "u64", 3) == 0 && type_node->name_len == 3)
        return "uint64_t";
    if(strncmp(type_node->name, "u128", 4) == 0 && type_node->name_len == 4)
        return "unsigned __int128";
    if(strncmp(type_node->name, "isize", 5) == 0 && type_node->name_len == 5)
        return "intptr_t";
    if(strncmp(type_node->name, "usize", 5) == 0 && type_node->name_len == 5)
        return "uintptr_t";
    if(strncmp(type_node->name, "f32", 3) == 0 && type_node->name_len == 3)
        return "float";
    if(strncmp(type_node->name, "f64", 3) == 0 && type_node->name_len == 3)
        return "double";
    if(strncmp(type_node->name, "bool", 4) == 0 && type_node->name_len == 4)
        return "bool";
    if(strncmp(type_node->name, "char", 4) == 0 && type_node->name_len == 4)
        return "uint32_t";
    if(strncmp(type_node->name, "str", 3) == 0 && type_node->name_len == 3)
        return "const char*";
    if(strncmp(type_node->name, "int", 3) == 0 && type_node->name_len == 3)
        return "int64_t";
    if(strncmp(type_node->name, "void", 4) == 0 && type_node->name_len == 4)
        return "void";

    /*
        If it's a known type (struct, enum, type alias),
        emit the name as-is so C typedefs resolve.
    */

    if(is_known_type(type_node->name, type_node->name_len))
        return type_node->name;

    /*
        Unknown type name — could be a generic type parameter
        (like T) or an unregistered type.
        Default to int64_t for safety.
    */

    return "int64_t";
}



/*
    Infer the C element type for an untyped array literal (`let nums
    := [4, 5, 6]`) or an array literal used directly as a call
    argument (`array_mutate([1, 2, 3])`), from its first element.
    Mirrors the same default-inference cascade already used for bare
    integer literals elsewhere in this file (Codex: unsuffixed
    integers default to i32) -- this is additive, not a new rule.
*/
static const char* codegen_infer_array_lit_elem_type(
    ASTNode* lit
    )
{
    if(!lit || lit->child_count == 0 || !lit->children[0])
        return "int32_t";

    ASTNode* first = lit->children[0];

    switch(first->type)
    {
    case AST_STRING_LIT:
        return "const char*";

    case AST_BOOL_LIT:
        return "bool";

    case AST_CHAR_LIT:
        return "uint32_t";

    case AST_FLOAT_LIT:
        return "double";

    case AST_INT_LIT:
        if(first->has_explicit_suffix && first->type_node)
            return codegen_c_type(first->type_node);

        return "int32_t";

    default:
        return "int32_t";
    }
}


/*
    Codex Ch.8: "The compiler verifies that 8080 fits in u16." This
    checks a literal initializer against its declared fixed-width
    integer type and reports an error instead of silently truncating
    (C itself will happily wrap uint16_t port = 99999 down to 34463
    with no diagnostic, which is not what the language promises).

    Returns false (and reports an error) only when type_node names a
    known fixed-width integer type AND the literal provably doesn't
    fit. Anything else (no type, non-literal initializer, unknown
    type name, i64/i128/isize/u128 -- not covered by this check) is
    left alone.

    Task D: lit->int_val is now a full uint64_t (previously a signed
    int64_t, which meant a u64 literal above INT64_MAX couldn't be
    represented at all, and this function's u64/usize branch could
    only degrade to "check it's non-negative"). u64/usize now get a
    real range check against the full unsigned 64-bit domain.
*/
static bool codegen_check_int_range(
    ASTNode* type_node,
    ASTNode* init_node,
    uint32_t line,
    uint32_t column
    )
{
    if(!type_node || !type_node->name || !init_node)
        return true;

    bool negate = false;
    ASTNode* lit = init_node;

    if(lit->type == AST_UNARY_EXPR &&
        lit->op == TOKEN_MINUS &&
        lit->child_count &&
        lit->children[0])
    {
        negate = true;
        lit = lit->children[0];
    }

    if(lit->type != AST_INT_LIT)
        return true;


    const char* name = type_node->name;
    uint32_t len = type_node->name_len;

    uint64_t magnitude = lit->int_val;


    /*
        Unsigned destination types: a negative literal never fits,
        full stop. Check that first, then range-check the magnitude
        directly in the unsigned domain (no signed intermediate that
        could itself overflow for large u64 magnitudes).
    */

    bool is_unsigned =
        (len == 2 && strncmp(name, "u8", 2) == 0) ||
        (len == 3 && strncmp(name, "u16", 3) == 0) ||
        (len == 3 && strncmp(name, "u32", 3) == 0) ||
        (len == 3 && strncmp(name, "u64", 3) == 0) ||
        (len == 5 && strncmp(name, "usize", 5) == 0);

    if(is_unsigned)
    {
        if(negate && magnitude != 0)
        {
            char msg[96];

            snprintf(
                msg,
                sizeof(msg),
                "K1040: literal -%llu does not fit in declared type '%.*s'",
                (unsigned long long)magnitude,
                len,
                name
                );

            codegen_error(msg, line, column);

            return false;
        }


        uint64_t umax = 0;
        bool checked = true;

        if(len == 2 && strncmp(name, "u8", 2) == 0)
            umax = UINT8_MAX;
        else if(len == 3 && strncmp(name, "u16", 3) == 0)
            umax = UINT16_MAX;
        else if(len == 3 && strncmp(name, "u32", 3) == 0)
            umax = UINT32_MAX;
        else if((len == 3 && strncmp(name, "u64", 3) == 0) ||
                 (len == 5 && strncmp(name, "usize", 5) == 0))
            umax = UINT64_MAX;
        else
            checked = false;

        if(checked && magnitude > umax)
        {
            char msg[96];

            snprintf(
                msg,
                sizeof(msg),
                "K1040: literal %llu does not fit in declared type '%.*s'",
                (unsigned long long)magnitude,
                len,
                name
                );

            codegen_error(msg, line, column);

            return false;
        }

        return true;
    }


    /*
        Signed destination types (only i8/i16/i32 are checked here,
        matching the pre-existing scope of this function -- i64,
        i128, and isize were never checked before Task D and remain
        unchecked; that's real type-system work, not this pass).
    */

    bool checked = true;
    int64_t min = 0;
    int64_t max = 0;

    if(len == 2 && strncmp(name, "i8", 2) == 0)
    {
        min = INT8_MIN; max = INT8_MAX;
    }
    else if(len == 3 && strncmp(name, "i16", 3) == 0)
    {
        min = INT16_MIN; max = INT16_MAX;
    }
    else if(len == 3 && strncmp(name, "i32", 3) == 0)
    {
        min = INT32_MIN; max = INT32_MAX;
    }
    else
    {
        checked = false;
    }

    if(!checked)
        return true;


    /*
        magnitude is the literal's unsigned value as written (e.g.
        "128" in "-128"). A positive magnitude up to INT64_MAX casts
        down safely; negate then applies the sign. This mirrors the
        pre-Task-D behavior for these three types exactly (they never
        approached the u64 range this function now also handles).
    */

    int64_t v =
        negate
            ? -(int64_t)magnitude
            : (int64_t)magnitude;

    if(v < min || v > max)
    {
        char msg[96];

        snprintf(
            msg,
            sizeof(msg),
            "K1040: literal %lld does not fit in declared type '%.*s'",
            (long long)v,
            len,
            name
            );

        codegen_error(msg, line, column);

        return false;
    }


    return true;
}



/*
    Emit a C array declarator for a Kru array type: "elem_t name[N]"
    (no trailing ';' or initializer -- caller adds those). Handles
    nested array types like [[i32; 4]; 8] by walking down through
    each TOKEN_LBRACKET-marked AST_TYPE level and printing one [N]
    per dimension, ending in the base scalar/named type.
*/
/*
    Shared dimension-parsing walk: type_node for a Kru array type is
    a chain of nested TOKEN_LBRACKET nodes (outermost first, e.g.
    [[i32; 2]; 2] is an outer [2] wrapping an inner [2] wrapping the
    base i32), each carrying its length in int_val. Walks that chain
    into out_dim_nodes (caller-provided, at least 8 entries) and
    returns how many dimensions were found, with *out_base_type_node
    left pointing at the underlying scalar type node (i32 above) once
    the chain bottoms out. Shared by codegen_emit_array_decl (which
    only needs the dimension lengths, to emit "[2][2]") and the three
    array-registration call sites (which also need the full dims[]
    to track every level for AST_INDEX_EXPR's bounds checking) so
    both always agree on what a given declaration's shape is.
*/
static int codegen_parse_array_dims(
    ASTNode* type_node,
    ASTNode** out_dim_nodes,
    ASTNode** out_base_type_node
    )
{
    int dim_count = 0;

    ASTNode* cursor = type_node;

    while(cursor &&
           cursor->op == TOKEN_LBRACKET &&
           dim_count < 8)
    {
        out_dim_nodes[dim_count++] = cursor;
        cursor = cursor->type_node;
    }

    if(out_base_type_node)
        *out_base_type_node = cursor;

    return dim_count;
}


static void codegen_emit_array_decl(
    FILE* out,
    ASTNode* type_node,
    const char* name,
    uint32_t name_len
    )
{
    ASTNode* dims[8];

    ASTNode* base_type_node = NULL;

    int dim_count =
        codegen_parse_array_dims(
            type_node,
            dims,
            &base_type_node
            );

    const char* base_type =
        codegen_c_type(base_type_node);

    fprintf(
        out,
        "%s %.*s",
        base_type,
        name_len,
        name
        );

    for(int i = 0; i < dim_count; i++)
    {
        fprintf(
            out,
            "[%llu]",
            (unsigned long long)dims[i]->int_val
            );
    }

    /*
        `.len` on this array resolves to a compile-time integer
        constant (see AST_FIELD_EXPR), so a Kru function that only
        ever reads an array's length -- never an element -- produces
        C that never references the array symbol itself. That's a
        real, silent-but-correct outcome of the array->constant
        optimization, not a genuine unused-variable bug in the Kru
        source (tests/stage4.kru's array_len() is exactly this
        case) -- and the test suite builds with -Werror, so it must
        not warn. GNU attribute rather than a synthesized `(void)`
        statement, since this helper only emits the declarator, not
        a full statement its caller can safely follow with one.
    */

    fprintf(out, " __attribute__((unused))");
}



/*
    Function signature table for type inference.
    Populated during codegen pre-pass.
*/

#define MAX_FUNCS 256


typedef struct {
    const char* name;
    uint32_t name_len;
    const char* ret_type;
} FuncSig;

static FuncSig func_table[MAX_FUNCS];
static uint32_t func_count = 0;


/*
    Enum type registry for Type.Variant access.
    Also used to distinguish known types from unknown ones.
*/

#define MAX_ENUMS 64
#define MAX_TYPES 128

typedef struct {
    const char* name;
    uint32_t name_len;
} TypeEntry;

static TypeEntry enum_table[MAX_ENUMS];
static uint32_t enum_count = 0;

static TypeEntry known_types[MAX_TYPES];
static uint32_t type_count = 0;


/*
    Boost pass: enum variant registry.

    Payload-carrying variants (Square(i32), Label(str)) previously had
    no real representation at all -- the constructor call codegen just
    discarded the tag and emitted the bare payload value (`Square(21)`
    -> `21`), so a variable holding one couldn't be told apart from a
    plain integer, and match couldn't compare it against anything.
    This registry tracks, per variant, which enum owns it and (if it
    carries a payload) the payload's C type, so enum decl, constructor
    calls, field access, and match arms can all agree on one tagged-
    union representation: `typedef struct { Tag tag; union { ... } as; } Name;`
    An enum with no payload variants at all keeps emitting a plain C
    enum exactly as before -- zero change for Color/ResultCode-style
    enums.
*/

#define MAX_VARIANTS 256

typedef struct {
    const char* enum_name;
    uint32_t enum_name_len;
    const char* variant_name;
    uint32_t variant_name_len;
    bool has_payload;
    const char* payload_c_type;
} VariantEntry;

static VariantEntry variant_table[MAX_VARIANTS];
static uint32_t variant_count = 0;

static TypeEntry payload_enum_table[MAX_ENUMS];
static uint32_t payload_enum_count = 0;


static void register_variant(
    const char* enum_name,
    uint32_t enum_name_len,
    const char* variant_name,
    uint32_t variant_name_len,
    bool has_payload,
    const char* payload_c_type
    )
{
    if(variant_count >= MAX_VARIANTS)
        return;

    variant_table[variant_count].enum_name = enum_name;
    variant_table[variant_count].enum_name_len = enum_name_len;
    variant_table[variant_count].variant_name = variant_name;
    variant_table[variant_count].variant_name_len = variant_name_len;
    variant_table[variant_count].has_payload = has_payload;
    variant_table[variant_count].payload_c_type = payload_c_type;
    variant_count++;
}


static VariantEntry* find_variant(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < variant_count; i++)
    {
        if(variant_table[i].variant_name_len == name_len &&
            strncmp(variant_table[i].variant_name, name, name_len) == 0)
        {
            return &variant_table[i];
        }
    }

    return NULL;
}


static bool enum_has_payload(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < payload_enum_count; i++)
    {
        if(payload_enum_table[i].name_len == name_len &&
            strncmp(payload_enum_table[i].name, name, name_len) == 0)
        {
            return true;
        }
    }

    return false;
}


static void register_enum(
    const char* name,
    uint32_t name_len
    )
{
    if(enum_count >= MAX_ENUMS)
        return;

    enum_table[enum_count].name = name;
    enum_table[enum_count].name_len = name_len;
    enum_count++;
}


static void register_type(
    const char* name,
    uint32_t name_len
    )
{
    if(type_count >= MAX_TYPES)
        return;

    known_types[type_count].name = name;
    known_types[type_count].name_len = name_len;
    type_count++;
}


static bool is_known_type(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < type_count; i++)
    {
        if(known_types[i].name_len == name_len &&
            strncmp(known_types[i].name, name, name_len) == 0)
        {
            return true;
        }
    }

    return false;
}


static bool is_enum_type(
    const char* name,
    uint32_t name_len
    )
{
    for(uint32_t i = 0; i < enum_count; i++)
    {
        if(enum_table[i].name_len == name_len &&
            strncmp(enum_table[i].name, name, name_len) == 0)
        {
            return true;
        }
    }

    return false;
}


static void register_function(
    const char* name,
    uint32_t name_len,
    const char* ret_type
    )
{
    if(func_count >= MAX_FUNCS)
        return;

    func_table[func_count].name = name;
    func_table[func_count].name_len = name_len;
    func_table[func_count].ret_type = ret_type;
    func_count++;
}


static const char* lookup_function_return(
    const char* name,
    uint32_t name_len
    )
{
    /*
        Stage 5 builtins: these aren't AST_FUNCTION nodes (they're
        libc wrappers emitted straight into the C preamble -- see
        the AST_PROGRAM preamble block), so they never populate
        func_table via collect_function_signatures. An untyped
        `let x := file_open(...)` still needs to know the real C
        return type to declare `x` correctly, so check this fixed
        table first.
    */

    static const struct
    {
        const char* name;
        const char* ret_type;
    }
    builtin_returns[] =
    {
        { "file_open",       "FILE*" },
        { "file_open_write", "FILE*" },
        { "file_read",       "const char*" },
        { "mem_alloc",       "void*" },
        { "mem_realloc",     "void*" },
        { "args_get",        "const char*" },
    };

    for(size_t i = 0;
         i < sizeof(builtin_returns) / sizeof(builtin_returns[0]);
         i++)
    {
        size_t blen = strlen(builtin_returns[i].name);

        if(blen == name_len &&
            strncmp(builtin_returns[i].name, name, name_len) == 0)
        {
            return builtin_returns[i].ret_type;
        }
    }


    for(uint32_t i = 0; i < func_count; i++)
    {
        if(func_table[i].name_len == name_len &&
            strncmp(func_table[i].name, name, name_len) == 0)
        {
            return func_table[i].ret_type;
        }
    }

    return NULL;
}


static void collect_function_signatures(
    ASTNode* program
    )
{
    if(!program || program->type != AST_PROGRAM)
        return;

    func_count = 0;
    enum_count = 0;
    type_count = 0;
    variant_count = 0;
    payload_enum_count = 0;

    /*
        Pass 1: register all type declarations
        (structs, enums, type aliases) so that function
        signatures can reference forward-declared types.
    */

    for(uint32_t i = 0;
         i < program->child_count;
         i++)
    {
        ASTNode* child =
            program->children[i];

        if(!child)
            continue;

        if(child->type == AST_ENUM_DECL)
        {
            register_enum(
                child->name,
                child->name_len
                );

            register_type(
                child->name,
                child->name_len
                );


            bool any_payload = false;

            for(uint32_t v = 0; v < child->child_count; v++)
            {
                ASTNode* variant = child->children[v];

                if(!variant || variant->type != AST_ENUM_VARIANT)
                    continue;

                bool has_payload =
                    variant->child_count > 0 &&
                    variant->children[0] != NULL;

                const char* payload_c_type =
                    has_payload
                        ? codegen_c_type(variant->children[0])
                        : NULL;

                register_variant(
                    child->name,
                    child->name_len,
                    variant->name,
                    variant->name_len,
                    has_payload,
                    payload_c_type
                    );

                if(has_payload)
                    any_payload = true;
            }

            if(any_payload &&
                payload_enum_count < MAX_ENUMS)
            {
                payload_enum_table[payload_enum_count].name = child->name;
                payload_enum_table[payload_enum_count].name_len = child->name_len;
                payload_enum_count++;
            }
        }
        else if(child->type == AST_STRUCT_DECL)
        {
            register_type(
                child->name,
                child->name_len
                );
        }
        else if(child->type == AST_TYPE_ALIAS)
        {
            register_type(
                child->name,
                child->name_len
                );
        }
    }

    /*
        Pass 2: register function signatures.
        Now all types are known so return types resolve correctly.
    */

    for(uint32_t i = 0;
         i < program->child_count;
         i++)
    {
        ASTNode* child =
            program->children[i];

        if(!child || child->type != AST_FUNCTION)
            continue;


        const char* ret_type = "int64_t";

        if(child->name && child->name_len == 4 &&
            strncmp(child->name, "main", 4) == 0)
        {
            ret_type = "int";
        }
        else if(child->type_node)
        {
            ret_type = codegen_c_type(child->type_node);
        }


        register_function(
            child->name,
            child->name_len,
            ret_type
            );
    }
}


/*
    Emit forward declarations for all functions
    so that forward references work in C.
*/
static void emit_function_prototypes(
    ASTNode* program,
    FILE* out
    )
{
    if(!program || program->type != AST_PROGRAM)
        return;

    for(uint32_t i = 0;
         i < program->child_count;
         i++)
    {
        ASTNode* fn =
            program->children[i];

        if(!fn || fn->type != AST_FUNCTION)
            continue;


        const char* ret_type = "int64_t";

        bool is_main =
            fn->name && fn->name_len == 4 &&
            strncmp(fn->name, "main", 4) == 0;

        if(is_main)
        {
            ret_type = "int";
        }
        else if(fn->type_node)
        {
            ret_type = codegen_c_type(fn->type_node);
        }


        fprintf(
            out,
            "%s %.*s(",
            ret_type,
            fn->name_len,
            fn->name
            );


        bool first_param = true;

        if(is_main)
        {
            /*
                Must match AST_FUNCTION's definition exactly (see the
                is_main branch there) -- otherwise C sees two
                conflicting declarations for main: this prototype's
                `int main()` vs the real definition's
                `int main(int argc, char** argv)`.
            */

            fprintf(out, "int argc, char** argv");
            first_param = false;
        }

        for(uint32_t j = 0;
             j < fn->child_count && !is_main;
             j++)
        {
            ASTNode* child =
                fn->children[j];

            if(!child || child->type != AST_PARAM)
                continue;


            if(!first_param)
                fprintf(out, ", ");

            first_param = false;


            if(child->type_node &&
                child->type_node->op == TOKEN_LBRACKET)
            {
                /*
                    Array parameter: must match the definition's
                    declarator exactly (element_type name[N], which
                    decays to element_type*), or C sees two
                    incompatible declarations for the same function
                    (int64_t here vs int32_t* in the real definition)
                    and refuses to compile. A prototype's parameter
                    name is optional in C, so the declarator alone
                    (no name) is fine here.
                */

                ASTNode* dims[8];
                int dim_count = 0;

                ASTNode* cursor = child->type_node;

                while(cursor &&
                       cursor->op == TOKEN_LBRACKET &&
                       dim_count < 8)
                {
                    dims[dim_count++] = cursor;
                    cursor = cursor->type_node;
                }

                fprintf(out, "%s", codegen_c_type(cursor));

                for(int d = 0; d < dim_count; d++)
                {
                    fprintf(
                        out,
                        "[%llu]",
                        (unsigned long long)dims[d]->int_val
                        );
                }

                continue;
            }


            const char* param_type = "int64_t";

            if(child->type_node)
                param_type = codegen_c_type(child->type_node);


            fprintf(
                out,
                "%s",
                param_type
                );
        }


        fprintf(
            out,
            ");\n"
            );
    }


    fprintf(
        out,
        "\n"
        );
}


static const char* operator_string(
    TokenType op
    )
{
    switch(op)
    {
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

    case TOKEN_AND_AND:
        return "&&";

    case TOKEN_OR_OR:
        return "||";

    default:
        return "?";
    }
}



static void codegen_node(
    ASTNode* node,
    FILE* out
    )
{
    if(!node)
        return;



    switch(node->type)
    {

    case AST_PROGRAM:
    {
        /*
            Pre-pass: collect function signatures for type inference.
        */

        collect_function_signatures(node);


        fprintf(
            out,
            "#include <stdio.h>\n"
            "#include <stdint.h>\n"
            "#include <stdbool.h>\n"
            "#include <string.h>\n"
            "#include <stdlib.h>\n\n"
            /*
                Interactive CLI support: read_int builtin
            */
            "static inline int64_t read_int(const char* prompt)\n"
            "{\n"
            "    if(prompt)\n"
            "    {\n"
            "        printf(\"%%s\", prompt);\n"
            "        fflush(stdout);\n"
            "    }\n"
            "    long long val = 0;\n"
            "    if(scanf(\"%%lld\", &val) != 1)\n"
            "    {\n"
            "        int c;\n"
            "        while((c = getchar()) != '\\n' && c != EOF);\n"
            "        return 0;\n"
            "    }\n"
            "    return (int64_t)val;\n"
            "}\n\n"
            /*
                Stage 4: str_eq / str_concat runtime support.
                str_len already mapped straight onto strlen() with no
                helper needed; these two need actual bodies.

                str_concat's ownership question (flagged as open in
                the Stage 4 gap analysis, since Kru's real String/
                ownership model -- Chapter 20, Chapter 24 -- isn't
                implemented by this bootstrap compiler yet): this
                allocates on the heap and returns the new buffer as a
                plain `str` (const char*), matching the Codex's "every
                concatenation is a visible function call, never a
                hidden allocation" principle -- the allocation is
                real, but it's caused by an explicit call the
                programmer wrote, not by an operator. Nothing in this
                bootstrap compiler frees it (no ownership/destructor
                tracking exists yet), so it lives for the rest of the
                process -- an accepted leak at this stage, not a
                silent one.
            */
            "static uint64_t __kru_str_concat_leaked_bytes = 0;\n\n"
            /*
                Shared by __kru_str_concat and file_read below -- both
                hand back a heap buffer this bootstrap compiler has no
                ownership/destructor tracking for (Stage 5+ scope; see
                the ownership note above), so both leak by design for
                now. One shared tracker/warning means a program mixing
                both calls gets one combined signal instead of two
                independent 64MB thresholds understating how much is
                actually outstanding.
            */
            "static inline void __kru_track_leak(uint64_t n)\n"
            "{\n"
            "    uint64_t before = __kru_str_concat_leaked_bytes;\n"
            "    __kru_str_concat_leaked_bytes += n;\n"
            "    if(before / (64ull * 1024 * 1024) !=\n"
            "        __kru_str_concat_leaked_bytes / (64ull * 1024 * 1024))\n"
            "    {\n"
            "        fprintf(\n"
            "            stderr,\n"
            "            \"Kru warning: %%llu MB total leaked so far "
            "(no ownership tracking yet -- avoid str_concat/file_read "
            "in long-running loops)\\n\",\n"
            "            (unsigned long long)\n"
            "                (__kru_str_concat_leaked_bytes / (1024 * 1024))\n"
            "            );\n"
            "    }\n"
            "}\n\n"
            "static inline bool __kru_str_eq(const char* a, const char* b)\n"
            "{\n"
            "    if(a == b) return true;\n"
            "    if(!a || !b) return false;\n"
            "    return strcmp(a, b) == 0;\n"
            "}\n\n"
            "static inline const char* __kru_str_concat(const char* a, const char* b)\n"
            "{\n"
            "    size_t la = a ? strlen(a) : 0;\n"
            "    size_t lb = b ? strlen(b) : 0;\n"
            "    char* buf = (char*)malloc(la + lb + 1);\n"
            "    if(!buf) return \"\";\n"
            "    if(a) memcpy(buf, a, la);\n"
            "    if(b) memcpy(buf + la, b, lb);\n"
            "    buf[la + lb] = '\\0';\n"
            "\n"
            "    __kru_track_leak((uint64_t)(la + lb + 1));\n"
            "\n"
            "    return buf;\n"
            "}\n\n"
            /*
                Runtime safety nets, added after the Stage 4 gap
                review (unchecked array indexing, unchecked runtime
                div/mod-by-zero, and the str_concat leak). Each of
                these turns a category of undefined behavior --
                silent memory corruption, a bare SIGFPE, an unbounded
                climb to OOM -- into a defined, loud failure instead:
                a clear stderr message plus abort() (or, for the
                leak, a periodic warning; see __kru_str_concat
                below). None of this changes behavior for a correct
                program -- it only changes what happens when
                something already wrong would otherwise go unnoticed,
                which matters far more for a process running for
                hours than for a script that runs for milliseconds.

                __kru_check_index backs AST_INDEX_EXPR: emitted only
                when the base is a variable with a comptime-known
                array length (Chapter 19 -- every Kru array is
                fixed-size), which covers the common case. An index
                expression on a base codegen can't resolve to a known
                array (e.g. through a raw pointer) still compiles to
                unchecked C indexing -- that gap is real and is not
                fixed by this pass.
            */
            "static inline int64_t __kru_check_index(\n"
            "    int64_t idx,\n"
            "    uint64_t len,\n"
            "    const char* var_name,\n"
            "    int line,\n"
            "    int col\n"
            "    )\n"
            "{\n"
            "    if(idx < 0 || (uint64_t)idx >= len)\n"
            "    {\n"
            "        fprintf(\n"
            "            stderr,\n"
            "            \"Kru runtime error: index %%lld out of bounds for "
            "'%%s' (length %%llu) at line %%d, column %%d\\n\",\n"
            "            (long long)idx,\n"
            "            var_name,\n"
            "            (unsigned long long)len,\n"
            "            line,\n"
            "            col\n"
            "            );\n"
            /*
                fflush before abort(): abort() doesn't flush stdio
                buffers (unlike exit()), so any legitimate output
                already pr()'d before this failure -- e.g. the "10"
                from an earlier arr[0] in the same run -- would
                otherwise be silently lost whenever stdout isn't
                line-buffered (i.e. whenever it's piped/redirected
                rather than an interactive terminal, which is the
                common case for a compiled program's output). Without
                this, the error message would be the only thing that
                survived, misleadingly suggesting nothing ran before
                the failure.
            */
            "        fflush(stdout);\n"
            "        abort();\n"
            "    }\n"
            "    return idx;\n"
            "}\n\n"
            /*
                Two bodies (integer vs floating) because C has no
                generic arithmetic -- __kru_checked_div below picks
                the right one via _Generic on the divisor's own type,
                so callers never choose manually. Both fail loudly on
                zero: integer div/mod-by-zero is undefined behavior
                in C (typically SIGFPE), and float division by zero
                is well-defined (inf/nan) but silently wrong for a
                Kru program, so it gets the same loud treatment for
                consistency, per the request driving this pass.
            */
            "static inline int64_t __kru_check_div_i(\n"
            "    int64_t v,\n"
            "    int line,\n"
            "    int col\n"
            "    )\n"
            "{\n"
            "    if(v == 0)\n"
            "    {\n"
            "        fprintf(\n"
            "            stderr,\n"
            "            \"Kru runtime error: division/modulo by zero "
            "at line %%d, column %%d\\n\",\n"
            "            line,\n"
            "            col\n"
            "            );\n"
            "        fflush(stdout);\n"
            "        abort();\n"
            "    }\n"
            "    return v;\n"
            "}\n\n"
            "static inline double __kru_check_div_f(\n"
            "    double v,\n"
            "    int line,\n"
            "    int col\n"
            "    )\n"
            "{\n"
            "    if(v == 0.0)\n"
            "    {\n"
            "        fprintf(\n"
            "            stderr,\n"
            "            \"Kru runtime error: floating-point division "
            "by zero at line %%d, column %%d\\n\",\n"
            "            line,\n"
            "            col\n"
            "            );\n"
            "        fflush(stdout);\n"
            "        abort();\n"
            "    }\n"
            "    return v;\n"
            "}\n\n"
            "#define __kru_checked_div(x, line, col) \\\n"
            "    (_Generic((x), \\\n"
            "        float: __kru_check_div_f, \\\n"
            "        double: __kru_check_div_f, \\\n"
            "        default: __kru_check_div_i \\\n"
            "    )((x), (line), (col)))\n\n"
            /*
                str_concat's leak (see the ownership note above --
                this compiler has no destructor/ownership tracking,
                so nothing frees these buffers) can't be fixed here;
                that needs the real ownership model, which is Stage
                5+ scope. What's cheap to add without risking a
                use-after-free from guessing wrong about a buffer's
                lifetime: track cumulative leaked bytes and print a
                one-time-per-64MB warning, so a long-running program
                gets a visible signal instead of silently climbing to
                OOM with no diagnostic at all. The counter itself
                (__kru_str_concat_leaked_bytes) is declared above,
                before __kru_str_concat, since it's used there.
            */
            /*
                Stage 5: dynamic allocation, raw pointers, file I/O,
                and CLI args. These are plain wrappers around libc --
                no special codegen dispatch needed (unlike `pr` or
                `str_len`), since the generic call-codegen path
                already emits `name(args)` for any callee it doesn't
                recognize as a struct/enum constructor, and a real C
                function with that exact name now exists right here.

                Size checks (mem_alloc/mem_realloc) and null checks
                (file ops) turn misuse into a defined NULL return
                instead of undefined behavior, per SECURITY.md's
                "checked arithmetic" requirement -- callers are
                expected to null-check (`if ptr == null`), same as
                the Codex examples do.
            */
            "static int64_t __kru_argc = 0;\n"
            "static char** __kru_argv = NULL;\n\n"
            "static inline void* mem_alloc(int64_t size)\n"
            "{\n"
            "    if(size <= 0) return NULL;\n"
            "    return malloc((size_t)size);\n"
            "}\n\n"
            "static inline void* mem_realloc(void* ptr, int64_t size)\n"
            "{\n"
            "    if(size <= 0)\n"
            "    {\n"
            "        free(ptr);\n"
            "        return NULL;\n"
            "    }\n"
            "    return realloc(ptr, (size_t)size);\n"
            "}\n\n"
            "static inline void mem_free(void* ptr)\n"
            "{\n"
            "    free(ptr);\n"
            "}\n\n"
            "static inline int64_t args_count(void)\n"
            "{\n"
            "    return __kru_argc;\n"
            "}\n\n"
            "static inline const char* args_get(int64_t idx)\n"
            "{\n"
            "    if(idx < 0 || idx >= __kru_argc) return \"\";\n"
            "    return __kru_argv[idx];\n"
            "}\n\n"
            "static inline FILE* file_open(const char* path)\n"
            "{\n"
            "    if(!path) return NULL;\n"
            "    return fopen(path, \"rb\");\n"
            "}\n\n"
            "static inline FILE* file_open_write(const char* path)\n"
            "{\n"
            "    if(!path) return NULL;\n"
            "    return fopen(path, \"wb\");\n"
            "}\n\n"
            "static inline const char* file_read(FILE* f)\n"
            "{\n"
            "    if(!f) return \"\";\n"
            "\n"
            "    long start = ftell(f);\n"
            "    if(start < 0) start = 0;\n"
            "\n"
            "    if(fseek(f, 0, SEEK_END) != 0) return \"\";\n"
            "    long size = ftell(f);\n"
            "    if(size < 0 || fseek(f, start, SEEK_SET) != 0) return \"\";\n"
            "\n"
            "    char* buf = (char*)malloc((size_t)size + 1);\n"
            "    if(!buf) return \"\";\n"
            "\n"
            "    size_t got = fread(buf, 1, (size_t)size, f);\n"
            "    buf[got] = '\\0';\n"
            "\n"
            "    __kru_track_leak((uint64_t)got + 1);\n"
            "\n"
            "    return buf;\n"
            "}\n\n"
            "static inline void file_write(FILE* f, const char* content)\n"
            "{\n"
            "    if(!f || !content) return;\n"
            "    fwrite(content, 1, strlen(content), f);\n"
            "}\n\n"
            "static inline void file_close(FILE* f)\n"
            "{\n"
            "    if(f) fclose(f);\n"
            "}\n\n"
            );


        /*
            Pass 1: emit type declarations (typedefs, structs, enums)
            so they're visible to function prototypes and consts.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(!child)
                continue;

            if(child->type == AST_TYPE_ALIAS ||
                child->type == AST_STRUCT_DECL ||
                child->type == AST_ENUM_DECL)
            {
                codegen_node(child, out);
            }
        }


        /*
            Pass 2: emit function prototypes.
        */

        emit_function_prototypes(node, out);


        /*
            Pass 3: emit const declarations.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(child && child->type == AST_CONST_DECL)
            {
                codegen_node(child, out);
            }
        }


        /*
            Pass 4: emit function bodies.
        */

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(child && child->type == AST_FUNCTION)
            {
                codegen_node(child, out);
            }
        }

        break;
    }



    case AST_CONST_DECL:
    {
        /*
            const NAME: type := value
            → static const type NAME = value;
        */

        const char* c_type = "int64_t";

        if(node->type_node)
            c_type = codegen_c_type(node->type_node);

        /*
            Avoid double-const: if c_type already starts with 'const ',
            don't add another 'static const' prefix.
            Also, if the initializer is a function call (non-constant),
            emit as 'static' instead of 'static const'.
        */

        bool is_call_init =
            (node->child_count &&
             node->children[0] &&
             node->children[0]->type == AST_CALL_EXPR);


        if(is_call_init)
        {
            /*
                Non-constant initializer (function call).
                C requires constant expressions for file-scope
                initializers. Defer to comptime evaluation (Stage 3+).
                Emit as a comment for now.
            */

            fprintf(
                out,
                "/* const %.*s := <runtime initializer> (deferred) */\n\n",
                node->name_len,
                node->name
                );

            break;
        }


        /*
            A file-scope `const` in Kru is a declaration, not a
            local the programmer forgot to reference -- it's normal
            for only some of a module's constants to be used by any
            given translation unit. Mark the C symbol unused so
            -Wunused-variable (run under -Werror by tests/run_tests.sh)
            doesn't turn a legitimately-unused constant into a build
            failure.
        */

        if(strncmp(c_type, "const ", 6) == 0)
        {
            fprintf(
                out,
                "static %s %.*s __attribute__((unused)) = ",
                c_type,
                node->name_len,
                node->name
                );
        }
        else
        {
            fprintf(
                out,
                "static const %s %.*s __attribute__((unused)) = ",
                c_type,
                node->name_len,
                node->name
                );
        }


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }
        else
        {
            fprintf(out, "0");
        }

        fprintf(
            out,
            ";\n\n"
            );

        break;
    }



    case AST_TYPE_ALIAS:
    {
        /*
            type Name := Target
            → typedef Target Name;
        */

        const char* c_type = "int64_t";

        if(node->type_node)
            c_type = codegen_c_type(node->type_node);

        fprintf(
            out,
            "typedef %s %.*s;\n\n",
            c_type,
            node->name_len,
            node->name
            );

        break;
    }



    case AST_STRUCT_DECL:
    {
        /*
            struct Name { field: type, ... }
            → typedef struct Name { type field; ... } Name;
        */

        fprintf(
            out,
            "typedef struct %.*s {\n",
            node->name_len,
            node->name
            );


        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* field =
                node->children[i];

            if(!field || field->type != AST_STRUCT_FIELD)
                continue;


            const char* field_type = "int64_t";

            if(field->type_node)
                field_type = codegen_c_type(field->type_node);


            fprintf(
                out,
                "    %s %.*s;\n",
                field_type,
                field->name_len,
                field->name
                );
        }


        fprintf(
            out,
            "} %.*s;\n\n",
            node->name_len,
            node->name
            );

        break;
    }



    case AST_ENUM_DECL:
    {
        bool has_payload =
            enum_has_payload(node->name, node->name_len);

        if(!has_payload)
        {
            /*
                enum Name { Variant1, Variant2, ... }
                → typedef enum { Variant1, Variant2, ... } Name;
                Unchanged from before -- plain tag-only enums
                (Color, ResultCode) don't need a tagged union.
            */

            fprintf(
                out,
                "typedef enum {\n"
                );


            for(uint32_t i = 0;
                 i < node->child_count;
                 i++)
            {
                ASTNode* variant =
                    node->children[i];

                if(!variant || variant->type != AST_ENUM_VARIANT)
                    continue;


                fprintf(
                    out,
                    "    %.*s,\n",
                    variant->name_len,
                    variant->name
                    );
            }


            fprintf(
                out,
                "} %.*s;\n\n",
                node->name_len,
                node->name
                );

            break;
        }


        /*
            Boost pass: at least one variant carries a payload, so
            Name needs a tagged union: a scoped tag enum
            (Name_Variant constants, since C enums are otherwise
            unscoped and could collide across two enums that both
            have an "Empty" variant) plus a union of the payload
            types, one member per payload-carrying variant.
        */

        fprintf(
            out,
            "typedef enum {\n"
            );

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* variant =
                node->children[i];

            if(!variant || variant->type != AST_ENUM_VARIANT)
                continue;

            fprintf(
                out,
                "    %.*s_%.*s,\n",
                node->name_len,
                node->name,
                variant->name_len,
                variant->name
                );
        }

        fprintf(
            out,
            "} %.*s__Tag;\n\n",
            node->name_len,
            node->name
            );

        fprintf(
            out,
            "typedef struct {\n    %.*s__Tag tag;\n    union {\n",
            node->name_len,
            node->name
            );

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* variant =
                node->children[i];

            if(!variant || variant->type != AST_ENUM_VARIANT)
                continue;

            if(variant->child_count == 0 || !variant->children[0])
                continue;

            fprintf(
                out,
                "        %s %.*s;\n",
                codegen_c_type(variant->children[0]),
                variant->name_len,
                variant->name
                );
        }

        fprintf(
            out,
            "    } as;\n} %.*s;\n\n",
            node->name_len,
            node->name
            );

        break;
    }



    case AST_FUNCTION:
    {
        codegen_reset_var_types();


        /*
            Emit return type using codegen_c_type.
            Special-case 'main' to emit 'int main()'.
        */

        const char* ret_type = "int64_t";

        bool is_main =
            node->name && node->name_len == 4 &&
            strncmp(node->name, "main", 4) == 0;

        if(is_main)
        {
            ret_type = "int";
        }
        else if(node->type_node)
        {
            ret_type = codegen_c_type(node->type_node);
        }


        fprintf(
            out,
            "%s %.*s(",
            ret_type,
            node->name_len,
            node->name
            );


        /*
            Separate parameters from the block.
            AST_PARAM nodes are children before the AST_BLOCK child.
        */

        bool first_param = true;

        if(is_main)
        {
            /*
                Stage 5: main always takes argc/argv in the emitted
                C, regardless of Kru's declared (always zero) params,
                so args_count()/args_get() elsewhere in the program
                have something to read. See codegen_pending_argv_capture
                below for how these reach that global state.
            */

            fprintf(out, "int argc, char** argv");
            first_param = false;
        }

        for(uint32_t i = 0;
             i < node->child_count && !is_main;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(!child || child->type != AST_PARAM)
                continue;


            if(!first_param)
                fprintf(out, ", ");

            first_param = false;


            /*
                Stage 4: array parameters were silently dropped to a
                scalar (`values: [i32; 4]` emitted as a bare
                `int64_t values`, discarding the type entirely, so
                every `values[i]`/`values.len` inside the function
                body operated on a fabricated int64_t). Mirror the
                array-let/var path: emit a real C array declarator
                (which decays to a pointer at the call boundary, same
                as C's own array parameters) and record the element
                type + length so `.len` and indexing resolve inside
                this function's body.
            */

            if(child->type_node &&
                child->type_node->op == TOKEN_LBRACKET)
            {
                codegen_emit_array_decl(
                    out,
                    child->type_node,
                    child->name,
                    child->name_len
                    );

                ASTNode* dim_nodes[8];
                ASTNode* base_type_node = NULL;

                int dim_count =
                    codegen_parse_array_dims(
                        child->type_node,
                        dim_nodes,
                        &base_type_node
                        );

                uint64_t dims[8];

                for(int d = 0; d < dim_count; d++)
                    dims[d] = dim_nodes[d]->int_val;

                codegen_record_array_var_dims(
                    child->name,
                    child->name_len,
                    codegen_c_type(base_type_node),
                    dims,
                    dim_count
                    );

                continue;
            }


            /*
                Use codegen_c_type for param types.
            */

            const char* param_type = "int64_t";

            if(child->type_node)
                param_type = codegen_c_type(child->type_node);

            fprintf(
                out,
                "%s %.*s",
                param_type,
                child->name_len,
                child->name
                );

            codegen_record_var_type(
                child->name,
                child->name_len,
                param_type
                );
        }

        fprintf(out, ") ");


        /*
            Emit the body (AST_BLOCK) and any other children.
        */

        if(is_main)
        {
            codegen_pending_argv_capture = true;
        }

        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            ASTNode* child =
                node->children[i];

            if(child && child->type == AST_PARAM)
                continue;

            codegen_node(
                child,
                out
                );
        }


        fprintf(
            out,
            "\n\n"
            );

        if(codegen_var_type_table_full)
        {
            char msg[160];

            snprintf(
                msg,
                sizeof(msg),
                "K2050: function '%.*s' declares more local/param "
                "names than the compiler can track; array bounds "
                "checking is unchecked for the overflow past "
                "here",
                node->name_len,
                node->name
                );

            codegen_warning(
                msg,
                node->line,
                node->column
                );
        }

        break;
    }



    case AST_BLOCK:
    {
        /*
            Scope boundary: see the long comment above
            codegen_var_type_slot for why this matters. Every name
            recorded while processing this block's children (locals,
            including any that shadow an outer name) is dropped when
            the block ends, so lookups after the block see whatever
            was visible before it.
        */
        int var_type_scope_mark =
            codegen_var_type_scope_mark();

        fprintf(
            out,
            "{\n"
            );

        if(codegen_pending_argv_capture)
        {
            codegen_pending_argv_capture = false;

            fprintf(
                out,
                "    __kru_argc = (int64_t)argc;\n"
                "    __kru_argv = argv;\n"
                );
        }


        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            fprintf(
                out,
                "    "
                );


            codegen_node(
                node->children[i],
                out
                );


            fprintf(
                out,
                ";\n"
                );
        }


        fprintf(
            out,
            "}\n"
            );

        codegen_var_type_scope_pop(var_type_scope_mark);

        break;
    }



    case AST_LET_STMT:
    case AST_VAR_STMT:
    {
        int binding_mark = codegen_var_type_count;
        char shadow_name[40];
        const char* emitted_name = node->name;
        uint32_t emitted_len = node->name_len;
        const char* previous = codegen_lookup_var_name(node->name, node->name_len);
        if(previous)
        {
            codegen_unique_name(shadow_name, sizeof(shadow_name),
                                "__kru_shadow_", &codegen_shadow_counter);
            emitted_name = shadow_name;
            emitted_len = (uint32_t)strlen(shadow_name);
        }

        if(node->type_node &&
            node->type_node->op == TOKEN_LBRACKET)
        {
            /*
                Array declaration: let nums: [i32; 4] := [...]
                → int32_t nums[4] = {...};
                C's array declarator puts the size after the name,
                so this can't reuse the plain "TYPE NAME = " path
                below.
            */

            codegen_emit_array_decl(
                out,
                node->type_node,
                emitted_name,
                emitted_len
                );

            fprintf(out, " = ");

            if(node->child_count &&
                node->children[0])
            {
                codegen_node(
                    node->children[0],
                    out
                    );
            }
            else
            {
                fprintf(out, "{0}");
            }

            /*
                Record every dimension's length + the base scalar
                type, so `.len`, `for x in <this array>`, and
                AST_INDEX_EXPR's per-level bounds checking (grid[i][j]
                checking i against dims[0] and j against dims[1]) can
                all resolve later in this function.
            */

            {
                ASTNode* dim_nodes[8];
                ASTNode* base_type_node = NULL;

                int dim_count =
                    codegen_parse_array_dims(
                        node->type_node,
                        dim_nodes,
                        &base_type_node
                        );

                uint64_t dims[8];

                for(int d = 0; d < dim_count; d++)
                    dims[d] = dim_nodes[d]->int_val;

                codegen_record_array_var_dims(
                    node->name,
                    node->name_len,
                    codegen_c_type(base_type_node),
                    dims,
                    dim_count
                    );
            }

            codegen_finish_binding(binding_mark, emitted_name);
            if(previous && codegen_var_type_count == binding_mark)
                codegen_error("K1001: cannot track shadowed binding", node->line, node->column);
            break;
        }


        int is_reference = 0;


        if(node->child_count &&
            node->children[0] &&
            node->children[0]->type == AST_REF_EXPR)
        {
            is_reference = 1;
        }



        if(is_reference)
        {
            const char* pointee_type =
                node->type_node
                    ? codegen_c_type(node->type_node)
                    : "int64_t";


            if(!node->type_node &&
                node->children[0]->child_count &&
                node->children[0]->children[0] &&
                node->children[0]->children[0]->type == AST_IDENT)
            {
                /*
                    No explicit type on the `ref` binding itself:
                    look up the actual variable being referenced
                    instead of assuming int64_t (see
                    codegen_lookup_var_type's comment above for why
                    this matters).
                */

                ASTNode* target =
                    node->children[0]->children[0];

                const char* looked_up =
                    codegen_lookup_var_type(
                        target->name,
                        target->name_len
                        );

                if(looked_up)
                    pointee_type = looked_up;
            }


            fprintf(
                out,
                "%s* %.*s = ",
                pointee_type,
                emitted_len,
                emitted_name
                );

            char ptr_type[40];

            snprintf(
                ptr_type,
                sizeof(ptr_type),
                "%s*",
                pointee_type
                );

            codegen_record_var_type(
                node->name,
                node->name_len,
                ptr_type
                );
        }
        else
        {
            const char* var_type = "int64_t";

            bool type_emitted = false;


            if(node->type_node)
            {
                var_type = codegen_c_type(node->type_node);

                if(node->child_count &&
                    node->children[0])
                {
                    codegen_check_int_range(
                        node->type_node,
                        node->children[0],
                        node->line,
                        node->column
                        );
                }
            }
            else if(node->child_count &&
                     node->children[0] &&
                     (
                         node->children[0]->type == AST_INT_LIT
                         ||
                         (
                             node->children[0]->type == AST_UNARY_EXPR
                             &&
                             node->children[0]->op == TOKEN_MINUS
                             &&
                             node->children[0]->child_count
                             &&
                             node->children[0]->children[0]
                             &&
                             node->children[0]->children[0]->type == AST_INT_LIT
                             )
                         ))
            {
                /*
                    Task B: no binding-level type annotation, but the
                    initializer is (possibly negated) an integer
                    literal, which by this point in parsing always
                    carries a type_node -- either an explicit suffix
                    (42u8) or the Codex default-inference cascade
                    (i32, promoted to i64/i128 if the value doesn't
                    fit). Use that instead of falling through to the
                    generic int64_t default below, which is exactly
                    the "untyped fallback should be i32, not int64_t"
                    fix this task calls for, scoped to where the
                    Codex rule actually applies (literal defaulting)
                    rather than changing codegen_c_type's blanket
                    fallback used by unrelated call sites (generic
                    type parameters, inferred return types, etc.).
                */

                ASTNode* lit =
                    (node->children[0]->type == AST_INT_LIT)
                        ? node->children[0]
                        : node->children[0]->children[0];

                if(lit->type_node && lit->has_explicit_suffix)
                    var_type = codegen_c_type(lit->type_node);
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_CALL_EXPR &&
                     !node->children[0]->is_struct_lit &&
                     node->children[0]->child_count &&
                     node->children[0]->children[0] &&
                     node->children[0]->children[0]->type == AST_IDENT)
            {
                /*
                    Infer type from function call return type:
                    let x := foo(...) → RetType x = ...
                */

                ASTNode* callee =
                    node->children[0]->children[0];

                const char* ret_type =
                    lookup_function_return(
                        callee->name,
                        callee->name_len
                        );

                if(ret_type)
                {
                    fprintf(
                        out,
                        "%s %.*s = ",
                        ret_type,
                        emitted_len,
                        emitted_name
                        );

                    codegen_record_var_type(
                        node->name,
                        node->name_len,
                        ret_type
                        );

                    type_emitted = true;
                }
                else
                {
                    /*
                        Boost pass: not a known function -- check
                        whether it's an enum variant constructor
                        instead (`let s := Square(21)`), so `s` gets
                        the enum's tagged-union type rather than
                        falling through to the int64_t default below
                        (which made it indistinguishable from a plain
                        integer and unmatchable in a later `match`).
                    */

                    VariantEntry* variant =
                        find_variant(callee->name, callee->name_len);

                    if(variant &&
                        variant->has_payload &&
                        enum_has_payload(
                            variant->enum_name,
                            variant->enum_name_len
                            ))
                    {
                        fprintf(
                            out,
                            "%.*s %.*s = ",
                            variant->enum_name_len, variant->enum_name,
                            node->name_len, node->name
                            );

                        /*
                            codegen_record_var_type null-terminates
                            via strlen() internally, but enum_name
                            points into the source buffer and is only
                            length-delimited -- copy it into a small
                            null-terminated buffer first rather than
                            handing over unterminated source text.
                        */

                        char enum_name_buf[64];

                        uint32_t copy_len =
                            variant->enum_name_len < sizeof(enum_name_buf) - 1
                                ? variant->enum_name_len
                                : sizeof(enum_name_buf) - 1;

                        memcpy(enum_name_buf, variant->enum_name, copy_len);
                        enum_name_buf[copy_len] = '\0';

                        codegen_record_var_type(
                            node->name,
                            node->name_len,
                            enum_name_buf
                            );

                        type_emitted = true;
                    }
                }
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_CALL_EXPR &&
                     node->children[0]->is_struct_lit &&
                     node->children[0]->child_count &&
                     node->children[0]->children[0])
            {
                /*
                    Infer type from struct literal:
                    let x := Foo { ... } → Foo x = ...
                */

                ASTNode* callee =
                    node->children[0]->children[0];

                if(callee->type == AST_IDENT && callee->name)
                {
                    fprintf(
                        out,
                        "%.*s %.*s = ",
                        callee->name_len,
                        callee->name,
                        emitted_len,
                        emitted_name
                        );

                    codegen_record_var_type(
                        node->name,
                        node->name_len,
                        callee->name
                        );

                    type_emitted = true;
                }
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_STRING_LIT)
            {
                /*
                    let greeting := "hello" → const char* greeting = "hello";
                    Previously fell through to the int64_t default
                    below, emitting `int64_t greeting = "hello";` --
                    a string literal assigned to an integer, which
                    doesn't even compile.
                */

                fprintf(
                    out,
                    "const char* %.*s = ",
                    emitted_len,
                    emitted_name
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    "const char*"
                    );

                type_emitted = true;
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_BOOL_LIT)
            {
                fprintf(
                    out,
                    "bool %.*s = ",
                    emitted_len,
                    emitted_name
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    "bool"
                    );

                type_emitted = true;
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_CHAR_LIT)
            {
                fprintf(
                    out,
                    "uint32_t %.*s = ",
                    emitted_len,
                    emitted_name
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    "uint32_t"
                    );

                type_emitted = true;
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_FLOAT_LIT)
            {
                fprintf(
                    out,
                    "double %.*s = ",
                    emitted_len,
                    emitted_name
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    "double"
                    );

                type_emitted = true;
            }
            else if(node->child_count &&
                     node->children[0] &&
                     node->children[0]->type == AST_ARRAY_LIT)
            {
                /*
                    let nums := [4, 5, 6] → int32_t nums[3] = {4, 5, 6};
                    Previously fell through to the int64_t default
                    below, emitting `int64_t nums = { 4, 5, 6 };` --
                    an array initializer assigned to a scalar, which
                    doesn't compile either. C's own array declarator
                    syntax (size after the name) still fits the
                    shared "TYPE NAME = " + trailing-initializer
                    pattern this whole cascade uses, by putting the
                    "[N]" directly after the name here.
                */

                ASTNode* lit =
                    node->children[0];

                const char* elem_type =
                    codegen_infer_array_lit_elem_type(lit);

                uint64_t length =
                    lit->child_count;

                fprintf(
                    out,
                    "%s %.*s[%llu] __attribute__((unused)) = ",
                    elem_type,
                    emitted_len,
                    emitted_name,
                    (unsigned long long)length
                    );

                codegen_record_array_var(
                    node->name,
                    node->name_len,
                    elem_type,
                    length
                    );

                type_emitted = true;
            }


            if(!type_emitted)
            {
                fprintf(
                    out,
                    "%s %.*s = ",
                    var_type,
                    emitted_len,
                    emitted_name
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    var_type
                    );
            }
        }



        /* Type metadata was recorded while choosing the C declarator. Hide
           that new binding while emitting the initializer: Kru resolves a
           shadow's initializer against the preceding lexical environment. */
        int binding_end = codegen_var_type_count;
        CodegenVarType pending_binding;
        if(binding_end > binding_mark)
            pending_binding = codegen_var_types[binding_mark];
        codegen_var_type_count = binding_mark;

        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }
        else
        {
            fprintf(
                out,
                "0"
                );
        }


        if(binding_end > binding_mark)
            codegen_var_types[binding_mark] = pending_binding;
        codegen_var_type_count = binding_end;
        codegen_finish_binding(binding_mark, emitted_name);
        if(previous && binding_end == binding_mark)
            codegen_error("K1001: cannot track shadowed binding", node->line, node->column);
        break;
    }



    case AST_ASSIGN_STMT:
    {
        if(node->child_count < 2)
            break;


        codegen_node(
            node->children[0],
            out
            );


        /*
            Check if this is a compound assignment.
            node->op holds the compound operator token
            (TOKEN_EOF means plain assignment).
        */

        if(node->op != TOKEN_EOF)
        {
            /*
                Map compound token to C operator suffix.
            */

            const char* suffix = "";

            switch(node->op)
            {
            case TOKEN_PLUS_EQUALS:    suffix = "+=";  break;
            case TOKEN_MINUS_EQUALS:   suffix = "-=";  break;
            case TOKEN_STAR_EQUALS:    suffix = "*=";  break;
            case TOKEN_SLASH_EQUALS:   suffix = "/=";  break;
            case TOKEN_PERCENT_EQUALS: suffix = "%=";  break;
            case TOKEN_AMP_EQUALS:     suffix = "&=";  break;
            case TOKEN_PIPE_EQUALS:    suffix = "|=";  break;
            case TOKEN_CARET_EQUALS:   suffix = "^=";  break;
            case TOKEN_SHL_EQUALS:     suffix = "<<="; break;
            case TOKEN_SHR_EQUALS:     suffix = ">>="; break;
            default: suffix = "="; break;
            }

            fprintf(
                out,
                " %s ",
                suffix
                );
        }
        else
        {
            fprintf(
                out,
                " = "
                );
        }


        codegen_node(
            node->children[1],
            out
            );


        break;
    }



    case AST_RET_STMT:
    {
        fprintf(
            out,
            "return "
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }
        else
        {
            fprintf(
                out,
                "0"
                );
        }


        break;
    }



    case AST_IF_STMT:
    {
        /*
            children[0] = condition
            children[1] = then-block
            children[2] = else-block (optional)
        */

        fprintf(out, "if (");

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);

        fprintf(out, ") ");

        if(node->child_count > 1 && node->children[1])
            codegen_node(node->children[1], out);
        else
            fprintf(out, "{}\n");


        if(node->child_count > 2 && node->children[2])
        {
            fprintf(out, " else ");
            codegen_node(node->children[2], out);
        }

        break;
    }



    case AST_WHILE_STMT:
    {
        fprintf(out, "while (");

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);

        fprintf(out, ") ");

        if(node->child_count > 1 && node->children[1])
            codegen_node(node->children[1], out);
        else
            fprintf(out, "{}\n");

        break;
    }



    case AST_LOOP_STMT:
    {
        fprintf(out, "while (1) ");

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);
        else
            fprintf(out, "{}\n");

        break;
    }



    case AST_FOR_STMT:
    {
        int loop_scope_mark = codegen_var_type_scope_mark();
        if(node->op == TOKEN_KW_IN)
        {
            /*
                Stage 4 collection form:
                    for n in nums { ... }        (by value)
                    for n in ref nums { ... }     (by reference)

                Both lower to an indexed C loop over the tracked
                array length (Chapter 19's desugaring is iterator-
                based for the full language; this bootstrap compiler
                has no iterator/trait machinery yet, so it goes
                straight to the indexed form the Codex's own
                'ITERATING BY INDEX' section shows as equivalent).
                The by-ref form binds the loop variable as a pointer
                to each element, so `n@` inside the body reads/writes
                the array in place; the by-value form binds a copy.
            */

            ASTNode* collection_expr =
                node->child_count > 0 ? node->children[0] : NULL;

            ASTNode* body =
                node->child_count > 1 ? node->children[1] : NULL;

            bool by_ref = false;
            ASTNode* array_ident = collection_expr;

            if(array_ident &&
                array_ident->type == AST_REF_EXPR &&
                array_ident->child_count &&
                array_ident->children[0])
            {
                by_ref = true;
                array_ident = array_ident->children[0];
            }

            const char* elem_type = "int64_t";
            uint64_t array_len = 0;
            bool known = false;

            if(array_ident && array_ident->type == AST_IDENT)
            {
                known =
                    codegen_lookup_array_info(
                        array_ident->name,
                        array_ident->name_len,
                        &elem_type,
                        &array_len
                        );
            }

            if(!known)
            {
                codegen_error(
                    "K1042: 'for ... in' requires a known fixed-size array",
                    node->line,
                    node->column
                    );

                break;
            }

            unsigned idx =
                codegen_for_in_counter++;

            fprintf(
                out,
                "for (int64_t __kru_idx%u = 0; __kru_idx%u < %lluLL; __kru_idx%u++) {\n",
                idx, idx,
                (unsigned long long)array_len,
                idx
                );

            if(by_ref)
            {
                fprintf(
                    out,
                    "%s* %.*s = &(",
                    elem_type,
                    node->name_len,
                    node->name
                    );

                codegen_node(array_ident, out);

                fprintf(
                    out,
                    ")[__kru_idx%u];\n",
                    idx
                    );

                char ptr_type[40];

                snprintf(
                    ptr_type,
                    sizeof(ptr_type),
                    "%s*",
                    elem_type
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    ptr_type
                    );
            }
            else
            {
                fprintf(
                    out,
                    "%s %.*s = (",
                    elem_type,
                    node->name_len,
                    node->name
                    );

                codegen_node(array_ident, out);

                fprintf(
                    out,
                    ")[__kru_idx%u];\n",
                    idx
                    );

                codegen_record_var_type(
                    node->name,
                    node->name_len,
                    elem_type
                    );
            }

            if(body)
            {
                /*
                    body is an AST_BLOCK, which normally emits its
                    own '{' ... '}'. Emit its statements directly
                    inside the loop we already opened above instead
                    of nesting another brace pair -- purely
                    cosmetic (an extra nested scope would still be
                    valid C), kept for output readability.

                    Because this bypasses the AST_BLOCK case, it also
                    bypasses that case's scope mark/pop (see the long
                    comment above codegen_var_type_slot) -- so it
                    needs its own, or a `let` inside this loop body
                    that shadows an outer name would leak past the
                    loop the same way the original bug did for `if`/
                    `while`/`loop` bodies.
                */

                int var_type_scope_mark =
                    codegen_var_type_scope_mark();

                for(uint32_t i = 0; i < body->child_count; i++)
                {
                    codegen_node(body->children[i], out);
                    fprintf(out, ";\n");
                }

                codegen_var_type_scope_pop(var_type_scope_mark);
            }

            fprintf(out, "}\n");
            codegen_var_type_scope_pop(loop_scope_mark);
            break;
        }


        /*
            for i in START..END { block }
            → for (int64_t i = START; i < END; i++) { block }
            Exclusive of END -- see the parser's comment on this
            same assumption.
        */

        ASTNode* start_expr =
            node->child_count > 0 ? node->children[0] : NULL;

        ASTNode* end_expr =
            node->child_count > 1 ? node->children[1] : NULL;

        ASTNode* body =
            node->child_count > 2 ? node->children[2] : NULL;

        fprintf(
            out,
            "for (int64_t %.*s = ",
            node->name_len,
            node->name
            );

        if(start_expr)
            codegen_node(start_expr, out);
        else
            fprintf(out, "0");

        fprintf(
            out,
            "; %.*s < ",
            node->name_len,
            node->name
            );

        if(end_expr)
            codegen_node(end_expr, out);
        else
            fprintf(out, "0");

        fprintf(
            out,
            "; %.*s++) ",
            node->name_len,
            node->name
            );

        codegen_record_var_type(
            node->name,
            node->name_len,
            "int64_t"
            );

        if(body)
            codegen_node(body, out);
        else
            fprintf(out, "{}\n");

        codegen_var_type_scope_pop(loop_scope_mark);
        break;
    }



    case AST_BREAK_STMT:
    {
        fprintf(out, "break");
        break;
    }



    case AST_CONTINUE_STMT:
    {
        fprintf(out, "continue");
        break;
    }



    case AST_EXPR_STMT:
    {
        if(node->child_count &&
            node->children[0])
        {
            ASTNode* expr =
                node->children[0];

            /*
                A struct literal used as a bare statement (e.g. a
                construct-only smoke test with no binding) computes a
                value that's then discarded. That's legitimate --
                unlike a stray `x + 1;` typo, it has an observable
                side effect worth keeping (exercising the literal's
                codegen path) -- so void it explicitly rather than
                letting -Wunused-value flag it.
            */

            if(expr->type == AST_CALL_EXPR && expr->is_struct_lit)
                fprintf(out, "(void)");

            codegen_node(
                expr,
                out
                );
        }

        break;
    }



    case AST_ARENA_STMT:
    {
        /*
            arena name { ... }
            Emit as a regular scoped block with a comment.
            Real arena memory is deferred.
        */

        fprintf(
            out,
            "/* arena %.*s */ {\n",
            node->name_len,
            node->name ? node->name : (const char*)""
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(out, "}\n");

        break;
    }



    case AST_BLOCK_EXPR:
    {
        /*
            Block expression: { stmts; expr }
            Emit as GNU C statement-expression: ({ ...; expr; })
            The last expression statement is the value.
        */

        fprintf(out, "({\n");


        /*
            children[0] is an AST_BLOCK.
            Emit its children directly (not the block wrapper).
        */

        if(node->child_count &&
            node->children[0] &&
            node->children[0]->type == AST_BLOCK)
        {
            ASTNode* block =
                node->children[0];

            /*
                Same bypass-the-AST_BLOCK-case situation as the
                `for n in <array>` loop body above -- this walks
                block's children directly rather than calling
                codegen_node(block, out), so it needs its own scope
                mark/pop or a `let` here that shadows an outer name
                would leak past this block expression.
            */

            int var_type_scope_mark =
                codegen_var_type_scope_mark();

            for(uint32_t i = 0;
                 i < block->child_count;
                 i++)
            {
                fprintf(out, "    ");

                codegen_node(
                    block->children[i],
                    out
                    );

                fprintf(out, ";\n");
            }

            codegen_var_type_scope_pop(var_type_scope_mark);
        }


        fprintf(out, "})");

        break;
    }



    case AST_MATCH_EXPR:
    {
        /*
            match scrutinee { pattern => block ... }
            Lower to if/else if chain.
        */

        if(node->child_count < 2)
            break;


        ASTNode* scrutinee =
            node->children[0];


        char match_name[40];
        codegen_unique_name(match_name, sizeof(match_name),
                            "__kru_match_", &codegen_match_counter);
        fprintf(out, "{ __auto_type %s = (", match_name);
        codegen_node(scrutinee, out);
        fprintf(out, ");\n");

        /*
            Emit each arm as: if (scrutinee == pattern) { block }
            except the last arm which may be a catch-all.
        */

        for(uint32_t i = 1;
             i < node->child_count;
             i++)
        {
            ASTNode* arm =
                node->children[i];

            if(!arm || arm->type != AST_MATCH_ARM)
                continue;


            int arm_scope_mark = codegen_var_type_scope_mark();
            if(i > 1)
                fprintf(out, "else ");


            ASTNode* pattern =
                arm->child_count > 0 ? arm->children[0] : NULL;

            ASTNode* body =
                arm->child_count > 1 ? arm->children[1] : NULL;


            /*
                Check for catch-all (variable binding pattern).
            */

            bool is_catchall =
                (pattern &&
                 pattern->type == AST_IDENT);


            if(is_catchall)
            {
                /*
                    Variable binding: bind scrutinee to variable.
                */

                fprintf(out, "{\n");

                /*
                    A match-arm catch-all binding, like a source-level
                    const, is legitimately allowed to go unused in the
                    arm body (e.g. `n => { ret 999 }` ignores the
                    bound value). Mark it unused so -Werror doesn't
                    turn a normal wildcard-style pattern into a build
                    failure.
                */

                fprintf(
                    out,
                    "__auto_type %.*s __attribute__((unused)) = ",
                    pattern->name_len,
                    pattern->name
                    );

                fprintf(out, "%s", match_name);

                fprintf(out, ";\n");
                const char* binding_type = "int64_t";
                if(scrutinee && scrutinee->type == AST_IDENT)
                {
                    const char* type = codegen_lookup_var_type(scrutinee->name, scrutinee->name_len);
                    if(type) binding_type = type;
                }
                codegen_record_var_type(pattern->name, pattern->name_len, binding_type);

                if(body)
                    codegen_node(body, out);


                fprintf(out, "}\n");
            }
            else if(
                pattern &&
                pattern->type == AST_CALL_EXPR &&
                pattern->child_count >= 1 &&
                pattern->children[0] &&
                pattern->children[0]->type == AST_IDENT
                )
            {
                /*
                    Boost pass: Variant(binding) pattern, e.g.
                    `Square(n) => { ret n * 2 }`. Parser builds this
                    as an AST_CALL_EXPR (variant name as callee, the
                    single binding identifier as its one argument) --
                    see parse_match's pattern parsing. Look the
                    variant up to know which enum/tag/payload type
                    it belongs to, compare the scrutinee's tag, and
                    declare the binding from its payload field.
                */

                ASTNode* variant_ident =
                    pattern->children[0];

                VariantEntry* variant =
                    find_variant(
                        variant_ident->name,
                        variant_ident->name_len
                        );

                ASTNode* binding =
                    pattern->child_count >= 2
                        ? pattern->children[1]
                        : NULL;

                if(variant && variant->has_payload)
                {
                    fprintf(out, "if ((");
                    fprintf(out, "%s", match_name);
                    fprintf(
                        out,
                        ").tag == %.*s_%.*s) {\n",
                        variant->enum_name_len, variant->enum_name,
                        variant->variant_name_len, variant->variant_name
                        );

                    if(binding && binding->type == AST_IDENT)
                    {
                        fprintf(
                            out,
                            "%s %.*s __attribute__((unused)) = (",
                            variant->payload_c_type,
                            binding->name_len,
                            binding->name
                            );
                        fprintf(out, "%s", match_name);
                        fprintf(
                            out,
                            ").as.%.*s;\n",
                            variant->variant_name_len,
                            variant->variant_name
                            );
                        codegen_record_var_type(binding->name, binding->name_len,
                                                variant->payload_c_type);
                    }

                    if(body)
                        codegen_node(body, out);

                    fprintf(out, "}\n");
                }
                else
                {
                    /*
                        Unknown variant -- fall back to a condition
                        that's always false rather than emitting
                        invalid C, so the arm is simply unreachable
                        instead of a compile failure.
                    */

                    fprintf(out, "if (0) {\n");
                    if(body)
                        codegen_node(body, out);
                    fprintf(out, "}\n");
                }
            }
            else if(
                pattern &&
                pattern->type == AST_FIELD_EXPR &&
                pattern->child_count &&
                pattern->children[0] &&
                pattern->children[0]->type == AST_IDENT &&
                enum_has_payload(
                    pattern->children[0]->name,
                    pattern->children[0]->name_len
                    )
                )
            {
                /*
                    Boost pass: Type.Variant pattern (no payload
                    capture, e.g. `Shape.Empty => ...`) matched
                    against a tagged-union enum -- compare the tag
                    field, not the whole struct (structs aren't
                    comparable with == in C).
                */

                fprintf(out, "if ((");
                fprintf(out, "%s", match_name);
                fprintf(
                    out,
                    ").tag == %.*s_%.*s) {\n",
                    pattern->children[0]->name_len,
                    pattern->children[0]->name,
                    pattern->name_len,
                    pattern->name
                    );

                if(body)
                    codegen_node(body, out);

                fprintf(out, "}\n");
            }
            else
            {
                fprintf(out, "if (");

                fprintf(out, "%s", match_name);

                fprintf(out, " == ");

                codegen_node(pattern, out);

                fprintf(out, ") {\n");


                if(body)
                    codegen_node(body, out);


                fprintf(out, "}\n");
            }
            codegen_var_type_scope_pop(arm_scope_mark);
        }
        fprintf(out, "}\n");

        break;
    }



    case AST_INT_LIT:
    {
        /*
            Task D: int_val is now uint64_t and can legitimately hold
            magnitudes above INT64_MAX (a u64/u128 literal). Casting
            those to (long long) would print a wrong, wrapped negative
            decimal into the generated C. Emit unsigned with a ULL
            suffix whenever the value doesn't fit signed 64-bit;
            ordinary signed-range literals print exactly as before.
        */

        if(node->int_val > (uint64_t)INT64_MAX)
        {
            fprintf(
                out,
                "%lluULL",
                (unsigned long long)node->int_val
                );
        }
        else
        {
            fprintf(
                out,
                "%lld",
                (long long)node->int_val
                );
        }

        break;
    }



    case AST_CHAR_LIT:
    {
        fprintf(
            out,
            "%lld",
            (long long)node->int_val
            );

        break;
    }



    case AST_BOOL_LIT:
    {
        fprintf(
            out,
            "%lld",
            (long long)(node->bool_val ? 1 : 0)
            );

        break;
    }



    case AST_NULL_LIT:
    {
        fprintf(
            out,
            "NULL"
            );

        break;
    }



    case AST_FLOAT_LIT:
    {
        fprintf(
            out,
            "%lf",
            node->float_val
            );

        break;
    }



    case AST_STRING_LIT:
    {
        /*
            Emit the raw string literal token.
            The token includes surrounding quotes.
        */

        fprintf(
            out,
            "%.*s",
            node->string_len,
            node->string_val
            );

        break;
    }



    case AST_IDENT:
    {
        const char* emitted_name = codegen_lookup_var_name(node->name, node->name_len);
        if(emitted_name)
            fprintf(out, "%s", emitted_name);
        else
            fprintf(out, "%.*s", node->name_len, node->name);
        break;
    }



    case AST_REF_EXPR:
    {
        fprintf(
            out,
            "&"
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        break;
    }



    case AST_DEREF_EXPR:
    {
        fprintf(
            out,
            "*("
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(
            out,
            ")"
            );


        break;
    }



    case AST_FIELD_EXPR:
    {
        /*
            expr.field → (expr).field
            But if expr is an enum type name, emit just
            the variant name (C enums are unscoped).
        */

        if(node->child_count &&
            node->children[0] &&
            node->children[0]->type == AST_IDENT &&
            node->children[0]->name)
        {
            const char* base_name = node->children[0]->name;
            uint32_t base_len = node->children[0]->name_len;

            if(is_enum_type(base_name, base_len))
            {
                if(enum_has_payload(base_name, base_len))
                {
                    /*
                        Boost pass: Shape.Empty used as a *value*
                        (not a match pattern) when Shape is a
                        tagged-union enum -- build the compound
                        literal rather than the old bare tag name,
                        since the bare C enum constant no longer
                        exists on its own for payload enums.
                    */

                    fprintf(
                        out,
                        "(%.*s){ .tag = %.*s_%.*s }",
                        base_len, base_name,
                        base_len, base_name,
                        node->name_len, node->name
                        );

                    break;
                }

                fprintf(
                    out,
                    "%.*s",
                    node->name_len,
                    node->name
                    );

                break;
            }
        }


        if(node->name &&
            node->name_len == 3 &&
            strncmp(node->name, "len", 3) == 0)
        {
            /*
                Stage 4: `.len` was emitted as a bare C field access
                on whatever the base expression evaluates to -- not
                valid C for either an array (C arrays have no .len
                member) or a string (const char* has no .len member
                either). Arrays are fixed-size, so their length is
                always comptime-known here (Chapter 19); substitute
                the tracked integer constant directly. Strings don't
                carry a length at runtime in this bootstrap compiler
                (`str` is just const char*), so `.len` there means
                the same thing str_len(x) already does: strlen().
            */

            ASTNode* base =
                node->child_count ? node->children[0] : NULL;

            if(base && base->type == AST_IDENT)
            {
                const char* elem_type = NULL;
                uint64_t array_len = 0;

                if(codegen_lookup_array_info(
                        base->name,
                        base->name_len,
                        &elem_type,
                        &array_len
                        ))
                {
                    fprintf(
                        out,
                        "%lluLL",
                        (unsigned long long)array_len
                        );

                    break;
                }
            }

            /*
                Fallback: string case (known str variable, string
                literal, or any base expression we can't statically
                resolve as an array). Every non-array `.len` call
                site in the Stage 4 surface is a str, so this is a
                safe default rather than a guess.
            */

            fprintf(out, "(int64_t)strlen(");

            if(base)
                codegen_node(base, out);

            fprintf(out, ")");

            break;
        }


        fprintf(
            out,
            "("
            );


        if(node->child_count &&
            node->children[0])
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(
            out,
            ").%.*s",
            node->name_len,
            node->name
            );


        break;
    }



    case AST_ARRAY_LIT:
    {
        /*
            [e1, e2, ...] → { e1, e2, ... }
            Valid as a C array initializer (used on the RHS of an
            array let/var declaration). Not valid as a standalone
            C expression elsewhere -- Kru doesn't support array
            literals outside of a declaration's initializer yet.
        */

        fprintf(out, "{ ");

        for(uint32_t i = 0; i < node->child_count; i++)
        {
            if(i > 0)
                fprintf(out, ", ");

            codegen_node(
                node->children[i],
                out
                );
        }

        fprintf(out, " }");

        break;
    }



    case AST_INDEX_EXPR:
    {
        /*
            expr[index] → (expr)[index]

            Bounds-checked when this index's base resolves back to a
            known fixed-size array variable through zero or more
            prior index operations (see codegen_index_chain_depth):
            (expr)[__kru_check_index(index, LEN, "name", line, col)],
            where LEN is the *specific dimension* this index applies
            to -- grid[i][j] checks i against grid's dims[0] and j
            against dims[1], not both against dims[0]. When the chain
            doesn't bottom out at a plain identifier (e.g. indexing
            through a pointer/deref, a struct field, or a call's
            return value), or that identifier isn't a tracked array,
            or the depth goes past what was tracked (see dims[8] in
            CodegenVarType), this falls back to plain unchecked C
            indexing, same as before this pass.
        */

        ASTNode* base =
            node->child_count > 0 ? node->children[0] : NULL;

        ASTNode* root_ident = NULL;
        int chain_depth = 0;
        uint64_t dim_len = 0;

        bool bounds_checked =
            base &&
            codegen_index_chain_depth(
                base,
                &root_ident,
                &chain_depth
                ) &&
            codegen_lookup_array_dim(
                root_ident->name,
                root_ident->name_len,
                chain_depth,
                &dim_len
                );

        fprintf(out, "(");

        if(base)
        {
            codegen_node(
                base,
                out
                );
        }

        fprintf(out, ")[");

        if(node->child_count > 1)
        {
            if(bounds_checked)
            {
                fprintf(
                    out,
                    "__kru_check_index((int64_t)("
                    );

                codegen_node(
                    node->children[1],
                    out
                    );

                fprintf(
                    out,
                    "), %lluULL, \"%.*s\", %u, %u)",
                    (unsigned long long)dim_len,
                    (int)root_ident->name_len,
                    root_ident->name,
                    node->children[1]->line,
                    node->children[1]->column
                    );
            }
            else
            {
                codegen_node(
                    node->children[1],
                    out
                    );
            }
        }

        fprintf(out, "]");

        break;
    }



    case AST_BINARY_EXPR:
    {
        bool is_div_or_mod =
            node->op == TOKEN_SLASH || node->op == TOKEN_PERCENT;

        if(is_div_or_mod &&
            node->child_count > 1 &&
            node->children[1] &&
            node->children[1]->type == AST_INT_LIT &&
            node->children[1]->int_val == 0)
        {
            /*
                Compile-time catch for the literal-zero case. This
                doesn't stop codegen (codegen_error only flags
                codegen_had_error; see its doc comment), so the
                runtime check below still gets emitted underneath it
                as a second, redundant net -- harmless, since this
                path is a hard compile error regardless.
            */
            codegen_error(
                node->op == TOKEN_SLASH
                    ? "K1039: division by constant zero"
                    : "K1039: modulo by constant zero",
                node->children[1]->line,
                node->children[1]->column
                );
        }


        fprintf(
            out,
            "("
            );


        if(node->child_count > 0)
        {
            codegen_node(
                node->children[0],
                out
                );
        }


        fprintf(
            out,
            " %s ",
            operator_string(node->op)
            );


        if(node->child_count > 1)
        {
            /*
                Runtime div/mod-by-zero check: wraps the divisor
                expression in __kru_checked_div, which is _Generic-
                dispatched (see the prologue) to the int or float
                variant based on the divisor's own C type, and
                aborts with a clear message on zero rather than
                leaving it as UB / an unchecked SIGFPE. Only wraps
                for SLASH/PERCENT -- every other operator's RHS is
                emitted exactly as before.
            */

            if(is_div_or_mod)
            {
                fprintf(out, "__kru_checked_div(");

                codegen_node(
                    node->children[1],
                    out
                    );

                fprintf(
                    out,
                    ", %u, %u)",
                    node->children[1]->line,
                    node->children[1]->column
                    );
            }
            else
            {
                codegen_node(
                    node->children[1],
                    out
                    );
            }
        }


        fprintf(
            out,
            ")"
            );


        break;
    }



    case AST_UNARY_EXPR:
    {
        const char* op_str = "?";

        switch(node->op)
        {
        case TOKEN_MINUS: op_str = "-"; break;
        case TOKEN_BANG:   op_str = "!"; break;
        case TOKEN_TILDE:  op_str = "~"; break;
        default: break;
        }

        fprintf(out, "%s", op_str);

        if(node->child_count > 0 && node->children[0])
            codegen_node(node->children[0], out);

        break;
    }



    case AST_CALL_EXPR:
    {
        if(!node->child_count)
            break;


        ASTNode* callee =
            node->children[0];


        /*
            Struct literal: TypeName { field := value, ... }
            → (TypeName){ .field = value, ... }
        */

        if(node->is_struct_lit)
        {
            fprintf(
                out,
                "(%.*s){",
                callee->name_len,
                callee->name
                );


            for(uint32_t i = 1;
                 i < node->child_count;
                 i++)
            {
                ASTNode* field =
                    node->children[i];

                if(!field || field->type != AST_ASSIGN_STMT)
                    continue;


                fprintf(
                    out,
                    ".%.*s = ",
                    field->name_len,
                    field->name
                    );


                if(field->child_count &&
                    field->children[0])
                {
                    codegen_node(
                        field->children[0],
                        out
                        );
                }


                if(i + 1 < node->child_count)
                {
                    fprintf(
                        out,
                        ", "
                        );
                }
            }


            fprintf(
                out,
                "}"
                );

            break;
        }


        if(callee &&
            callee->type == AST_IDENT &&
            callee->name_len == 2 &&
            strncmp(
                callee->name,
                "pr",
                2
                ) == 0)
        {
            ASTNode* arg =
                node->child_count > 1 ? node->children[1] : NULL;

            /*
                Stage 4: pr() was always formatted as %lld, which
                breaks for pr(string_concat()) -- the argument is a
                str (const char*), not an integer, and casting a
                pointer to long long prints the address instead of
                the text (and warns under -Wall). Branch on whether
                the argument is string-typed; every existing non-
                string caller keeps the original %lld path exactly.
            */

            if(codegen_expr_is_string(arg))
            {
                fprintf(
                    out,
                    "printf(\"%%s\\n\", (const char*)("
                    );

                if(arg)
                    codegen_node(arg, out);

                fprintf(out, "))");

                break;
            }

            fprintf(
                out,
                "printf(\"%%lld\\n\", (long long)("
                );


            if(arg)
            {
                codegen_node(
                    arg,
                    out
                    );
            }


            fprintf(
                out,
                "))"
                );

            break;
        }


        if(callee &&
            callee->type == AST_IDENT &&
            callee->name_len == 7 &&
            strncmp(
                callee->name,
                "str_len",
                7
                ) == 0)
        {
            /*
                Boost pass: str_len(x) is used throughout the test
                suite (stage4, stage5*) as a stdlib intrinsic but was
                never implemented anywhere -- every caller would fail
                to link with "undefined reference to str_len". Map it
                straight onto C's strlen from <string.h> (already
                included in codegen.c itself; add it to the generated
                output's includes too -- see AST_PROGRAM's header
                block).
            */

            fprintf(
                out,
                "(int64_t)strlen("
                );

            if(node->child_count > 1)
            {
                codegen_node(
                    node->children[1],
                    out
                    );
            }

            fprintf(
                out,
                ")"
                );

            break;
        }


        if(callee &&
            callee->type == AST_IDENT &&
            callee->name_len == 6 &&
            strncmp(
                callee->name,
                "str_eq",
                6
                ) == 0)
        {
            /*
                str_eq(a, b) → __kru_str_eq(a, b). Same gap as
                str_len: used throughout stage4 but never implemented,
                so every caller failed to link.
            */

            fprintf(out, "__kru_str_eq(");

            if(node->child_count > 1)
                codegen_node(node->children[1], out);

            fprintf(out, ", ");

            if(node->child_count > 2)
                codegen_node(node->children[2], out);

            fprintf(out, ")");

            break;
        }


        if(callee &&
            callee->type == AST_IDENT &&
            callee->name_len == 10 &&
            strncmp(
                callee->name,
                "str_concat",
                10
                ) == 0)
        {
            /* str_concat(a, b) → __kru_str_concat(a, b). See the
               ownership note on __kru_str_concat's definition in the
               generated preamble (AST_PROGRAM header emission). */

            fprintf(out, "__kru_str_concat(");

            if(node->child_count > 1)
                codegen_node(node->children[1], out);

            fprintf(out, ", ");

            if(node->child_count > 2)
                codegen_node(node->children[2], out);

            fprintf(out, ")");

            break;
        }



        /*
            Check if callee is a known function.
            If not, and it starts with uppercase, treat as
            enum variant constructor: Ok(100) → 100.
        */

        bool is_known_func = false;

        if(callee && callee->type == AST_IDENT && callee->name)
        {
            for(uint32_t i = 0; i < func_count; i++)
            {
                if(func_table[i].name_len == callee->name_len &&
                    strncmp(func_table[i].name, callee->name,
                            callee->name_len) == 0)
                {
                    is_known_func = true;
                    break;
                }
            }
        }


        if(!is_known_func &&
            callee && callee->type == AST_IDENT &&
            callee->name_len > 0 &&
            callee->name[0] >= 'A' &&
            callee->name[0] <= 'Z')
        {
            /*
                Boost pass: enum variant constructor. Previously this
                discarded the tag entirely and emitted just the raw
                payload value (`Square(21)` -> `21`), so the result
                couldn't be told apart from a plain integer and
                couldn't be matched against a tag. Look the variant up
                in the registry (populated in collect_function_signatures)
                and, if it belongs to a payload-carrying enum, build a
                real tagged-union compound literal instead.
            */

            VariantEntry* variant =
                find_variant(callee->name, callee->name_len);

            if(variant &&
                variant->has_payload &&
                enum_has_payload(variant->enum_name, variant->enum_name_len))
            {
                fprintf(
                    out,
                    "(%.*s){ .tag = %.*s_%.*s, .as.%.*s = ",
                    variant->enum_name_len, variant->enum_name,
                    variant->enum_name_len, variant->enum_name,
                    variant->variant_name_len, variant->variant_name,
                    variant->variant_name_len, variant->variant_name
                    );

                if(node->child_count > 1)
                {
                    codegen_node(
                        node->children[1],
                        out
                        );
                }
                else
                {
                    fprintf(out, "0");
                }

                fprintf(out, " }");

                break;
            }


            /*
                Fallback: unknown/no-payload "constructor" call
                (e.g. a variant of a still-plain tag-only enum called
                with parens by mistake). Preserve the old behaviour
                rather than erroring, since sema doesn't verify this.
            */

            if(node->child_count > 1)
            {
                codegen_node(
                    node->children[1],
                    out
                    );
            }
            else
            {
                fprintf(out, "0");
            }

            break;
        }



        codegen_node(
            callee,
            out
            );


        fprintf(
            out,
            "("
            );


        for(uint32_t i = 1;
             i < node->child_count;
             i++)
        {
            ASTNode* arg =
                node->children[i];

            if(arg && arg->type == AST_ARRAY_LIT)
            {
                /*
                    Stage 4: an array literal passed directly as a
                    call argument (`array_mutate([1, 2, 3])`) used to
                    emit a bare `{1, 2, 3}` in argument position --
                    invalid C outside of an initializer. A compound
                    literal (`(int32_t[]){1, 2, 3}`) is the C form
                    that's valid as an expression/argument; the
                    element type is inferred the same way an untyped
                    `let nums := [...]` infers it.
                */

                fprintf(
                    out,
                    "(%s[])",
                    codegen_infer_array_lit_elem_type(arg)
                    );

                codegen_node(arg, out);
            }
            else
            {
                codegen_node(
                    arg,
                    out
                    );
            }


            if(i + 1 < node->child_count)
            {
                fprintf(
                    out,
                    ", "
                    );
            }
        }


        fprintf(
            out,
            ")"
            );


        break;
    }



    default:
    {
        for(uint32_t i = 0;
             i < node->child_count;
             i++)
        {
            codegen_node(
                node->children[i],
                out
                );
        }

        break;
    }

    }
}



int codegen_generate(
    ASTNode* root_node,
    const char* output_filepath
    )
{
    codegen_had_error = false;
    codegen_source_root = root_node;


    FILE* out =
        fopen(
            output_filepath,
            "w"
            );


    if(!out)
        return -1;



    codegen_node(
        root_node,
        out
        );


    fclose(out);


    if(codegen_had_error)
        return -1;


    return 0;
}