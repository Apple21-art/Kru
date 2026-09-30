#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/native_backend.h"

#define KRU_ELF_ET_EXEC 2u
#define KRU_ELF_EM_X86_64 62u
#define KRU_ELF_EV_CURRENT 1u
#define KRU_ELF_PT_LOAD 1u
#define KRU_ELF_PF_X 1u
#define KRU_ELF_PF_R 4u

typedef struct
{
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} KruElf64Ehdr;

typedef struct
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} KruElf64Phdr;

_Static_assert(sizeof(KruElf64Ehdr) == 64, "unexpected ELF header layout");
_Static_assert(sizeof(KruElf64Phdr) == 56, "unexpected ELF program header layout");

static int native_error(const char* message)
{
    fprintf(stderr, "[kru] native backend: %s\n", message);
    return -1;
}

#include "../include/native_runtime_blob.h"
#define CODE_LIMIT (1024u * 1024u)
#define LOCAL_LIMIT 2048u
#define FIXUP_LIMIT 8192u
#define FUNCTION_LIMIT 512u
#define PARAM_LIMIT 32u
#define ENUM_KIND_BASE 10000
#define STRUCT_KIND_BASE 20000
#define ARRAY_KIND_BASE 30000
#define ARRAY_TYPE_LIMIT 2048u

typedef enum { N_BAD=-1, N_I32, N_BOOL, N_STRING, N_POINTER, N_NULL, N_HANDLE, N_VOID, N_ARRAY } NativeKind;
typedef struct { const char* name; uint32_t length, slot; NativeKind kind; uint32_t array_count; NativeKind element; } NativeLocal;
typedef struct {
    const char* name; uint32_t length; ASTNode* node; uint32_t argc;
    NativeKind params[PARAM_LIMIT], result; uint32_t offset; int runtime;
    uint32_t array_count[PARAM_LIMIT]; NativeKind elements[PARAM_LIMIT];
} NativeFunction;
typedef struct { uint32_t count; NativeKind element; } NativeArrayType;
typedef struct { uint32_t at, target; } NativeFixup;
typedef struct { uint32_t at; ASTNode* literal; } NativeString;
typedef struct {
    unsigned char code[CODE_LIMIT]; uint32_t size;
    NativeLocal locals[LOCAL_LIMIT]; uint32_t local_count, slots, depth, temp_depth;
    NativeFunction functions[FUNCTION_LIMIT]; uint32_t function_count;
    NativeFixup calls[FIXUP_LIMIT]; uint32_t call_count;
    NativeString strings[FIXUP_LIMIT]; uint32_t string_count;
    uint32_t breaks[FIXUP_LIMIT], break_count, loop_depth, continue_target[128], loop_temp_depth[128];
    uint32_t continues[FIXUP_LIMIT], continue_count;
    NativeKind return_kind; uint32_t return_slot; ASTNode* root;
    NativeArrayType array_types[ARRAY_TYPE_LIMIT]; uint32_t array_type_count;
    const char* error; ASTNode* error_node;
} NativeCompiler;

static int fail(NativeCompiler* c, ASTNode* n, const char* error)
{ if(!c->error) { c->error=error; c->error_node=n; } return -1; }
static int emit(NativeCompiler* c, const unsigned char* bytes, uint32_t count)
{
    if(count > CODE_LIMIT-c->size) return fail(c,NULL,"native output exceeds 1 MiB limit");
    memcpy(c->code+c->size,bytes,count); c->size+=count; return 0;
}
#define BYTES(c, ...) emit(c,(const unsigned char[]){__VA_ARGS__},sizeof((const unsigned char[]){__VA_ARGS__}))
static int imm32(NativeCompiler* c,uint32_t value)
{ unsigned char b[4]; for(int i=0;i<4;i++) b[i]=(unsigned char)(value>>(8*i)); return emit(c,b,4); }
static int patch32(NativeCompiler* c,uint32_t at,uint32_t target)
{
    if(at>c->size || c->size-at<4 || target>c->size) return fail(c,NULL,"invalid native relocation");
    int64_t value=(int64_t)target-(int64_t)(at+4);
    if(value<INT32_MIN || value>INT32_MAX) return fail(c,NULL,"native rel32 relocation overflow");
    for(int i=0;i<4;i++) c->code[at+i]=(unsigned char)((uint32_t)value>>(8*i));
    return 0;
}
static int jump(NativeCompiler* c,int condition,uint32_t* patch)
{
    if(condition==0) { if(BYTES(c,0xE9)) return -1; }
    else if(BYTES(c,0x0F,condition==1?0x84:0x85)) return -1;
    *patch=c->size; return imm32(c,0);
}
static int name_is(ASTNode* n,const char* s)
{ return n && n->name && strlen(s)==n->name_len && !memcmp(n->name,s,n->name_len); }
static NativeLocal* find_local(NativeCompiler* c,ASTNode* n)
{
    for(uint32_t i=c->local_count;i;i--) if(n->name && n->name_len==c->locals[i-1].length &&
        !memcmp(n->name,c->locals[i-1].name,n->name_len)) return &c->locals[i-1];
    return NULL;
}
static int find_function(NativeCompiler* c,ASTNode* n)
{
    for(uint32_t i=0;i<c->function_count;i++) if(n->name && n->name_len==c->functions[i].length &&
        !memcmp(n->name,c->functions[i].name,n->name_len)) return (int)i;
    return -1;
}
static int is_array(NativeKind kind) { return kind>=ARRAY_KIND_BASE; }
static int is_struct(NativeKind kind) { return kind>=STRUCT_KIND_BASE&&kind<ARRAY_KIND_BASE; }
static int is_enum(NativeKind kind) { return kind>=ENUM_KIND_BASE&&kind<STRUCT_KIND_BASE; }
static NativeKind array_kind(NativeCompiler* c, uint32_t count, NativeKind element)
{
    if(element==N_BAD||element==N_VOID||count>LOCAL_LIMIT*8)return N_BAD;
    for(uint32_t i=0;i<c->array_type_count;i++)
        if(c->array_types[i].count==count&&c->array_types[i].element==element)return (NativeKind)(ARRAY_KIND_BASE+i);
    if(c->array_type_count==ARRAY_TYPE_LIMIT)return N_BAD;
    uint32_t index=c->array_type_count++;
    c->array_types[index]=(NativeArrayType){count,element};
    return (NativeKind)(ARRAY_KIND_BASE+index);
}
static NativeKind type_kind(NativeCompiler* c,ASTNode* t,uint32_t depth)
{
    if(!t) return N_VOID;
    if(depth>64 || t->type!=AST_TYPE) return N_BAD;
    if(t->op==TOKEN_LBRACKET && t->type_node && t->int_val<=LOCAL_LIMIT*8)
        return array_kind(c,(uint32_t)t->int_val,type_kind(c,t->type_node,depth+1));
    if(t->op==TOKEN_STAR && t->type_node && (name_is(t->type_node,"i32")||name_is(t->type_node,"int"))) return N_POINTER;
    if(t->op!=TOKEN_EOF) return N_BAD;
    if(name_is(t,"i32")||name_is(t,"int")) return N_I32;
    if(name_is(t,"bool")) return N_BOOL;
    if(name_is(t,"str")) return N_STRING;
    for(uint32_t i=0;i<c->root->child_count;i++) {
        ASTNode* a=c->root->children[i];
        if(!a||a->name_len!=t->name_len||!a->name||memcmp(a->name,t->name,t->name_len))continue;
        if(a->type==AST_ENUM_DECL) {
            if(!a->child_count||a->child_count>512)return N_BAD;
            return (NativeKind)(ENUM_KIND_BASE+(int)i);
        }
        if(a->type==AST_STRUCT_DECL)return (NativeKind)(STRUCT_KIND_BASE+(int)i);
        if(a->type==AST_TYPE_ALIAS)return type_kind(c,a->type_node,depth+1);
    }
    return N_BAD;
}
static NativeKind named_kind(NativeCompiler* c,ASTNode* name)
{
    ASTNode type={.type=AST_TYPE,.name=name->name,.name_len=name->name_len,.op=TOKEN_EOF};
    return type_kind(c,&type,0);
}
static int is_aggregate(NativeCompiler* c,NativeKind kind)
{
    if(is_array(kind)||is_struct(kind))return 1;
    if(is_enum(kind)) {
        ASTNode* e=c->root->children[(int)kind-ENUM_KIND_BASE];
        for(uint32_t i=0;i<e->child_count;i++)if(e->children[i]->child_count)return 1;
    }
    return 0;
}
static uint32_t kind_size(NativeCompiler* c,NativeKind kind,uint32_t depth);
static uint32_t kind_align(NativeCompiler* c,NativeKind kind,uint32_t depth)
{
    if(depth>64)return 0;
    if(kind==N_BOOL)return 1;
    if(kind==N_I32)return 4;
    if(kind==N_STRING||kind==N_POINTER||kind==N_HANDLE)return 8;
    if(is_array(kind))return kind_align(c,c->array_types[(int)kind-ARRAY_KIND_BASE].element,depth+1);
    if(is_struct(kind)||is_enum(kind)) {
        ASTNode* d=c->root->children[(int)kind-(is_struct(kind)?STRUCT_KIND_BASE:ENUM_KIND_BASE)];
        uint32_t align=is_enum(kind)?4:1;
        for(uint32_t i=0;i<d->child_count;i++) {
            ASTNode* field=d->children[i];
            if(is_struct(kind)) {
                uint32_t a=kind_align(c,type_kind(c,field->type_node,0),depth+1);if(!a)return 0;
                if(a>align)align=a;
            } else for(uint32_t j=0;j<field->child_count;j++) {
                uint32_t a=kind_align(c,type_kind(c,field->children[j],0),depth+1);if(!a)return 0;
                if(a>align)align=a;
            }
        }
        return align;
    }
    return 0;
}
static uint32_t kind_size(NativeCompiler* c,NativeKind kind,uint32_t depth)
{
    if(depth>64)return 0;
    if(kind==N_BOOL)return 1;
    if(kind==N_I32)return 4;
    if(kind==N_STRING||kind==N_POINTER||kind==N_HANDLE)return 8;
    if(is_array(kind)) {
        NativeArrayType a=c->array_types[(int)kind-ARRAY_KIND_BASE];
        uint32_t size=kind_size(c,a.element,depth+1);
        if(!size||a.count>LOCAL_LIMIT*8/size)return 0;
        return a.count*size;
    }
    if(is_struct(kind)||is_enum(kind)) {
        int structure=is_struct(kind);
        ASTNode* d=c->root->children[(int)kind-(structure?STRUCT_KIND_BASE:ENUM_KIND_BASE)];
        uint32_t size=structure?0:4,align=kind_align(c,kind,depth+1);if(!align)return 0;
        for(uint32_t i=0;i<d->child_count;i++) {
            ASTNode* field=d->children[i];uint32_t part=0;
            if(structure) {
                NativeKind fk=type_kind(c,field->type_node,0);uint32_t a=kind_align(c,fk,depth+1), n=kind_size(c,fk,depth+1);
                if(!a||(!n&&!is_array(fk)))return 0;
                size=(size+a-1)&~(a-1);if(n>LOCAL_LIMIT*8-size)return 0;size+=n;
            } else {
                for(uint32_t j=0;j<field->child_count;j++) {
                    NativeKind fk=type_kind(c,field->children[j],0);uint32_t a=kind_align(c,fk,depth+1), n=kind_size(c,fk,depth+1);
                    if(!a||(!n&&!is_array(fk)))return 0;
                    part=(part+a-1)&~(a-1);if(n>LOCAL_LIMIT*8-part)return 0;part+=n;
                }
                uint32_t total=((4+align-1)&~(align-1))+part;if(total>size)size=total;
            }
        }
        if(size>LOCAL_LIMIT*8-align+1)return 0;
        size=(size+align-1)&~(align-1);return size?size:1;
    }
    return 0;
}
static int valid_layout_depth(NativeCompiler* c,NativeKind kind,uint32_t depth)
{
    if(depth>64)return 0;
    if(is_array(kind)) {
        NativeArrayType t=c->array_types[(int)kind-ARRAY_KIND_BASE];
        if(!valid_layout_depth(c,t.element,depth+1))return 0;
        return (uint64_t)t.count*kind_size(c,t.element,0)<=LOCAL_LIMIT*8;
    }
    if(is_struct(kind)||is_enum(kind)) {
        int structure=is_struct(kind);
        ASTNode* d=c->root->children[(int)kind-(structure?STRUCT_KIND_BASE:ENUM_KIND_BASE)];
        for(uint32_t i=0;i<d->child_count;i++) {
            ASTNode* field=d->children[i];
            if(structure) {
                if(!valid_layout_depth(c,type_kind(c,field->type_node,0),depth+1))return 0;
            } else for(uint32_t j=0;j<field->child_count;j++)
                if(!valid_layout_depth(c,type_kind(c,field->children[j],0),depth+1))return 0;
        }
        uint32_t bytes=kind_size(c,kind,0);
        return bytes>0&&bytes<=LOCAL_LIMIT*8;
    }
    return kind_size(c,kind,0)>0;
}
static int valid_layout(NativeCompiler* c,NativeKind kind)
{
    return valid_layout_depth(c,kind,0);
}
static NativeKind field_info(NativeCompiler* c,NativeKind kind,ASTNode* name,uint32_t* offset)
{
    if(!is_struct(kind))return N_BAD;
    ASTNode* d=c->root->children[(int)kind-STRUCT_KIND_BASE];uint32_t at=0;
    for(uint32_t i=0;i<d->child_count;i++) {
        ASTNode* f=d->children[i];NativeKind fk=type_kind(c,f->type_node,0);
        uint32_t align=kind_align(c,fk,0),size=kind_size(c,fk,0);if(!align||!valid_layout(c,fk))return N_BAD;
        at=(at+align-1)&~(align-1);
        if(f->name_len==name->name_len&&!memcmp(f->name,name->name,name->name_len)){*offset=at;return fk;}
        at+=size;
    }
    return N_BAD;
}
static int compatible(NativeKind a,NativeKind b)
{ return a==b || ((a==N_POINTER||a==N_HANDLE) && b==N_NULL); }
static NativeKind expression_kind(NativeCompiler* c,ASTNode* n,uint32_t depth);
static int expression(NativeCompiler* c,ASTNode* n,uint32_t depth);
static int statement(NativeCompiler* c,ASTNode* n);
static int address(NativeCompiler* c,ASTNode* n);
static NativeKind enum_variant(NativeCompiler* c,ASTNode* n,uint32_t* tag)
{
    if(!n||n->type!=AST_FIELD_EXPR||n->child_count!=1||n->children[0]->type!=AST_IDENT)return N_BAD;
    for(uint32_t i=0;i<c->root->child_count;i++) {
        ASTNode* e=c->root->children[i];
        if(!e||e->type!=AST_ENUM_DECL||e->name_len!=n->children[0]->name_len||memcmp(e->name,n->children[0]->name,e->name_len))continue;
        for(uint32_t j=0;j<e->child_count;j++)
            if(e->children[j]->name_len==n->name_len&&!memcmp(e->children[j]->name,n->name,n->name_len)) {
                *tag=j;return (NativeKind)(ENUM_KIND_BASE+(int)i);
            }
    }
    return N_BAD;
}
static NativeKind enum_constructor(NativeCompiler* c,ASTNode* name,uint32_t* tag)
{
    if(name->type==AST_FIELD_EXPR)return enum_variant(c,name,tag);
    if(name->type!=AST_IDENT||find_function(c,name)>=0)return N_BAD;
    NativeKind found=N_BAD;
    for(uint32_t i=0;i<c->root->child_count;i++) {
        ASTNode* e=c->root->children[i];if(!e||e->type!=AST_ENUM_DECL)continue;
        for(uint32_t j=0;j<e->child_count;j++) {
            ASTNode* v=e->children[j];
            if(v->name_len==name->name_len&&!memcmp(v->name,name->name,name->name_len)) {
                if(found!=N_BAD)return N_BAD;
                found=(NativeKind)(ENUM_KIND_BASE+i);*tag=j;
            }
        }
    }
    return found;
}
static NativeKind payload_info(NativeCompiler* c,NativeKind kind,uint32_t tag,uint32_t field,uint32_t* offset)
{
    ASTNode* e=c->root->children[(int)kind-ENUM_KIND_BASE];
    if(tag>=e->child_count||field>=e->children[tag]->child_count)return N_BAD;
    ASTNode* v=e->children[tag];uint32_t align=kind_align(c,kind,0);
    uint32_t at=(4+align-1)&~(align-1);
    for(uint32_t i=0;i<=field;i++) {
        NativeKind fk=type_kind(c,v->children[i],0);uint32_t a=kind_align(c,fk,0);if(!a)return N_BAD;
        at=(at+a-1)&~(a-1);
        if(i==field){*offset=at;return fk;}
        at+=kind_size(c,fk,0);
    }
    return N_BAD;
}
static uint32_t array_count(NativeCompiler* c,ASTNode* n)
{
    NativeKind kind=expression_kind(c,n,0);
    return is_array(kind)?c->array_types[(int)kind-ARRAY_KIND_BASE].count:0;
}
static NativeKind array_element(NativeCompiler* c,ASTNode* n)
{
    NativeKind kind=expression_kind(c,n,0);
    return is_array(kind)?c->array_types[(int)kind-ARRAY_KIND_BASE].element:N_BAD;
}
static int initializer_fits(NativeCompiler* c,NativeKind target,ASTNode* value,uint32_t depth)
{
    if(!value||depth>128)return 0;
    if(is_array(target)&&value->type==AST_ARRAY_LIT) {
        NativeArrayType t=c->array_types[(int)target-ARRAY_KIND_BASE];
        if(value->child_count>t.count)return 0;
        for(uint32_t i=0;i<value->child_count;i++)
            if(!initializer_fits(c,t.element,value->children[i],depth+1))return 0;
        return 1;
    }
    return compatible(target,expression_kind(c,value,depth+1));
}
static NativeKind block_kind(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(!n||n->child_count!=1||n->children[0]->type!=AST_BLOCK)return N_BAD;
    ASTNode* body=n->children[0];uint32_t saved=c->local_count;NativeKind result=N_VOID;
    for(uint32_t i=0;i<body->child_count;i++) {
        ASTNode* stmt=body->children[i];result=N_VOID;
        if(stmt->type==AST_LET_STMT||stmt->type==AST_VAR_STMT) {
            if(c->local_count==LOCAL_LIMIT){result=N_BAD;break;}
            NativeKind value=stmt->child_count?expression_kind(c,stmt->children[0],depth+1):N_VOID;
            NativeKind kind=stmt->type_node?type_kind(c,stmt->type_node,0):value;
            if(kind==N_BAD||(stmt->child_count&&!initializer_fits(c,kind,stmt->children[0],depth+1))){result=N_BAD;break;}
            c->locals[c->local_count++]=(NativeLocal){stmt->name,stmt->name_len,0,kind,0,N_I32};
        } else if(stmt->type==AST_EXPR_STMT&&stmt->child_count==1)result=expression_kind(c,stmt->children[0],depth+1);
    }
    c->local_count=saved;return result;
}

static NativeKind call_kind(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(!n->child_count || !n->children[0]) return N_BAD;
    ASTNode* callee=n->children[0];
    if(callee->type==AST_FIELD_EXPR&&name_is(callee,"len")&&callee->child_count==1&&n->child_count==1)
        return is_array(expression_kind(c,callee->children[0],depth+1))||expression_kind(c,callee->children[0],depth+1)==N_STRING ? N_I32:N_BAD;
    uint32_t tag;NativeKind constructor=enum_constructor(c,callee,&tag);
    if(constructor!=N_BAD&&!n->is_struct_lit) {
        ASTNode* v=c->root->children[(int)constructor-ENUM_KIND_BASE]->children[tag];
        if(n->child_count!=v->child_count+1)return N_BAD;
        for(uint32_t i=0;i<v->child_count;i++)
            if(!compatible(type_kind(c,v->children[i],0),expression_kind(c,n->children[i+1],depth+1)))return N_BAD;
        return constructor;
    }
    if(n->is_struct_lit) {
        NativeKind kind=named_kind(c,callee);if(!is_struct(kind))return N_BAD;
        ASTNode* d=c->root->children[(int)kind-STRUCT_KIND_BASE];
        if(n->child_count!=d->child_count+1)return N_BAD;
        for(uint32_t i=1;i<n->child_count;i++) {
            ASTNode* value=n->children[i];uint32_t offset;
            NativeKind fk=field_info(c,kind,value,&offset);
            if(value->child_count!=1||fk==N_BAD||!initializer_fits(c,fk,value->children[0],depth+1))return N_BAD;
            for(uint32_t j=1;j<i;j++)if(value->name_len==n->children[j]->name_len&&!memcmp(value->name,n->children[j]->name,value->name_len))return N_BAD;
        }
        return kind;
    }
    if(callee->type!=AST_IDENT)return N_BAD;
    if(name_is(callee,"pr")) {
        if(n->child_count!=2) return N_BAD;
        NativeKind arg=expression_kind(c,n->children[1],depth+1);
        return arg==N_I32||arg==N_BOOL||arg==N_STRING||(is_enum(arg)&&!is_aggregate(c,arg)) ? N_VOID : N_BAD;
    }
    int index=find_function(c,callee);
    if(index<0) return N_BAD;
    NativeFunction* fn=&c->functions[index];
    if(n->child_count-1!=fn->argc) return N_BAD;
    for(uint32_t i=0;i<fn->argc;i++) {
        if(!compatible(fn->params[i],expression_kind(c,n->children[i+1],depth+1))) return N_BAD;

    }
    return fn->result;
}
static NativeKind expression_kind(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(!n||depth>128) return N_BAD;
    switch(n->type) {
    case AST_INT_LIT:
        if(n->has_explicit_suffix && type_kind(c,n->type_node,0)!=N_I32) return N_BAD;
        return n->int_val<=INT32_MAX ? N_I32:N_BAD;
    case AST_BOOL_LIT: return N_BOOL;
    case AST_STRING_LIT: return N_STRING;
    case AST_NULL_LIT: return N_NULL;
    case AST_ARRAY_LIT: {
        NativeKind elem=n->child_count?expression_kind(c,n->children[0],depth+1):N_I32;
        if(elem==N_BAD||elem==N_VOID||elem==N_NULL)return N_BAD;
        for(uint32_t i=0;i<n->child_count;i++)if(expression_kind(c,n->children[i],depth+1)!=elem)return N_BAD;
        return array_kind(c,n->child_count,elem);
    }
    case AST_INDEX_EXPR:
        if(n->child_count!=2||expression_kind(c,n->children[1],depth+1)!=N_I32)return N_BAD;
        if(expression_kind(c,n->children[0],depth+1)==N_STRING)return N_I32;
        return is_array(expression_kind(c,n->children[0],depth+1))?array_element(c,n->children[0]):N_BAD;
    case AST_FIELD_EXPR: {
        uint32_t offset;NativeKind kind=enum_variant(c,n,&offset);
        if(kind!=N_BAD)return c->root->children[(int)kind-ENUM_KIND_BASE]->children[offset]->child_count?N_BAD:kind;
        if(n->child_count!=1)return N_BAD;
        NativeKind base=expression_kind(c,n->children[0],depth+1);
        if(is_struct(base))return field_info(c,base,n,&offset);
        return name_is(n,"len")&&(is_array(base)||base==N_STRING)?N_I32:N_BAD;
    }
    case AST_BLOCK_EXPR:return block_kind(c,n,depth);
    case AST_IDENT: {
        NativeLocal* local=find_local(c,n); if(local) return local->kind;
        for(uint32_t i=0;i<c->root->child_count;i++) {
            ASTNode* a=c->root->children[i];
            if(a && a->type==AST_CONST_DECL && a->name_len==n->name_len && !memcmp(a->name,n->name,n->name_len) && a->child_count==1)
                return expression_kind(c,a->children[0],depth+1);
        }
        return N_BAD;
    }
    case AST_CALL_EXPR: return call_kind(c,n,depth);
    case AST_DEREF_EXPR:
        return n->child_count==1 && expression_kind(c,n->children[0],depth+1)==N_POINTER ? N_I32:N_BAD;
    case AST_REF_EXPR:
        if(n->child_count!=1)return N_BAD;
        if(is_array(expression_kind(c,n->children[0],depth+1)))return expression_kind(c,n->children[0],depth+1);
        return expression_kind(c,n->children[0],depth+1)==N_I32 ? N_POINTER:N_BAD;
    case AST_UNARY_EXPR:
        if(n->child_count!=1) return N_BAD;
        if(n->op==TOKEN_MINUS && n->children[0]->type==AST_INT_LIT &&
           n->children[0]->int_val==(uint64_t)INT32_MAX+1 && !n->children[0]->has_explicit_suffix) return N_I32;
        if(n->op==TOKEN_BANG) return expression_kind(c,n->children[0],depth+1)==N_BOOL ? N_BOOL:N_BAD;
        return expression_kind(c,n->children[0],depth+1)==N_I32 ? N_I32:N_BAD;
    case AST_BINARY_EXPR: {
        if(n->child_count!=2) return N_BAD;
        NativeKind a=expression_kind(c,n->children[0],depth+1), b=expression_kind(c,n->children[1],depth+1);
        if(n->op==TOKEN_AND_AND||n->op==TOKEN_OR_OR) return a==N_BOOL && b==N_BOOL ? N_BOOL:N_BAD;
        if(n->op==TOKEN_EQ_EQ||n->op==TOKEN_BANG_EQUAL)
            return a!=N_VOID&&a!=N_BAD&&!is_aggregate(c,a)&&b!=N_BAD&&!is_aggregate(c,b)&&(compatible(a,b)||compatible(b,a)) ? N_BOOL:N_BAD;
        if(n->op==TOKEN_LT||n->op==TOKEN_GT||n->op==TOKEN_LT_EQUAL||n->op==TOKEN_GT_EQUAL)
            return a==N_I32&&b==N_I32 ? N_BOOL:N_BAD;
        if((n->op==TOKEN_PLUS||n->op==TOKEN_MINUS)&&a==N_POINTER&&b==N_I32) return N_POINTER;
        if(n->op==TOKEN_MINUS&&a==N_POINTER&&b==N_POINTER) return N_I32;
        return a==N_I32&&b==N_I32 ? N_I32:N_BAD;
    }
    default: return N_BAD;
    }
}
static int slot_access(NativeCompiler* c,uint32_t slot,int store)
{
    if(BYTES(c,0x48,store?0x89:0x8B,0x85)) return -1;
    return imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)));
}
static int push_value(NativeCompiler* c)
{ if(BYTES(c,0x50)) return -1; c->temp_depth++; return 0; }
static int pop_value(NativeCompiler* c)
{ if(!c->temp_depth) return fail(c,NULL,"unbalanced native temporary"); if(BYTES(c,0x59))return -1; c->temp_depth--;return 0; }
static int emit_call(NativeCompiler* c,uint32_t fn)
{
    if(c->call_count==FIXUP_LIMIT) return fail(c,NULL,"too many native call relocations");
    int pad=(c->temp_depth&1)!=0;
    if(pad&&BYTES(c,0x48,0x83,0xEC,0x08))return -1;
    if(BYTES(c,0xE8))return -1;
    c->calls[c->call_count++]=(NativeFixup){c->size,fn};
    if(imm32(c,0))return -1;
    if(pad&&BYTES(c,0x48,0x83,0xC4,0x08))return -1;
    return 0;
}
static int reserve_storage(NativeCompiler* c,uint32_t bytes,uint32_t* slot);
static int initialize_storage(NativeCompiler* c,uint32_t slot,uint32_t offset,NativeKind kind,ASTNode* value);
static int slot_address(NativeCompiler* c,uint32_t slot);
static int call_expression(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    int fn;
    if(name_is(n->children[0],"pr")) {
        NativeKind kind=expression_kind(c,n->children[1],depth+1);
        ASTNode name={.name=kind==N_STRING?"__native_print_str":"__native_print_i32"};
        name.name_len=(uint32_t)strlen(name.name); fn=find_function(c,&name);
    } else fn=find_function(c,n->children[0]);
    if(fn<0 || n->child_count>PARAM_LIMIT+1) return fail(c,n,"unsupported native call or argument count");
    uint32_t args=n->child_count-1;
    uint32_t saved_depth=c->temp_depth;
    int sret=is_aggregate(c,c->functions[fn].result);
    if(sret) {
        uint32_t slot;
        if(reserve_storage(c,kind_size(c,c->functions[fn].result,0),&slot)||slot_address(c,slot)||push_value(c))return -1;
    }
    /* Evaluate left to right into temporary slots before populating the ABI.
       This also protects earlier arguments from nested calls. */
    for(uint32_t i=0;i<args;i++) {
        NativeKind arg_kind=expression_kind(c,n->children[i+1],depth+1);
        if(is_aggregate(c,arg_kind)) {
            uint32_t slot;
            if(reserve_storage(c,kind_size(c,arg_kind,0),&slot)||initialize_storage(c,slot,0,arg_kind,n->children[i+1])||slot_address(c,slot))return -1;
        } else if(expression(c,n->children[i+1],depth+1))return -1;
        if(c->functions[fn].params[i]==N_HANDLE && expression_kind(c,n->children[i+1],depth+1)==N_NULL)
            if(BYTES(c,0x48,0xC7,0xC0,0xFF,0xFF,0xFF,0xFF))return -1;
        if(push_value(c))return -1;
    }
    args+=(uint32_t)sret;
    uint32_t stack_args=args>6?args-6:0;
    uint32_t padding=(c->temp_depth+stack_args)&1u;
    uint32_t outgoing=stack_args+padding;
    if(outgoing&&(BYTES(c,0x48,0x81,0xEC)||imm32(c,outgoing*8)))return -1;
    c->temp_depth+=outgoing;
    for(uint32_t i=6;i<args;i++) {
        if(BYTES(c,0x48,0x8B,0x84,0x24)||imm32(c,(outgoing+args-1-i)*8)||
           BYTES(c,0x48,0x89,0x84,0x24)||imm32(c,(i-6)*8))return -1;
    }
    for(uint32_t i=0;i<args&&i<6;i++) {
        static const unsigned char modrm[]={0xBC,0xB4,0x94,0x8C,0x84,0x8C};
        if(BYTES(c,i>=4?0x4C:0x48,0x8B,modrm[i],0x24)||imm32(c,(outgoing+args-1-i)*8))return -1;
    }
    if(emit_call(c,(uint32_t)fn))return -1;
    if(args+outgoing)if(BYTES(c,0x48,0x81,0xC4)||imm32(c,(args+outgoing)*8))return -1;
    c->temp_depth=saved_depth;
    return 0;
}
static int slot_address(NativeCompiler* c,uint32_t slot)
{
    if(BYTES(c,0x48,0x8D,0x85))return -1;
    return imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)));
}
static int reserve_storage(NativeCompiler* c,uint32_t bytes,uint32_t* slot)
{
    uint32_t needed=(bytes+7)/8;if(!needed)needed=1;
    if(needed>LOCAL_LIMIT-c->slots)return fail(c,NULL,"native stack layout limit exceeded");
    c->slots+=needed;*slot=c->slots-1;return 0;
}
static int copy_bytes(NativeCompiler* c,uint32_t bytes)
{
    /* Source RAX, destination RCX. Fixed-size copies preserve both addresses. */
    uint32_t offset=0;
    while(bytes>=8) {
        if(BYTES(c,0x48,0x8B,0x90)||imm32(c,offset)||BYTES(c,0x48,0x89,0x91)||imm32(c,offset))return -1;
        offset+=8;bytes-=8;
    }
    if(bytes>=4) {
        if(BYTES(c,0x8B,0x90)||imm32(c,offset)||BYTES(c,0x89,0x91)||imm32(c,offset))return -1;
        offset+=4;bytes-=4;
    }
    if(bytes>=2) {
        if(BYTES(c,0x66,0x8B,0x90)||imm32(c,offset)||BYTES(c,0x66,0x89,0x91)||imm32(c,offset))return -1;
        offset+=2;bytes-=2;
    }
    return bytes ? (BYTES(c,0x8A,0x90)||imm32(c,offset)||BYTES(c,0x88,0x91)||imm32(c,offset)) : 0;
}
static int load_address(NativeCompiler* c,NativeKind kind)
{
    if(is_aggregate(c,kind))return 0;
    if(kind==N_BOOL)return BYTES(c,0x0F,0xB6,0x00);
    return kind==N_I32||is_enum(kind)?BYTES(c,0x48,0x63,0x00):BYTES(c,0x48,0x8B,0x00);
}
static int store_address(NativeCompiler* c,NativeKind kind)
{
    if(kind==N_BOOL)return BYTES(c,0x88,0x01);
    return kind==N_I32||is_enum(kind)?BYTES(c,0x89,0x01):BYTES(c,0x48,0x89,0x01);
}
static int storage_address(NativeCompiler* c,uint32_t slot,uint32_t offset)
{
    if(slot_address(c,slot))return -1;
    return offset ? (BYTES(c,0x48,0x05)||imm32(c,offset)):0;
}
static int zero_storage(NativeCompiler* c,uint32_t slot,uint32_t offset,uint32_t bytes)
{
    if(BYTES(c,0x31,0xC0))return -1;
    while(bytes>=8) {
        if(BYTES(c,0x48,0x89,0x85)||imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)+offset)))return -1;
        offset+=8;bytes-=8;
    }
    while(bytes) {
        if(BYTES(c,0x88,0x85)||imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)+offset)))return -1;
        offset++;bytes--;
    }
    return 0;
}
static int initialize_storage(NativeCompiler* c,uint32_t slot,uint32_t offset,NativeKind kind,ASTNode* value)
{
    uint32_t bytes=kind_size(c,kind,0);
    if(!value)return zero_storage(c,slot,offset,bytes);
    if(is_array(kind)&&value->type==AST_ARRAY_LIT) {
        NativeArrayType t=c->array_types[(int)kind-ARRAY_KIND_BASE];
        if(value->child_count>t.count)return fail(c,value,"too many native array initializer elements");
        if(zero_storage(c,slot,offset,bytes))return -1;
        uint32_t width=kind_size(c,t.element,0);
        for(uint32_t i=0;i<value->child_count;i++) {
            if(!initializer_fits(c,t.element,value->children[i],0))return fail(c,value,"native array initializer type mismatch");
            if(initialize_storage(c,slot,offset+i*width,t.element,value->children[i]))return -1;
        }
        return 0;
    }
    if(is_struct(kind)&&value->type==AST_CALL_EXPR&&value->is_struct_lit) {
        if(expression_kind(c,value,0)!=kind)return fail(c,value,"native struct initializer type mismatch");
        if(zero_storage(c,slot,offset,bytes))return -1;
        for(uint32_t i=1;i<value->child_count;i++) {
            ASTNode* field=value->children[i];uint32_t at;
            NativeKind fk=field_info(c,kind,field,&at);
            if(initialize_storage(c,slot,offset+at,fk,field->children[0]))return -1;
        }
        return 0;
    }
    if(!compatible(kind,expression_kind(c,value,0)))return fail(c,value,"native initializer type mismatch");
    if(storage_address(c,slot,offset)||push_value(c)||expression(c,value,0)||pop_value(c))return -1;
    return is_aggregate(c,kind)?copy_bytes(c,bytes):store_address(c,kind);
}
static int construct_enum(NativeCompiler* c,NativeKind kind,uint32_t tag,ASTNode* call)
{
    if(!is_aggregate(c,kind))return BYTES(c,0xB8)||imm32(c,tag);
    uint32_t slot,bytes=kind_size(c,kind,0);
    if(!bytes||reserve_storage(c,bytes,&slot)||zero_storage(c,slot,0,bytes)||BYTES(c,0xC7,0x85)||imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)))||imm32(c,tag))return -1;
    if(call)for(uint32_t i=1;i<call->child_count;i++) {
        uint32_t at;NativeKind fk=payload_info(c,kind,tag,i-1,&at);
        if(fk==N_BAD||initialize_storage(c,slot,at,fk,call->children[i]))return -1;
    }
    return slot_address(c,slot);
}
static int expression(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    NativeKind kind=expression_kind(c,n,depth);
    if(kind==N_BAD) return fail(c,n,"unsupported native expression, mismatched types, or call signature");
    switch(n->type) {
    case AST_INT_LIT: if(BYTES(c,0xB8)||imm32(c,(uint32_t)n->int_val))return -1;
            break;
    case AST_BOOL_LIT: if(BYTES(c,0xB8)||imm32(c,n->bool_val?1:0))return -1;
            break;
    case AST_NULL_LIT: return BYTES(c,0x31,0xC0);
    case AST_ARRAY_LIT: {
        uint32_t slot;
        if(reserve_storage(c,kind_size(c,kind,0),&slot)||initialize_storage(c,slot,0,kind,n))return -1;
        return slot_address(c,slot);
    }
    case AST_INDEX_EXPR:
        if(expression_kind(c,n->children[0],depth+1)==N_STRING) {
            ASTNode name={.type=AST_IDENT,.name="str_index",.name_len=9};
            ASTNode* args[]={&name,n->children[0],n->children[1]};
            ASTNode call={.type=AST_CALL_EXPR,.children=args,.child_count=3};
            return call_expression(c,&call,depth+1);
        }
        if(address(c,n))return -1;
        return load_address(c,kind);
    case AST_FIELD_EXPR: {
        uint32_t tag;
        if(enum_variant(c,n,&tag)!=N_BAD)return construct_enum(c,kind,tag,NULL);
        if(is_struct(expression_kind(c,n->children[0],depth+1))) {
            if(address(c,n))return -1;
            return load_address(c,kind);
        }
        if(expression_kind(c,n->children[0],depth+1)==N_STRING) {
            ASTNode name={.type=AST_IDENT,.name="str_len",.name_len=7};
            ASTNode* args[]={&name,n->children[0]};
            ASTNode call={.type=AST_CALL_EXPR,.children=args,.child_count=2};
            return call_expression(c,&call,depth+1);
        }
        if(expression(c,n->children[0],depth+1)||BYTES(c,0xB8)||imm32(c,array_count(c,n->children[0])))return -1;
        return 0;
    }
    case AST_BLOCK_EXPR: {
        ASTNode* body=n->children[0];uint32_t saved=c->local_count;
        if(c->depth==128)return fail(c,n,"native block-expression nesting limit");
        ++c->depth;
        for(uint32_t i=0;i<body->child_count;i++)if(statement(c,body->children[i])) {--c->depth;c->local_count=saved;return -1;}
        --c->depth;c->local_count=saved;return 0;
    }
    case AST_STRING_LIT:
        if(c->string_count==FIXUP_LIMIT)return fail(c,n,"too many native string literals");
        if(BYTES(c,0x48,0x8D,0x05))return -1;
        c->strings[c->string_count++]=(NativeString){c->size,n}; return imm32(c,0);
    case AST_IDENT: {
        NativeLocal* local=find_local(c,n);if(local) {
            if(is_aggregate(c,local->kind))return slot_address(c,local->slot);
            if(slot_address(c,local->slot))return -1;
            return load_address(c,local->kind);
        }
        for(uint32_t i=0;i<c->root->child_count;i++) {
            ASTNode* a=c->root->children[i];
            if(a && a->type==AST_CONST_DECL && a->name_len==n->name_len && !memcmp(a->name,n->name,n->name_len))
                return expression(c,a->children[0],depth+1);
        }
        return fail(c,n,"unknown native identifier");
    }
    case AST_CALL_EXPR: {
        uint32_t tag;NativeKind ctor=enum_constructor(c,n->children[0],&tag);
        if(ctor!=N_BAD&&!n->is_struct_lit)return construct_enum(c,ctor,tag,n);
        if(n->is_struct_lit) {
            uint32_t slot;
            if(reserve_storage(c,kind_size(c,kind,0),&slot)||initialize_storage(c,slot,0,kind,n))return -1;
            return slot_address(c,slot);
        }
        if(n->children[0]->type==AST_FIELD_EXPR)return expression(c,n->children[0],depth+1);
        return call_expression(c,n,depth);
    }
    case AST_DEREF_EXPR:
        if(expression(c,n->children[0],depth+1)||BYTES(c,0x48,0x63,0x00))return -1;
        return 0;
    case AST_REF_EXPR: {
        return address(c,n->children[0]);
    }
    case AST_UNARY_EXPR:
        if(n->op==TOKEN_MINUS && n->children[0]->type==AST_INT_LIT && n->children[0]->int_val==(uint64_t)INT32_MAX+1) {
            if(BYTES(c,0xB8)||imm32(c,(uint32_t)INT32_MIN))return -1;
            break;
        }
        if(expression(c,n->children[0],depth+1))return -1;
        if(n->op==TOKEN_MINUS) {if(BYTES(c,0xF7,0xD8))return -1;}
        else if(n->op==TOKEN_TILDE) {if(BYTES(c,0xF7,0xD0))return -1;}
        else if(n->op==TOKEN_BANG)return BYTES(c,0x85,0xC0,0x0F,0x94,0xC0,0x0F,0xB6,0xC0);
        else return fail(c,n,"unsupported native unary operator");
        break;
    case AST_BINARY_EXPR: {
        NativeKind a=expression_kind(c,n->children[0],depth+1), b=expression_kind(c,n->children[1],depth+1);
        if(a==N_STRING&&b==N_STRING&&(n->op==TOKEN_EQ_EQ||n->op==TOKEN_BANG_EQUAL)) {
            ASTNode name={.type=AST_IDENT,.name="str_eq",.name_len=6};
            ASTNode* args[]={&name,n->children[0],n->children[1]};
            ASTNode call={.type=AST_CALL_EXPR,.children=args,.child_count=3};
            if(call_expression(c,&call,depth+1))return -1;
            return n->op==TOKEN_BANG_EQUAL ? BYTES(c,0x83,0xF0,0x01):0;
        }
        if(n->op==TOKEN_AND_AND||n->op==TOKEN_OR_OR) {
            if(expression(c,n->children[0],depth+1)||BYTES(c,0x85,0xC0))return -1;
            uint32_t branch,end;if(jump(c,n->op==TOKEN_AND_AND?1:2,&branch))return -1;
            if(expression(c,n->children[1],depth+1)||BYTES(c,0x85,0xC0,0x0F,0x95,0xC0,0x0F,0xB6,0xC0)||jump(c,0,&end))return -1;
            if(patch32(c,branch,c->size)||BYTES(c,0xB8)||imm32(c,n->op==TOKEN_AND_AND?0:1))return -1;
            return patch32(c,end,c->size);
        }
        if(expression(c,n->children[0],depth+1)||push_value(c)||expression(c,n->children[1],depth+1)||pop_value(c))return -1;
        if((n->op==TOKEN_PLUS||n->op==TOKEN_MINUS)&&a==N_POINTER) {
            if(b==N_POINTER) {if(BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8,0x48,0xC1,0xF8,0x02))return -1;
            break;}
            if(BYTES(c,0x48,0xC1,0xE0,0x02))return -1;
            return n->op==TOKEN_PLUS ? BYTES(c,0x48,0x01,0xC8):BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8);
        }
        switch(n->op) {
        case TOKEN_PLUS: if(BYTES(c,0x01,0xC8))return -1;
            break;
        case TOKEN_MINUS: if(BYTES(c,0x29,0xC1,0x89,0xC8))return -1;
            break;
        case TOKEN_STAR: if(BYTES(c,0x0F,0xAF,0xC1))return -1;
            break;
        case TOKEN_SLASH:case TOKEN_PERCENT:
            if(BYTES(c,0x91,0x99,0xF7,0xF9))return -1;
            if(n->op==TOKEN_PERCENT&&BYTES(c,0x89,0xD0))return -1;
            break;
        case TOKEN_AMP:if(BYTES(c,0x21,0xC8))return -1;
            break;
        case TOKEN_PIPE:if(BYTES(c,0x09,0xC8))return -1;
            break;
        case TOKEN_CARET:if(BYTES(c,0x31,0xC8))return -1;
            break;
        case TOKEN_SHL:case TOKEN_SHR:
            if(BYTES(c,0x91,0xD3,n->op==TOKEN_SHL?0xE0:0xF8))return -1;
            break;
        case TOKEN_EQ_EQ:case TOKEN_BANG_EQUAL:case TOKEN_LT:case TOKEN_GT:case TOKEN_LT_EQUAL:case TOKEN_GT_EQUAL: {
            if(a==N_HANDLE&&b==N_NULL) {if(BYTES(c,0x48,0xC7,0xC0,0xFF,0xFF,0xFF,0xFF))return -1;}
            if(a==N_NULL&&b==N_HANDLE) {if(BYTES(c,0x48,0xC7,0xC1,0xFF,0xFF,0xFF,0xFF))return -1;}
            if(a==N_POINTER||a==N_STRING||a==N_NULL||a==N_HANDLE||b==N_POINTER||b==N_STRING||b==N_HANDLE) {
                if(BYTES(c,0x48,0x39,0xC1))return -1;
            } else if(BYTES(c,0x39,0xC1))return -1;
            unsigned char cc=n->op==TOKEN_EQ_EQ?0x94:n->op==TOKEN_BANG_EQUAL?0x95:n->op==TOKEN_LT?0x9C:n->op==TOKEN_GT?0x9F:n->op==TOKEN_LT_EQUAL?0x9E:0x9D;
            unsigned char code[]={0x0F,cc,0xC0,0x0F,0xB6,0xC0};return emit(c,code,sizeof(code));
        }
        default:return fail(c,n,"unsupported native binary operator");
        }
        break;
    }
    default:return fail(c,n,"unsupported native expression form");
    }
    return kind==N_I32 ? BYTES(c,0x48,0x63,0xC0):0;
}
static int block(NativeCompiler* c,ASTNode* n);
static int address(NativeCompiler* c,ASTNode* n)
{
    if(n->type==AST_IDENT) {
        NativeLocal* local=find_local(c,n);if(!local)return fail(c,n,"unknown assignment target");
        if(BYTES(c,0x48,0x8D,0x85))return -1;
        return imm32(c,(uint32_t)(-(int32_t)((local->slot+1)*8)));
    }
    if(n->type==AST_DEREF_EXPR && n->child_count==1 && expression_kind(c,n->children[0],0)==N_POINTER)
        return expression(c,n->children[0],0);
    if(n->type==AST_FIELD_EXPR&&n->child_count==1) {
        uint32_t offset;NativeKind base=expression_kind(c,n->children[0],0);
        if(field_info(c,base,n,&offset)==N_BAD)return fail(c,n,"unknown native field");
        if(expression(c,n->children[0],0))return -1;
        return offset ? (BYTES(c,0x48,0x05)||imm32(c,offset)):0;
    }
    if(n->type==AST_INDEX_EXPR&&expression_kind(c,n,0)!=N_BAD&&is_array(expression_kind(c,n->children[0],0))) {
        ASTNode* base=n->children[0];uint32_t count=array_count(c,base),width=kind_size(c,array_element(c,base),0);
        if(expression(c,base,0)||push_value(c)||expression(c,n->children[1],0)||pop_value(c))return -1;
        if(BYTES(c,0x3D)||imm32(c,count)||BYTES(c,0x0F,0x82))return -1;
        uint32_t valid=c->size;if(imm32(c,0)||BYTES(c,0x0F,0x0B)||patch32(c,valid,c->size))return -1;
        if(width!=1&&(BYTES(c,0x48,0x69,0xC0)||imm32(c,width)))return -1;
        return BYTES(c,0x48,0x01,0xC8);
    }
    return fail(c,n,"unsupported native assignment target");
}
static TokenType compound_op(TokenType op)
{
    switch(op) {
    case TOKEN_PLUS_EQUALS:return TOKEN_PLUS; case TOKEN_MINUS_EQUALS:return TOKEN_MINUS;
    case TOKEN_STAR_EQUALS:return TOKEN_STAR; case TOKEN_SLASH_EQUALS:return TOKEN_SLASH;
    case TOKEN_PERCENT_EQUALS:return TOKEN_PERCENT; case TOKEN_AMP_EQUALS:return TOKEN_AMP;
    case TOKEN_PIPE_EQUALS:return TOKEN_PIPE; case TOKEN_CARET_EQUALS:return TOKEN_CARET;
    case TOKEN_SHL_EQUALS:return TOKEN_SHL; case TOKEN_SHR_EQUALS:return TOKEN_SHR;
    default:return TOKEN_ERROR;
    }
}
static int statement(NativeCompiler* c,ASTNode* n)
{
    if(!n)return fail(c,n,"missing native statement");
    switch(n->type) {
    case AST_BLOCK:return block(c,n);
    case AST_LET_STMT:case AST_VAR_STMT: {
        if(c->local_count==LOCAL_LIMIT||c->slots==LOCAL_LIMIT||n->child_count>1)return fail(c,n,"native local limit exceeded");
        NativeKind value=n->child_count ? expression_kind(c,n->children[0],0):N_VOID;
        NativeKind declared=n->type_node ? type_kind(c,n->type_node,0):value;
        if(declared==N_VOID||declared==N_BAD||(n->child_count&&!initializer_fits(c,declared,n->children[0],0)))return fail(c,n,"native binding type mismatch or unsupported type");
        if(is_aggregate(c,declared)) {
            uint32_t bytes=kind_size(c,declared,0),slot;
            if(!valid_layout(c,declared)||reserve_storage(c,bytes,&slot)||initialize_storage(c,slot,0,declared,n->child_count?n->children[0]:NULL))return -1;
            c->locals[c->local_count++]=(NativeLocal){n->name,n->name_len,slot,declared,0,N_I32};return 0;
        }
        if(n->child_count) {if(expression(c,n->children[0],0))return -1;} else if(BYTES(c,0x31,0xC0))return -1;
        uint32_t slot=c->slots++;if(slot_access(c,slot,1))return -1;
        c->locals[c->local_count++]=(NativeLocal){n->name,n->name_len,slot,declared,0,N_I32};return 0;
    }
    case AST_ASSIGN_STMT: {
        if(n->child_count!=2)return fail(c,n,"invalid native assignment");
        ASTNode* lhs=n->children[0]; NativeKind kind=expression_kind(c,lhs,0);
        if(kind==N_BAD||kind==N_VOID)return fail(c,n,"unknown assignment type");
        if(address(c,lhs)||push_value(c))return -1;
        if(is_aggregate(c,kind)) {
            ASTNode* rhs=n->children[1];
            if((n->op!=TOKEN_EOF&&n->op!=TOKEN_EQUALS)||expression_kind(c,rhs,0)!=kind)return fail(c,n,"native aggregate assignment type mismatch");
            if(expression(c,rhs,0)||pop_value(c))return -1;
            return copy_bytes(c,kind_size(c,kind,0));
        }
        if(n->op==TOKEN_EOF||n->op==TOKEN_EQUALS) {
            if(!compatible(kind,expression_kind(c,n->children[1],0)))return fail(c,n,"native assignment type mismatch");
            if(expression(c,n->children[1],0))return -1;
            if(kind==N_HANDLE&&expression_kind(c,n->children[1],0)==N_NULL)
                if(BYTES(c,0x48,0xC7,0xC0,0xFF,0xFF,0xFF,0xFF))return -1;
        } else {
            TokenType op=compound_op(n->op);
            if(expression_kind(c,n->children[1],0)!=N_I32||(kind!=N_I32&&kind!=N_POINTER))return fail(c,n,"invalid native compound assignment types");
            if(kind==N_POINTER) {if(BYTES(c,0x48,0x8B,0x00))return -1;}
            else if(BYTES(c,0x48,0x63,0x00))return -1;
            if(push_value(c)||expression(c,n->children[1],0)||pop_value(c))return -1;
            if(kind==N_POINTER) {
                if(op!=TOKEN_PLUS&&op!=TOKEN_MINUS)return fail(c,n,"invalid pointer compound assignment");
                if(BYTES(c,0x48,0xC1,0xE0,0x02))return -1;
                if(op==TOKEN_PLUS) {if(BYTES(c,0x48,0x01,0xC8))return -1;}
                else if(BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8))return -1;
            } else {
                switch(op) {
                case TOKEN_PLUS:if(BYTES(c,0x01,0xC8))return -1;break;
                case TOKEN_MINUS:if(BYTES(c,0x29,0xC1,0x89,0xC8))return -1;break;
                case TOKEN_STAR:if(BYTES(c,0x0F,0xAF,0xC1))return -1;break;
                case TOKEN_SLASH:case TOKEN_PERCENT:
                    if(BYTES(c,0x91,0x99,0xF7,0xF9))return -1;
                    if(op==TOKEN_PERCENT&&BYTES(c,0x89,0xD0))return -1;
                    break;
                case TOKEN_AMP:if(BYTES(c,0x21,0xC8))return -1;break;
                case TOKEN_PIPE:if(BYTES(c,0x09,0xC8))return -1;break;
                case TOKEN_CARET:if(BYTES(c,0x31,0xC8))return -1;break;
                case TOKEN_SHL:case TOKEN_SHR:if(BYTES(c,0x91,0xD3,op==TOKEN_SHL?0xE0:0xF8))return -1;break;
                default:return fail(c,n,"unsupported native compound operator");
                }
                if(BYTES(c,0x48,0x63,0xC0))return -1;
            }
        }
        if(pop_value(c))return -1;
        return store_address(c,kind);
    }
    case AST_EXPR_STMT:
        return n->child_count==1 ? expression(c,n->children[0],0):fail(c,n,"invalid native expression statement");
    case AST_RET_STMT:
        if(c->return_kind==N_VOID) {
            if(n->child_count)return fail(c,n,"value returned from void native function");
        } else {
            if(n->child_count!=1||!compatible(c->return_kind,expression_kind(c,n->children[0],0)))return fail(c,n,"native return type mismatch");
            if(is_aggregate(c,c->return_kind)) {
                if(slot_access(c,c->return_slot,0)||push_value(c)||expression(c,n->children[0],0)||pop_value(c)||copy_bytes(c,kind_size(c,c->return_kind,0))||BYTES(c,0x48,0x89,0xC8))return -1;
            } else if(expression(c,n->children[0],0))return -1;
        }
        return BYTES(c,0xC9,0xC3);
    case AST_MATCH_EXPR: {
        if(n->child_count<2||n->child_count>513||c->slots==LOCAL_LIMIT)return fail(c,n,"native match arm/stack limit");
        NativeKind kind=expression_kind(c,n->children[0],0);
        if(kind!=N_I32&&kind!=N_BOOL&&!is_enum(kind))return fail(c,n,"native match requires integer/bool/enum");
        uint32_t slot,ends[512],end_count=0;unsigned char seen[512]={0};int wildcard=0;
        ASTNode* enumeration=is_enum(kind)?c->root->children[(int)kind-ENUM_KIND_BASE]:NULL;
        if(reserve_storage(c,kind_size(c,kind,0),&slot)||initialize_storage(c,slot,0,kind,n->children[0]))return -1;
        for(uint32_t i=1;i<n->child_count;i++) {
            ASTNode* arm=n->children[i];
            if(!arm||arm->type!=AST_MATCH_ARM||arm->child_count!=2)return fail(c,arm,"invalid native match arm");
            ASTNode* pattern=arm->children[0];uint32_t skip=0,tag=0;
            NativeKind pattern_enum=enumeration?enum_constructor(c,pattern->type==AST_CALL_EXPR?pattern->children[0]:pattern,&tag):N_BAD;
            int catchall=pattern->type==AST_IDENT&&pattern_enum==N_BAD;
            if(wildcard)return fail(c,arm,"unreachable native match arm after catch-all");
            if(!catchall) {
                if(enumeration) {
                    if(pattern_enum!=kind||seen[tag])return fail(c,pattern,"duplicate or invalid native enum match pattern");
                    seen[tag]=1;
                    ASTNode* v=enumeration->children[tag];
                    if(pattern->type==AST_CALL_EXPR&&pattern->child_count!=v->child_count+1)return fail(c,pattern,"native payload pattern binding count mismatch");
                    if(storage_address(c,slot,0)||BYTES(c,0x8B,0x00,0x3D)||imm32(c,tag)||jump(c,2,&skip))return -1;
                } else {
                    if(expression_kind(c,pattern,0)!=kind)return fail(c,pattern,"native match pattern type mismatch");
                    if(storage_address(c,slot,0)||load_address(c,kind)||push_value(c)||expression(c,pattern,0)||pop_value(c)||BYTES(c,0x39,0xC1)||jump(c,2,&skip))return -1;
                }
            } else wildcard=1;
            uint32_t saved=c->local_count;
            if(catchall&&!name_is(pattern,"_")) {
                if(c->local_count==LOCAL_LIMIT)return fail(c,pattern,"native match binding limit");
                c->locals[c->local_count++]=(NativeLocal){pattern->name,pattern->name_len,slot,kind,0,N_I32};
            }
            if(enumeration&&pattern->type==AST_CALL_EXPR)for(uint32_t j=1;j<pattern->child_count;j++) {
                ASTNode* binding=pattern->children[j];uint32_t offset,own;
                NativeKind fk=payload_info(c,kind,tag,j-1,&offset);
                if(binding->type!=AST_IDENT||fk==N_BAD||c->local_count==LOCAL_LIMIT)return fail(c,binding,"invalid native payload binding");
                if(name_is(binding,"_"))continue;
                for(uint32_t k=saved;k<c->local_count;k++)if(c->locals[k].length==binding->name_len&&!memcmp(c->locals[k].name,binding->name,binding->name_len))return fail(c,binding,"duplicate native payload binding");
                if(reserve_storage(c,kind_size(c,fk,0),&own)||slot_address(c,own)||push_value(c)||storage_address(c,slot,offset)||load_address(c,fk)||pop_value(c))return -1;
                if(is_aggregate(c,fk)) {if(copy_bytes(c,kind_size(c,fk,0)))return -1;}
                else if(store_address(c,fk))return -1;
                c->locals[c->local_count++]=(NativeLocal){binding->name,binding->name_len,own,fk,0,N_I32};
            }
            if(block(c,arm->children[1]))return -1;
            c->local_count=saved;
            if(jump(c,0,&ends[end_count++]))return -1;
            if(!catchall&&patch32(c,skip,c->size))return -1;
        }
        if(!wildcard) {
            if(!enumeration)return fail(c,n,"non-exhaustive native match requires catch-all");
            for(uint32_t i=0;i<enumeration->child_count;i++)if(!seen[i])return fail(c,n,"non-exhaustive native enum match");
        }
        if(BYTES(c,0x0F,0x0B))return -1;
        for(uint32_t i=0;i<end_count;i++)if(patch32(c,ends[i],c->size))return -1;
        return 0;
    }
    case AST_IF_STMT: {
        if(n->child_count<2||n->child_count>3||expression_kind(c,n->children[0],0)!=N_BOOL)return fail(c,n,"native if condition must be bool");
        if(expression(c,n->children[0],0)||BYTES(c,0x85,0xC0))return -1;
        uint32_t skip,end;if(jump(c,1,&skip)||block(c,n->children[1]))return -1;
        if(n->child_count==3) {
            if(jump(c,0,&end)||patch32(c,skip,c->size)||block(c,n->children[2]))return -1;
            return patch32(c,end,c->size);
        }
        return patch32(c,skip,c->size);
    }
    case AST_WHILE_STMT:case AST_LOOP_STMT: {
        if(c->loop_depth==128||n->child_count!=(n->type==AST_WHILE_STMT?2u:1u))return fail(c,n,"native loop limit or shape invalid");
        uint32_t head=c->size,skip=0,start=c->break_count;
        if(n->type==AST_WHILE_STMT) {
            if(expression_kind(c,n->children[0],0)!=N_BOOL)return fail(c,n,"native while condition must be bool");
            if(expression(c,n->children[0],0)||BYTES(c,0x85,0xC0)||jump(c,1,&skip))return -1;
        }
        c->loop_temp_depth[c->loop_depth]=c->temp_depth;
        c->continue_target[c->loop_depth++]=head;
        int rc=block(c,n->children[n->type==AST_WHILE_STMT?1:0]);c->loop_depth--;if(rc)return -1;
        uint32_t back;if(jump(c,0,&back)||patch32(c,back,head))return -1;
        if(n->type==AST_WHILE_STMT&&patch32(c,skip,c->size))return -1;
        for(uint32_t i=start;i<c->break_count;i++)if(patch32(c,c->breaks[i],c->size))return -1;
        c->break_count=start;return 0;
    }
    case AST_FOR_STMT: {
        int range=n->op==TOKEN_DOT_DOT;
        if(c->loop_depth==128||n->child_count!=(range?3u:2u)||c->slots+4>LOCAL_LIMIT||c->local_count==LOCAL_LIMIT)
            return fail(c,n,"invalid native for loop or stack limit");
        ASTNode* source=n->children[0];NativeKind elem=N_I32;
        int byref=!range&&source->type==AST_REF_EXPR;
        if(range) {
            if(expression_kind(c,source,0)!=N_I32||expression_kind(c,n->children[1],0)!=N_I32)return fail(c,n,"range bounds must be i32");
        } else {
            if(!is_array(expression_kind(c,source,0)))return fail(c,n,"native collection for requires fixed array");
            elem=array_element(c,source);
            if(byref&&elem!=N_I32)return fail(c,n,"native reference iteration currently requires i32 elements");
        }
        uint32_t saved=c->local_count,counter=c->slots++,limit=c->slots++,base=c->slots++,value;
        if(reserve_storage(c,byref?8:kind_size(c,elem,0),&value))return -1;
        if(range) {
            if(expression(c,source,0)||slot_access(c,counter,1)||expression(c,n->children[1],0)||slot_access(c,limit,1))return -1;
        } else {
            if(expression(c,source,0)||slot_access(c,base,1)||BYTES(c,0x31,0xC0)||slot_access(c,counter,1)||
               BYTES(c,0xB8)||imm32(c,array_count(c,source))||slot_access(c,limit,1))return -1;
        }
        c->locals[c->local_count++]=(NativeLocal){n->name,n->name_len,range?counter:value,byref?N_POINTER:elem,0,N_I32};
        uint32_t head=c->size,skip,start=c->break_count,contstart=c->continue_count;
        if(slot_access(c,counter,0)||push_value(c)||slot_access(c,limit,0)||pop_value(c)||
           BYTES(c,0x39,0xC1,0x0F,0x9C,0xC0,0x0F,0xB6,0xC0,0x85,0xC0)||jump(c,1,&skip))return -1;
        if(!range) {
            uint32_t width=kind_size(c,elem,0);
            if(slot_access(c,base,0)||push_value(c)||slot_access(c,counter,0)||pop_value(c))return -1;
            if(width!=1&&(BYTES(c,0x48,0x69,0xC0)||imm32(c,width)))return -1;
            if(BYTES(c,0x48,0x01,0xC8))return -1;
            if(!byref&&is_aggregate(c,elem)) {
                if(push_value(c)||slot_address(c,value)||BYTES(c,0x48,0x89,0xC1)||BYTES(c,0x58))return -1;
                c->temp_depth--;
                if(copy_bytes(c,width))return -1;
            } else {
                if(!byref&&load_address(c,elem))return -1;
                if(slot_access(c,value,1))return -1;
            }
        }
        c->loop_temp_depth[c->loop_depth]=c->temp_depth;
        c->continue_target[c->loop_depth++]=UINT32_MAX;
        int rc=block(c,n->children[range?2:1]);c->loop_depth--;if(rc)return -1;
        uint32_t next=c->size;
        for(uint32_t i=contstart;i<c->continue_count;i++)if(patch32(c,c->continues[i],next))return -1;
        c->continue_count=contstart;
        if(slot_access(c,counter,0)||BYTES(c,0x83,0xC0,0x01,0x48,0x63,0xC0)||slot_access(c,counter,1))return -1;
        uint32_t back;if(jump(c,0,&back)||patch32(c,back,head)||patch32(c,skip,c->size))return -1;
        for(uint32_t i=start;i<c->break_count;i++)if(patch32(c,c->breaks[i],c->size))return -1;
        c->break_count=start;c->local_count=saved;return 0;
    }
    case AST_BREAK_STMT:case AST_CONTINUE_STMT: {
        if(!c->loop_depth||n->name_len)return fail(c,n,"break/continue outside loop or unsupported label");
        uint32_t target_depth=c->loop_temp_depth[c->loop_depth-1];
        if(c->temp_depth<target_depth)return fail(c,n,"invalid native loop temporary depth");
        uint32_t discarded=c->temp_depth-target_depth;
        /* Only the transfer path discards these values. Keep compile-time depth
           for the other path through the enclosing expression. */
        if(discarded&&(BYTES(c,0x48,0x81,0xC4)||imm32(c,discarded*8)))return -1;
        uint32_t at;if(jump(c,0,&at))return -1;
        if(n->type==AST_CONTINUE_STMT) {
            uint32_t target=c->continue_target[c->loop_depth-1];
            if(target!=UINT32_MAX)return patch32(c,at,target);
            if(c->continue_count==FIXUP_LIMIT)return fail(c,n,"native continue limit exceeded");
            c->continues[c->continue_count++]=at;return 0;
        }
        if(c->break_count==FIXUP_LIMIT)return fail(c,n,"native break limit exceeded");
        c->breaks[c->break_count++]=at;return 0;
    }
    default:return fail(c,n,"unsupported native statement form");
    }
}
static int block(NativeCompiler* c,ASTNode* n)
{
    if(!n||n->type!=AST_BLOCK||c->depth==128)return fail(c,n,"invalid or excessively nested native block");
    uint32_t saved=c->local_count;++c->depth;
    for(uint32_t i=0;i<n->child_count;i++)if(statement(c,n->children[i])) {--c->depth;return -1;}
    c->local_count=saved;--c->depth;return 0;
}
static int definitely_returns(ASTNode* n)
{
    if(!n)return 0;
    if(n->type==AST_RET_STMT)return 1;
    if(n->type==AST_IF_STMT&&n->child_count==3)return definitely_returns(n->children[1])&&definitely_returns(n->children[2]);
    if(n->type==AST_MATCH_EXPR&&n->child_count>1) {
        for(uint32_t i=1;i<n->child_count;i++)
            if(n->children[i]->child_count!=2||!definitely_returns(n->children[i]->children[1]))return 0;
        return 1; /* Native lowering separately verifies exhaustiveness. */
    }
    if(n->type==AST_BLOCK)for(uint32_t i=0;i<n->child_count;i++)if(definitely_returns(n->children[i]))return 1;
    return 0;
}
static int add_runtime(NativeCompiler* c,const char* name,int symbol,NativeKind result,uint32_t argc,NativeKind a,NativeKind b)
{
    if(c->function_count==FUNCTION_LIMIT)return fail(c,NULL,"native function limit exceeded");
    NativeFunction* fn=&c->functions[c->function_count++];
    *fn=(NativeFunction){.name=name,.length=(uint32_t)strlen(name),.argc=argc,.result=result,.runtime=symbol};
    fn->params[0]=a;fn->params[1]=b;return 0;
}
static int collect_functions(NativeCompiler* c)
{
#define R(name,symbol,result,count,a,b) if(add_runtime(c,name,symbol,result,count,a,b))return -1
    R("__native_print_i32",RT_PRINT_I32,N_VOID,1,N_I32,N_VOID);
    R("__native_print_str",RT_PRINT_STR,N_VOID,1,N_STRING,N_VOID);
    R("str_len",RT_STR_LEN,N_I32,1,N_STRING,N_VOID);
    R("str_eq",RT_STR_EQ,N_BOOL,2,N_STRING,N_STRING);
    R("str_concat",RT_STR_CONCAT,N_STRING,2,N_STRING,N_STRING);
    R("str_index",RT_STR_INDEX,N_I32,2,N_STRING,N_I32);
    R("file_open",RT_FILE_OPEN,N_HANDLE,1,N_STRING,N_VOID);
    R("file_open_write",RT_FILE_OPEN_WRITE,N_HANDLE,1,N_STRING,N_VOID);
    R("file_close",RT_FILE_CLOSE,N_VOID,1,N_HANDLE,N_VOID);
    R("file_write",RT_FILE_WRITE,N_VOID,2,N_HANDLE,N_STRING);
    R("file_read",RT_FILE_READ,N_STRING,1,N_HANDLE,N_VOID);
    R("mem_alloc",RT_MEM_ALLOC,N_POINTER,1,N_I32,N_VOID);
    R("mem_free",RT_MEM_FREE,N_VOID,1,N_POINTER,N_VOID);
    R("mem_realloc",RT_MEM_REALLOC,N_POINTER,2,N_POINTER,N_I32);
    R("args_count",RT_ARGS_COUNT,N_I32,0,N_VOID,N_VOID);
    R("args_get",RT_ARGS_GET,N_STRING,1,N_I32,N_VOID);
#undef R
    for(uint32_t i=0;i<c->root->child_count;i++) {
        ASTNode* n=c->root->children[i];if(!n)continue;
        if(n->is_secure)return fail(c,n,"secure declarations require native wiping/encryption support, not implemented yet");
        if(n->type==AST_STRUCT_DECL||n->type==AST_ENUM_DECL) {
            for(uint32_t j=0;j<n->child_count;j++)for(uint32_t k=0;k<j;k++) {
                ASTNode* x=n->children[j];ASTNode* y=n->children[k];
                if(x->name_len==y->name_len&&!memcmp(x->name,y->name,x->name_len))return fail(c,x,"duplicate native field or variant name");
            }
        }
        if(n->type==AST_TYPE_ALIAS||n->type==AST_CONST_DECL)continue;
        if(n->type==AST_STRUCT_DECL) {
            NativeKind kind=(NativeKind)(STRUCT_KIND_BASE+i);
            if(!valid_layout(c,kind))return fail(c,n,"invalid or recursive native struct layout");
            continue;
        }
        if(n->type==AST_ENUM_DECL) {
            if(!n->child_count||n->child_count>512)return fail(c,n,"native scalar enum variant limit");
            if(!valid_layout(c,(NativeKind)(ENUM_KIND_BASE+i)))return fail(c,n,"invalid or recursive native enum layout");
            continue;
        }
        if(n->type!=AST_FUNCTION)return fail(c,n,"top-level declaration outside current native type support");
        if(c->function_count==FUNCTION_LIMIT||find_function(c,n)>=0)return fail(c,n,"duplicate/reserved native function or function limit");
        NativeFunction* fn=&c->functions[c->function_count++];
        *fn=(NativeFunction){.name=n->name,.length=n->name_len,.node=n,.result=type_kind(c,n->type_node,0),.runtime=-1};
        if(fn->result==N_BAD)return fail(c,n,"unsupported native function return type");
        if(fn->result!=N_VOID&&!valid_layout(c,fn->result))return fail(c,n,"invalid native result layout");
        for(uint32_t j=0;j<n->child_count;j++)if(n->children[j]->type==AST_PARAM) {
            if(fn->argc==PARAM_LIMIT)return fail(c,n,"native parameter limit exceeded");
            NativeKind kind=type_kind(c,n->children[j]->type_node,0);
            if(kind==N_BAD||kind==N_VOID||!valid_layout(c,kind))return fail(c,n->children[j],"unsupported native parameter type or layout");
            fn->params[fn->argc++]=kind;
        }
    }
    return 0;
}
static int compile_function(NativeCompiler* c,NativeFunction* fn)
{
    ASTNode* body=NULL;
    for(uint32_t i=0;i<fn->node->child_count;i++)if(fn->node->children[i]->type==AST_BLOCK)body=fn->node->children[i];
    if(!body)return fail(c,fn->node,"native function body missing");
    if(fn->result!=N_VOID&&!definitely_returns(body))return fail(c,fn->node,"native function may fall through without ret");
    c->local_count=c->slots=c->depth=c->temp_depth=c->loop_depth=c->break_count=c->continue_count=0;c->return_kind=fn->result;
    fn->offset=c->size;
    if(BYTES(c,0x55,0x48,0x89,0xE5,0x48,0x81,0xEC))return -1;
    uint32_t frame=c->size;if(imm32(c,0))return -1;
    int sret=is_aggregate(c,fn->result);
    if(sret) {
        c->return_slot=c->slots++;
        if(BYTES(c,0x48,0x89,0xF8)||slot_access(c,c->return_slot,1))return -1;
    }
    uint32_t param=0;
    for(uint32_t i=0;i<fn->node->child_count;i++) {
        ASTNode* p=fn->node->children[i];if(p->type!=AST_PARAM)continue;
        uint32_t physical=param+(uint32_t)sret;
        static const unsigned char modrm[]={0xF8,0xF0,0xD0,0xC8,0xC0,0xC8};
        if(physical<6) {
            if(BYTES(c,physical>=4?0x4C:0x48,0x89,modrm[physical]))return -1;
        } else if(BYTES(c,0x48,0x8B,0x85)||imm32(c,16+(physical-6)*8))return -1;
        if(!is_aggregate(c,fn->params[param])&&(fn->params[param]==N_I32||is_enum(fn->params[param]))&&BYTES(c,0x48,0x63,0xC0))return -1;
        uint32_t slot=c->slots++;if(slot_access(c,slot,1))return -1;
        c->locals[c->local_count++]=(NativeLocal){p->name,p->name_len,slot,fn->params[param],0,N_I32};param++;
    }
    /* Spill every incoming argument before aggregate copies clobber temporaries. */
    for(uint32_t i=0;i<fn->argc;i++)if(is_aggregate(c,fn->params[i])) {
        NativeLocal* local=&c->locals[i];uint32_t slot,bytes=kind_size(c,local->kind,0);
        if(reserve_storage(c,bytes,&slot)||slot_address(c,slot)||push_value(c)||slot_access(c,local->slot,0)||pop_value(c)||copy_bytes(c,bytes))return -1;
        local->slot=slot;
    }
    if(block(c,body))return -1;
    if(c->temp_depth)return fail(c,fn->node,"native temporary stack not balanced");
    if(fn->result==N_VOID) {if(BYTES(c,0x31,0xC0,0xC9,0xC3))return -1;}
    else if(BYTES(c,0x0F,0x0B))return -1;
    uint32_t bytes=(c->slots*8+15u)&~15u;
    for(int i=0;i<4;i++)c->code[frame+i]=(unsigned char)(bytes>>(8*i));
    return 0;
}

static int hex_digit(unsigned char ch)
{
    if(ch>='0'&&ch<='9')return ch-'0';
    if(ch>='a'&&ch<='f')return ch-'a'+10;
    if(ch>='A'&&ch<='F')return ch-'A'+10;
    return -1;
}
static int emit_string(NativeCompiler* c,ASTNode* n)
{
    const unsigned char* s=(const unsigned char*)n->string_val;
    uint32_t length=n->string_len;
    int raw=length>=3&&s[0]=='r'&&s[1]=='"';
    uint32_t start=raw?2u:1u;
    if(length<=start||s[start-1]!='"'||s[length-1]!='"')return fail(c,n,"invalid string token");
    for(uint32_t i=start;i<length-1;i++) {
        unsigned char ch=s[i];
        if(ch=='\\'&&!raw) {
            if(++i>=length-1)return fail(c,n,"incomplete string escape");
            switch(s[i]) {
            case 'n':ch='\n';break;case 'r':ch='\r';break;case 't':ch='\t';break;case '0':ch=0;break;
            case '\\':ch='\\';break;case '"':ch='"';break;case '\'':ch='\'';break;
            case 'x': {
                if(i+2>=length-1||hex_digit(s[i+1])<0||hex_digit(s[i+2])<0)return fail(c,n,"invalid hex string escape");
                ch=(unsigned char)(hex_digit(s[i+1])*16+hex_digit(s[i+2]));i+=2;break;
            }
            case 'u': {
                if(i+1>=length-1||s[++i]!='{')return fail(c,n,"invalid Unicode string escape");
                uint32_t scalar=0,digits=0;
                while(++i<length-1&&s[i]!='}') {
                    int digit=hex_digit(s[i]);if(digit<0||++digits>6)return fail(c,n,"invalid Unicode scalar");
                    scalar=scalar*16+(uint32_t)digit;
                }
                if(i>=length-1||!digits||scalar>0x10FFFF||(scalar>=0xD800&&scalar<=0xDFFF))return fail(c,n,"invalid Unicode scalar");
                unsigned char utf8[4];uint32_t count;
                if(scalar<0x80){utf8[0]=(unsigned char)scalar;count=1;}
                else if(scalar<0x800){utf8[0]=(unsigned char)(0xC0|(scalar>>6));utf8[1]=(unsigned char)(0x80|(scalar&63));count=2;}
                else if(scalar<0x10000){utf8[0]=(unsigned char)(0xE0|(scalar>>12));utf8[1]=(unsigned char)(0x80|((scalar>>6)&63));utf8[2]=(unsigned char)(0x80|(scalar&63));count=3;}
                else{utf8[0]=(unsigned char)(0xF0|(scalar>>18));utf8[1]=(unsigned char)(0x80|((scalar>>12)&63));utf8[2]=(unsigned char)(0x80|((scalar>>6)&63));utf8[3]=(unsigned char)(0x80|(scalar&63));count=4;}
                if(emit(c,utf8,count))return -1;
                continue;
            }
            default:return fail(c,n,"unrecognized string escape");
            }
        }
        if(emit(c,&ch,1))return -1;
    }
    return BYTES(c,0);
}
static int compile_main(NativeCompiler* c,ASTNode* root)
{
    if(!root||root->type!=AST_PROGRAM||root->child_count>=ENUM_KIND_BASE)return fail(c,root,"invalid native program or declaration limit");
    c->root=root;if(collect_functions(c))return -1;
    ASTNode main_name={.name="main",.name_len=4};int main=find_function(c,&main_name);
    if(main<0||c->functions[main].argc||c->functions[main].result!=N_I32)return fail(c,root,"native main requires zero parameters and i32/int return");
    /* Preserve the kernel entry stack for argc/argv; align before calling main. */
    if(BYTES(c,0x49,0x89,0xE4,0x48,0x83,0xE4,0xF0)||emit_call(c,(uint32_t)main)||
       BYTES(c,0x89,0xC7,0xB8,0x3C,0,0,0,0x0F,0x05))return -1;
    for(uint32_t i=0;i<c->function_count;i++)if(c->functions[i].node&&compile_function(c,&c->functions[i]))return -1;
    uint32_t runtime=c->size;if(emit(c,native_runtime_blob,sizeof(native_runtime_blob)))return -1;
    for(uint32_t i=0;i<c->function_count;i++)if(c->functions[i].runtime>=0)
        c->functions[i].offset=runtime+native_runtime_offsets[c->functions[i].runtime];
    for(uint32_t i=0;i<c->call_count;i++)if(patch32(c,c->calls[i].at,c->functions[c->calls[i].target].offset))return -1;
    for(uint32_t i=0;i<c->string_count;i++) {
        uint32_t offset=c->size;ASTNode* n=c->strings[i].literal;
        if(emit_string(c,n)||patch32(c,c->strings[i].at,offset))return -1;
    }
    return 0;
}

int native_backend_emit_x86_64_linux(ASTNode* root, const char* output_path)
{
#if !defined(__linux__) || !defined(__x86_64__)
    (void)root;
    (void)output_path;
    return native_error("direct backend currently targets x86-64 Linux only");
#else
    if(!output_path)
        return native_error("missing native output path");

    NativeCompiler* compiler = calloc(1, sizeof(*compiler));
    if(!compiler) return native_error("out of memory");
    if(compile_main(compiler, root) != 0) {
        if(compiler->error_node && compiler->error_node->line) fprintf(stderr,"[kru] native source: line %u, column %u\n",compiler->error_node->line,compiler->error_node->column);
        int result = native_error(compiler->error ? compiler->error : "native compilation failed");
        free(compiler);
        return result;
    }

    const uint64_t base = 0x400000u;
    const uint64_t code_offset = sizeof(KruElf64Ehdr) + sizeof(KruElf64Phdr);
    const uint64_t file_size = code_offset + compiler->size;

    KruElf64Ehdr eh;
    memset(&eh, 0, sizeof(eh));
    eh.ident[0] = 0x7f;
    eh.ident[1] = 'E';
    eh.ident[2] = 'L';
    eh.ident[3] = 'F';
    eh.ident[4] = 2; /* ELFCLASS64 */
    eh.ident[5] = 1; /* ELFDATA2LSB */
    eh.ident[6] = 1; /* EV_CURRENT */
    eh.type = KRU_ELF_ET_EXEC;
    eh.machine = KRU_ELF_EM_X86_64;
    eh.version = KRU_ELF_EV_CURRENT;
    eh.entry = base + code_offset;
    eh.phoff = sizeof(KruElf64Ehdr);
    eh.ehsize = sizeof(KruElf64Ehdr);
    eh.phentsize = sizeof(KruElf64Phdr);
    eh.phnum = 1;

    KruElf64Phdr ph;
    memset(&ph, 0, sizeof(ph));
    ph.type = KRU_ELF_PT_LOAD;
    ph.flags = KRU_ELF_PF_R | KRU_ELF_PF_X;
    ph.offset = 0;
    ph.vaddr = base;
    ph.paddr = base;
    ph.filesz = file_size;
    ph.memsz = file_size;
    ph.align = 0x1000;

    FILE* out = fopen(output_path, "wb");
    if(!out)
    {
        fprintf(stderr, "[kru] native backend: cannot open '%s': %s\n", output_path, strerror(errno));
        free(compiler);
        return -1;
    }

    int ok =
        fwrite(&eh, 1, sizeof(eh), out) == sizeof(eh) &&
        fwrite(&ph, 1, sizeof(ph), out) == sizeof(ph) &&
        fwrite(compiler->code, 1, compiler->size, out) == compiler->size;

    if(fclose(out) != 0)
        ok = 0;

    free(compiler);
    if(!ok)
    {
        remove(output_path);
        return native_error("failed while writing ELF executable");
    }

    if(chmod(output_path, 0755) != 0)
        return native_error("failed to mark ELF executable");

    return 0;
#endif
}
