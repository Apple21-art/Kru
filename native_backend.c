#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/native_backend.h"
#include "../include/types.h"
#include "../include/native_abi.h"
#include "../include/literals.h"
#include "../include/move_flow.h"
#include "../include/diagnostic.h"

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

typedef KruTypeId NativeKind;
#define N_BAD KRU_BAD
#define N_I32 KRU_I32
#define N_BOOL KRU_BOOL
#define N_STRING KRU_STR
#define N_POINTER KRU_LEGACY_I32_PTR
#define N_NULL KRU_NULL
#define N_HANDLE KRU_LEGACY_HANDLE
#define N_VOID KRU_VOID
#define N_ARRAY KRU_LEGACY_ARRAY
typedef struct { const char* name; uint32_t length, slot; NativeKind kind; uint32_t array_count; NativeKind element; ASTNode* declaration; uint32_t drop_flag; } NativeLocal;
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
    NativeLocal locals[LOCAL_LIMIT]; uint32_t local_count, slots, max_slots, depth, temp_depth, keep_expression_value;
    NativeFunction functions[FUNCTION_LIMIT]; uint32_t function_count;
    NativeFixup calls[FIXUP_LIMIT]; uint32_t call_count;
    NativeString strings[FIXUP_LIMIT]; uint32_t string_count;
    uint32_t breaks[FIXUP_LIMIT], break_count, loop_depth, continue_target[128], loop_temp_depth[128], loop_local_count[128];
    uint32_t continues[FIXUP_LIMIT], continue_count;
    NativeKind return_kind; uint32_t return_slot; ASTNode* root;
    NativeFunction* tail_function; uint32_t tail_head;
    int leaf_registers, check_heap;
    KruTypeTable types;
    NativeArrayType array_types[ARRAY_TYPE_LIMIT]; uint32_t array_type_count;
    uint32_t secure_function, wipe_count;
    struct {uint32_t address,count,extra;} wipes[FIXUP_LIMIT];
    KruMoveFlow move_flow;
    const char* error; ASTNode* error_node;
    const char* error_code; const char* error_help;
    char error_text[512],error_hint[256];
} NativeCompiler;

static int fail(NativeCompiler* c, ASTNode* n, const char* error)
{ if(!c->error) { c->error=error; c->error_node=n; } return -1; }
static int fail_code(NativeCompiler* c,ASTNode* n,const char* code,const char* message,const char* help)
{
    if(!c->error){c->error_code=code;c->error_help=help;}
    return fail(c,n,message);
}
static int emit(NativeCompiler* c, const unsigned char* bytes, uint32_t count)
{
    if(count > CODE_LIMIT-c->size) return fail_code(c,NULL,"K5002","native output exceeds 1 MiB limit","Reduce generated code/data or split the program; this is a compiler capacity limit.");
    memcpy(c->code+c->size,bytes,count); c->size+=count; return 0;
}
#define BYTES(c, ...) emit(c,(const unsigned char[]){__VA_ARGS__},sizeof((const unsigned char[]){__VA_ARGS__}))
static int imm32(NativeCompiler* c,uint32_t value)
{ unsigned char b[4]; for(int i=0;i<4;i++) b[i]=(unsigned char)(value>>(8*i)); return emit(c,b,4); }
static int imm64(NativeCompiler* c,uint64_t value)
{ unsigned char b[8]; for(int i=0;i<8;i++)b[i]=(unsigned char)(value>>(8*i));return emit(c,b,8); }
static int native_integer(NativeKind kind)
{ const KruPrimitive* t=kru_primitive(kind);return t&&t->category==KRU_INTEGER&&t->size<=8; }
static int unsigned_integer(NativeKind kind)
{ const KruPrimitive* t=kru_primitive(kind);return t&&t->category==KRU_INTEGER&&!t->is_signed; }
static int native_float(NativeKind kind)
{ return kind==KRU_F32||kind==KRU_F64; }
static int wide_integer(NativeKind kind)
{ return kind==KRU_I128||kind==KRU_U128; }
static int native_numeric(NativeKind kind)
{ return native_integer(kind)||native_float(kind)||kind==KRU_CHAR; }
static double power_two(unsigned power)
{ union {uint64_t bits;double value;} v={.bits=(uint64_t)(power+1023)<<52};return v.value; }
static int patch32(NativeCompiler* c,uint32_t at,uint32_t target)
{
    if(at>c->size || c->size-at<4 || target>c->size) return fail(c,NULL,"invalid native relocation");
    int64_t value=(int64_t)target-(int64_t)(at+4);
    if(value<INT32_MIN || value>INT32_MAX) return fail(c,NULL,"native rel32 relocation overflow");
    kru_move_patch(&c->move_flow,at,target);
    for(int i=0;i<4;i++) c->code[at+i]=(unsigned char)((uint32_t)value>>(8*i));
    return 0;
}
static int jump_op(NativeCompiler* c,int condition,unsigned char opcode,uint32_t* patch)
{
    if(condition==0) { if(BYTES(c,0xE9)) return -1; }
    else if(BYTES(c,0x0F,opcode)) return -1;
    *patch=c->size;
    if(kru_move_event(&c->move_flow,(KruMoveEvent){.position=c->size,.action=KRU_MOVE_BRANCH,.relocation=*patch,.target=UINT32_MAX,.conditional=condition!=0}))return fail(c,NULL,"ownership control-flow limit");
    return imm32(c,0);
}
static int jump(NativeCompiler* c,int condition,uint32_t* patch)
{ return jump_op(c,condition,condition==1?0x84:0x85,patch); }
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
static int is_array(NativeKind kind) { return kind>=ARRAY_KIND_BASE&&kind<KRU_COMPOUND_BASE; }
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
static const KruCompoundType* compound_type(NativeCompiler* c,NativeKind kind)
{ return kru_compound(&c->types,kind); }
static int is_slice(NativeCompiler* c,NativeKind kind)
{ const KruCompoundType* t=compound_type(c,kind);return t&&t->category==KRU_SLICE; }
static int raw_pointer(NativeCompiler* c,NativeKind kind)
{const KruCompoundType* t=compound_type(c,kind);return kind==N_POINTER||(t&&t->category==KRU_RAW_POINTER);}
static NativeKind pointee(NativeCompiler* c,NativeKind kind)
{ const KruCompoundType* t=compound_type(c,kind);return kind==N_POINTER?N_I32:t&&(t->category==KRU_REFERENCE||t->category==KRU_RAW_POINTER)?t->element:N_BAD; }
static NativeKind type_kind(NativeCompiler* c,ASTNode* t,uint32_t depth)
{
    if(!t) return N_VOID;
    if(depth>64 || t->type!=AST_TYPE) return N_BAD;
    if(t->op==TOKEN_KW_REF&&t->type_node) {
        ASTNode* elem=t->type_node;
        if(elem->op==TOKEN_LBRACKET&&elem->int_val==UINT64_MAX)
            return kru_type_compound(&c->types,KRU_SLICE,type_kind(c,elem->type_node,depth+1),t->is_mut);
        return kru_type_compound(&c->types,KRU_REFERENCE,type_kind(c,elem,depth+1),t->is_mut);
    }
    if(t->child_count)return N_BAD; /* Generic identity is never silently erased. */
    if(t->op==TOKEN_LBRACKET && t->type_node && t->int_val<=LOCAL_LIMIT*8)
        return array_kind(c,(uint32_t)t->int_val,type_kind(c,t->type_node,depth+1));
    if(t->op==TOKEN_STAR&&t->type_node) {
        NativeKind elem=type_kind(c,t->type_node,depth+1);
        return elem==N_I32?N_POINTER:kru_type_compound(&c->types,KRU_RAW_POINTER,elem,1);
    }
    if(t->op!=TOKEN_EOF) return N_BAD;
    NativeKind primitive=kru_type_named(t->name,t->name_len);
    if(native_integer(primitive)||wide_integer(primitive)||native_float(primitive)||primitive==KRU_CHAR||primitive==N_VOID||primitive==N_BOOL||primitive==N_STRING)return primitive;
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
    if(wide_integer(kind))return 1; /* Internal expression representation is an address. ABI is a register pair. */
    if(kind==N_STRING||is_slice(c,kind)||is_array(kind)||is_struct(kind))return 1;
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
    if(native_integer(kind)||wide_integer(kind)||native_float(kind)||kind==KRU_CHAR)return kru_primitive(kind)->align;
    if(compound_type(c,kind))return 8;
    if(kind==N_BOOL)return 1;
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
    if(native_integer(kind)||wide_integer(kind)||native_float(kind)||kind==KRU_CHAR)return kru_primitive(kind)->size;
    if(compound_type(c,kind))return is_slice(c,kind)?16:8;
    if(kind==N_BOOL)return 1;
    if(kind==N_STRING)return 16;
    if(kind==N_POINTER||kind==N_HANDLE)return 8;
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
static int compatible(NativeCompiler* c,NativeKind a,NativeKind b)
{ return a==b || ((raw_pointer(c,a)||a==N_HANDLE) && b==N_NULL); }
static NativeKind expression_kind(NativeCompiler* c,ASTNode* n,uint32_t depth);
static int expression(NativeCompiler* c,ASTNode* n,uint32_t depth);
static int statement(NativeCompiler* c,ASTNode* n);
static int cleanup(NativeCompiler* c,unsigned start,NativeLocal* moved);
static int secure_return(NativeCompiler* c);
static int needs_drop(NativeCompiler* c,NativeKind kind,unsigned depth);
static int mark_move(NativeCompiler* c,ASTNode* value,NativeKind kind);
static int ownership_flag(NativeCompiler* c,NativeLocal* local,int live);
static int initialize_ownership(NativeCompiler* c,NativeLocal* local);
static int address(NativeCompiler* c,ASTNode* n);
static int reserve_storage(NativeCompiler* c,uint32_t bytes,uint32_t* slot);
static int slot_address(NativeCompiler* c,uint32_t slot);
static int diagnose_expression(NativeCompiler* c,ASTNode* n,unsigned depth);
static int slot_access(NativeCompiler* c,uint32_t slot,int store);
static ASTNode* integer_node(ASTNode* n)
{return n&&n->type==AST_UNARY_EXPR&&n->op==TOKEN_MINUS&&n->child_count==1?n->children[0]:n;}
static int integer_literal(ASTNode* n,uint64_t* magnitude,int* negative,int* suffixed)
{
    *negative=0;
    if(n&&n->type==AST_UNARY_EXPR&&n->op==TOKEN_MINUS&&n->child_count==1) {
        *negative=1;n=n->children[0];
    }
    if(!n||n->type!=AST_INT_LIT)return 0;
    *magnitude=n->int_val;*suffixed=n->has_explicit_suffix;return 1;
}
static NativeKind literal_kind(NativeCompiler* c,ASTNode* n)
{
    uint64_t magnitude;int negative,suffixed;
    if(!integer_literal(n,&magnitude,&negative,&suffixed))return N_BAD;
    if(suffixed) {
        ASTNode* value=negative?n->children[0]:n;
        NativeKind kind=type_kind(c,value->type_node,0);
        if((native_integer(kind)||wide_integer(kind))&&!kru_integer_fits128(kind,magnitude,value->int_high,negative))
            fail_code(c,value,"K1040","integer literal out of range for its suffix","Use a representable value or a wider integer suffix.");
        return (native_integer(kind)||wide_integer(kind))&&kru_integer_fits128(kind,magnitude,value->int_high,negative)?kind:N_BAD;
    }
    if(integer_node(n)->int_high) return kru_integer_fits128(KRU_I128,magnitude,integer_node(n)->int_high,negative)?KRU_I128:N_BAD;
    if(kru_integer_fits(KRU_I32,magnitude,negative))return KRU_I32;
    if(kru_integer_fits(KRU_I64,magnitude,negative))return KRU_I64;
    return KRU_I128;
}
static int value_fits(NativeCompiler* c,NativeKind target,ASTNode* n,uint32_t depth)
{
    uint64_t magnitude;int negative,suffixed;
    if((native_integer(target)||wide_integer(target))&&integer_literal(n,&magnitude,&negative,&suffixed))
        return kru_integer_fits128(target,magnitude,integer_node(n)->int_high,negative)&&(!suffixed||literal_kind(c,n)==target);
    ASTNode* float_value=n;
    if(n&&n->type==AST_UNARY_EXPR&&n->op==TOKEN_MINUS&&n->child_count==1)float_value=n->children[0];
    if(native_float(target)&&float_value&&float_value->type==AST_FLOAT_LIT)
        return (!float_value->has_explicit_suffix||type_kind(c,float_value->type_node,0)==target)&&
               isfinite(float_value->float_val)&&(target!=KRU_F32||isfinite((float)float_value->float_val));
    return compatible(c,target,expression_kind(c,n,depth));
}
static NativeKind binary_operand_kind(NativeCompiler* c,ASTNode* n,NativeKind a,NativeKind b,uint32_t depth)
{
    if(a==b)return a;
    if(native_float(a)&&value_fits(c,a,n->children[1],depth+1))return a;
    if(native_float(b)&&value_fits(c,b,n->children[0],depth+1))return b;
    uint64_t magnitude;int negative,suffixed;
    if((native_integer(a)||wide_integer(a))&&integer_literal(n->children[1],&magnitude,&negative,&suffixed)&&!suffixed&&value_fits(c,a,n->children[1],depth+1))return a;
    if((native_integer(b)||wide_integer(b))&&integer_literal(n->children[0],&magnitude,&negative,&suffixed)&&!suffixed&&value_fits(c,b,n->children[0],depth+1))return b;
    return N_BAD;
}
static int normalize_integer(NativeCompiler* c,NativeKind kind)
{
    if(!native_integer(kind))return 0;
    const KruPrimitive* t=kru_primitive(kind);
    if(t->size==8)return 0;
    if(t->size==4)return t->is_signed?BYTES(c,0x48,0x63,0xC0):BYTES(c,0x89,0xC0);
    if(t->size==2)return t->is_signed?BYTES(c,0x48,0x0F,0xBF,0xC0):BYTES(c,0x0F,0xB7,0xC0);
    return t->is_signed?BYTES(c,0x48,0x0F,0xBE,0xC0):BYTES(c,0x0F,0xB6,0xC0);
}
static int expression_as(NativeCompiler* c,ASTNode* n,NativeKind target,uint32_t depth)
{
    ASTNode* float_value=n;int float_negative=0;
    if(n&&n->type==AST_UNARY_EXPR&&n->op==TOKEN_MINUS&&n->child_count==1) {float_value=n->children[0];float_negative=1;}
    if(native_float(target)&&float_value&&float_value->type==AST_FLOAT_LIT) {
        if(!value_fits(c,target,n,depth))return fail(c,n,"native float literal does not fit expected type");
        double value=float_negative?-float_value->float_val:float_value->float_val;
        uint64_t bits=0;
        if(target==KRU_F32) {float narrow=(float)value;uint32_t word;memcpy(&word,&narrow,4);bits=word;}
        else memcpy(&bits,&value,8);
        return BYTES(c,0x48,0xB8)||imm64(c,bits);
    }
    uint64_t magnitude;int negative,suffixed;
    if(wide_integer(target)&&integer_literal(n,&magnitude,&negative,&suffixed)) {
        if(!value_fits(c,target,n,depth))return fail(c,n,"native 128-bit literal does not fit expected type");
        uint64_t high=integer_node(n)->int_high;
        if(negative) {high=0-high-(magnitude!=0);magnitude=0-magnitude;}
        uint32_t slot;
        if(reserve_storage(c,16,&slot)||BYTES(c,0x48,0xB8)||imm64(c,magnitude)||slot_access(c,slot,1)||BYTES(c,0x48,0xB8)||imm64(c,high)||slot_access(c,slot-1,1))return -1;
        return slot_address(c,slot);
    }
    if(native_integer(target)&&integer_literal(n,&magnitude,&negative,&suffixed)) {
        if(!value_fits(c,target,n,depth))return fail(c,n,"native integer literal does not fit expected type");
        /* value_fits guarantees the literal is already in normalized form, so the
           shortest encoding that yields the same 64-bit register value is enough. */
        uint64_t bits=negative?UINT64_C(0)-magnitude:magnitude;
        if(bits<=UINT32_MAX)return BYTES(c,0xB8)||imm32(c,(uint32_t)bits);
        if((int64_t)bits>=INT32_MIN&&(int64_t)bits<0)return BYTES(c,0x48,0xC7,0xC0)||imm32(c,(uint32_t)bits);
        return BYTES(c,0x48,0xB8)||imm64(c,bits);
    }
    return expression(c,n,depth);
}
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
        ASTNode* e=c->root->children[i];if(!e||e->generic_parameters||e->type!=AST_ENUM_DECL)continue;
        for(uint32_t j=0;j<e->child_count;j++) {
            ASTNode* v=e->children[j];
            int unqualified=v->name_len==name->name_len&&!memcmp(v->name,name->name,name->name_len);
            int qualified=name->name_len==e->name_len+2+v->name_len&&
                !memcmp(name->name,e->name,e->name_len)&&
                name->name[e->name_len]==':'&&name->name[e->name_len+1]==':'&&
                !memcmp(name->name+e->name_len+2,v->name,v->name_len);
            if(unqualified||qualified) {
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
    return value_fits(c,target,value,depth+1);
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
            c->locals[c->local_count++]=(NativeLocal){stmt->name,stmt->name_len,0,kind,0,N_I32,stmt,UINT32_MAX};
        } else if(stmt->type==AST_EXPR_STMT&&stmt->child_count==1)result=expression_kind(c,stmt->children[0],depth+1);
    }
    c->local_count=saved;return result;
}

static NativeKind call_kind(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(!n->child_count || !n->children[0]) return N_BAD;
    ASTNode* callee=n->children[0];
    if(callee->type==AST_FIELD_EXPR&&name_is(callee,"len")&&callee->child_count==1&&n->child_count==1)
        return is_slice(c,expression_kind(c,callee->children[0],depth+1))||is_array(expression_kind(c,callee->children[0],depth+1))||expression_kind(c,callee->children[0],depth+1)==N_STRING ? N_I32:N_BAD;
    uint32_t tag;NativeKind constructor=enum_constructor(c,callee,&tag);
    if(constructor!=N_BAD&&!n->is_struct_lit) {
        ASTNode* v=c->root->children[(int)constructor-ENUM_KIND_BASE]->children[tag];
        if(n->child_count!=v->child_count+1)return N_BAD;
        for(uint32_t i=0;i<v->child_count;i++)
            if(!value_fits(c,type_kind(c,v->children[i],0),n->children[i+1],depth+1))return N_BAD;
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
    if(name_is(callee,"cast")) {
        if(callee->child_count!=1||n->child_count!=2)return N_BAD;
        NativeKind target=type_kind(c,callee->children[0],0);
        NativeKind source=expression_kind(c,n->children[1],depth+1);
        if(is_enum(source)&&!is_aggregate(c,source)&&native_integer(target))return target;
        if(raw_pointer(c,target)&&(raw_pointer(c,source)||native_integer(source)||source==N_NULL))return target;
        uint64_t magnitude;int negative,suffixed;
        if((!native_numeric(target)&&!wide_integer(target))||(!native_numeric(source)&&!wide_integer(source)))return N_BAD;
        if((target==KRU_CHAR||source==KRU_CHAR)&&(native_float(target)||native_float(source)||wide_integer(target)||wide_integer(source)))return N_BAD;
        if(target==KRU_CHAR&&integer_literal(n->children[1],&magnitude,&negative,&suffixed)&&
           (negative||integer_node(n->children[1])->int_high||magnitude>0x10FFFF||(magnitude>=0xD800&&magnitude<=0xDFFF)))return N_BAD;
        if(native_integer(target)&&integer_literal(n->children[1],&magnitude,&negative,&suffixed)&&!kru_integer_fits(target,magnitude,negative))return N_BAD;
        if((native_integer(target)||wide_integer(target))&&integer_literal(n->children[1],&magnitude,&negative,&suffixed)&&!kru_integer_fits128(target,magnitude,integer_node(n->children[1])->int_high,negative))return N_BAD;
        ASTNode* literal=n->children[1];int sign=0;
        if(literal->type==AST_UNARY_EXPR&&literal->op==TOKEN_MINUS&&literal->child_count==1){sign=1;literal=literal->children[0];}
        if(literal->type==AST_FLOAT_LIT) {
            double value=literal->float_val;
            if(source==KRU_F32)value=(float)value;
            if(sign)value=-value;
            if(native_integer(target)||wide_integer(target)) {
                const KruPrimitive* to=kru_primitive(target);unsigned bits=to->size*8;
                double upper=power_two(bits-(to->is_signed?1u:0u));
                double lower=to->is_signed?-upper:-1.0;
                if(to->is_signed&&bits<64)lower-=1.0;
                if(!isfinite(value)||value>=upper||(to->is_signed&&bits>=64?value<lower:value<=lower))return N_BAD;
            } else if(!isfinite(value)||(target==KRU_F32&&!isfinite((float)value)))return N_BAD;
        }
        return target;
    }
    if(callee->child_count)return N_BAD;
    if(name_is(callee,"pr")) {
        if(n->child_count!=2) return N_BAD;
        NativeKind arg=expression_kind(c,n->children[1],depth+1);
        return native_integer(arg)||wide_integer(arg)||native_float(arg)||arg==KRU_CHAR||arg==N_BOOL||arg==N_STRING||(is_enum(arg)&&!is_aggregate(c,arg)) ? N_VOID : N_BAD;
    }
    int index=find_function(c,callee);
    if(index<0) return N_BAD;
    NativeFunction* fn=&c->functions[index];
    if(n->child_count-1!=fn->argc) return N_BAD;
    for(uint32_t i=0;i<fn->argc;i++) {
        if(!value_fits(c,fn->params[i],n->children[i+1],depth+1)) return N_BAD;

    }
    return fn->result;
}
static NativeKind expression_kind(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(!n||depth>128) return N_BAD;
    switch(n->type) {
    case AST_INT_LIT:return literal_kind(c,n);
    case AST_FLOAT_LIT:return n->has_explicit_suffix?type_kind(c,n->type_node,0):KRU_F64;
    case AST_BOOL_LIT: return N_BOOL;
    case AST_CHAR_LIT:return KRU_CHAR;
    case AST_STRING_LIT: return N_STRING;
    case AST_NULL_LIT: return N_NULL;
    case AST_ARRAY_LIT: {
        NativeKind elem=n->child_count?expression_kind(c,n->children[0],depth+1):N_I32;
        if(elem==N_BAD||elem==N_VOID||elem==N_NULL)return N_BAD;
        for(uint32_t i=0;i<n->child_count;i++)if(expression_kind(c,n->children[i],depth+1)!=elem)return N_BAD;
        return array_kind(c,n->child_count,elem);
    }
    case AST_TRY_EXPR: {
        if(n->child_count!=1)return N_BAD;
        NativeKind base=expression_kind(c,n->children[0],depth+1);
        if(!is_enum(base))return N_BAD;
        ASTNode* en=c->root->children[(int)base-ENUM_KIND_BASE];
        if(en->child_count!=2||en->children[0]->child_count!=1||
           (!(name_is(en->children[0],"Ok")&&name_is(en->children[1],"Err"))&&
           !(name_is(en->children[0],"Some")&&name_is(en->children[1],"None"))))return N_BAD;
        return type_kind(c,en->children[0]->children[0],0);
    }
    case AST_SLICE_EXPR: {
        if(n->child_count<2||n->child_count>3)return N_BAD;
        NativeKind base=expression_kind(c,n->children[0],depth+1);
        NativeKind elem=base==N_STRING?KRU_U8:is_array(base)?c->array_types[(int)base-ARRAY_KIND_BASE].element:is_slice(c,base)?compound_type(c,base)->element:N_BAD;
        if(elem==N_BAD)return N_BAD;
        for(uint32_t i=1;i<n->child_count;i++)if(!native_integer(expression_kind(c,n->children[i],depth+1)))return N_BAD;
        return base==N_STRING?N_STRING:kru_type_compound(&c->types,KRU_SLICE,elem,is_slice(c,base)?compound_type(c,base)->mutable:0);
    }
    case AST_INDEX_EXPR:
        if(n->child_count!=2||!native_integer(expression_kind(c,n->children[1],depth+1)))return N_BAD;
        if(is_slice(c,expression_kind(c,n->children[0],depth+1)))return compound_type(c,expression_kind(c,n->children[0],depth+1))->element;
        if(expression_kind(c,n->children[0],depth+1)==N_STRING)return N_I32;
        return is_array(expression_kind(c,n->children[0],depth+1))?array_element(c,n->children[0]):N_BAD;
    case AST_FIELD_EXPR: {
        uint32_t offset;NativeKind kind=enum_variant(c,n,&offset);
        if(kind!=N_BAD)return c->root->children[(int)kind-ENUM_KIND_BASE]->children[offset]->child_count?N_BAD:kind;
        if(n->child_count!=1)return N_BAD;
        NativeKind base=expression_kind(c,n->children[0],depth+1);
        if(is_struct(base))return field_info(c,base,n,&offset);
        return name_is(n,"len")&&(is_slice(c,base)||is_array(base)||base==N_STRING)?N_I32:N_BAD;
    }
    case AST_BLOCK_EXPR:return block_kind(c,n,depth);
    case AST_IDENT: {
        NativeLocal* local=find_local(c,n); if(local) return local->kind;
        { uint32_t tag;NativeKind en=enum_constructor(c,n,&tag);
          if(en!=N_BAD&&!c->root->children[(int)en-ENUM_KIND_BASE]->children[tag]->child_count)return en; }
        for(uint32_t i=0;i<c->root->child_count;i++) {
            ASTNode* a=c->root->children[i];
            if(a && a->type==AST_CONST_DECL && a->name_len==n->name_len && !memcmp(a->name,n->name,n->name_len) && a->child_count==1)
                return a->type_node?type_kind(c,a->type_node,0):expression_kind(c,a->children[0],depth+1);
        }
        return N_BAD;
    }
    case AST_CALL_EXPR: return call_kind(c,n,depth);
    case AST_DEREF_EXPR:
        return n->child_count==1?pointee(c,expression_kind(c,n->children[0],depth+1)):N_BAD;
    case AST_REF_EXPR:
        if(n->child_count!=1)return N_BAD;
        { NativeKind base=expression_kind(c,n->children[0],depth+1);
          if(is_array(base))return kru_type_compound(&c->types,KRU_SLICE,c->array_types[(int)base-ARRAY_KIND_BASE].element,n->is_mut);
          if(is_slice(c,base))return base;
          return kru_type_compound(&c->types,KRU_REFERENCE,base,n->is_mut); }
    case AST_UNARY_EXPR:
        if(n->child_count!=1) return N_BAD;
        if(n->op==TOKEN_MINUS&&n->children[0]->type==AST_INT_LIT)return literal_kind(c,n);
        if(n->op==TOKEN_BANG) return expression_kind(c,n->children[0],depth+1)==N_BOOL ? N_BOOL:N_BAD;
        { NativeKind operand=expression_kind(c,n->children[0],depth+1);
          return native_integer(operand)||wide_integer(operand)||(native_float(operand)&&n->op==TOKEN_MINUS)?operand:N_BAD; }
    case AST_BINARY_EXPR: {
        if(n->child_count!=2) return N_BAD;
        NativeKind a=expression_kind(c,n->children[0],depth+1), b=expression_kind(c,n->children[1],depth+1);
        NativeKind operand=binary_operand_kind(c,n,a,b,depth);
        if(n->op==TOKEN_AND_AND||n->op==TOKEN_OR_OR) return a==N_BOOL && b==N_BOOL ? N_BOOL:N_BAD;
        if(n->op==TOKEN_EQ_EQ||n->op==TOKEN_BANG_EQUAL)
            return ((a==N_STRING&&b==N_STRING)||native_integer(operand)||wide_integer(operand)||native_float(operand)||(a!=N_VOID&&a!=N_BAD&&!is_aggregate(c,a)&&b!=N_BAD&&!is_aggregate(c,b)&&(compatible(c,a,b)||compatible(c,b,a)))) ? N_BOOL:N_BAD;
        if(n->op==TOKEN_LT||n->op==TOKEN_GT||n->op==TOKEN_LT_EQUAL||n->op==TOKEN_GT_EQUAL)
            return native_integer(operand)||wide_integer(operand)||native_float(operand)||operand==KRU_CHAR ? N_BOOL:N_BAD;
        if((n->op==TOKEN_PLUS||n->op==TOKEN_MINUS)&&raw_pointer(c,a)&&native_integer(b)) return a;
        if(n->op==TOKEN_MINUS&&a==N_POINTER&&b==N_POINTER) return N_I32;
        if(native_float(operand))return n->op==TOKEN_PLUS||n->op==TOKEN_MINUS||n->op==TOKEN_STAR||n->op==TOKEN_SLASH?operand:N_BAD;
        return native_integer(operand)||wide_integer(operand) ? operand:N_BAD;
    }
    default: return N_BAD;
    }
}
static int diagnose_expression(NativeCompiler* c,ASTNode* n,unsigned depth)
{
    if(!n||depth>128||c->error)return c->error?-1:0;
    if(n->type==AST_IDENT&&expression_kind(c,n,0)==N_BAD) {
        snprintf(c->error_text,sizeof(c->error_text),"unknown identifier '%.*s'",(int)n->name_len,n->name);
        snprintf(c->error_hint,sizeof(c->error_hint),"Declare this name or check its spelling.");
        for(unsigned i=0;i<c->local_count;i++) {
            NativeLocal* local=&c->locals[i];
            if(local->length!=n->name_len||local->length>100)continue;
            unsigned differences=0;
            for(unsigned j=0;j<n->name_len;j++)differences+=local->name[j]!=n->name[j];
            if(differences<=2) {snprintf(c->error_hint,sizeof(c->error_hint),"Did you mean '%.*s'?",(int)local->length,local->name);break;}
        }
        return fail_code(c,n,"K1044",c->error_text,c->error_hint);
    }
    if(n->type==AST_CALL_EXPR&&n->child_count&&!n->is_struct_lit) {
        ASTNode* callee=n->children[0];int index=find_function(c,callee);
        int count=name_is(callee,"pr")?1:index>=0?(int)c->functions[index].argc:-1;
        if(count>=0&&n->child_count-1!=(unsigned)count) {
            snprintf(c->error_text,sizeof(c->error_text),"function '%.*s' expects %d argument%s, received %u",(int)callee->name_len,callee->name,count,count==1?"":"s",n->child_count-1);
            return fail_code(c,callee,"K1045",c->error_text,"Pass exactly the declared number of arguments.");
        }
        for(unsigned i=1;i<n->child_count;i++)if(expression_kind(c,n->children[i],0)==N_BAD&&diagnose_expression(c,n->children[i],depth+1))return -1;
        return 0;
    }
    for(unsigned i=0;i<n->child_count;i++)if(expression_kind(c,n->children[i],0)==N_BAD&&diagnose_expression(c,n->children[i],depth+1))return -1;
    if(n->type==AST_FIELD_EXPR&&expression_kind(c,n,0)==N_BAD) {
        snprintf(c->error_text,sizeof(c->error_text),"unknown field '%.*s'",(int)n->name_len,n->name);
        return fail_code(c,n,"K1046",c->error_text,"Check the field name and the receiver's declared type.");
    }
    return 0;
}
static int slot_access(NativeCompiler* c,uint32_t slot,int store)
{
    if(slot+1>c->max_slots)c->max_slots=slot+1;
    if(c->leaf_registers&&slot<4)
        return store?BYTES(c,0x49,0x89,(unsigned char)(0xC0+slot)):
                     BYTES(c,0x4C,0x89,(unsigned char)(0xC0+(slot<<3)));
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
static int ownership_use(NativeCompiler* c,NativeLocal* local,ASTNode* source)
{
    if(!local||local->drop_flag==UINT32_MAX)return 0;
    int id=kru_move_local(&c->move_flow,local->declaration);
    if(id<0||kru_move_event(&c->move_flow,(KruMoveEvent){.position=c->size,.action=KRU_MOVE_USE,.local=(uint32_t)id,.source=source}))return fail(c,source,"ownership control-flow limit");
    return 0;
}
static int ownership_exit(NativeCompiler* c)
{
    return kru_move_event(&c->move_flow,(KruMoveEvent){.position=c->size,.action=KRU_MOVE_EXIT})?fail(c,NULL,"ownership control-flow limit"):0;
}

static int reference_event(NativeCompiler* c,KruMoveAction action,ASTNode* value,ASTNode* target,unsigned mutable,ASTNode* source)
{
    int id=kru_move_local(&c->move_flow,value),to=target?kru_move_local(&c->move_flow,target):0;
    if(id<0||to<0||kru_move_event(&c->move_flow,(KruMoveEvent){.position=c->size,.action=action,.local=(uint32_t)id,.target=(uint32_t)to,.conditional=mutable,.source=source}))return fail(c,source,"reference control-flow limit");
    return 0;
}
static int direct_reference(NativeCompiler* c,NativeKind kind)
{
    const KruCompoundType* type=compound_type(c,kind);
    return type&&type->category==KRU_REFERENCE;
}
static int borrowed_value(NativeCompiler* c,NativeKind kind)
{return direct_reference(c,kind)||is_slice(c,kind)||kind==N_STRING;}
static ASTNode* reference_value(NativeCompiler* c,ASTNode* n)
{
    if(!n||!borrowed_value(c,expression_kind(c,n,0)))return NULL;
    if(n->type==AST_IDENT) {NativeLocal* local=find_local(c,n);return local?local->declaration:NULL;}
    if(n->type==AST_REF_EXPR||n->type==AST_SLICE_EXPR||n->type==AST_CALL_EXPR)return n;
    if(n->type==AST_BLOCK_EXPR)return n;
    return NULL;
}
static int reference_binding(NativeCompiler* c,NativeLocal* local,ASTNode* value,ASTNode* source)
{
    if(!borrowed_value(c,local->kind))return 0;
    return reference_event(c,source?KRU_REF_COPY:KRU_REF_CLEAR,local->declaration,source,0,value?value:local->declaration);
}
static int slot_address(NativeCompiler* c,uint32_t slot);
static int cast_bound(NativeCompiler* c,uint64_t bound,unsigned char condition)
{
    if(BYTES(c,0x48,0xBA)||imm64(c,bound)||BYTES(c,0x48,0x39,0xD0,condition,0x02,0x0F,0x0B))return -1;
    return 0;
}
static int float_bound(NativeCompiler* c,double bound,unsigned char condition)
{
    uint64_t bits;memcpy(&bits,&bound,8);
    if(BYTES(c,0x48,0xBA)||imm64(c,bits)||BYTES(c,0x66,0x48,0x0F,0x6E,0xCA,0x66,0x0F,0x2E,0xC1,condition,0x02,0x0F,0x0B))return -1;
    return 0;
}
static int numeric_cast(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    NativeKind target=type_kind(c,n->children[0]->children[0],0);
    ASTNode* value=n->children[1];NativeKind source=expression_kind(c,value,depth+1);
    if(is_enum(source)&&!is_aggregate(c,source))source=N_I32;
    if(raw_pointer(c,target))return expression(c,value,depth+1);
    uint64_t magnitude;int negative,suffixed;
    if((wide_integer(source)&&native_float(target))||(native_float(source)&&wide_integer(target))) {
        const char* helper=wide_integer(source)?"__native_int128_to_float":"__native_float_to_int128";
        ASTNode name={.name=(char*)helper,.name_len=(uint32_t)strlen(helper)};
        int fn=find_function(c,&name);if(fn<0)return fail(c,n,"missing wide conversion runtime");
        if(expression(c,value,depth+1))return -1;
        if(wide_integer(source)) {
            if(BYTES(c,0x48,0x8B,0x38,0x48,0x8B,0x70,0x08,0xBA)||imm32(c,source==KRU_I128)||
               BYTES(c,0xB9)||imm32(c,target==KRU_F32?32:64)||emit_call(c,(uint32_t)fn))return -1;
            return 0; /* Raw float bits use the native expression RAX convention. */
        }
        if(BYTES(c,0x48,0x89,0xC7,0xBE)||imm32(c,source==KRU_F32?32:64)||
           BYTES(c,0xBA)||imm32(c,target==KRU_I128)||emit_call(c,(uint32_t)fn))return -1;
        uint32_t slot;
        if(reserve_storage(c,16,&slot)||slot_access(c,slot,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,slot-1,1))return -1;
        return slot_address(c,slot);
    }
    if(wide_integer(target)||wide_integer(source)) {
        if(integer_literal(value,&magnitude,&negative,&suffixed)) {
            uint64_t high=integer_node(value)->int_high;
            if(negative){high=0-high-(magnitude!=0);magnitude=0-magnitude;}
            if(BYTES(c,0x48,0xB8)||imm64(c,magnitude))return -1;
            if(wide_integer(target)) {
                uint32_t slot;
                if(reserve_storage(c,16,&slot)||slot_access(c,slot,1)||BYTES(c,0x48,0xB8)||imm64(c,high)||slot_access(c,slot-1,1))return -1;
                return slot_address(c,slot);
            }
            return normalize_integer(c,target);
        }
        if(expression(c,value,depth+1))return -1;
        if(wide_integer(source)) {
            if(BYTES(c,0x48,0x8B,0x50,0x08,0x48,0x8B,0x00))return -1;
        } else {
            if(target==KRU_U128&&!unsigned_integer(source)&&cast_bound(c,0,0x7D))return -1;
            if(unsigned_integer(source)) {if(BYTES(c,0x31,0xD2))return -1;}
            else if(BYTES(c,0x48,0x89,0xC2,0x48,0xC1,0xFA,0x3F))return -1;
        }
        if(wide_integer(target)) {
            if(wide_integer(source)&&target!=source) {
                if(BYTES(c,0x48,0x85,0xD2,0x79,0x02,0x0F,0x0B))return -1;
            }
            uint32_t slot;
            if(reserve_storage(c,16,&slot)||slot_access(c,slot,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,slot-1,1))return -1;
            return slot_address(c,slot);
        }
        const KruPrimitive* to=kru_primitive(target);unsigned bits=to->size*8;
        uint64_t upper=to->is_signed?(UINT64_C(1)<<(bits-1))-1:bits==64?UINT64_MAX:(UINT64_C(1)<<bits)-1;
        if(!to->is_signed||source==KRU_U128) {
            if(BYTES(c,0x48,0x85,0xD2,0x74,0x02,0x0F,0x0B)||cast_bound(c,upper,0x76))return -1;
        } else {
            uint32_t positive,end;
            if(BYTES(c,0x48,0x85,0xD2)||jump(c,1,&positive)||BYTES(c,0x48,0x83,0xFA,0xFF,0x74,0x02,0x0F,0x0B)||cast_bound(c,UINT64_C(0)-(UINT64_C(1)<<(bits-1)),0x73)||jump(c,0,&end)||patch32(c,positive,c->size)||cast_bound(c,upper,0x76)||patch32(c,end,c->size))return -1;
        }
        return normalize_integer(c,target);
    }
    if(native_integer(target)&&integer_literal(value,&magnitude,&negative,&suffixed)) {
        if(BYTES(c,0x48,0xB8)||imm64(c,negative?UINT64_C(0)-magnitude:magnitude))return -1;
        return normalize_integer(c,target);
    }
    if(expression(c,value,depth+1))return -1;
    const KruPrimitive* from=kru_primitive(source);const KruPrimitive* to=kru_primitive(target);
    if(target==KRU_CHAR) {
        if(cast_bound(c,0x10FFFF,0x76))return -1;
        uint32_t below;
        if(BYTES(c,0x48,0x3D)||imm32(c,0xD800)||BYTES(c,0x0F,0x82))return -1;
        below=c->size;
        if(imm32(c,0)||cast_bound(c,0xDFFF,0x77)||patch32(c,below,c->size))return -1;
        return 0;
    }
    if(native_float(source)) {
        if(BYTES(c,0x66,0x48,0x0F,0x6E,0xC0))return -1;
        if(source==KRU_F32&&BYTES(c,0xF3,0x0F,0x5A,0xC0))return -1;
        if(native_float(target)) {
            if(target==KRU_F32) {
                if(BYTES(c,0x66,0x48,0x0F,0x7E,0xC2,0xF2,0x0F,0x5A,0xC0,0x66,0x48,0x0F,0x7E,0xC0,0x89,0xC1,0x81,0xE1)||imm32(c,0x7fffffff)||BYTES(c,0x81,0xF9)||imm32(c,0x7f800000))return -1;
                uint32_t finite,original_infinite;
                if(jump(c,2,&finite)||BYTES(c,0x48,0xC1,0xEA,0x34,0x81,0xE2)||imm32(c,0x7ff)||BYTES(c,0x81,0xFA)||imm32(c,0x7ff)||jump(c,1,&original_infinite)||BYTES(c,0x0F,0x0B)||patch32(c,finite,c->size)||patch32(c,original_infinite,c->size))return -1;
                return BYTES(c,0x89,0xC0);
            }
            return BYTES(c,0x66,0x48,0x0F,0x7E,0xC0);
        }
        unsigned bits=to->size*8;
        double upper=power_two(bits-(to->is_signed?1u:0u));
        double lower=to->is_signed?-upper:-1.0;
        if(to->is_signed&&bits<64)lower-=1.0;
        if(float_bound(c,lower,to->is_signed&&bits==64?0x73:0x77)||float_bound(c,upper,0x72))return -1;
        if(!to->is_signed&&bits==64) {
            uint64_t midpoint;double half=power_two(63);memcpy(&midpoint,&half,8);
            if(BYTES(c,0x48,0xBA)||imm64(c,midpoint)||BYTES(c,0x66,0x48,0x0F,0x6E,0xCA,0x66,0x0F,0x2E,0xC1,0x0F,0x82))return -1;
            uint32_t small=c->size,end;if(imm32(c,0)||BYTES(c,0xF2,0x0F,0x5C,0xC1,0xF2,0x48,0x0F,0x2C,0xC0,0x48,0xBA)||imm64(c,UINT64_C(1)<<63)||BYTES(c,0x48,0x09,0xD0)||jump(c,0,&end)||patch32(c,small,c->size)||BYTES(c,0xF2,0x48,0x0F,0x2C,0xC0)||patch32(c,end,c->size))return -1;
        } else if(BYTES(c,0xF2,0x48,0x0F,0x2C,0xC0))return -1;
        return normalize_integer(c,target);
    }
    if(native_float(target)) {
        unsigned char prefix=target==KRU_F32?0xF3:0xF2;
        if(!from->is_signed&&from->size==8) {
            if(BYTES(c,0x48,0x85,0xC0,0x0F,0x89))return -1;
            uint32_t low=c->size,end;
            if(imm32(c,0)||BYTES(c,0x48,0x89,0xC2,0x83,0xE2,0x01,0x48,0xD1,0xE8,0x48,0x09,0xD0,prefix,0x48,0x0F,0x2A,0xC0,prefix,0x0F,0x58,0xC0)||jump(c,0,&end)||patch32(c,low,c->size)||BYTES(c,prefix,0x48,0x0F,0x2A,0xC0)||patch32(c,end,c->size))return -1;
        } else if(BYTES(c,prefix,0x48,0x0F,0x2A,0xC0))return -1;
        return BYTES(c,0x66,0x48,0x0F,0x7E,0xC0);
    }
    unsigned bits=to->size*8;
    uint64_t upper=to->is_signed ? (UINT64_C(1)<<(bits-1))-1 : bits==64?UINT64_MAX:(UINT64_C(1)<<bits)-1;
    if(from->is_signed) {
        if(!to->is_signed) {if(cast_bound(c,0,0x7D))return -1;}
        else if(to->size<from->size) {
            uint64_t lower=UINT64_C(0)-(UINT64_C(1)<<(bits-1));
            if(cast_bound(c,lower,0x7D))return -1;
        }
        if((to->is_signed&&to->size<from->size)||(!to->is_signed&&to->size<from->size))
            if(cast_bound(c,upper,0x7E))return -1;
    } else if(to->is_signed||to->size<from->size) {
        if(cast_bound(c,upper,0x76))return -1;
    }
    return normalize_integer(c,target);
}
static int call_expression(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(name_is(n->children[0],"cast"))return numeric_cast(c,n,depth);
    int fn;
    if(name_is(n->children[0],"pr")) {
        NativeKind kind=expression_kind(c,n->children[1],depth+1);
        ASTNode name={.name=kind==N_STRING?"__native_print_str":kind==KRU_CHAR?"__native_print_char":kind==KRU_F32?"__native_print_f32":kind==KRU_F64?"__native_print_f64":kind==KRU_I128?"__native_print_i128":kind==KRU_U128?"__native_print_u128":unsigned_integer(kind)?"__native_print_u64":native_integer(kind)?"__native_print_i64":"__native_print_i32"};
        name.name_len=(uint32_t)strlen(name.name); fn=find_function(c,&name);
    } else fn=find_function(c,n->children[0]);
    if(fn<0 || n->child_count>PARAM_LIMIT+1) return fail(c,n,"unsupported native call or argument count");
    uint32_t args=n->child_count-1;
    uint32_t saved_depth=c->temp_depth;
    int sret=is_aggregate(c,c->functions[fn].result)&&!wide_integer(c->functions[fn].result);
    uint32_t word_starts[PARAM_LIMIT+1];
    if(sret) {
        word_starts[0]=0;
        uint32_t slot;
        if(reserve_storage(c,kind_size(c,c->functions[fn].result,0),&slot)||slot_address(c,slot)||push_value(c))return -1;
    }
    /* Fast path: all-scalar GP arguments (at most six, no stack words) go
       straight into registers: earlier values wait on the stack, the last
       one is moved from RAX. */
    if(!sret&&!name_is(n->children[0],"pr")&&args>=1&&args<=6&&c->functions[fn].argc==args) {
        KruTypeId abi_types[6];KruAbiLocation locations[6];int eligible=1;
        for(uint32_t i=0;i<args&&eligible;i++) {
            NativeKind expected=c->functions[fn].params[i],arg_kind=expression_kind(c,n->children[i+1],depth+1);
            if(wide_integer(expected)||is_aggregate(c,arg_kind)||is_aggregate(c,expected)||expected==N_HANDLE||arg_kind==N_BAD)eligible=0;
            abi_types[i]=expected;
        }
        if(eligible&&!kru_x64_scalar_arguments(abi_types,args,locations)) {
            for(uint32_t i=0;i<args;i++)if(locations[i].kind!=KRU_ABI_GP||locations[i].words!=1||locations[i].index!=i)eligible=0;
        } else eligible=0;
        if(eligible) {
            static const unsigned char mov_reg[6][3]={{0x48,0x89,0xC7},{0x48,0x89,0xC6},{0x48,0x89,0xC2},{0x48,0x89,0xC1},{0x49,0x89,0xC0},{0x49,0x89,0xC1}};
            for(uint32_t i=0;i<args;i++) {
                if(expression_as(c,n->children[i+1],c->functions[fn].params[i],depth+1))return -1;
                if(i+1<args&&push_value(c))return -1;
            }
            if(emit(c,mov_reg[args-1],3))return -1;
            for(uint32_t i=args-1;i-->0;) {
                static const unsigned char pop_reg[6][2]={{0x5F,0},{0x5E,0},{0x5A,0},{0x59,0},{0x41,0x58},{0x41,0x59}};
                if(c->temp_depth<=saved_depth)return fail(c,NULL,"unbalanced native temporary");
                if(emit(c,pop_reg[i],i>=4?2:1))return -1;
                c->temp_depth--;
            }
            for(uint32_t i=1;i<n->child_count;i++) {
                ASTNode* value=reference_value(c,n->children[i]);
                const KruCompoundType* type=compound_type(c,expression_kind(c,n->children[i],depth+1));
                if(value&&type&&(type->category==KRU_REFERENCE||type->category==KRU_SLICE)&&type->mutable) {
                    for(uint32_t j=1;j<i;j++)if(value==reference_value(c,n->children[j]))return fail(c,n,"reference alias conflict: repeated mutable call argument");
                }
                if(value&&reference_event(c,KRU_REF_USE,value,NULL,0,n->children[i]))return -1;
            }
            if(emit_call(c,(uint32_t)fn))return -1;
            c->temp_depth=saved_depth;
            if(native_float(c->functions[fn].result))return BYTES(c,0x66,0x48,0x0F,0x7E,0xC0);
            if(wide_integer(c->functions[fn].result)) {
                uint32_t slot;
                if(reserve_storage(c,16,&slot)||slot_access(c,slot,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,slot-1,1))return -1;
                return slot_address(c,slot);
            }
            return 0;
        }
    }
    /* Evaluate left to right into temporary slots before populating the ABI.
       This also protects earlier arguments from nested calls. */
    for(uint32_t i=0;i<args;i++) {
        word_starts[i+(uint32_t)sret]=c->temp_depth-saved_depth;
        NativeKind arg_kind=expression_kind(c,n->children[i+1],depth+1);
        NativeKind expected=name_is(n->children[0],"pr")?arg_kind:c->functions[fn].params[i];
        if(wide_integer(expected)) {
            if(expression_as(c,n->children[i+1],expected,depth+1)||BYTES(c,0x48,0x89,0xC2,0x48,0x8B,0x02)||push_value(c)||BYTES(c,0x48,0x8B,0x42,0x08)||push_value(c))return -1;
            continue;
        }
        if(is_aggregate(c,arg_kind)) {
            uint32_t slot;
            if(reserve_storage(c,kind_size(c,arg_kind,0),&slot)||initialize_storage(c,slot,0,arg_kind,n->children[i+1])||slot_address(c,slot))return -1;
        } else if(expression_as(c,n->children[i+1],name_is(n->children[0],"pr")?arg_kind:c->functions[fn].params[i],depth+1))return -1;
        if(c->functions[fn].params[i]==N_HANDLE && expression_kind(c,n->children[i+1],depth+1)==N_NULL)
            if(BYTES(c,0x48,0xC7,0xC0,0xFF,0xFF,0xFF,0xFF))return -1;
        if(push_value(c))return -1;
    }
    for(uint32_t i=1;i<n->child_count;i++) {
        ASTNode* value=reference_value(c,n->children[i]);
        const KruCompoundType* type=compound_type(c,expression_kind(c,n->children[i],depth+1));
        if(value&&type&&(type->category==KRU_REFERENCE||type->category==KRU_SLICE)&&type->mutable) {
            for(uint32_t j=1;j<i;j++)if(value==reference_value(c,n->children[j]))return fail(c,n,"reference alias conflict: repeated mutable call argument");
        }
        if(value&&reference_event(c,KRU_REF_USE,value,NULL,0,n->children[i]))return -1;
    }
    args+=(uint32_t)sret;
    uint32_t input_words=c->temp_depth-saved_depth;
    KruTypeId abi_types[PARAM_LIMIT+1];KruAbiLocation locations[PARAM_LIMIT+1];
    if(sret)abi_types[0]=N_POINTER;
    for(uint32_t i=0;i<c->functions[fn].argc;i++)abi_types[i+(uint32_t)sret]=c->functions[fn].params[i];
    uint32_t stack_args=kru_x64_scalar_arguments(abi_types,args,locations);
    uint32_t padding=(c->temp_depth+stack_args)&1u;
    uint32_t outgoing=stack_args+padding;
    if(outgoing&&(BYTES(c,0x48,0x81,0xEC)||imm32(c,outgoing*8)))return -1;
    c->temp_depth+=outgoing;
    for(uint32_t i=0;i<args;i++) {
        KruAbiLocation location=locations[i];
        for(uint32_t word=0;word<location.words;word++) {
        uint32_t at=(outgoing+input_words-1-word_starts[i]-word)*8;
        if(location.kind==KRU_ABI_STACK) {
            if(BYTES(c,0x48,0x8B,0x84,0x24)||imm32(c,at)||BYTES(c,0x48,0x89,0x84,0x24)||imm32(c,(location.index+word)*8))return -1;
        } else if(location.kind==KRU_ABI_SSE) {
            if(BYTES(c,0xF3,0x0F,0x7E,(unsigned char)(0x84+(location.index<<3)),0x24)||imm32(c,at))return -1;
        } else {
        static const unsigned char modrm[]={0xBC,0xB4,0x94,0x8C,0x84,0x8C};
        if(BYTES(c,location.index+word>=4?0x4C:0x48,0x8B,modrm[location.index+word],0x24)||imm32(c,at))return -1;
        }
        }
    }
    if(emit_call(c,(uint32_t)fn))return -1;
    if(input_words+outgoing)if(BYTES(c,0x48,0x81,0xC4)||imm32(c,(input_words+outgoing)*8))return -1;
    c->temp_depth=saved_depth;
    if(native_float(c->functions[fn].result))return BYTES(c,0x66,0x48,0x0F,0x7E,0xC0);
    if(wide_integer(c->functions[fn].result)) {
        uint32_t slot;
        if(reserve_storage(c,16,&slot)||slot_access(c,slot,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,slot-1,1))return -1;
        return slot_address(c,slot);
    }
    return 0;
}
static int slot_address(NativeCompiler* c,uint32_t slot)
{
    if(slot+1>c->max_slots)c->max_slots=slot+1;
    if(BYTES(c,0x48,0x8D,0x85))return -1;
    return imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)));
}
static int reserve_storage(NativeCompiler* c,uint32_t bytes,uint32_t* slot)
{
    uint32_t needed=(bytes+7)/8;if(!needed)needed=1;
    if(bytes>=16&&((c->slots+needed)&1u))needed++;
    if(needed>LOCAL_LIMIT-c->slots)return fail(c,NULL,"native stack layout limit exceeded");
    c->slots+=needed;*slot=c->slots-1;
    if(c->slots>c->max_slots)c->max_slots=c->slots;
    return 0;
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
static int load_local(NativeCompiler* c,uint32_t slot,NativeKind kind)
{
    /* Only the proven leaf subset can keep a slot in R8-R11. */
    if(c->leaf_registers&&slot<4) {
        if(BYTES(c,0x4C,0x89,(unsigned char)(0xC0+(slot<<3))))return -1;
        return normalize_integer(c,kind);
    }
    /* Fold address formation into the load without changing extension rules. */
    if(slot+1>c->max_slots)c->max_slots=slot+1;
    if(native_integer(kind)) {
        const KruPrimitive* type=kru_primitive(kind);
        if(type->size==1) {if(type->is_signed?BYTES(c,0x48,0x0F,0xBE,0x85):BYTES(c,0x0F,0xB6,0x85))return -1;}
        else if(type->size==2) {if(type->is_signed?BYTES(c,0x48,0x0F,0xBF,0x85):BYTES(c,0x0F,0xB7,0x85))return -1;}
        else if(type->size==4) {if(type->is_signed?BYTES(c,0x48,0x63,0x85):BYTES(c,0x8B,0x85))return -1;}
        else if(BYTES(c,0x48,0x8B,0x85))return -1;
    } else if(kind==N_BOOL) {if(BYTES(c,0x0F,0xB6,0x85))return -1;}
    else if(kind==KRU_F32||kind==KRU_CHAR) {if(BYTES(c,0x8B,0x85))return -1;}
    else if(is_enum(kind)) {if(BYTES(c,0x48,0x63,0x85))return -1;}
    else if(BYTES(c,0x48,0x8B,0x85))return -1;
    return imm32(c,(uint32_t)(-(int32_t)((slot+1)*8)));
}
static int load_address(NativeCompiler* c,NativeKind kind)
{
    if(is_aggregate(c,kind))return 0;
    if(kind==N_BOOL)return BYTES(c,0x0F,0xB6,0x00);
    if(kind==KRU_F32||kind==KRU_CHAR)return BYTES(c,0x8B,0x00);
    if(native_integer(kind)) {
        const KruPrimitive* t=kru_primitive(kind);
        if(t->size==1)return t->is_signed?BYTES(c,0x48,0x0F,0xBE,0x00):BYTES(c,0x0F,0xB6,0x00);
        if(t->size==2)return t->is_signed?BYTES(c,0x48,0x0F,0xBF,0x00):BYTES(c,0x0F,0xB7,0x00);
        if(t->size==4)return t->is_signed?BYTES(c,0x48,0x63,0x00):BYTES(c,0x8B,0x00);
    }
    return kind==N_I32||is_enum(kind)?BYTES(c,0x48,0x63,0x00):BYTES(c,0x48,0x8B,0x00);
}
static int store_address(NativeCompiler* c,NativeKind kind)
{
    if(kind==N_BOOL)return BYTES(c,0x88,0x01);
    if(kind==KRU_F32||kind==KRU_CHAR)return BYTES(c,0x89,0x01);
    if(native_integer(kind)) {
        unsigned size=kru_primitive(kind)->size;
        if(size==1)return BYTES(c,0x88,0x01);
        if(size==2)return BYTES(c,0x66,0x89,0x01);
        if(size==4)return BYTES(c,0x89,0x01);
    }
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
    if(!value_fits(c,kind,value,0))return fail(c,value,"native initializer type mismatch");
    if(storage_address(c,slot,offset)||push_value(c)||expression_as(c,value,kind,0)||pop_value(c))return -1;
    if(is_aggregate(c,kind)?copy_bytes(c,bytes):store_address(c,kind))return -1;
    return mark_move(c,value,kind);
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
/* Left operand RCX, right operand RAX. Both are normalized to their type. */
static int integer_operation(NativeCompiler* c,TokenType op,NativeKind kind)
{
    switch(op) {
    case TOKEN_PLUS:if(BYTES(c,0x48,0x01,0xC8))return -1;break;
    case TOKEN_MINUS:if(BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8))return -1;break;
    case TOKEN_STAR:if(BYTES(c,0x48,0x0F,0xAF,0xC1))return -1;break;
    case TOKEN_SLASH:case TOKEN_PERCENT:
        if(BYTES(c,0x48,0x91))return -1;
        if(unsigned_integer(kind)) {if(BYTES(c,0x31,0xD2,0x48,0xF7,0xF1))return -1;}
        else if(BYTES(c,0x48,0x99,0x48,0xF7,0xF9))return -1;
        if(op==TOKEN_PERCENT&&BYTES(c,0x48,0x89,0xD0))return -1;
        break;
    case TOKEN_AMP:if(BYTES(c,0x48,0x21,0xC8))return -1;break;
    case TOKEN_PIPE:if(BYTES(c,0x48,0x09,0xC8))return -1;break;
    case TOKEN_CARET:if(BYTES(c,0x48,0x31,0xC8))return -1;break;
    case TOKEN_SHL:case TOKEN_SHR:
        if(BYTES(c,0x48,0x91,0x48,0xD3,op==TOKEN_SHL?0xE0:unsigned_integer(kind)?0xE8:0xF8))return -1;
        break;
    default:return fail(c,NULL,"unsupported native integer operator");
    }
    return normalize_integer(c,kind);
}
static int float_operation(NativeCompiler* c,TokenType op,NativeKind kind)
{
    if(BYTES(c,0x66,0x48,0x0F,0x6E,0xC1,0x66,0x48,0x0F,0x6E,0xC8))return -1;
    if(op==TOKEN_PLUS||op==TOKEN_MINUS||op==TOKEN_STAR||op==TOKEN_SLASH) {
        unsigned char opcode=op==TOKEN_PLUS?0x58:op==TOKEN_MINUS?0x5C:op==TOKEN_STAR?0x59:0x5E;
        if(BYTES(c,kind==KRU_F32?0xF3:0xF2,0x0F,opcode,0xC1))return -1;
        return BYTES(c,0x66,0x48,0x0F,0x7E,0xC0);
    }
    if(kind==KRU_F64&&BYTES(c,0x66))return -1;
    if(BYTES(c,0x0F,0x2E,0xC1))return -1;
    unsigned char cc=op==TOKEN_EQ_EQ?0x94:op==TOKEN_BANG_EQUAL?0x95:op==TOKEN_LT?0x92:op==TOKEN_LT_EQUAL?0x96:op==TOKEN_GT?0x97:0x93;
    if(BYTES(c,0x0F,cc,0xC0))return -1;
    if(op==TOKEN_EQ_EQ||op==TOKEN_LT||op==TOKEN_LT_EQUAL) {
        if(BYTES(c,0x0F,0x9B,0xC2,0x20,0xD0))return -1;
    } else if(op==TOKEN_BANG_EQUAL&&BYTES(c,0x0F,0x9A,0xC2,0x08,0xD0))return -1;
    return BYTES(c,0x0F,0xB6,0xC0);
}
static int wide_result(NativeCompiler* c)
{
    uint32_t slot;
    if(reserve_storage(c,16,&slot)||slot_access(c,slot,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,slot-1,1))return -1;
    return slot_address(c,slot);
}
static int wide_operation(NativeCompiler* c,TokenType op,NativeKind kind)
{
    const char* name;
    int comparison=0;
    switch(op) {
    case TOKEN_PLUS:name="__native_add128";break;
    case TOKEN_MINUS:name="__native_sub128";break;
    case TOKEN_STAR:name="__native_mul128";break;
    case TOKEN_SLASH:name=kind==KRU_I128?"__native_sdiv128":"__native_udiv128";break;
    case TOKEN_PERCENT:name=kind==KRU_I128?"__native_smod128":"__native_umod128";break;
    case TOKEN_AMP:name="__native_and128";break;
    case TOKEN_PIPE:name="__native_or128";break;
    case TOKEN_CARET:name="__native_xor128";break;
    case TOKEN_SHL:name="__native_shl128";break;
    case TOKEN_SHR:name=kind==KRU_I128?"__native_sshr128":"__native_ushr128";break;
    case TOKEN_EQ_EQ:case TOKEN_BANG_EQUAL:case TOKEN_LT:case TOKEN_GT:case TOKEN_LT_EQUAL:case TOKEN_GT_EQUAL:
        name="__native_cmp128";comparison=1;break;
    default:return fail(c,NULL,"invalid native 128-bit operation");
    }
    /* Left and right are snapshot addresses; helpers receive four SysV words. */
    if(BYTES(c,0x48,0x8B,0x10,0x48,0x8B,0x40,0x08,0x48,0x8B,0x39,0x48,0x8B,0x71,0x08,0x48,0x89,0xC1))return -1;
    if(comparison&&(BYTES(c,0x41,0xB8)||imm32(c,kind==KRU_I128)))return -1;
    ASTNode ident={.name=(char*)name,.name_len=(uint32_t)strlen(name)};
    int fn=find_function(c,&ident);if(fn<0)return fail(c,NULL,"missing native 128-bit helper");
    if(emit_call(c,(uint32_t)fn))return -1;
    if(!comparison)return wide_result(c);
    unsigned char cc=op==TOKEN_EQ_EQ?0x94:op==TOKEN_BANG_EQUAL?0x95:op==TOKEN_LT?0x9C:op==TOKEN_GT?0x9F:op==TOKEN_LT_EQUAL?0x9E:0x9D;
    return BYTES(c,0x48,0x83,0xF8,0x00,0x0F,cc,0xC0,0x0F,0xB6,0xC0);
}
static int slice_expression(NativeCompiler* c,ASTNode* n,ASTNode* base,ASTNode* low,ASTNode* high,uint32_t depth)
{
    NativeKind bk=expression_kind(c,base,depth+1);
    NativeKind elem=bk==N_STRING?KRU_U8:is_slice(c,bk)?compound_type(c,bk)->element:c->array_types[(int)bk-ARRAY_KIND_BASE].element;
    uint32_t slot,width=kind_size(c,elem,0);
    if(reserve_storage(c,16,&slot)||expression(c,base,depth+1))return -1;
    if(bk==N_STRING||is_slice(c,bk)) { if(BYTES(c,0x48,0x8B,0x48,0x08,0x48,0x8B,0x00))return -1; }
    else if(BYTES(c,0x48,0xB9)||imm64(c,array_count(c,base)))return -1;
    if(push_value(c)||BYTES(c,0x48,0x89,0xC8)||push_value(c))return -1;
    if(low) {if(expression(c,low,depth+1))return -1;} else if(BYTES(c,0x31,0xC0))return -1;
    if(push_value(c))return -1;
    if(high) {if(expression(c,high,depth+1))return -1;} else if(BYTES(c,0x48,0x8B,0x44,0x24,0x08))return -1;
    if(pop_value(c)||BYTES(c,0x48,0x8B,0x14,0x24,0x48,0x39,0xC1,0x76,0x02,0x0F,0x0B,0x48,0x39,0xD0,0x76,0x02,0x0F,0x0B,0x48,0x89,0xCE,0x48,0x29,0xC8,0x48,0x89,0xC2)||pop_value(c)||pop_value(c))return -1;
    if(BYTES(c,0x48,0x89,0xF0))return -1;
    if(width!=1&&(BYTES(c,0x48,0x69,0xC0)||imm32(c,width)))return -1;
    if(BYTES(c,0x48,0x01,0xC8)||slot_access(c,slot,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,slot-1,1))return -1;
    if(slot_address(c,slot))return -1;
    if(bk==N_STRING) {
        ASTNode name={.name="__native_validate_str",.name_len=21};
        int fn=find_function(c,&name);if(fn<0)return fail(c,n,"missing string validation runtime");
        return BYTES(c,0x48,0x89,0xC7)||emit_call(c,(uint32_t)fn);
    }
    return 0;
}
/* ---- Fast integer binary path -------------------------------------------
   When the right operand is a literal or a plain integer local it needs no
   temporary: the left value stays in RAX and the right value is loaded
   straight into RCX.  This removes the push/pop pair, the redundant
   re-extension and, for conditions, the setcc/movzx/test round trip. */
typedef struct {int literal; int64_t value; NativeLocal* local;} FastOperand;
static int is_comparison_op(TokenType op)
{ return op==TOKEN_EQ_EQ||op==TOKEN_BANG_EQUAL||op==TOKEN_LT||op==TOKEN_GT||op==TOKEN_LT_EQUAL||op==TOKEN_GT_EQUAL; }
static unsigned char compare_setcc(TokenType op,int is_unsigned)
{
    if(is_unsigned)return op==TOKEN_EQ_EQ?0x94:op==TOKEN_BANG_EQUAL?0x95:op==TOKEN_LT?0x92:op==TOKEN_GT?0x97:op==TOKEN_LT_EQUAL?0x96:0x93;
    return op==TOKEN_EQ_EQ?0x94:op==TOKEN_BANG_EQUAL?0x95:op==TOKEN_LT?0x9C:op==TOKEN_GT?0x9F:op==TOKEN_LT_EQUAL?0x9E:0x9D;
}
static int fast_integer_binary(NativeCompiler* c,ASTNode* n,NativeKind a,NativeKind b,NativeKind operand,FastOperand* f)
{
    if(operand==N_BAD||!native_integer(operand)||n->child_count!=2)return 0;
    switch(n->op) {
    case TOKEN_PLUS:case TOKEN_MINUS:case TOKEN_STAR:case TOKEN_SLASH:case TOKEN_PERCENT:
    case TOKEN_AMP:case TOKEN_PIPE:case TOKEN_CARET:case TOKEN_SHL:case TOKEN_SHR:break;
    default:if(!is_comparison_op(n->op))return 0;
    }
    uint64_t magnitude;int negative,suffixed;
    int left_literal=integer_literal(n->children[0],&magnitude,&negative,&suffixed);
    if(a!=operand&&!left_literal)return 0;
    if(integer_literal(n->children[1],&magnitude,&negative,&suffixed)) {
        f->literal=1;f->local=NULL;f->value=(int64_t)(negative?UINT64_C(0)-magnitude:magnitude);
        return 1;
    }
    if(b==operand&&n->children[1]->type==AST_IDENT&&!n->children[1]->child_count) {
        NativeLocal* local=find_local(c,n->children[1]);
        if(local&&local->kind==operand&&!is_aggregate(c,local->kind)) {f->literal=0;f->local=local;f->value=0;return 1;}
    }
    return 0;
}
static int fast_load_rcx(NativeCompiler* c,ASTNode* n,NativeKind operand,const FastOperand* f,uint32_t depth)
{
    if(f->literal) {
        if(!value_fits(c,operand,n,depth))return fail(c,n,"native integer literal does not fit expected type");
        if(f->value>=INT32_MIN&&f->value<=INT32_MAX)return BYTES(c,0x48,0xC7,0xC1)||imm32(c,(uint32_t)(int32_t)f->value);
        return BYTES(c,0x48,0xB9)||imm64(c,(uint64_t)f->value);
    }
    NativeLocal* local=f->local;
    if(reference_event(c,KRU_REF_ACCESS,local->declaration,NULL,0,n))return -1;
    if(borrowed_value(c,local->kind)&&reference_event(c,KRU_REF_USE,local->declaration,NULL,0,n))return -1;
    if(ownership_use(c,local,n))return -1;
    if(c->leaf_registers&&local->slot<4)
        return BYTES(c,0x4C,0x89,(unsigned char)(0xC1+(local->slot<<3)));
    if(local->slot+1>c->max_slots)c->max_slots=local->slot+1;
    const KruPrimitive* type=kru_primitive(operand);
    if(type->size==1) {if(type->is_signed?BYTES(c,0x48,0x0F,0xBE,0x8D):BYTES(c,0x0F,0xB6,0x8D))return -1;}
    else if(type->size==2) {if(type->is_signed?BYTES(c,0x48,0x0F,0xBF,0x8D):BYTES(c,0x0F,0xB7,0x8D))return -1;}
    else if(type->size==4) {if(type->is_signed?BYTES(c,0x48,0x63,0x8D):BYTES(c,0x8B,0x8D))return -1;}
    else if(BYTES(c,0x48,0x8B,0x8D))return -1;
    return imm32(c,(uint32_t)(-(int32_t)((local->slot+1)*8)));
}
/* Literal RHS that fits a sign-extended imm32: operate on RAX directly. */
static int fast_imm_alu(NativeCompiler* c,TokenType op,const FastOperand* f,int* done)
{
    *done=0;
    if(!f->literal||f->value<INT32_MIN||f->value>INT32_MAX)return 0;
    if(op==TOKEN_STAR) {
        *done=1;
        if(f->value==0)return BYTES(c,0x31,0xC0);
        if(f->value==1)return 0;
        if(f->value==-1)return BYTES(c,0x48,0xF7,0xD8);
        if(f->value==3||f->value==5||f->value==9)
            return BYTES(c,0x48,0x8D,0x04,(unsigned char)(f->value==3?0x40:f->value==5?0x80:0xC0));
        if(f->value>0&&((uint64_t)f->value&((uint64_t)f->value-1))==0) {
            unsigned shift=0;for(uint64_t bits=(uint64_t)f->value;bits>1;bits>>=1)shift++;
            return BYTES(c,0x48,0xC1,0xE0,(unsigned char)shift);
        }
        if(f->value>=INT8_MIN&&f->value<=INT8_MAX)
            return BYTES(c,0x48,0x6B,0xC0,(unsigned char)(int8_t)f->value);
        return BYTES(c,0x48,0x69,0xC0)||imm32(c,(uint32_t)(int32_t)f->value);
    }
    unsigned char opcode;
    switch(op) {
    case TOKEN_PLUS:opcode=0x05;break; case TOKEN_MINUS:opcode=0x2D;break;
    case TOKEN_AMP:opcode=0x25;break; case TOKEN_PIPE:opcode=0x0D;break;
    case TOKEN_CARET:opcode=0x35;break;
    default:if(!is_comparison_op(op))return 0;opcode=0x3D;
    }
    *done=1;return BYTES(c,0x48,opcode)||imm32(c,(uint32_t)(int32_t)f->value);
}
/* Returns 0 on success with *imm set when the operation was already applied. */
static int fast_rhs_apply(NativeCompiler* c,TokenType op,ASTNode* n,NativeKind operand,const FastOperand* f,uint32_t depth,int* applied)
{
    *applied=0;
    if(f->literal&&!value_fits(c,operand,n,depth))return fail(c,n,"native integer literal does not fit expected type");
    if(fast_imm_alu(c,op,f,applied))return -1;
    if(*applied)return 0;
    return fast_load_rcx(c,n,operand,f,depth);
}
/* LHS in RAX, RHS in RCX. */
static int fast_integer_op(NativeCompiler* c,TokenType op,NativeKind kind)
{
    switch(op) {
    case TOKEN_PLUS:if(BYTES(c,0x48,0x01,0xC8))return -1;break;
    case TOKEN_MINUS:if(BYTES(c,0x48,0x29,0xC8))return -1;break;
    case TOKEN_STAR:if(BYTES(c,0x48,0x0F,0xAF,0xC1))return -1;break;
    case TOKEN_AMP:if(BYTES(c,0x48,0x21,0xC8))return -1;break;
    case TOKEN_PIPE:if(BYTES(c,0x48,0x09,0xC8))return -1;break;
    case TOKEN_CARET:if(BYTES(c,0x48,0x31,0xC8))return -1;break;
    case TOKEN_SLASH:case TOKEN_PERCENT:
        if(unsigned_integer(kind)) {if(BYTES(c,0x31,0xD2,0x48,0xF7,0xF1))return -1;}
        else if(BYTES(c,0x48,0x99,0x48,0xF7,0xF9))return -1;
        if(op==TOKEN_PERCENT&&BYTES(c,0x48,0x89,0xD0))return -1;
        break;
    case TOKEN_SHL:if(BYTES(c,0x48,0xD3,0xE0))return -1;break;
    case TOKEN_SHR:if(BYTES(c,0x48,0xD3,unsigned_integer(kind)?0xE8:0xF8))return -1;break;
    default:return fail(c,NULL,"unsupported native integer operator");
    }
    return normalize_integer(c,kind);
}
/* Emits the comparison only (flags set); returns 1 if handled, 0 if not
   eligible, -1 on error. */
static int fast_compare_flags(NativeCompiler* c,ASTNode* cond,NativeKind* operand_out)
{
    if(cond->type!=AST_BINARY_EXPR||cond->child_count!=2||!is_comparison_op(cond->op))return 0;
    NativeKind a=expression_kind(c,cond->children[0],1),b=expression_kind(c,cond->children[1],1);
    NativeKind operand=binary_operand_kind(c,cond,a,b,0);
    FastOperand f;
    if(!fast_integer_binary(c,cond,a,b,operand,&f))return 0;
    int applied;
    if(expression_as(c,cond->children[0],operand,1)||fast_rhs_apply(c,cond->op,cond->children[1],operand,&f,1,&applied))return -1;
    if(!applied&&BYTES(c,0x48,0x39,0xC8))return -1;
    *operand_out=operand;return 1;
}
/* Jump to *patch when the bool condition is false. */
static int branch_if_false(NativeCompiler* c,ASTNode* cond,uint32_t* patch)
{
    NativeKind operand=N_BAD;
    int fused=fast_compare_flags(c,cond,&operand);
    if(fused<0)return -1;
    if(fused)return jump_op(c,1,(unsigned char)((compare_setcc(cond->op,unsigned_integer(operand))-0x10)^1),patch);
    if(expression(c,cond,0)||BYTES(c,0x85,0xC0))return -1;
    return jump(c,1,patch);
}
static int expression_emit(NativeCompiler* c,ASTNode* n,uint32_t depth);
static int expression(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    if(n&&n->type==AST_IDENT) {
        NativeLocal* local=find_local(c,n);
        if(local&&reference_event(c,KRU_REF_ACCESS,local->declaration,NULL,0,n))return -1;
        if(local&&borrowed_value(c,local->kind)&&reference_event(c,KRU_REF_USE,local->declaration,NULL,0,n))return -1;
    }
    int result=expression_emit(c,n,depth);
    if(!result&&n&&reference_value(c,n)) {
        if(n->type==AST_REF_EXPR||n->type==AST_SLICE_EXPR) {
            ASTNode* base=n->children[0];NativeKind base_kind=expression_kind(c,base,0);
            if(is_slice(c,base_kind)||(n->type==AST_SLICE_EXPR&&base_kind==N_STRING)) {
                ASTNode* source=reference_value(c,base);
                result=reference_event(c,source?KRU_REF_COPY:KRU_REF_CLEAR,n,source,0,n);
            } else {
                NativeLocal* owner=base->type==AST_IDENT?find_local(c,base):NULL;
                if(owner)result=reference_event(c,KRU_REF_BORROW,n,owner->declaration,n->is_mut,n);
            }
        } else if(n->type==AST_CALL_EXPR) {
            /* Until function summaries describe exact returned provenance,
               conservatively retain every borrowed argument's loan. */
            result=reference_event(c,KRU_REF_CLEAR,n,NULL,0,n);
            int fn=find_function(c,n->children[0]);
            int runtime=fn>=0?c->functions[fn].runtime:-1;
            int independent=runtime==RT_SLICE_CONCAT||runtime==RT_SLICE_CHECKED_READ||
                            runtime==RT_SLICE_FILE_READ||runtime==RT_SLICE_ARGS_GET;
            for(unsigned i=1;!independent&&!result&&i<n->child_count;i++) {
                ASTNode* source=reference_value(c,n->children[i]);
                if(source)result=reference_event(c,KRU_REF_JOIN,n,source,0,n);
            }
        }
    }
    return result;
}
static int expression_emit(NativeCompiler* c,ASTNode* n,uint32_t depth)
{
    NativeKind kind=expression_kind(c,n,depth);
    if(kind==N_BAD) {if(diagnose_expression(c,n,0))return -1;return fail(c,n,"unsupported native expression, mismatched types, or call signature");}
    switch(n->type) {
    case AST_INT_LIT:case AST_FLOAT_LIT:return expression_as(c,n,kind,depth);
    case AST_CHAR_LIT:return BYTES(c,0xB8)||imm32(c,(uint32_t)n->int_val);
    case AST_BOOL_LIT: if(BYTES(c,0xB8)||imm32(c,n->bool_val?1:0))return -1;
            break;
    case AST_NULL_LIT: return BYTES(c,0x31,0xC0);
    case AST_ARRAY_LIT: {
        uint32_t slot;
        if(reserve_storage(c,kind_size(c,kind,0),&slot)||initialize_storage(c,slot,0,kind,n))return -1;
        return slot_address(c,slot);
    }
    case AST_TRY_EXPR: {
        NativeKind base=expression_kind(c,n->children[0],depth+1);
        if(!is_enum(c->return_kind)||!is_aggregate(c,c->return_kind))return fail(c,n,"propagation requires Option/Result function result");
        ASTNode* from=c->root->children[(int)base-ENUM_KIND_BASE];
        ASTNode* to=c->root->children[(int)c->return_kind-ENUM_KIND_BASE];
        if(to->child_count!=2||from->children[1]->name_len!=to->children[1]->name_len||memcmp(from->children[1]->name,to->children[1]->name,from->children[1]->name_len)||from->children[1]->child_count!=to->children[1]->child_count)return fail(c,n,"incompatible propagation error families");
        uint32_t from_error=0,to_error=0;
        NativeKind error_kind=N_VOID;
        if(from->children[1]->child_count) {
            error_kind=payload_info(c,base,1,0,&from_error);
            if(error_kind!=payload_info(c,c->return_kind,1,0,&to_error))return fail(c,n,"propagation error types require explicit conversion");
        }
        uint32_t pointer=c->slots++,ok;
        /* Either branch transfers the active payload. The source enum must
           relinquish ownership before cleanup on the failure path as well. */
        if(expression(c,n->children[0],depth+1)||mark_move(c,n->children[0],base)||slot_access(c,pointer,1)||BYTES(c,0x83,0x38,0x00)||jump(c,1,&ok))return -1;
        /* Failure returns immediately, preserving the caller's result storage. */
        if(slot_access(c,c->return_slot,0))return -1;
        uint32_t bytes=kind_size(c,c->return_kind,0);
        for(uint32_t at=0;at<bytes;at++)if(BYTES(c,0xC6,0x80)||imm32(c,at)||BYTES(c,0x00))return -1;
        if(BYTES(c,0xC7,0x00)||imm32(c,1))return -1;
        if(error_kind!=N_VOID) {
            if(to_error&&(BYTES(c,0x48,0x05)||imm32(c,to_error)))return -1;
            if(push_value(c)||slot_access(c,pointer,0))return -1;
            if(from_error&&(BYTES(c,0x48,0x05)||imm32(c,from_error)))return -1;
            if(load_address(c,error_kind)||pop_value(c))return -1;
            if(is_aggregate(c,error_kind)?copy_bytes(c,kind_size(c,error_kind,0)):store_address(c,error_kind))return -1;
        }
        if(slot_access(c,c->return_slot,0)||cleanup(c,0,NULL)||secure_return(c)||ownership_exit(c)||BYTES(c,0xC9,0xC3)||patch32(c,ok,c->size))return -1;
        uint32_t offset;NativeKind success=payload_info(c,base,0,0,&offset);
        if(slot_access(c,pointer,0)||(offset&&(BYTES(c,0x48,0x05)||imm32(c,offset))))return -1;
        return load_address(c,success);
    }
    case AST_SLICE_EXPR:return slice_expression(c,n,n->children[0],n->children[1],n->child_count==3?n->children[2]:NULL,depth);
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
        if(is_slice(c,expression_kind(c,n->children[0],depth+1)))return expression(c,n->children[0],depth+1)||BYTES(c,0x48,0x8B,0x40,0x08);
        if(expression(c,n->children[0],depth+1)||BYTES(c,0xB8)||imm32(c,array_count(c,n->children[0])))return -1;
        return 0;
    }
    case AST_BLOCK_EXPR: {
        ASTNode* body=n->children[0];uint32_t saved=c->local_count;
        if(c->depth==128)return fail(c,n,"native block-expression nesting limit");
        ++c->depth;uint32_t keep=c->keep_expression_value;
        for(uint32_t i=0;i<body->child_count;i++) {
            c->keep_expression_value=i+1==body->child_count;
            if(statement(c,body->children[i])) {c->keep_expression_value=keep;--c->depth;c->local_count=saved;return -1;}
        }
        ASTNode* last=body->child_count?body->children[body->child_count-1]:NULL;
        NativeLocal* moved=last&&last->type==AST_EXPR_STMT&&last->child_count&&last->children[0]->type==AST_IDENT?find_local(c,last->children[0]):NULL;
        c->keep_expression_value=keep;
        if(borrowed_value(c,kind)&&last&&last->type==AST_EXPR_STMT&&last->child_count) {
            ASTNode* source=reference_value(c,last->children[0]);
            if(reference_event(c,source?KRU_REF_COPY:KRU_REF_CLEAR,n,source,0,n))return -1;
        }
        if(last&&last->type==AST_EXPR_STMT&&last->child_count&&mark_move(c,last->children[0],kind))return -1;
        if(cleanup(c,saved,moved)){--c->depth;c->local_count=saved;return -1;}
        --c->depth;c->local_count=saved;return 0;
    }
    case AST_STRING_LIT: {
        if(c->string_count==FIXUP_LIMIT)return fail(c,n,"too many native string literals");
        unsigned char* bytes=malloc(n->string_len?n->string_len:1);size_t length=0;
        if(!bytes)return fail(c,n,"string allocation failed");
        int invalid=kru_decode_string(n->string_val,n->string_len,bytes,&length);free(bytes);
        if(invalid)return fail(c,n,"invalid string escape or Unicode scalar");
        uint32_t slot;if(reserve_storage(c,16,&slot)||BYTES(c,0x48,0x8D,0x05))return -1;
        c->strings[c->string_count++]=(NativeString){c->size,n};
        if(imm32(c,0)||slot_access(c,slot,1)||BYTES(c,0x48,0xB8)||imm64(c,length)||slot_access(c,slot-1,1))return -1;
        return slot_address(c,slot);
    }
    case AST_IDENT: {
        uint32_t tag;NativeKind en=enum_constructor(c,n,&tag);
        if(en!=N_BAD&&!find_local(c,n))return construct_enum(c,en,tag,NULL);
        NativeLocal* local=find_local(c,n);if(local) {
            if(ownership_use(c,local,n))return -1;
            if(is_aggregate(c,local->kind))return slot_address(c,local->slot);
            return load_local(c,local->slot,local->kind);
        }
        for(uint32_t i=0;i<c->root->child_count;i++) {
            ASTNode* a=c->root->children[i];
            if(a && a->type==AST_CONST_DECL && a->name_len==n->name_len && !memcmp(a->name,n->name,n->name_len))
                return expression_as(c,a->children[0],a->type_node?type_kind(c,a->type_node,0):expression_kind(c,a->children[0],depth+1),depth+1);
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
        if(expression(c,n->children[0],depth+1))return -1;
        return load_address(c,kind);
    case AST_REF_EXPR: {
        NativeKind base=expression_kind(c,n->children[0],depth+1);
        { ASTNode* root=n->children[0];while((root->type==AST_FIELD_EXPR||root->type==AST_INDEX_EXPR||root->type==AST_SLICE_EXPR)&&root->child_count)root=root->children[0];
          NativeLocal* owner=root->type==AST_IDENT?find_local(c,root):NULL;
          if(ownership_use(c,owner,n))return -1; }
        if(n->is_mut) {
            ASTNode* root=n->children[0];
            while((root->type==AST_FIELD_EXPR||root->type==AST_INDEX_EXPR||root->type==AST_SLICE_EXPR)&&root->child_count)root=root->children[0];
            if(root->type==AST_IDENT) {
                NativeLocal* owner=find_local(c,root);
                if(!owner||!owner->declaration||owner->declaration->type!=AST_VAR_STMT)return fail(c,n,"mutable reference requires mutable storage");
            } else if(root->type==AST_DEREF_EXPR) {
                const KruCompoundType* t=compound_type(c,expression_kind(c,root->children[0],depth+1));
                if(t&&t->category==KRU_REFERENCE&&!t->mutable)return fail(c,n,"cannot reborrow immutable reference as mutable");
            } else return fail(c,n,"mutable reference requires a mutable place");
        }
        if(is_slice(c,base))return expression(c,n->children[0],depth+1);
        if(is_array(base))return slice_expression(c,n,n->children[0],NULL,NULL,depth);
        if(n->children[0]->type==AST_IDENT||n->children[0]->type==AST_INDEX_EXPR||n->children[0]->type==AST_FIELD_EXPR||n->children[0]->type==AST_DEREF_EXPR)return address(c,n->children[0]);
        uint32_t slot;
        if(reserve_storage(c,kind_size(c,base,0),&slot)||initialize_storage(c,slot,0,base,n->children[0]))return -1;
        return slot_address(c,slot);
    }
    case AST_UNARY_EXPR:
        if(wide_integer(kind)) {
            if(n->children[0]->type==AST_INT_LIT&&n->op==TOKEN_MINUS)return expression_as(c,n,kind,depth);
            if(expression(c,n->children[0],depth+1)||BYTES(c,0x48,0x8B,0x50,0x08,0x48,0x8B,0x00))return -1;
            if(n->op==TOKEN_MINUS) {if(BYTES(c,0x48,0xF7,0xD8,0x48,0x83,0xD2,0x00,0x48,0xF7,0xDA))return -1;}
            else if(n->op==TOKEN_TILDE) {if(BYTES(c,0x48,0xF7,0xD0,0x48,0xF7,0xD2))return -1;}
            else return fail(c,n,"invalid native wide unary operator");
            return wide_result(c);
        }
        if(native_float(kind)) {
            if(n->children[0]->type==AST_FLOAT_LIT)return expression_as(c,n,kind,depth);
            if(expression(c,n->children[0],depth+1)||BYTES(c,0x48,0xBA)||imm64(c,kind==KRU_F32?UINT64_C(0x80000000):UINT64_C(0x8000000000000000)))return -1;
            return BYTES(c,0x48,0x31,0xD0);
        }
        if(n->op==TOKEN_MINUS&&n->children[0]->type==AST_INT_LIT)return expression_as(c,n,kind,depth);
        if(expression(c,n->children[0],depth+1))return -1;
        if(n->op==TOKEN_MINUS) {if(BYTES(c,0x48,0xF7,0xD8))return -1;}
        else if(n->op==TOKEN_TILDE) {if(BYTES(c,0x48,0xF7,0xD0))return -1;}
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
            if(n->children[0]->type==AST_BOOL_LIT)kru_move_known_branch(&c->move_flow,branch,n->op==TOKEN_AND_AND?!n->children[0]->bool_val:n->children[0]->bool_val);
            if(expression(c,n->children[1],depth+1)||BYTES(c,0x85,0xC0,0x0F,0x95,0xC0,0x0F,0xB6,0xC0)||jump(c,0,&end))return -1;
            if(patch32(c,branch,c->size)||BYTES(c,0xB8)||imm32(c,n->op==TOKEN_AND_AND?0:1))return -1;
            return patch32(c,end,c->size);
        }
        NativeKind operand=binary_operand_kind(c,n,a,b,depth);
        { FastOperand fast;
          if(fast_integer_binary(c,n,a,b,operand,&fast)) {
              int applied;
              if(expression_as(c,n->children[0],operand,depth+1)||fast_rhs_apply(c,n->op,n->children[1],operand,&fast,depth+1,&applied))return -1;
              if(!is_comparison_op(n->op)) {
                  if(applied)return normalize_integer(c,operand);
                  return fast_integer_op(c,n->op,operand);
              }
              unsigned char cmp_code[]={0x48,0x39,0xC8,0x0F,compare_setcc(n->op,unsigned_integer(operand)),0xC0,0x0F,0xB6,0xC0};
              if(applied)return BYTES(c,0x0F,cmp_code[4],0xC0,0x0F,0xB6,0xC0);
              return emit(c,cmp_code,sizeof(cmp_code));
          } }
        if(wide_integer(operand)) {
            uint32_t snapshot;
            if(reserve_storage(c,16,&snapshot)||initialize_storage(c,snapshot,0,operand,n->children[0])||slot_address(c,snapshot))return -1;
        } else if(expression_as(c,n->children[0],operand==N_BAD?a:operand,depth+1))return -1;
        if(push_value(c)||expression_as(c,n->children[1],operand==N_BAD?b:operand,depth+1)||pop_value(c))return -1;
        if(wide_integer(operand))return wide_operation(c,n->op,operand);
        if(native_float(operand))return float_operation(c,n->op,operand);
        if((n->op==TOKEN_PLUS||n->op==TOKEN_MINUS)&&raw_pointer(c,a)) {
            if(b==N_POINTER) {if(BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8,0x48,0xC1,0xF8,0x02))return -1;
            break;}
            if(BYTES(c,0x48,0xC1,0xE0,0x02))return -1;
            return n->op==TOKEN_PLUS ? BYTES(c,0x48,0x01,0xC8):BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8);
        }
        if(native_integer(operand)&&n->op!=TOKEN_EQ_EQ&&n->op!=TOKEN_BANG_EQUAL&&n->op!=TOKEN_LT&&n->op!=TOKEN_GT&&n->op!=TOKEN_LT_EQUAL&&n->op!=TOKEN_GT_EQUAL)
            return integer_operation(c,n->op,operand);
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
            if(native_integer(operand)||raw_pointer(c,a)||raw_pointer(c,b)||a==N_STRING||a==N_NULL||a==N_HANDLE||b==N_POINTER||b==N_STRING||b==N_HANDLE) {
                if(BYTES(c,0x48,0x39,0xC1))return -1;
            } else if(BYTES(c,0x39,0xC1))return -1;
            unsigned char cc=n->op==TOKEN_EQ_EQ?0x94:n->op==TOKEN_BANG_EQUAL?0x95:n->op==TOKEN_LT?0x9C:n->op==TOKEN_GT?0x9F:n->op==TOKEN_LT_EQUAL?0x9E:0x9D;
            if(unsigned_integer(operand))cc=n->op==TOKEN_EQ_EQ?0x94:n->op==TOKEN_BANG_EQUAL?0x95:n->op==TOKEN_LT?0x92:n->op==TOKEN_GT?0x97:n->op==TOKEN_LT_EQUAL?0x96:0x93;
            unsigned char code[]={0x0F,cc,0xC0,0x0F,0xB6,0xC0};return emit(c,code,sizeof(code));
        }
        default:return fail(c,n,"unsupported native binary operator");
        }
        break;
    }
    default:return fail(c,n,"unsupported native expression form");
    }
    return normalize_integer(c,kind);
}
static int block(NativeCompiler* c,ASTNode* n);
static int address(NativeCompiler* c,ASTNode* n)
{
    if(n->type==AST_IDENT) {
        NativeLocal* local=find_local(c,n);if(!local)return fail(c,n,"unknown assignment target");
        if(BYTES(c,0x48,0x8D,0x85))return -1;
        return imm32(c,(uint32_t)(-(int32_t)((local->slot+1)*8)));
    }
    if(n->type==AST_DEREF_EXPR && n->child_count==1 && pointee(c,expression_kind(c,n->children[0],0))!=N_BAD)
        return expression(c,n->children[0],0);
    if(n->type==AST_FIELD_EXPR&&n->child_count==1) {
        uint32_t offset;NativeKind base=expression_kind(c,n->children[0],0);
        if(field_info(c,base,n,&offset)==N_BAD)return fail(c,n,"unknown native field");
        if(expression(c,n->children[0],0))return -1;
        return offset ? (BYTES(c,0x48,0x05)||imm32(c,offset)):0;
    }
    if(n->type==AST_INDEX_EXPR&&n->child_count==2&&is_slice(c,expression_kind(c,n->children[0],0))) {
        uint32_t width=kind_size(c,expression_kind(c,n,0),0);
        if(expression(c,n->children[0],0)||push_value(c)||expression(c,n->children[1],0)||pop_value(c))return -1;
        if(BYTES(c,0x48,0x3B,0x41,0x08,0x72,0x02,0x0F,0x0B,0x48,0x8B,0x09))return -1;
        if(width!=1&&(BYTES(c,0x48,0x69,0xC0)||imm32(c,width)))return -1;
        return BYTES(c,0x48,0x01,0xC8);
    }
    if(n->type==AST_INDEX_EXPR&&expression_kind(c,n,0)!=N_BAD&&is_array(expression_kind(c,n->children[0],0))) {
        ASTNode* base=n->children[0];uint32_t count=array_count(c,base),width=kind_size(c,array_element(c,base),0);
        if(expression(c,base,0)||push_value(c)||expression(c,n->children[1],0)||pop_value(c))return -1;
        if(BYTES(c,0x48,0x3D)||imm32(c,count)||BYTES(c,0x0F,0x82))return -1;
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
static int secure_kind(NativeCompiler* c,NativeKind kind,unsigned depth)
{
    if(depth>64)return 0;
    if(is_struct(kind)) {
        ASTNode* type=c->root->children[(int)kind-STRUCT_KIND_BASE];if(type->is_secure)return 1;
        for(unsigned i=0;i<type->child_count;i++)if(secure_kind(c,type_kind(c,type->children[i]->type_node,0),depth+1))return 1;
    }
    if(is_array(kind))return secure_kind(c,c->array_types[(int)kind-ARRAY_KIND_BASE].element,depth+1);
    return 0;
}
static int secure_tree(NativeCompiler* c,ASTNode* n,unsigned depth)
{
    if(!n||depth>256)return 0;
    if(n->type==AST_TYPE&&secure_kind(c,type_kind(c,n,0),0))return 1;
    if(n->type==AST_CALL_EXPR&&n->child_count) {
        ASTNode* name=n->children[0];
        if(name->type==AST_IDENT) {
            int fn=find_function(c,name);if(fn>=0&&secure_kind(c,c->functions[fn].result,0))return 1;
            if(n->is_struct_lit&&secure_kind(c,named_kind(c,name),0))return 1;
        }
    }
    if(secure_tree(c,n->type_node,depth+1))return 1;
    for(unsigned i=0;i<n->child_count;i++)if(secure_tree(c,n->children[i],depth+1))return 1;
    return 0;
}
static int secure_return(NativeCompiler* c)
{
    if(!c->secure_function)return 0;
    if(c->wipe_count==FIXUP_LIMIT)return fail(c,NULL,"secure return limit");
    uint32_t extra=c->temp_depth*8;
    if(BYTES(c,0x50,0x52,0x48,0x8D,0xBD))return -1;
    uint32_t address=c->size;if(imm32(c,0)||BYTES(c,0xB9))return -1;
    uint32_t count=c->size;if(imm32(c,0)||BYTES(c,0x31,0xC0,0xF3,0xAA,0x5A,0x58))return -1;
    c->wipes[c->wipe_count++]=(typeof(c->wipes[0])){address,count,extra};
    if(BYTES(c,0x31,0xFF,0x31,0xF6,0x31,0xC9,0x45,0x31,0xC0,0x45,0x31,0xC9,0x45,0x31,0xD2,0x45,0x31,0xDB))return -1;
    if(!wide_integer(c->return_kind)&&BYTES(c,0x31,0xD2))return -1;
    for(unsigned i=native_float(c->return_kind)?1u:0u;i<8;i++)if(BYTES(c,0x66,0x0F,0xEF,(unsigned char)(0xC0+i*9)))return -1;
    return 0;
}
static int needs_drop(NativeCompiler* c,NativeKind kind,unsigned depth)
{
    if(depth>64)return 0;
    if(is_struct(kind)) {
        ASTNode* type=c->root->children[(int)kind-STRUCT_KIND_BASE];char name[1024];
        if(type->name_len+7<sizeof(name)) {
            snprintf(name,sizeof(name),"%s__drop",type->name);
            ASTNode ident={.name=name,.name_len=(uint32_t)strlen(name)};
            if(find_function(c,&ident)>=0)return 1;
        }
        if(type->is_secure)return 1;
        for(unsigned i=0;i<type->child_count;i++)if(needs_drop(c,type_kind(c,type->children[i]->type_node,0),depth+1))return 1;
    }
    if(is_array(kind))return needs_drop(c,c->array_types[(int)kind-ARRAY_KIND_BASE].element,depth+1);
    if(is_enum(kind)) {
        ASTNode* type=c->root->children[(int)kind-ENUM_KIND_BASE];
        for(unsigned i=0;i<type->child_count;i++)for(unsigned j=0;j<type->children[i]->child_count;j++)if(needs_drop(c,type_kind(c,type->children[i]->children[j],0),depth+1))return 1;
    }
    return 0;
}
static int ownership_flag(NativeCompiler* c,NativeLocal* local,int live)
{
    if(local->drop_flag==UINT32_MAX)return 0;
    int id=kru_move_local(&c->move_flow,local->declaration);
    if(id<0||kru_move_event(&c->move_flow,(KruMoveEvent){.position=c->size,.action=live?KRU_MOVE_INIT:KRU_MOVE_TAKE,.local=(uint32_t)id,.source=local->declaration}))return fail(c,local->declaration,"ownership control-flow limit");
    if(BYTES(c,0x48,0xC7,0x85)||imm32(c,(uint32_t)(-(int32_t)((local->drop_flag+1)*8)))||imm32(c,(uint32_t)live))return -1;
    return 0;
}
static int initialize_ownership(NativeCompiler* c,NativeLocal* local)
{
    if(!needs_drop(c,local->kind,0))return 0;
    if(c->slots==LOCAL_LIMIT)return fail(c,local->declaration,"ownership flag stack limit");
    local->drop_flag=c->slots++;if(c->slots>c->max_slots)c->max_slots=c->slots;
    int initialized=!(local->declaration&&
        (local->declaration->type==AST_LET_STMT||local->declaration->type==AST_VAR_STMT)&&
        !local->declaration->child_count);
    return ownership_flag(c,local,initialized);
}
static int mark_move(NativeCompiler* c,ASTNode* value,NativeKind kind)
{
    if(!value||!needs_drop(c,kind,0))return 0;
    if(value->type==AST_IDENT) {
        NativeLocal* local=find_local(c,value);
        if(local)return ownership_flag(c,local,0);
    }
    if(value->type==AST_FIELD_EXPR||value->type==AST_INDEX_EXPR)return fail(c,value,"moving owning fields requires place cleanup tracking");
    if(value->type==AST_DEREF_EXPR)return fail(c,value,"moving owning values through references requires place cleanup tracking");
    return 0;
}
static int drop_address(NativeCompiler* c,NativeKind kind,unsigned depth)
{
    if(!needs_drop(c,kind,depth))return 0;
    if(push_value(c))return -1;
    if(is_struct(kind)) {
        ASTNode* type=c->root->children[(int)kind-STRUCT_KIND_BASE];char name[1024];
        snprintf(name,sizeof(name),"%s__drop",type->name);
        ASTNode ident={.name=name,.name_len=(uint32_t)strlen(name)};int method=find_function(c,&ident);
        if(method>=0) {
            const KruCompoundType* receiver=compound_type(c,c->functions[method].params[0]);
            if(c->functions[method].argc!=1||c->functions[method].result!=N_VOID||
               !receiver||receiver->category!=KRU_REFERENCE||!receiver->mutable||receiver->element!=kind)
                return fail(c,type,"drop method must take one mutable reference and return void");
            if(BYTES(c,0x48,0x8B,0x3C,0x24)||emit_call(c,(uint32_t)method))return -1;
        }
        for(unsigned i=type->child_count;i;i--) {
            uint32_t offset;NativeKind field=field_info(c,kind,type->children[i-1],&offset);
            if(needs_drop(c,field,depth+1)) {
                if(BYTES(c,0x48,0x8B,0x04,0x24)||(offset&&(BYTES(c,0x48,0x05)||imm32(c,offset)))||drop_address(c,field,depth+1))return -1;
            }
        }
        if(type->is_secure) {
            if(BYTES(c,0x48,0x8B,0x04,0x24))return -1;
            for(uint32_t i=0;i<kind_size(c,kind,0);i++)if(BYTES(c,0xC6,0x80)||imm32(c,i)||BYTES(c,0x00))return -1;
        }
    } else if(is_array(kind)) {
        NativeArrayType array=c->array_types[(int)kind-ARRAY_KIND_BASE];uint32_t size=kind_size(c,array.element,0);
        for(unsigned i=array.count;i;i--)if(BYTES(c,0x48,0x8B,0x04,0x24)||(i>1&&(BYTES(c,0x48,0x05)||imm32(c,(i-1)*size)))||drop_address(c,array.element,depth+1))return -1;
    } else if(is_enum(kind)) {
        ASTNode* type=c->root->children[(int)kind-ENUM_KIND_BASE];
        for(unsigned i=0;i<type->child_count;i++) {
            uint32_t skip;if(BYTES(c,0x48,0x8B,0x04,0x24,0x81,0x38)||imm32(c,i)||jump(c,2,&skip))return -1;
            for(unsigned j=type->children[i]->child_count;j;j--) {
                uint32_t offset;NativeKind field=payload_info(c,kind,i,j-1,&offset);
                if(needs_drop(c,field,depth+1)&&(BYTES(c,0x48,0x8B,0x04,0x24)||BYTES(c,0x48,0x05)||imm32(c,offset)||drop_address(c,field,depth+1)))return -1;
            }
            if(patch32(c,skip,c->size))return -1;
        }
    }
    return pop_value(c);
}
static int cleanup(NativeCompiler* c,unsigned start,NativeLocal* moved)
{
    for(unsigned i=start;i<c->local_count;i++)if(&c->locals[i]!=moved&&reference_event(c,KRU_REF_END,c->locals[i].declaration,NULL,0,c->locals[i].declaration))return -1;
    /* Preserve expression/return registers while destructors execute. */
    int any=0;for(unsigned i=start;i<c->local_count;i++)if(&c->locals[i]!=moved&&needs_drop(c,c->locals[i].kind,0))any=1;
    if(!any)return 0;
    if(push_value(c)||BYTES(c,0x48,0x89,0xD0)||push_value(c))return -1;
    for(unsigned i=c->local_count;i>start;i--) {
        NativeLocal* local=&c->locals[i-1];
        if(local!=moved&&needs_drop(c,local->kind,0)) {
            uint32_t skip=0;
            if(local->drop_flag!=UINT32_MAX&&(slot_access(c,local->drop_flag,0)||BYTES(c,0x48,0x85,0xC0)||jump(c,1,&skip)))return -1;
            if(slot_address(c,local->slot)||drop_address(c,local->kind,0)||ownership_flag(c,local,0))return -1;
            if(skip&&patch32(c,skip,c->size))return -1;
        }
    }
    if(pop_value(c)||BYTES(c,0x48,0x89,0xCA)||pop_value(c)||BYTES(c,0x48,0x89,0xC8))return -1;
    return 0;
}
static int local_reference(NativeCompiler* c,ASTNode* n,unsigned depth)
{
    if(!n||depth>64)return 1;
    if(n->type==AST_REF_EXPR) {
        ASTNode* place=n->children[0];
        while((place->type==AST_FIELD_EXPR||place->type==AST_INDEX_EXPR)&&place->child_count)place=place->children[0];
        if(place->type==AST_DEREF_EXPR)return local_reference(c,place->children[0],depth+1);
        if(place->type==AST_SLICE_EXPR)return local_reference(c,place,depth+1);
        return 1; /* The referenced value itself lives in this function. */
    }
    if(n->type==AST_SLICE_EXPR) {
        NativeKind base=expression_kind(c,n->children[0],0);
        return is_array(base)?1:local_reference(c,n->children[0],depth+1);
    }
    if(n->type==AST_IDENT) {
        NativeLocal* local=find_local(c,n);if(!local)return 0;
        if(local->declaration&&local->declaration->type==AST_PARAM)return 0;
        return local->declaration&&local->declaration->child_count?local_reference(c,local->declaration->children[0],depth+1):0;
    }
    if(n->type==AST_BLOCK_EXPR)return 1;
    if(n->type==AST_CALL_EXPR) {
        for(unsigned i=1;i<n->child_count;i++)if(local_reference(c,n->children[i],depth+1))return 1;
    }
    return 0;
}
static int statement(NativeCompiler* c,ASTNode* n)
{
    if(!n)return fail(c,n,"missing native statement");
    switch(n->type) {
    case AST_BLOCK:return block(c,n);
    case AST_ARENA_STMT:return n->child_count==1?block(c,n->children[0]):fail(c,n,"invalid arena body");
    case AST_LET_STMT:case AST_VAR_STMT: {
        if(c->local_count==LOCAL_LIMIT||c->slots==LOCAL_LIMIT||n->child_count>1)return fail(c,n,"native local limit exceeded");
        NativeKind value=n->child_count ? expression_kind(c,n->children[0],0):N_VOID;
        NativeKind declared=n->type_node ? type_kind(c,n->type_node,0):value;
        if(n->child_count&&(native_integer(declared)||wide_integer(declared))) {
            uint64_t magnitude;int negative,suffixed;
            if(integer_literal(n->children[0],&magnitude,&negative,&suffixed)&&!kru_integer_fits128(declared,magnitude,integer_node(n->children[0])->int_high,negative))
                return fail_code(c,n->children[0],"K1040","integer literal out of range for declared type","Use a representable value or declare a wider integer type.");
        }
        if(n->child_count&&value==N_BAD&&diagnose_expression(c,n->children[0],0))return -1;
        if(declared==N_VOID||declared==N_BAD||(n->child_count&&!initializer_fits(c,declared,n->children[0],0)))return fail(c,n,"native binding type mismatch or unsupported type");
        ASTNode* reference_source=n->child_count?reference_value(c,n->children[0]):NULL;
        if(is_aggregate(c,declared)) {
            uint32_t bytes=kind_size(c,declared,0),slot;
            if(!valid_layout(c,declared)||reserve_storage(c,bytes,&slot)||initialize_storage(c,slot,0,declared,n->child_count?n->children[0]:NULL))return -1;
            c->locals[c->local_count++]=(NativeLocal){n->name,n->name_len,slot,declared,0,N_I32,n,UINT32_MAX};
            NativeLocal* bound=&c->locals[c->local_count-1];
            if(reference_binding(c,bound,n->child_count?n->children[0]:NULL,reference_source))return -1;
            return initialize_ownership(c,bound);
        }
        if(n->child_count) {if(expression_as(c,n->children[0],declared,0))return -1;} else if(BYTES(c,0x31,0xC0))return -1;
        uint32_t slot=c->slots++;if(slot_access(c,slot,1))return -1;
        c->locals[c->local_count++]=(NativeLocal){n->name,n->name_len,slot,declared,0,N_I32,n,UINT32_MAX};
        NativeLocal* bound=&c->locals[c->local_count-1];
        if(reference_binding(c,bound,n->child_count?n->children[0]:NULL,reference_source))return -1;
        return initialize_ownership(c,bound);
    }
    case AST_ASSIGN_STMT: {
        if(n->child_count!=2)return fail(c,n,"invalid native assignment");
        ASTNode* lhs=n->children[0]; NativeKind kind=expression_kind(c,lhs,0);
        NativeLocal* assigned=lhs->type==AST_IDENT?find_local(c,lhs):NULL;
        if(assigned&&reference_event(c,KRU_REF_ACCESS,assigned->declaration,NULL,1,lhs))return -1;
        ASTNode* written=lhs;
        while((written->type==AST_FIELD_EXPR||written->type==AST_INDEX_EXPR)&&written->child_count)written=written->children[0];
        NativeLocal* container=written!=lhs&&written->type==AST_IDENT?find_local(c,written):NULL;
        if(container&&!direct_reference(c,container->kind)&&reference_event(c,KRU_REF_ACCESS,container->declaration,NULL,1,lhs))return -1;
        if(lhs->type==AST_DEREF_EXPR) {
            const KruCompoundType* t=compound_type(c,expression_kind(c,lhs->children[0],0));
            if(t&&t->category==KRU_REFERENCE&&!t->mutable)return fail(c,lhs,"cannot write through immutable reference");
        }
        if(lhs->type==AST_INDEX_EXPR) {
            const KruCompoundType* t=compound_type(c,expression_kind(c,lhs->children[0],0));
            if(t&&t->category==KRU_SLICE&&!t->mutable)return fail(c,lhs,"cannot write through immutable slice");
        }
        if(kind==N_BAD||kind==N_VOID)return fail(c,n,"unknown assignment type");
        if(assigned&&native_integer(kind)&&(n->op==TOKEN_EOF||n->op==TOKEN_EQUALS)) {
            if(!value_fits(c,kind,n->children[1],0))return fail(c,n,"native assignment type mismatch");
            /* Each scalar local owns an eight-byte slot. Keep the write event
               above, but no computed address is needed for a plain local. */
            if(expression_as(c,n->children[1],kind,0)||slot_access(c,assigned->slot,1))return -1;
            return 0;
        }
        if(address(c,lhs)||push_value(c))return -1;
        if(wide_integer(kind)&&n->op!=TOKEN_EOF&&n->op!=TOKEN_EQUALS) {
            if(!value_fits(c,kind,n->children[1],0))return fail(c,n,"native 128-bit compound type mismatch");
            uint32_t snapshot;
            if(reserve_storage(c,16,&snapshot)||push_value(c)||slot_address(c,snapshot)||pop_value(c)||BYTES(c,0x48,0x91)||copy_bytes(c,16)||slot_address(c,snapshot)||push_value(c))return -1;
            if(expression_as(c,n->children[1],kind,0)||pop_value(c)||wide_operation(c,compound_op(n->op),kind)||pop_value(c))return -1;
            return copy_bytes(c,16);
        }
        if(is_aggregate(c,kind)) {
            ASTNode* rhs=n->children[1];
            if((n->op!=TOKEN_EOF&&n->op!=TOKEN_EQUALS)||expression_kind(c,rhs,0)!=kind)return fail(c,n,"native aggregate assignment type mismatch");
            if(needs_drop(c,kind,0)) {
                /* Evaluate and transfer the replacement before destroying the old
                   value: the RHS may read or move the destination itself. */
                uint32_t replacement,skip=0;
                NativeLocal* owner=lhs->type==AST_IDENT?find_local(c,lhs):NULL;
                if(reserve_storage(c,kind_size(c,kind,0),&replacement)||
                   initialize_storage(c,replacement,0,kind,rhs))return -1;
                if(owner&&owner->drop_flag!=UINT32_MAX&&
                   (slot_access(c,owner->drop_flag,0)||BYTES(c,0x48,0x85,0xC0)||jump(c,1,&skip)))return -1;
                if(BYTES(c,0x48,0x8B,0x04,0x24)||drop_address(c,kind,0))return -1;
                if(skip&&patch32(c,skip,c->size))return -1;
                if(slot_address(c,replacement)||pop_value(c)||copy_bytes(c,kind_size(c,kind,0)))return -1;
                if(owner)return ownership_flag(c,owner,1);
                return 0;
            }
            if(expression(c,rhs,0)||pop_value(c))return -1;
            if(copy_bytes(c,kind_size(c,kind,0)))return -1;
            return assigned?reference_binding(c,assigned,rhs,reference_value(c,rhs)):0;
        }
        if(n->op==TOKEN_EOF||n->op==TOKEN_EQUALS) {
            if(!value_fits(c,kind,n->children[1],0))return fail(c,n,"native assignment type mismatch");
            if(expression_as(c,n->children[1],kind,0))return -1;
            if(kind==N_HANDLE&&expression_kind(c,n->children[1],0)==N_NULL)
                if(BYTES(c,0x48,0xC7,0xC0,0xFF,0xFF,0xFF,0xFF))return -1;
        } else {
            TokenType op=compound_op(n->op);
            if(!value_fits(c,kind==N_POINTER?N_I32:kind,n->children[1],0)||(!native_integer(kind)&&!native_float(kind)&&kind!=N_POINTER))return fail(c,n,"invalid native compound assignment types");
            if(native_float(kind)&&op!=TOKEN_PLUS&&op!=TOKEN_MINUS&&op!=TOKEN_STAR&&op!=TOKEN_SLASH)return fail(c,n,"invalid native float compound operator");
            if(kind==N_POINTER) {if(BYTES(c,0x48,0x8B,0x00))return -1;}
            else if(load_address(c,kind))return -1;
            if(push_value(c)||expression_as(c,n->children[1],kind==N_POINTER?N_I32:kind,0)||pop_value(c))return -1;
            if(kind==N_POINTER) {
                if(op!=TOKEN_PLUS&&op!=TOKEN_MINUS)return fail(c,n,"invalid pointer compound assignment");
                if(BYTES(c,0x48,0xC1,0xE0,0x02))return -1;
                if(op==TOKEN_PLUS) {if(BYTES(c,0x48,0x01,0xC8))return -1;}
                else if(BYTES(c,0x48,0x29,0xC1,0x48,0x89,0xC8))return -1;
            } else {
                if(native_float(kind)?float_operation(c,op,kind):integer_operation(c,op,kind))return -1;

            }
        }
        if(pop_value(c))return -1;
        if(store_address(c,kind))return -1;
        return assigned?reference_binding(c,assigned,n->children[1],reference_value(c,n->children[1])):0;
    }
    case AST_EXPR_STMT: {
        uint32_t saved_slots=c->slots;
        int result=n->child_count==1 ? expression(c,n->children[0],0):fail(c,n,"invalid native expression statement");
        if(!result&&!c->keep_expression_value&&n->child_count&&needs_drop(c,expression_kind(c,n->children[0],0),0)) {
            NativeKind discarded=expression_kind(c,n->children[0],0);
            result=mark_move(c,n->children[0],discarded);
            if(!result)result=drop_address(c,discarded,0);
        }
        c->slots=saved_slots;return result;
    }
    case AST_RET_STMT:
        /* Reuse the frame only for a scalar, binding-free function. All new
           arguments are evaluated before overwriting any old parameter. */
        if(c->tail_function&&c->temp_depth==0&&n->child_count==1) {
            ASTNode* call=n->children[0];NativeFunction* fn=c->tail_function;
            if(call->type==AST_CALL_EXPR&&call->child_count==fn->argc+1&&
               name_is(call->children[0],fn->name)&&expression_kind(c,call,0)==fn->result) {
                for(uint32_t i=0;i<fn->argc;i++)
                    if(expression_as(c,call->children[i+1],fn->params[i],0)||push_value(c))return -1;
                for(uint32_t i=fn->argc;i-->0;) {
                    if(BYTES(c,0x58))return -1;
                    c->temp_depth--;
                    if(slot_access(c,c->locals[i].slot,1))return -1;
                }
                uint32_t back;
                return jump(c,0,&back)||patch32(c,back,c->tail_head);
            }
        }
        if((c->return_kind==N_STRING||is_slice(c,c->return_kind)||(compound_type(c,c->return_kind)&&compound_type(c,c->return_kind)->category==KRU_REFERENCE))&&n->child_count&&local_reference(c,n->children[0],0))return fail_code(c,n,"K3010","reference to local storage escapes its owning function","Return an owned value or a reference derived from caller-owned storage.");
        if(c->return_kind==N_VOID) {
            if(n->child_count)return fail(c,n,"value returned from void native function");
        } else {
            if(n->child_count==1&&expression_kind(c,n->children[0],0)==N_BAD&&diagnose_expression(c,n->children[0],0))return -1;
            if(n->child_count!=1||!value_fits(c,c->return_kind,n->children[0],0))return fail(c,n,"native return type mismatch");
            if(wide_integer(c->return_kind)) {
                if(expression_as(c,n->children[0],c->return_kind,0)||BYTES(c,0x48,0x8B,0x50,0x08,0x48,0x8B,0x00))return -1;
            } else if(is_aggregate(c,c->return_kind)) {
                if(slot_access(c,c->return_slot,0)||push_value(c)||expression(c,n->children[0],0)||pop_value(c)||copy_bytes(c,kind_size(c,c->return_kind,0))||BYTES(c,0x48,0x89,0xC8))return -1;
            } else if(expression_as(c,n->children[0],c->return_kind,0))return -1;
        }
        { NativeLocal* moved=n->child_count&&n->children[0]->type==AST_IDENT?find_local(c,n->children[0]):NULL;
          if(n->child_count&&mark_move(c,n->children[0],c->return_kind))return -1;
          if(cleanup(c,0,moved))return -1; }
        if(native_float(c->return_kind)&&BYTES(c,0x66,0x48,0x0F,0x6E,0xC0))return -1;
        if(secure_return(c))return -1;
        return ownership_exit(c)||BYTES(c,0xC9,0xC3);
    case AST_MATCH_EXPR: {
        if(n->child_count<2||n->child_count>513||c->slots==LOCAL_LIMIT)return fail(c,n,"native match arm/stack limit");
        NativeKind kind=expression_kind(c,n->children[0],0);
        if(!native_integer(kind)&&!wide_integer(kind)&&kind!=KRU_CHAR&&kind!=N_BOOL&&!is_enum(kind))return fail(c,n,"native match requires integer/char/bool/enum");
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
                    if(!value_fits(c,kind,pattern,0))return fail(c,pattern,"native match pattern type mismatch");
                    if(kind==N_BOOL) {
                        if(pattern->type!=AST_BOOL_LIT||seen[pattern->bool_val])return fail(c,pattern,"duplicate or invalid native bool pattern");
                        seen[pattern->bool_val]=1;
                    }
                    if(storage_address(c,slot,0)||load_address(c,kind)||push_value(c)||expression_as(c,pattern,kind,0)||pop_value(c))return -1;
                    if(wide_integer(kind)) {
                        if(wide_operation(c,TOKEN_EQ_EQ,kind)||BYTES(c,0x85,0xC0)||jump(c,1,&skip))return -1;
                    } else if(BYTES(c,0x48,0x39,0xC1)||jump(c,2,&skip))return -1;
                }
            } else wildcard=1;
            uint32_t saved=c->local_count;
            if((catchall&&!name_is(pattern,"_"))||
               (pattern->type!=AST_CALL_EXPR&&needs_drop(c,kind,0))) {
                if(c->local_count==LOCAL_LIMIT)return fail(c,pattern,"native match binding limit");
                c->locals[c->local_count++]=(NativeLocal){pattern->name,pattern->name_len,slot,kind,0,N_I32,pattern,UINT32_MAX};
                if(initialize_ownership(c,&c->locals[c->local_count-1]))return -1;
            }
            if(enumeration&&pattern->type==AST_CALL_EXPR)for(uint32_t j=1;j<pattern->child_count;j++) {
                ASTNode* binding=pattern->children[j];uint32_t offset,own;
                NativeKind fk=payload_info(c,kind,tag,j-1,&offset);
                if(binding->type!=AST_IDENT||fk==N_BAD||c->local_count==LOCAL_LIMIT)return fail(c,binding,"invalid native payload binding");
                if(name_is(binding,"_")&&!needs_drop(c,fk,0))continue;
                for(uint32_t k=saved;k<c->local_count;k++)if(!name_is(binding,"_")&&c->locals[k].length==binding->name_len&&!memcmp(c->locals[k].name,binding->name,binding->name_len))return fail(c,binding,"duplicate native payload binding");
                if(reserve_storage(c,kind_size(c,fk,0),&own)||slot_address(c,own)||push_value(c)||storage_address(c,slot,offset)||load_address(c,fk)||pop_value(c))return -1;
                if(is_aggregate(c,fk)) {if(copy_bytes(c,kind_size(c,fk,0)))return -1;}
                else if(store_address(c,fk))return -1;
                c->locals[c->local_count++]=(NativeLocal){binding->name,binding->name_len,own,fk,0,N_I32,binding,UINT32_MAX};
                if(initialize_ownership(c,&c->locals[c->local_count-1]))return -1;
            }
            if(block(c,arm->children[1])||cleanup(c,saved,NULL))return -1;
            c->local_count=saved;
            if(jump(c,0,&ends[end_count++]))return -1;
            if(!catchall&&patch32(c,skip,c->size))return -1;
        }
        if(!wildcard) {
            if(!enumeration) {
                if(kind!=N_BOOL||!seen[0]||!seen[1])return fail_code(c,n,"K1048","non-exhaustive native match requires catch-all","Add the missing cases or a catch-all arm.");
            } else for(uint32_t i=0;i<enumeration->child_count;i++)if(!seen[i])return fail_code(c,n,"K1048","non-exhaustive native enum match","Add an arm for every variant or a catch-all arm.");
        }
        if(BYTES(c,0x0F,0x0B))return -1;
        for(uint32_t i=0;i<end_count;i++)if(patch32(c,ends[i],c->size))return -1;
        return 0;
    }
    case AST_IF_STMT: {
        if(n->child_count<2||n->child_count>3||expression_kind(c,n->children[0],0)!=N_BOOL)return fail(c,n,"native if condition must be bool");
        uint32_t skip,end;if(branch_if_false(c,n->children[0],&skip))return -1;
        if(n->children[0]->type==AST_BOOL_LIT)kru_move_known_branch(&c->move_flow,skip,!n->children[0]->bool_val);
        if(block(c,n->children[1]))return -1;
        if(n->child_count==3) {
            if(jump(c,0,&end)||patch32(c,skip,c->size)||block(c,n->children[2])||patch32(c,end,c->size))return -1;
        } else if(patch32(c,skip,c->size))return -1;
        return 0;
    }
    case AST_WHILE_STMT:case AST_LOOP_STMT: {
        if(c->loop_depth==128||n->child_count!=(n->type==AST_WHILE_STMT?2u:1u))return fail(c,n,"native loop limit or shape invalid");
        uint32_t head=c->size,skip=0,start=c->break_count;
        if(n->type==AST_WHILE_STMT) {
            if(expression_kind(c,n->children[0],0)!=N_BOOL)return fail(c,n,"native while condition must be bool");
            if(branch_if_false(c,n->children[0],&skip))return -1;
            if(n->children[0]->type==AST_BOOL_LIT)kru_move_known_branch(&c->move_flow,skip,!n->children[0]->bool_val);
        }
        c->loop_local_count[c->loop_depth]=c->local_count;
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
        NativeKind collection=expression_kind(c,source,0);
        int byref=!range&&source->type==AST_REF_EXPR;
        if(range) {
            if(expression_kind(c,source,0)!=N_I32||expression_kind(c,n->children[1],0)!=N_I32)return fail(c,n,"range bounds must be i32");
        } else {
            if(!is_array(collection)&&!is_slice(c,collection))return fail(c,n,"native collection for requires array or slice");
            elem=is_slice(c,collection)?compound_type(c,collection)->element:array_element(c,source);
        }
        uint32_t saved=c->local_count,counter=c->slots++,limit=c->slots++,base=c->slots++,value;
        if(reserve_storage(c,byref?8:kind_size(c,elem,0),&value))return -1;
        if(range) {
            if(expression(c,source,0)||slot_access(c,counter,1)||expression(c,n->children[1],0)||slot_access(c,limit,1))return -1;
        } else {
            if(expression(c,source,0))return -1;
            if(is_slice(c,collection)) {
                if(BYTES(c,0x48,0x8B,0x50,0x08,0x48,0x8B,0x00)||slot_access(c,base,1)||BYTES(c,0x48,0x89,0xD0)||slot_access(c,limit,1))return -1;
            } else if(slot_access(c,base,1)||BYTES(c,0xB8)||imm32(c,array_count(c,source))||slot_access(c,limit,1))return -1;
            if(BYTES(c,0x31,0xC0)||slot_access(c,counter,1))return -1;
        }
        c->locals[c->local_count++]=(NativeLocal){n->name,n->name_len,range?counter:value,byref?kru_type_compound(&c->types,KRU_REFERENCE,elem,source->is_mut):elem,0,N_I32,n,UINT32_MAX};
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
        c->loop_local_count[c->loop_depth]=c->local_count;
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
        if(cleanup(c,c->loop_local_count[c->loop_depth-1],NULL))return -1;
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
    if(cleanup(c,saved,NULL)){--c->depth;return -1;}
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
    R("__native_print_str",RT_SLICE_PRINT,N_VOID,1,N_STRING,N_VOID);
    R("__native_validate_str",RT_SLICE_VALIDATE,N_POINTER,1,N_POINTER,N_VOID);
    R("__native_print_i64",RT_PRINT_I64,N_VOID,1,KRU_I64,N_VOID);
    R("__native_print_u64",RT_PRINT_U64,N_VOID,1,KRU_U64,N_VOID);
    R("__native_print_f32",RT_PRINT_F32,N_VOID,1,KRU_F32,N_VOID);
    R("__native_print_f64",RT_PRINT_F64,N_VOID,1,KRU_F64,N_VOID);
    R("__native_print_i128",RT_PRINT_I128,N_VOID,1,KRU_I128,N_VOID);
    R("__native_print_u128",RT_PRINT_U128,N_VOID,1,KRU_U128,N_VOID);
    R("__native_print_char",RT_PRINT_CHAR,N_VOID,1,KRU_CHAR,N_VOID);
    R("__native_add128",RT_ADD128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_sub128",RT_SUB128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_mul128",RT_MUL128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_and128",RT_AND128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_or128",RT_OR128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_xor128",RT_XOR128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_udiv128",RT_UDIV128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_sdiv128",RT_SDIV128,KRU_I128,4,KRU_U64,KRU_U64);
    R("__native_umod128",RT_UMOD128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_smod128",RT_SMOD128,KRU_I128,4,KRU_U64,KRU_U64);
    R("__native_shl128",RT_SHL128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_ushr128",RT_USHR128,KRU_U128,4,KRU_U64,KRU_U64);
    R("__native_sshr128",RT_SSHR128,KRU_I128,4,KRU_U64,KRU_U64);
    R("__native_cmp128",RT_CMP128,KRU_I64,5,KRU_U64,KRU_U64);
    R("__native_int128_to_float",RT_INT128_TO_FLOAT,KRU_U64,4,KRU_U64,KRU_U64);
    R("__native_float_to_int128",RT_FLOAT_TO_INT128,KRU_U128,3,KRU_U64,KRU_U64);
    R("__kru_str_len",RT_SLICE_LEN,N_I32,1,N_STRING,N_VOID);
    R("__kru_str_eq",RT_SLICE_EQ,N_BOOL,2,N_STRING,N_STRING);
    R("read_int",RT_SLICE_READ_INT,N_I32,1,N_STRING,N_VOID);
    R("str_len",RT_SLICE_LEN,N_I32,1,N_STRING,N_VOID);
    R("str_eq",RT_SLICE_EQ,N_BOOL,2,N_STRING,N_STRING);
    R("str_concat",RT_SLICE_CONCAT,N_STRING,2,N_STRING,N_STRING);
    R("str_free",RT_SLICE_FREE,N_VOID,1,N_STRING,N_VOID);
    R("str_index",RT_SLICE_INDEX,N_I32,2,N_STRING,N_I32);
    { ASTNode type={.type=AST_TYPE,.name="kru_string__String",.name_len=18};
      NativeKind string=type_kind(c,&type,0);
      if(string!=N_BAD) {
        R("__kru_owned_read",RT_OWNED_FILE_READ,string,2,KRU_I64,kru_type_compound(&c->types,KRU_REFERENCE,KRU_I64,1));
        R("__kru_string_push",RT_SLICE_STRING_PUSH,N_VOID,2,kru_type_compound(&c->types,KRU_REFERENCE,string,1),N_STRING);
        R("__kru_string_view",RT_STRING_VIEW,kru_type_compound(&c->types,KRU_REFERENCE,N_STRING,0),1,kru_type_compound(&c->types,KRU_REFERENCE,string,0),N_VOID);
        R("__kru_string_drop",RT_STRING_DROP,N_VOID,1,kru_type_compound(&c->types,KRU_REFERENCE,string,1),N_VOID);
      }
    }
    R("__kru_sys_open",RT_SLICE_SYS_OPEN,KRU_I64,2,N_STRING,KRU_I64);
    R("__kru_sys_close",RT_SYS_CLOSE,KRU_I64,1,KRU_I64,N_VOID);
    R("__kru_sys_write",RT_SLICE_SYS_WRITE,KRU_I64,2,KRU_I64,N_STRING);
    R("__kru_checked_read",RT_SLICE_CHECKED_READ,N_STRING,2,KRU_I64,kru_type_compound(&c->types,KRU_REFERENCE,KRU_I64,1));
    R("file_open",RT_SLICE_FILE_OPEN,N_HANDLE,1,N_STRING,N_VOID);
    R("file_open_write",RT_SLICE_FILE_OPEN_WRITE,N_HANDLE,1,N_STRING,N_VOID);
    R("file_close",RT_FILE_CLOSE,N_VOID,1,N_HANDLE,N_VOID);
    R("file_write",RT_SLICE_SYS_WRITE,N_VOID,2,N_HANDLE,N_STRING);
    R("file_read",RT_SLICE_FILE_READ,N_STRING,1,N_HANDLE,N_VOID);
    R("mem_alloc",RT_MEM_ALLOC,N_POINTER,1,N_I32,N_VOID);
    R("mem_free",RT_MEM_FREE,N_VOID,1,N_POINTER,N_VOID);
    R("mem_realloc",RT_MEM_REALLOC,N_POINTER,2,N_POINTER,N_I32);
    R("__kru_alloc",RT_MEM_ALLOC,kru_type_compound(&c->types,KRU_RAW_POINTER,KRU_U8,1),1,N_I32,N_VOID);
    R("__kru_free",RT_MEM_FREE,N_VOID,1,kru_type_compound(&c->types,KRU_RAW_POINTER,KRU_U8,1),N_VOID);
    R("__kru_realloc",RT_MEM_REALLOC,kru_type_compound(&c->types,KRU_RAW_POINTER,KRU_U8,1),2,kru_type_compound(&c->types,KRU_RAW_POINTER,KRU_U8,1),N_I32);
    R("__kru_args_count",RT_ARGS_COUNT,N_I32,0,N_VOID,N_VOID);
    R("__kru_args_get",RT_SLICE_ARGS_GET,N_STRING,1,N_I32,N_VOID);
    R("args_count",RT_ARGS_COUNT,N_I32,0,N_VOID,N_VOID);
    R("args_get",RT_SLICE_ARGS_GET,N_STRING,1,N_I32,N_VOID);
#undef R
    for(uint32_t i=0;i<c->root->child_count;i++) {
        ASTNode* n=c->root->children[i];if(!n)continue;
        if(n->generic_parameters||n->type==AST_IMPL_DECL||n->type==AST_TRAIT_DECL||n->type==AST_COMPTIME_BLOCK)continue;

        if(n->type==AST_STRUCT_DECL||n->type==AST_ENUM_DECL) {
            for(uint32_t j=0;j<n->child_count;j++)for(uint32_t k=0;k<j;k++) {
                ASTNode* x=n->children[j];ASTNode* y=n->children[k];
                if(x->name_len==y->name_len&&!memcmp(x->name,y->name,x->name_len))return fail(c,x,"duplicate native field or variant name");
            }
        }
        if(n->type==AST_CONST_DECL) {
            NativeKind kind=n->type_node?type_kind(c,n->type_node,0):expression_kind(c,n->children[0],0);
            if(n->child_count!=1||kind==N_BAD||!value_fits(c,kind,n->children[0],0))return fail(c,n,"native constant initializer does not fit declared type");
            continue;
        }
        if(n->type==AST_TYPE_ALIAS)continue;
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
        if(c->function_count==FUNCTION_LIMIT)return fail_code(c,n,"K5002","native function capacity exceeded (512 total, including runtime and specializations)","Reduce function/specialization count; this is a compiler capacity limit.");
        if(find_function(c,n)>=0)return fail_code(c,n,"K1043","duplicate or reserved native function name","Rename the function or remove its duplicate declaration.");
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
/* A deliberately conservative first tail-call pass: no bindings, references,
   owned values, cleanup scopes or secure-frame wiping can be bypassed. */
static int scalar_tail_tree(ASTNode* n,unsigned depth)
{
    if(!n||depth>200)return 0;
    switch(n->type) {
    case AST_BLOCK:case AST_IF_STMT:case AST_RET_STMT:case AST_INT_LIT:
    case AST_BOOL_LIT:case AST_IDENT:case AST_BINARY_EXPR:case AST_UNARY_EXPR:
    case AST_CALL_EXPR:break;
    default:return 0;
    }
    for(uint32_t i=0;i<n->child_count;i++)if(!scalar_tail_tree(n->children[i],depth+1))return 0;
    return 1;
}
/* Whole-function proof for a small register-placement tier. R8-R11 are
   caller-saved and unused by scalar integer expression emission. Calls,
   references, aggregate values, hidden iteration slots and cleanup are excluded.
   Slots beyond the first four retain their ordinary frame representation. */
static int leaf_integer_tree(NativeCompiler* c,ASTNode* n,unsigned depth)
{
    if(!n||depth>200||n->is_secure)return 0;
    switch(n->type) {
    case AST_LET_STMT:case AST_VAR_STMT:
        if(!n->type_node||!native_integer(type_kind(c,n->type_node,0)))return 0;
        break;
    case AST_ASSIGN_STMT:
        if(n->child_count!=2||n->children[0]->type!=AST_IDENT||
           (n->op!=TOKEN_EQUALS&&n->op!=TOKEN_EOF))return 0;
        break;
    case AST_INT_LIT:
        if(n->int_high||(n->type_node&&!native_integer(type_kind(c,n->type_node,0))))return 0;
        break;
    case AST_BLOCK:case AST_IF_STMT:case AST_WHILE_STMT:case AST_LOOP_STMT:
    case AST_BREAK_STMT:case AST_CONTINUE_STMT:case AST_RET_STMT:
    case AST_BOOL_LIT:case AST_IDENT:case AST_BINARY_EXPR:case AST_UNARY_EXPR:break;
    default:return 0;
    }
    for(uint32_t i=0;i<n->child_count;i++)if(!leaf_integer_tree(c,n->children[i],depth+1))return 0;
    return 1;
}
static int compile_function(NativeCompiler* c,NativeFunction* fn)
{
    ASTNode* body=NULL;
    for(uint32_t i=0;i<fn->node->child_count;i++)if(fn->node->children[i]->type==AST_BLOCK)body=fn->node->children[i];
    if(!body)return fail(c,fn->node,"native function body missing");
    if(fn->result!=N_VOID&&!definitely_returns(body))return fail_code(c,fn->node,"K1047","native function may fall through without ret","Return a value on every reachable path.");
    c->leaf_registers=0;
    c->local_count=c->slots=c->max_slots=c->depth=c->temp_depth=c->loop_depth=c->break_count=c->continue_count=0;c->return_kind=fn->result;c->keep_expression_value=0;c->secure_function=fn->node->is_secure||secure_tree(c,fn->node,0);c->wipe_count=0;
    kru_move_begin(&c->move_flow);
    if(kru_move_event(&c->move_flow,(KruMoveEvent){.position=c->size,.action=KRU_MOVE_ENTRY}))return fail(c,fn->node,"ownership control-flow limit");
    fn->offset=c->size;
    if(BYTES(c,0x55,0x48,0x89,0xE5,0x48,0x81,0xEC))return -1;
    uint32_t frame=c->size;if(imm32(c,0))return -1;
    int sret=is_aggregate(c,fn->result)&&!wide_integer(fn->result);
    if(sret) {
        c->return_slot=c->slots++;
        if(BYTES(c,0x48,0x89,0xF8)||slot_access(c,c->return_slot,1))return -1;
    }
    KruTypeId abi_types[PARAM_LIMIT+1];KruAbiLocation locations[PARAM_LIMIT+1];
    if(sret)abi_types[0]=N_POINTER;
    for(uint32_t i=0;i<fn->argc;i++)abi_types[i+(uint32_t)sret]=fn->params[i];
    kru_x64_scalar_arguments(abi_types,fn->argc+(uint32_t)sret,locations);
    uint32_t param=0;
    for(uint32_t i=0;i<fn->node->child_count;i++) {
        ASTNode* p=fn->node->children[i];if(p->type!=AST_PARAM)continue;
        uint32_t physical=param+(uint32_t)sret;
        static const unsigned char modrm[]={0xF8,0xF0,0xD0,0xC8,0xC0,0xC8};
        KruAbiLocation location=locations[physical];
        if(wide_integer(fn->params[param])) {
            uint32_t slot;if(reserve_storage(c,16,&slot))return -1;
            for(uint32_t word=0;word<2;word++) {
                if(location.kind==KRU_ABI_GP) {
                    unsigned index=location.index+word;
                    if(BYTES(c,index>=4?0x4C:0x48,0x89,modrm[index]))return -1;
                } else if(BYTES(c,0x48,0x8B,0x85)||imm32(c,16+(location.index+word)*8))return -1;
                if(slot_access(c,slot-word,1))return -1;
            }
            c->locals[c->local_count++]=(NativeLocal){p->name,p->name_len,slot,fn->params[param],0,N_I32,p,UINT32_MAX};param++;continue;
        }
        /* Typed local loads perform the required extension. Store integer
           argument bits directly, without routing them through RAX. */
        if(location.kind==KRU_ABI_GP&&native_integer(fn->params[param])) {
            static const unsigned char stores[]={0xBD,0xB5,0x95,0x8D,0x85,0x8D};
            uint32_t slot=c->slots++;
            if(slot+1>c->max_slots)c->max_slots=slot+1;
            if(BYTES(c,location.index>=4?0x4C:0x48,0x89,stores[location.index])||imm32(c,(uint32_t)(-(int32_t)((slot+1)*8))))return -1;
            c->locals[c->local_count++]=(NativeLocal){p->name,p->name_len,slot,fn->params[param],0,N_I32,p,UINT32_MAX};param++;continue;
        }
        if(location.kind==KRU_ABI_GP) {
            if(BYTES(c,location.index>=4?0x4C:0x48,0x89,modrm[location.index]))return -1;
        } else if(location.kind==KRU_ABI_SSE) {
            if(BYTES(c,0x66,0x48,0x0F,0x7E,(unsigned char)(0xC0+(location.index<<3))))return -1;
            if(fn->params[param]==KRU_F32&&BYTES(c,0x89,0xC0))return -1;
        } else if(BYTES(c,0x48,0x8B,0x85)||imm32(c,16+location.index*8))return -1;
        if(!is_aggregate(c,fn->params[param])&&normalize_integer(c,fn->params[param]))return -1;
        if(!is_aggregate(c,fn->params[param])&&is_enum(fn->params[param])&&BYTES(c,0x48,0x63,0xC0))return -1;
        uint32_t slot=c->slots++;if(slot_access(c,slot,1))return -1;
        c->locals[c->local_count++]=(NativeLocal){p->name,p->name_len,slot,fn->params[param],0,N_I32,p,UINT32_MAX};param++;
    }
    /* Spill every incoming argument before aggregate copies clobber temporaries. */
    for(uint32_t i=0;i<fn->argc;i++)if(is_aggregate(c,fn->params[i])&&!wide_integer(fn->params[i])) {
        NativeLocal* local=&c->locals[i];uint32_t slot,bytes=kind_size(c,local->kind,0);
        if(reserve_storage(c,bytes,&slot)||slot_address(c,slot)||push_value(c)||slot_access(c,local->slot,0)||pop_value(c)||copy_bytes(c,bytes))return -1;
        local->slot=slot;
    }
    for(uint32_t i=0;i<fn->argc;i++)if(initialize_ownership(c,&c->locals[i]))return -1;
    int leaf_eligible=!c->secure_function&&native_integer(fn->result)&&leaf_integer_tree(c,body,0);
    for(uint32_t i=0;i<fn->argc;i++)if(!native_integer(fn->params[i]))leaf_eligible=0;
    if(leaf_eligible) {
        /* Spill all ABI arguments before assigning registers, since R8/R9
           may themselves contain the fifth and sixth incoming arguments. */
        for(uint32_t i=0;i<fn->argc&&i<4;i++) {
            if(load_local(c,c->locals[i].slot,fn->params[i])||
               BYTES(c,0x49,0x89,(unsigned char)(0xC0+i)))return -1;
        }
        c->leaf_registers=1;
    }
    c->tail_function=NULL;c->tail_head=c->size;
    int tail_eligible=!c->secure_function&&native_integer(fn->result)&&scalar_tail_tree(body,0);
    for(uint32_t i=0;i<fn->argc;i++)if(!native_integer(fn->params[i]))tail_eligible=0;
    if(tail_eligible)c->tail_function=fn;
    if(block(c,body))return -1;
    if(c->temp_depth)return fail(c,fn->node,"native temporary stack not balanced");
    if(fn->result==N_VOID) {if(cleanup(c,0,NULL)||secure_return(c)||ownership_exit(c)||BYTES(c,0x31,0xC0,0xC9,0xC3))return -1;}
    else if(BYTES(c,0x0F,0x0B))return -1;
    uint32_t bytes=(c->max_slots*8+15u)&~15u;
    for(int i=0;i<4;i++)c->code[frame+i]=(unsigned char)(bytes>>(8*i));
    for(uint32_t j=0;j<c->wipe_count;j++)for(int i=0;i<4;i++) {
        uint32_t count=bytes+c->wipes[j].extra;
        c->code[c->wipes[j].address+i]=(unsigned char)((0u-count)>>(8*i));
        c->code[c->wipes[j].count+i]=(unsigned char)(count>>(8*i));
    }
    ASTNode* invalid=NULL;int flow_status=kru_move_validate(&c->move_flow,&invalid);
    if(flow_status)return fail_code(c,invalid?invalid:fn->node,flow_status>0?"K3011":"K5001",flow_status>0?"use of moved owning value on a reachable control-flow path":"ownership control-flow analysis failed",flow_status>0?"Reinitialize the owner before reuse, or borrow it instead of moving it.":"Report this compiler analysis failure with the source program.");
    flow_status=kru_reference_validate(&c->move_flow,&invalid);
    if(flow_status)return fail_code(c,invalid?invalid:fn->node,flow_status==1?"K3002":flow_status==2?"K3001":"K5001",flow_status==1?"reference alias conflict on a reachable control-flow path":flow_status==2?"reference outlives its storage region":"reference control-flow analysis failed",flow_status>0?"End the conflicting borrow before mutation, and keep references within their source storage lifetime.":"Report this compiler analysis failure with the source program.");
    return 0;
}

static int emit_string(NativeCompiler* c,ASTNode* n)
{
    unsigned char* bytes=malloc(n->string_len?n->string_len:1);size_t length=0;
    if(!bytes)return fail(c,n,"string allocation failed");
    if(kru_decode_string(n->string_val,n->string_len,bytes,&length)) {
        free(bytes);return fail(c,n,"invalid string escape or Unicode scalar");
    }
    int result=emit(c,bytes,(uint32_t)length);free(bytes);
    return result?result:BYTES(c,0);
}
static int compile_main(NativeCompiler* c,ASTNode* root)
{
    if(!root||root->type!=AST_PROGRAM||root->child_count>=ENUM_KIND_BASE)return fail(c,root,"invalid native program or declaration limit");
    c->root=root;if(collect_functions(c))return -1;
    ASTNode main_name={.name="main",.name_len=4};int main=find_function(c,&main_name);
    if(main<0||c->functions[main].argc||c->functions[main].result!=N_I32)return fail(c,root,"native main requires zero parameters and i32/int return");
    /* R12 preserves argc/argv. Reserve a private entry-stack word for the
       runtime's live mapping count; it is outside every function frame. */
    if(BYTES(c,0x49,0x89,0xE4,0x48,0x83,0xE4,0xF0,0x48,0x83,0xEC,0x10,
               0x49,0xC7,0x44,0x24,0xF8,0,0,0,0)||emit_call(c,(uint32_t)main)||
       BYTES(c,0x89,0xC7))return -1;
    uint32_t message_fixup=0,clean=0;
    static const char heap_message[]="kru: heap allocations remain live at exit\n";
    if(c->check_heap) {
        if(BYTES(c,0x49,0x83,0x7C,0x24,0xF8,0)||jump(c,1,&clean)||
           BYTES(c,0xB8,1,0,0,0,0xBF,2,0,0,0,0x48,0x8D,0x35))return -1;
        message_fixup=c->size;
        if(imm32(c,0)||BYTES(c,0xBA)||imm32(c,sizeof(heap_message)-1)||
           BYTES(c,0x0F,0x05,0xBF,99,0,0,0)||patch32(c,clean,c->size))return -1;
    }
    if(BYTES(c,0xB8,0x3C,0,0,0,0x0F,0x05))return -1;
    if(c->check_heap&&(patch32(c,message_fixup,c->size)||emit(c,(const unsigned char*)heap_message,sizeof(heap_message)-1)))return -1;
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

int native_backend_emit_x86_64_linux(ASTNode* root, const char* output_path, int check_heap)
{
#if !defined(__linux__) || !defined(__x86_64__)
    (void)root;
    (void)output_path;
    (void)check_heap;
    return native_error("direct backend currently targets x86-64 Linux only");
#else
    if(!output_path)
        return native_error("missing native output path");

    NativeCompiler* compiler = calloc(1, sizeof(*compiler));
    if(!compiler) return native_error("out of memory");
    compiler->check_heap=check_heap;
    if(compile_main(compiler, root) != 0) {
        if(compiler->error_code) {
            ASTNode* n=compiler->error_node;
            while(n&&!n->line&&n->child_count)n=n->children[0];
            Diagnostic diagnostic={.severity="error",.code=compiler->error_code,.message=compiler->error,
                .source_id=n?n->source_id:0,.line=n?n->line:0,.column=n?n->column:0,.underline_length=n?(n->source_length?n->source_length:n->name_len):0,.suggested_fix=compiler->error_help};
            diagnostic_print(&diagnostic);free(compiler);return -1;
        }
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
