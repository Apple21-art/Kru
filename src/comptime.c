#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/comptime.h"
#include "../include/types.h"
#include "../include/literals.h"
#define BINDINGS 2048
#define CONSTANTS 1024
#define FUEL 1000000
/* This evaluator is a compiler pass; emitted programs contain no evaluator. */
typedef unsigned __int128 U128;
typedef __int128 I128;
typedef struct {KruTypeId type;U128 bits;double floating;ASTNode* string;} Value;
typedef struct {const char* name;Value value;int mutable;} Binding;
typedef struct {ASTNode* node;Value value;int state;} Constant;
typedef struct {ASTNode* root;Binding bindings[BINDINGS];unsigned count,binding_start;KruTypeId return_type;Constant constants[CONSTANTS];unsigned constant_count,fuel,call_depth;int failed;Value returned;} Context;
static int failure(Context* c,ASTNode* n,const char* reason){if(!c->failed)fprintf(stderr,"[kru] comptime: line %u: %s\n",n?n->line:0,reason);c->failed=1;return -1;}
static int tick(Context* c,ASTNode* n){return c->fuel--?0:failure(c,n,"evaluation fuel exhausted");}
static ASTNode* find(Context* c,const char* name){if(!name)return NULL;for(unsigned i=0;i<c->root->child_count;i++)if(c->root->children[i]->name&&!strcmp(c->root->children[i]->name,name))return c->root->children[i];return NULL;}
static KruTypeId type(Context* c,ASTNode* n,unsigned depth)
{
    if(!n)return KRU_VOID;
    if(depth>64||n->op!=TOKEN_EOF)return KRU_BAD;
    KruTypeId id=kru_type_named(n->name,n->name_len);if(id!=KRU_BAD)return id;
    ASTNode* d=find(c,n->name);return d&&d->type==AST_TYPE_ALIAS?type(c,d->type_node,depth+1):KRU_BAD;
}
static Value normalized(Value value)
{
    const KruPrimitive* t=kru_primitive(value.type);
    if(t&&t->category==KRU_INTEGER&&t->size<16)value.bits&=((U128)1<<(t->size*8))-1;
    if(value.type==KRU_F32)value.floating=(float)value.floating;
    return value;
}
static I128 signed_value(Value value)
{
    const KruPrimitive* t=kru_primitive(value.type);unsigned bits=t?t->size*8:128;
    if(bits<128&&(value.bits&((U128)1<<(bits-1))))return (I128)(value.bits|(~(U128)0<<bits));
    return (I128)value.bits;
}
static int evaluate(Context*,ASTNode*,KruTypeId,Value*);
static int statement(Context*,ASTNode*);
static int constant(Context* c,Constant* entry,Value* out)
{
    if(entry->state==1)return failure(c,entry->node,"cyclic constant definition");
    if(entry->state==2){*out=entry->value;return 0;}
    entry->state=1;
    if(entry->node->child_count!=1||evaluate(c,entry->node->children[0],entry->node->type_node?type(c,entry->node->type_node,0):KRU_BAD,&entry->value))return -1;
    entry->state=2;*out=entry->value;return 0;
}
static int evaluate(Context* c,ASTNode* n,KruTypeId expected,Value* out)
{
    if(!n||tick(c,n))return -1;
    Value a={0},b={0};
    switch(n->type) {
    case AST_INT_LIT: {
        KruTypeId id=n->has_explicit_suffix?type(c,n->type_node,0):expected;
        if(!kru_type_integer(id))id=n->int_high?KRU_I128:n->int_val<=INT32_MAX?KRU_I32:n->int_val<=INT64_MAX?KRU_I64:KRU_I128;
        if(!kru_integer_fits128(id,n->int_val,n->int_high,0))return failure(c,n,"integer literal range overflow");
        *out=(Value){.type=id,.bits=((U128)n->int_high<<64)|n->int_val};return 0;
    }
    case AST_FLOAT_LIT: {
        KruTypeId id=n->has_explicit_suffix?type(c,n->type_node,0):(expected==KRU_F32||expected==KRU_F64)?expected:KRU_F64;
        *out=normalized((Value){.type=id,.floating=n->float_val});return 0;
    }
    case AST_BOOL_LIT:*out=(Value){.type=KRU_BOOL,.bits=n->bool_val};return 0;
    case AST_CHAR_LIT:*out=(Value){.type=KRU_CHAR,.bits=n->int_val};return 0;
    case AST_STRING_LIT:*out=(Value){.type=KRU_STR,.string=n};return 0;
    case AST_IDENT:
        for(unsigned i=c->count;i>c->binding_start;i--)if(!strcmp(c->bindings[i-1].name,n->name)){*out=c->bindings[i-1].value;return 0;}
        for(unsigned i=0;i<c->constant_count;i++)if(!strcmp(c->constants[i].node->name,n->name))return constant(c,&c->constants[i],out);
        return failure(c,n,"value is not available at compile time");
    case AST_UNARY_EXPR:
        if(n->op==TOKEN_MINUS&&n->child_count==1&&n->children[0]->type==AST_INT_LIT) {
            ASTNode* literal=n->children[0];KruTypeId id=literal->has_explicit_suffix?type(c,literal->type_node,0):kru_type_integer(expected)?expected:literal->int_high?KRU_I128:literal->int_val<=UINT64_C(2147483648)?KRU_I32:literal->int_val<=UINT64_C(9223372036854775808)?KRU_I64:KRU_I128;
            if(!kru_integer_fits128(id,literal->int_val,literal->int_high,1))return failure(c,n,"negative integer literal range overflow");
            *out=normalized((Value){.type=id,.bits=0-(((U128)literal->int_high<<64)|literal->int_val)});return 0;
        }
        if(n->child_count!=1||evaluate(c,n->children[0],expected,&a))return -1;
        if(n->op==TOKEN_MINUS) {if(a.type==KRU_F32||a.type==KRU_F64)a.floating=-a.floating;else if(kru_type_integer(a.type))a.bits=0-a.bits;else return failure(c,n,"numeric negation requires numeric operand");}
        else if(n->op==TOKEN_BANG){if(a.type!=KRU_BOOL)return failure(c,n,"boolean negation requires bool");a.bits=!a.bits;}
        else if(n->op==TOKEN_TILDE){if(!kru_type_integer(a.type))return failure(c,n,"bitwise negation requires integer");a.bits=~a.bits;}
        else return failure(c,n,"unsupported compile-time unary operator");
        *out=normalized(a);return 0;
    case AST_BINARY_EXPR: {
        if(n->child_count!=2||evaluate(c,n->children[0],KRU_BAD,&a))return -1;
        if(n->op==TOKEN_AND_AND||n->op==TOKEN_OR_OR) {
            if(a.type!=KRU_BOOL)return failure(c,n,"logical operands require bool");
            if((n->op==TOKEN_AND_AND&&!a.bits)||(n->op==TOKEN_OR_OR&&a.bits)){*out=a;return 0;}
        }
        if(evaluate(c,n->children[1],a.type,&b))return -1;
        if(a.type!=b.type)return failure(c,n,"compile-time operand type mismatch");
        if(a.type==KRU_STR) {
            if(n->op!=TOKEN_EQ_EQ&&n->op!=TOKEN_BANG_EQUAL)return failure(c,n,"unsupported compile-time string operator");
            unsigned char* left=malloc(a.string->string_len?a.string->string_len:1);
            unsigned char* right=malloc(b.string->string_len?b.string->string_len:1);
            size_t left_size=0,right_size=0;
            if(!left||!right||kru_decode_string(a.string->string_val,a.string->string_len,left,&left_size)||
               kru_decode_string(b.string->string_val,b.string->string_len,right,&right_size)) {
                free(left);free(right);return failure(c,n,"invalid compile-time string literal");
            }
            int same=left_size==right_size&&!memcmp(left,right,left_size);free(left);free(right);
            *out=(Value){.type=KRU_BOOL,.bits=n->op==TOKEN_EQ_EQ?same:!same};return 0;
        }
        int floating=a.type==KRU_F32||a.type==KRU_F64;
        int comparison=n->op==TOKEN_EQ_EQ||n->op==TOKEN_BANG_EQUAL||n->op==TOKEN_LT||n->op==TOKEN_GT||n->op==TOKEN_LT_EQUAL||n->op==TOKEN_GT_EQUAL;
        int logical=n->op==TOKEN_AND_AND||n->op==TOKEN_OR_OR;
        if((logical&&a.type!=KRU_BOOL)||
           (a.type==KRU_BOOL&&!logical&&n->op!=TOKEN_EQ_EQ&&n->op!=TOKEN_BANG_EQUAL)||
           (a.type==KRU_CHAR&&!comparison)||
           (floating&&(n->op==TOKEN_AMP||n->op==TOKEN_PIPE||n->op==TOKEN_CARET||n->op==TOKEN_SHL||n->op==TOKEN_SHR)))
            return failure(c,n,"compile-time operator is invalid for operand type");
        int sign=kru_type_integer(a.type)&&kru_primitive(a.type)->is_signed;
        I128 sa=signed_value(a),sb=signed_value(b);
        switch(n->op) {
        case TOKEN_PLUS:if(floating)a.floating+=b.floating;else a.bits+=b.bits;break;
        case TOKEN_MINUS:if(floating)a.floating-=b.floating;else a.bits-=b.bits;break;
        case TOKEN_STAR:if(floating)a.floating*=b.floating;else a.bits*=b.bits;break;
        case TOKEN_SLASH:case TOKEN_PERCENT:
            if(floating){if(n->op==TOKEN_PERCENT)return failure(c,n,"float remainder is undefined");a.floating/=b.floating;break;}
            if(!b.bits)return failure(c,n,"compile-time division by zero");
            if(sign&&sa==(I128)((U128)1<<127)&&sb==-1)return failure(c,n,"compile-time signed division overflow");
            a.bits=sign?(U128)(n->op==TOKEN_SLASH?sa/sb:sa%sb):(n->op==TOKEN_SLASH?a.bits/b.bits:a.bits%b.bits);break;
        case TOKEN_AMP:a.bits&=b.bits;break;case TOKEN_PIPE:a.bits|=b.bits;break;case TOKEN_CARET:a.bits^=b.bits;break;
        case TOKEN_SHL:case TOKEN_SHR:
            if(!kru_type_integer(a.type)||b.bits>=kru_primitive(a.type)->size*8)return failure(c,n,"compile-time shift count out of range");
            a.bits=n->op==TOKEN_SHL?a.bits<<(unsigned)b.bits:sign?(U128)(sa>>(unsigned)b.bits):a.bits>>(unsigned)b.bits;break;
        case TOKEN_EQ_EQ:a.bits=floating?a.floating==b.floating:a.bits==b.bits;a.type=KRU_BOOL;break;
        case TOKEN_BANG_EQUAL:a.bits=floating?a.floating!=b.floating:a.bits!=b.bits;a.type=KRU_BOOL;break;
        case TOKEN_LT:a.bits=floating?a.floating<b.floating:sign?sa<sb:a.bits<b.bits;a.type=KRU_BOOL;break;
        case TOKEN_GT:a.bits=floating?a.floating>b.floating:sign?sa>sb:a.bits>b.bits;a.type=KRU_BOOL;break;
        case TOKEN_LT_EQUAL:a.bits=floating?a.floating<=b.floating:sign?sa<=sb:a.bits<=b.bits;a.type=KRU_BOOL;break;
        case TOKEN_GT_EQUAL:a.bits=floating?a.floating>=b.floating:sign?sa>=sb:a.bits>=b.bits;a.type=KRU_BOOL;break;
        case TOKEN_AND_AND:case TOKEN_OR_OR:a.bits=b.bits;break;
        default:return failure(c,n,"unsupported compile-time binary operator");
        }
        *out=normalized(a);return 0;
    }
    case AST_BLOCK_EXPR: {
        if(n->child_count!=1||n->children[0]->type!=AST_BLOCK)return failure(c,n,"invalid compile-time block");
        ASTNode* body=n->children[0];unsigned saved=c->count;int result=0;
        if(!body->child_count)return failure(c,n,"compile-time value block requires a tail expression");
        for(unsigned i=0;i+1<body->child_count;i++)if((result=statement(c,body->children[i])))break;
        if(!result) {
            ASTNode* tail=body->children[body->child_count-1];
            result=tail->type==AST_EXPR_STMT&&tail->child_count==1?
                evaluate(c,tail->children[0],expected,out):failure(c,tail,"compile-time value block requires a tail expression");
        }
        c->count=saved;return result?failure(c,n,"invalid control transfer in compile-time value block"):0;
    }
    case AST_CALL_EXPR: {
        if(!n->child_count||n->children[0]->type!=AST_IDENT)return failure(c,n,"compile-time call requires resolved function");
        ASTNode* fn=find(c,n->children[0]->name);
        if(!fn||fn->type!=AST_FUNCTION||!fn->is_comptime||fn->generic_parameters)return failure(c,n,"compile-time sandbox forbids non-comptime calls");
        if(c->call_depth==64)return failure(c,n,"compile-time call depth exhausted");
        Value arguments[32];unsigned params=0;
        ASTNode* body=NULL;
        for(unsigned i=0;i<fn->child_count;i++) {
            ASTNode* p=fn->children[i];
            if(p->type==AST_PARAM) {
                if(params==32||params+1>=n->child_count||evaluate(c,n->children[params+1],type(c,p->type_node,0),&arguments[params]))return failure(c,n,"compile-time call arguments invalid");
                if(arguments[params].type!=type(c,p->type_node,0))return failure(c,n,"compile-time argument type mismatch");
                params++;
            } else if(p->type==AST_BLOCK)body=p;
        }
        if(params+1!=n->child_count||!body)return failure(c,n,"compile-time call signature mismatch");
        unsigned saved=c->count,previous_start=c->binding_start;KruTypeId previous_type=c->return_type;Value previous=c->returned;
        c->binding_start=saved;c->return_type=type(c,fn->type_node,0);c->call_depth++;unsigned arg=0;
        for(unsigned i=0;i<fn->child_count;i++)if(fn->children[i]->type==AST_PARAM) {
            if(c->count==BINDINGS)return failure(c,n,"compile-time binding limit");
            c->bindings[c->count++]=(Binding){fn->children[i]->name,arguments[arg++],0};
        }
        int status=statement(c,body);*out=c->returned;c->returned=previous;c->count=saved;c->binding_start=previous_start;c->return_type=previous_type;c->call_depth--;
        if(status!=1)return failure(c,n,"compile-time function must return a value");
        if(out->type!=type(c,fn->type_node,0))return failure(c,n,"compile-time return type mismatch");
        return 0;
    }
    default:return failure(c,n,"expression outside compile-time evaluator subset");
    }
}
static int statement(Context* c,ASTNode* n)
{
    if(!n||tick(c,n))return -1;
    switch(n->type) {
    case AST_BLOCK: {
        unsigned saved=c->count;int result=0;
        for(unsigned i=0;i<n->child_count;i++)if((result=statement(c,n->children[i])))break;
        c->count=saved;return result;
    }
    case AST_LET_STMT:case AST_VAR_STMT: {
        if(c->count==BINDINGS||n->child_count!=1)return failure(c,n,"compile-time binding requires initializer");
        Value v;if(evaluate(c,n->children[0],n->type_node?type(c,n->type_node,0):KRU_BAD,&v))return -1;
        if(n->type_node&&v.type!=type(c,n->type_node,0))return failure(c,n,"compile-time binding type mismatch");
        c->bindings[c->count++]=(Binding){n->name,v,n->type==AST_VAR_STMT};return 0;
    }
    case AST_ASSIGN_STMT: {
        if(n->child_count!=2||n->children[0]->type!=AST_IDENT)return failure(c,n,"compile-time assignment requires local scalar");
        for(unsigned i=c->count;i>c->binding_start;i--)if(!strcmp(c->bindings[i-1].name,n->children[0]->name)) {
            Binding* binding=&c->bindings[i-1];if(!binding->mutable)return failure(c,n,"compile-time immutable assignment");
            Value v;if(evaluate(c,n->children[1],binding->value.type,&v)||v.type!=binding->value.type)return failure(c,n,"compile-time assignment type mismatch");
            if(n->op==TOKEN_PLUS_EQUALS) {if(v.type==KRU_F32||v.type==KRU_F64)v.floating+=binding->value.floating;else v.bits+=binding->value.bits;}
            else if(n->op==TOKEN_MINUS_EQUALS) {if(v.type==KRU_F32||v.type==KRU_F64)v.floating=binding->value.floating-v.floating;else v.bits=binding->value.bits-v.bits;}
            else if(n->op!=TOKEN_EQUALS&&n->op!=TOKEN_EOF)return failure(c,n,"compile-time compound operator unsupported");
            binding->value=normalized(v);return 0;
        }
        return failure(c,n,"compile-time assignment name unresolved");
    }
    case AST_RET_STMT:if(n->child_count!=1||evaluate(c,n->children[0],c->return_type,&c->returned))return -1;return 1;
    case AST_IF_STMT: {
        Value condition;if(n->child_count<2||evaluate(c,n->children[0],KRU_BOOL,&condition)||condition.type!=KRU_BOOL)return failure(c,n,"compile-time condition requires bool");
        return condition.bits?statement(c,n->children[1]):n->child_count==3?statement(c,n->children[2]):0;
    }
    case AST_WHILE_STMT:case AST_LOOP_STMT:
        for(;;) {
            if(n->type==AST_WHILE_STMT) {Value condition;if(evaluate(c,n->children[0],KRU_BOOL,&condition))return -1;if(condition.type!=KRU_BOOL)return failure(c,n,"compile-time condition requires bool");if(!condition.bits)return 0;}
            int result=statement(c,n->children[n->type==AST_WHILE_STMT?1:0]);if(result==-2)return 0;if(result&&result!=-3)return result;
        }
    case AST_FOR_STMT: {
        if(n->op!=TOKEN_DOT_DOT||n->child_count!=3||c->count==BINDINGS)return failure(c,n,"compile-time for requires integer range");
        Value first,last;
        if(evaluate(c,n->children[0],KRU_BAD,&first)||evaluate(c,n->children[1],first.type,&last)||
           !kru_type_integer(first.type)||first.type!=last.type)return failure(c,n,"compile-time range bounds mismatch");
        unsigned saved=c->count;c->bindings[c->count++]=(Binding){n->name,first,0};
        int result=0,sign=kru_primitive(first.type)->is_signed;
        while(sign?signed_value(c->bindings[saved].value)<signed_value(last):c->bindings[saved].value.bits<last.bits) {
            result=statement(c,n->children[2]);
            if(result==-2){result=0;break;}
            if(result&&result!=-3)break;
            result=0;c->bindings[saved].value.bits++;
            c->bindings[saved].value=normalized(c->bindings[saved].value);
        }
        c->count=saved;return result;
    }
    case AST_BREAK_STMT:return -2;case AST_CONTINUE_STMT:return -3;
    case AST_EXPR_STMT: {Value v;return n->child_count==1?evaluate(c,n->children[0],KRU_BAD,&v):failure(c,n,"invalid compile-time expression statement");}
    default:return failure(c,n,"statement outside compile-time evaluator subset");
    }
}
static ASTNode* literal(Value v)
{
    if(v.string)return ast_clone(v.string);
    ASTNode* node=ast_create(v.type==KRU_BOOL?AST_BOOL_LIT:v.type==KRU_F32||v.type==KRU_F64?AST_FLOAT_LIT:v.type==KRU_CHAR?AST_CHAR_LIT:AST_INT_LIT);
    if(v.type==KRU_BOOL){node->bool_val=!!v.bits;return node;}
    if(v.type==KRU_CHAR){node->int_val=(uint64_t)v.bits;return node;}
    node->type_node=ast_create(AST_TYPE);const KruPrimitive* t=kru_primitive(v.type);ast_set_name(node->type_node,t->name,(unsigned)strlen(t->name));node->has_explicit_suffix=true;
    if(v.type==KRU_F32||v.type==KRU_F64){node->float_val=v.floating;return node;}
    int negative=t->is_signed&&signed_value(v)<0;U128 magnitude=negative?0-(U128)signed_value(v):v.bits;
    node->int_val=(uint64_t)magnitude;node->int_high=(uint64_t)(magnitude>>64);
    if(negative){ASTNode* neg=ast_create(AST_UNARY_EXPR);neg->op=TOKEN_MINUS;ast_add_child(neg,node);return neg;}return node;
}
static int fold_blocks(Context* c,ASTNode** node,KruTypeId expected,unsigned depth)
{
    ASTNode* n=*node;
    while(n&&n->type==AST_BINARY_EXPR&&n->child_count==2) {
        if(fold_blocks(c,&n->children[1],KRU_BAD,depth+1))return -1;
        node=&n->children[0];n=*node;
    }
    if(!n)return 0;
    if(depth>512)return failure(c,n,"compile-time syntax depth limit");
    if(n->type==AST_FUNCTION&&n->is_comptime)return 0;
    if(n->type==AST_BLOCK_EXPR&&n->is_comptime) {
        Value v;if(evaluate(c,n,expected,&v))return -1;
        ASTNode* folded=literal(v);folded->source_id=n->source_id;folded->source_length=n->source_length;folded->line=n->line;folded->column=n->column;
        *node=folded;ast_free(n);return 0;
    }
    for(unsigned i=0;i<n->child_count;i++) {
        KruTypeId child_expected=(n->type==AST_LET_STMT||n->type==AST_VAR_STMT||n->type==AST_CONST_DECL)&&n->type_node?type(c,n->type_node,0):KRU_BAD;
        if(fold_blocks(c,&n->children[i],child_expected,depth+1))return -1;
    }
    return 0;
}
int kru_evaluate_comptime(ASTNode* root)
{
    Context* c=calloc(1,sizeof(*c));if(!c)return -1;c->root=root;c->fuel=FUEL;
    for(unsigned i=0;i<root->child_count;i++)if(root->children[i]->type==AST_CONST_DECL) {
        if(c->constant_count==CONSTANTS){free(c);return -1;}
        c->constants[c->constant_count++]=(Constant){.node=root->children[i]};
    }
    ASTNode* originals[CONSTANTS]={0};int status=0;
    for(unsigned i=0;i<c->constant_count;i++) {
        Value v;if(constant(c,&c->constants[i],&v)){status=-1;break;}
        ASTNode* decl=c->constants[i].node;ASTNode* folded=literal(v);folded->source_id=decl->source_id;folded->source_length=decl->source_length;folded->line=decl->line;folded->column=decl->column;
        originals[i]=decl->children[0];decl->children[0]=folded;
        if(v.string)c->constants[i].value.string=folded;
    }
    if(!status)for(unsigned i=0;i<root->child_count;i++)if(root->children[i]->type==AST_COMPTIME_BLOCK) {
        if(root->children[i]->child_count!=1||statement(c,root->children[i]->children[0])){status=failure(c,root->children[i],"compile-time block failed");break;}
    }
    if(!status)status=fold_blocks(c,&root,KRU_BAD,0);
    for(unsigned i=0;i<c->constant_count;i++)ast_free(originals[i]);
    free(c);return status;
}
