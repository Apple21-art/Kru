#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../include/generics.h"
#include "../include/parser.h"
#define INSTANCE_LIMIT 1024
#define KEY_LIMIT 4096
#define BINDING_LIMIT 2048
typedef struct {ASTNode* declaration;char* key;ASTNode* instance;} Instance;
typedef struct {const char* name;ASTNode* type;} Binding;
typedef struct {ASTNode* root;Instance instances[INSTANCE_LIMIT];unsigned count;Binding bindings[BINDING_LIMIT];unsigned binding_count;int failed;} Context;
static int same(ASTNode* n,const char* name){return n&&n->name&&!strcmp(n->name,name);}
static int error(Context* c,ASTNode* n,const char* reason){fprintf(stderr,"[kru] generics: line %u: %s\n",n?n->line:0,reason);c->failed=1;return -1;}
static ASTNode* named(const char* name){ASTNode* t=ast_create(AST_TYPE);ast_set_name(t,name,(unsigned)strlen(name));return t;}
static ASTNode* declaration(Context* c,const char* name)
{for(unsigned i=0;i<c->root->child_count;i++)if(same(c->root->children[i],name))return c->root->children[i];return NULL;}
static int key_type(ASTNode* t,char* key,size_t* size,unsigned depth)
{
    if(!t||depth>64)return -1;
    char part[80];int n=snprintf(part,sizeof(part),"(%d,%u,%llu,%u:",(int)t->op,t->is_mut,(unsigned long long)t->int_val,t->name_len);
    size_t length=strlen(t->name?t->name:"");
    if(n<0||*size+(size_t)n+length+2>=KEY_LIMIT)return -1;
    memcpy(key+*size,part,(size_t)n);*size+=(size_t)n;memcpy(key+*size,t->name?t->name:"",length);*size+=length;
    if(t->type_node&&key_type(t->type_node,key,size,depth+1))return -1;
    for(unsigned i=0;i<t->child_count;i++)if(key_type(t->children[i],key,size,depth+1))return -1;
    key[(*size)++]=')';key[*size]=0;return 0;
}
static void substitute(ASTNode** place,ASTNode* parameters,ASTNode** arguments,unsigned count,unsigned depth)
{
    ASTNode* n=*place;if(!n||depth>256)return;
    if(n->type==AST_TYPE&&n->op==TOKEN_EOF)for(unsigned i=0;i<count;i++)if(same(n,parameters->children[i]->name)) {
        ASTNode* replacement=ast_clone(arguments[i]);ast_free(n);*place=replacement;return;
    }
    substitute(&n->type_node,parameters,arguments,count,depth+1);
    for(unsigned i=0;i<n->child_count;i++)substitute(&n->children[i],parameters,arguments,count,depth+1);
}
static void self_type(ASTNode** place,ASTNode* target,unsigned depth)
{
    ASTNode* n=*place;if(!n||depth>256)return;
    if(n->type==AST_TYPE&&same(n,"Self")){ASTNode* clone=ast_clone(target);ast_free(n);*place=clone;return;}
    if(n->type==AST_IDENT&&same(n,"Self")&&target->name)ast_set_name(n,target->name,target->name_len);
    self_type(&n->type_node,target,depth+1);
    for(unsigned i=0;i<n->child_count;i++)self_type(&n->children[i],target,depth+1);
}
static void self_fields(ASTNode* n,unsigned depth)
{
    if(!n||depth>256)return;
    if(n->type==AST_FIELD_EXPR&&n->child_count==1&&n->children[0]->type==AST_IDENT&&same(n->children[0],"self")) {
        ASTNode* load=ast_create(AST_DEREF_EXPR);ast_add_child(load,n->children[0]);n->children[0]=load;
    }
    for(unsigned i=0;i<n->child_count;i++)self_fields(n->children[i],depth+1);
}
static int equal_type(ASTNode* a,ASTNode* b)
{
    if(!a||!b)return a==b||(!a&&same(b,"void"))||(!b&&same(a,"void"));
    char left[KEY_LIMIT]={0},right[KEY_LIMIT]={0};size_t ls=0,rs=0;
    return !key_type(a,left,&ls,0)&&!key_type(b,right,&rs,0)&&!strcmp(left,right);
}
static int trait_implementation(Context* c,ASTNode* impl)
{
    if(!impl->name)return 0; /* Inherent implementation. */
    ASTNode* trait=declaration(c,impl->name);
    if(!trait||trait->type!=AST_TRAIT_DECL)return error(c,impl,"implementation names an unknown trait");
    for(unsigned i=0;i<trait->child_count;i++) {
        ASTNode* required=trait->children[i],*provided=NULL;
        for(unsigned j=0;j<impl->child_count;j++)if(same(impl->children[j],required->name))provided=impl->children[j];
        if(!provided)return error(c,impl,"trait implementation is missing a required method");
        ASTNode* resolved=ast_clone(required);self_type(&resolved,impl->type_node,0);
        ASTNode* actual=ast_clone(provided);self_type(&actual,impl->type_node,0);
        int valid=equal_type(resolved->type_node,actual->type_node);unsigned parameter=0;
        for(unsigned j=0;j<resolved->child_count;j++)if(resolved->children[j]->type==AST_PARAM) {
            while(parameter<actual->child_count&&actual->children[parameter]->type!=AST_PARAM)parameter++;
            if(parameter==actual->child_count||!equal_type(resolved->children[j]->type_node,actual->children[parameter]->type_node))valid=0;
            if(parameter<actual->child_count)parameter++;
        }
        for(;parameter<actual->child_count;parameter++)if(actual->children[parameter]->type==AST_PARAM)valid=0;
        ast_free(resolved);ast_free(actual);
        if(!valid)return error(c,provided,"trait method signature does not match declaration");
    }
    return 0;
}
static int satisfies(Context* c,ASTNode* concrete,ASTNode* bound)
{
    if(!bound)return 1;
    ASTNode* trait=declaration(c,bound->name);
    if(!trait||trait->type!=AST_TRAIT_DECL)return 0;
    for(unsigned i=0;i<c->root->child_count;i++) {
        ASTNode* impl=c->root->children[i];
        if(impl->type==AST_IMPL_DECL&&same(impl,bound->name)&&equal_type(impl->type_node,concrete))return 1;
    }
    return 0;
}
static int types(Context* c,ASTNode* n,unsigned depth);
static int lower_methods(Context* c,ASTNode* impl)
{
    if(!impl->type_node||!impl->type_node->name)return error(c,impl,"impl requires named target");
    if(trait_implementation(c,impl))return -1;
    for(unsigned j=0;j<impl->child_count;j++) {
        ASTNode* fn=ast_clone(impl->children[j]);char name[1024];
        if(strlen(impl->type_node->name)+fn->name_len+3>=sizeof(name)){ast_free(fn);return error(c,impl,"method name limit");}
        snprintf(name,sizeof(name),"%s__%s",impl->type_node->name,fn->name);
        if(declaration(c,name)){ast_free(fn);return error(c,impl,"duplicate method");}
        ast_set_name(fn,name,(unsigned)strlen(name));self_type(&fn,impl->type_node,0);
        for(unsigned p=0;p<fn->child_count;p++)if(fn->children[p]->type==AST_PARAM&&same(fn->children[p],"self")&&fn->children[p]->type_node&&fn->children[p]->type_node->op==TOKEN_KW_REF) {
            for(unsigned b=0;b<fn->child_count;b++)if(fn->children[b]->type==AST_BLOCK)self_fields(fn->children[b],0);
        }
        ast_add_child(c->root,fn);
    }
    return 0;
}
static int methods(Context* c)
{
    unsigned initial=c->root->child_count;
    for(unsigned i=0;i<initial;i++) {
        ASTNode* impl=c->root->children[i];if(impl->type!=AST_IMPL_DECL)continue;
        if(impl->generic_parameters)return error(c,impl,"blanket generic impl blocks are deferred by Codex Chapter 36; name a concrete type");
        if(types(c,impl->type_node,0))return -1;
        if(lower_methods(c,impl))return -1;
    }
    return 0;
}
static ASTNode* instantiate(Context* c,ASTNode* original,ASTNode** arguments,unsigned count)
{
    if(!original||!original->generic_parameters||original->generic_parameters->child_count!=count){error(c,original,"generic argument count mismatch");return NULL;}
    for(unsigned i=0;i<count;i++) {
        ASTNode* parameter=original->generic_parameters->children[i];
        if(!satisfies(c,arguments[i],parameter->type_node)){error(c,original,"generic argument does not implement required trait");return NULL;}
        for(unsigned j=0;j<parameter->child_count;j++)if(!satisfies(c,arguments[i],parameter->children[j])){error(c,original,"generic argument does not implement required trait");return NULL;}
    }
    char key[KEY_LIMIT]={0};size_t size=0;
    for(unsigned i=0;i<count;i++)if(key_type(arguments[i],key,&size,0)){error(c,original,"generic type nesting limit");return NULL;}
    for(unsigned i=0;i<c->count;i++)if(c->instances[i].declaration==original&&!strcmp(c->instances[i].key,key))return c->instances[i].instance;
    if(c->count==INSTANCE_LIMIT){error(c,original,"monomorphization limit");return NULL;}
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<size;i++){hash^=(unsigned char)key[i];hash*=UINT64_C(1099511628211);}
    char link[1024];if(strlen(original->name)+24>=sizeof(link)){error(c,original,"generic name too long");return NULL;}
    snprintf(link,sizeof(link),"%s__g%016llx",original->name,(unsigned long long)hash);
    if(declaration(c,link)){error(c,original,"generic linkage collision");return NULL;}
    ASTNode* instance=ast_clone(original);ast_set_name(instance,link,(unsigned)strlen(link));
    ast_free(instance->generic_parameters);instance->generic_parameters=NULL;
    substitute(&instance,original->generic_parameters,arguments,count,0);
    c->instances[c->count++]=(Instance){original,strdup(key),instance};
    ast_add_child(c->root,instance);
    if(types(c,instance,0))return NULL;
    return instance;
}
static int types(Context* c,ASTNode* n,unsigned depth)
{
    while(n&&n->type==AST_BINARY_EXPR&&n->child_count==2) {
        if(types(c,n->children[1],depth+1))return -1;
        n=n->children[0];
    }
    if(!n)return 0;
    if(depth>256)return error(c,n,"type traversal nesting limit");
    if(n->generic_parameters)return 0;
    if(types(c,n->type_node,depth+1))return -1;
    for(unsigned i=0;i<n->child_count;i++)if(types(c,n->children[i],depth+1))return -1;
    if(n->type==AST_TYPE&&n->child_count) {
        ASTNode* template=declaration(c,n->name?n->name:"");
        ASTNode* instance=instantiate(c,template,n->children,n->child_count);if(!instance)return -1;
        ast_set_name(n,instance->name,instance->name_len);
        for(unsigned i=0;i<n->child_count;i++)ast_free(n->children[i]);
        n->child_count=0;
    }
    if(n->type==AST_CALL_EXPR&&n->child_count&&n->children[0]->type==AST_IDENT&&n->children[0]->child_count&&!same(n->children[0],"cast")) {
        ASTNode* callee=n->children[0];ASTNode* instance=instantiate(c,declaration(c,callee->name),callee->children,callee->child_count);if(!instance)return -1;
        ast_set_name(callee,instance->name,instance->name_len);
        for(unsigned i=0;i<callee->child_count;i++)ast_free(callee->children[i]);
        callee->child_count=0;
    }
    return 0;
}
static ASTNode* infer(Context* c,ASTNode* n,unsigned depth)
{
    if(!n||depth>128)return NULL;
    if(n->type==AST_INT_LIT||n->type==AST_FLOAT_LIT)return n->type_node?ast_clone(n->type_node):named(n->type==AST_FLOAT_LIT?"f64":"i32");
    if(n->type==AST_BOOL_LIT)return named("bool");
    if(n->type==AST_STRING_LIT)return named("str");
    if(n->type==AST_CHAR_LIT)return named("char");
    if(n->type==AST_IDENT) {
        for(unsigned i=c->binding_count;i;i--)if(!strcmp(c->bindings[i-1].name,n->name))return ast_clone(c->bindings[i-1].type);
        ASTNode* d=declaration(c,n->name);if(d&&d->type==AST_CONST_DECL)return d->type_node?ast_clone(d->type_node):infer(c,d->children[0],depth+1);
    }
    if(n->type==AST_CALL_EXPR&&n->child_count) {
        ASTNode* d=n->children[0]->name?declaration(c,n->children[0]->name):NULL;
        if(d&&d->type==AST_FUNCTION)return ast_clone(d->type_node);
        if(n->is_struct_lit&&d&&d->type==AST_STRUCT_DECL)return named(d->name);
        if(same(n->children[0],"cast")&&n->children[0]->child_count==1)return ast_clone(n->children[0]->children[0]);
    }
    if(n->type==AST_FIELD_EXPR&&n->child_count) {
        ASTNode* base=infer(c,n->children[0],depth+1);
        ASTNode* target=base&&base->name?declaration(c,base->name):NULL;ASTNode* out=NULL;
        if(target&&target->type==AST_STRUCT_DECL)for(unsigned i=0;i<target->child_count;i++)if(same(target->children[i],n->name))out=ast_clone(target->children[i]->type_node);
        ast_free(base);return out;
    }
    if(n->type==AST_REF_EXPR&&n->child_count) {
        ASTNode* elem=infer(c,n->children[0],depth+1);if(!elem)return NULL;
        if(elem->op==TOKEN_KW_REF)return elem;
        if(elem->op==TOKEN_LBRACKET)elem->int_val=UINT64_MAX;
        ASTNode* t=ast_create(AST_TYPE);t->op=TOKEN_KW_REF;t->is_mut=n->is_mut;t->type_node=elem;return t;
    }
    if(n->type==AST_DEREF_EXPR&&n->child_count) {ASTNode* t=infer(c,n->children[0],depth+1);ASTNode* out=t?ast_clone(t->type_node):NULL;ast_free(t);return out;}
    if(n->type==AST_TRY_EXPR&&n->child_count) {
        ASTNode* t=infer(c,n->children[0],depth+1);ASTNode* d=t&&t->name?declaration(c,t->name):NULL;
        ASTNode* out=d&&d->type==AST_ENUM_DECL&&d->child_count&&d->children[0]->child_count?ast_clone(d->children[0]->children[0]):NULL;ast_free(t);return out;
    }
    if(n->type==AST_BINARY_EXPR&&n->child_count==2) {
        switch(n->op){case TOKEN_EQ_EQ:case TOKEN_BANG_EQUAL:case TOKEN_LT:case TOKEN_GT:case TOKEN_LT_EQUAL:case TOKEN_GT_EQUAL:case TOKEN_AND_AND:case TOKEN_OR_OR:return named("bool");default:return infer(c,n->children[0],depth+1);}
    }
    if(n->type==AST_UNARY_EXPR&&n->child_count)return infer(c,n->children[0],depth+1);
    if(n->type==AST_ARRAY_LIT&&n->child_count) {ASTNode* t=ast_create(AST_TYPE);t->op=TOKEN_LBRACKET;t->int_val=n->child_count;t->type_node=infer(c,n->children[0],depth+1);return t;}
    if(n->type==AST_INDEX_EXPR&&n->child_count) {
        ASTNode* t=infer(c,n->children[0],depth+1);ASTNode* array=t&&t->op==TOKEN_KW_REF?t->type_node:t;
        ASTNode* out=array?ast_clone(array->type_node):NULL;ast_free(t);return out;
    }
    return NULL;
}
static const char* variant_name(ASTNode* n)
{
    if(!n||!n->name)return NULL;
    const char* name=n->name;const char* colon=strrchr(name,':');return colon?colon+1:name;
}
static int constructor(Context* c,ASTNode* expression,ASTNode* expected)
{
    if(!expression||!expected||!expected->name)return 0;
    ASTNode* en=declaration(c,expected->name);if(!en||en->generic_parameters)return 0;
    if(en->type==AST_STRUCT_DECL&&expression->type==AST_CALL_EXPR&&expression->is_struct_lit&&expression->child_count) {
        ASTNode* callee=expression->children[0];
        const char* specialization=strstr(en->name,"__g");
        size_t base_length=specialization?(size_t)(specialization-en->name):en->name_len;
        if(callee->type!=AST_IDENT||!callee->name||
           !(same(callee,en->name)||(callee->name_len==base_length&&!memcmp(callee->name,en->name,base_length))))return 0;
        ast_set_name(callee,en->name,en->name_len);
        for(unsigned i=1;i<expression->child_count;i++) {
            ASTNode* field=expression->children[i];
            for(unsigned j=0;j<en->child_count;j++)if(same(en->children[j],field->name)&&field->child_count)
                constructor(c,field->children[0],en->children[j]->type_node);
        }
        return 0;
    }
    if(en->type!=AST_ENUM_DECL)return 0;
    ASTNode* name=expression->type==AST_CALL_EXPR&&expression->child_count?expression->children[0]:expression;
    const char* variant=variant_name(name);if(!variant)return 0;
    /* An expected type may resolve an unqualified constructor, but must never
       silently change an explicitly named enum to a different nominal type. */
    const char* owner=NULL;size_t owner_length=0;
    if(name->type==AST_FIELD_EXPR&&name->child_count&&name->children[0]->name) {
        owner=name->children[0]->name;owner_length=name->children[0]->name_len;
    } else if(name->type==AST_IDENT&&variant>name->name) {
        owner=name->name;owner_length=(size_t)(variant-name->name)-2;
    }
    if(owner) {
        const char* specialization=strstr(en->name,"__g");
        size_t base_length=specialization?(size_t)(specialization-en->name):en->name_len;
        if(!((owner_length==en->name_len&&!memcmp(owner,en->name,owner_length))||
             (owner_length==base_length&&!memcmp(owner,en->name,owner_length))))return 0;
    }
    for(unsigned i=0;i<en->child_count;i++)if(same(en->children[i],variant)) {
        if(name->type!=AST_IDENT&&name->type!=AST_FIELD_EXPR)return 0;
        if(name->type==AST_FIELD_EXPR&&name->child_count&&same(name->children[0],en->name))return 0;
        ASTNode* owner=ast_create(AST_IDENT);ast_set_name(owner,en->name,en->name_len);
        for(unsigned j=0;j<name->child_count;j++)ast_free(name->children[j]);
        name->child_count=0;
        ast_set_name(name,variant,(unsigned)strlen(variant));name->type=AST_FIELD_EXPR;ast_add_child(name,owner);
        return 0;
    }
    return 0;
}
static int method_call(Context* c,ASTNode* call)
{
    ASTNode* callee=call->children[0];char link[1024];
    if(callee->type==AST_IDENT&&callee->name) {
        char* last=strrchr(callee->name,':');
        if(last&&last>callee->name&&last[-1]==':') {
            size_t prefix=(size_t)(last-callee->name-1);
            if(prefix+strlen(last+1)+3>=sizeof(link))return error(c,call,"associated method name limit");
            memcpy(link,callee->name,prefix);link[prefix]=0;strcat(link,"__");strcat(link,last+1);
            ASTNode* fn=declaration(c,link);if(fn&&fn->type==AST_FUNCTION)ast_set_name(callee,link,(unsigned)strlen(link));
        }
        return 0;
    }
    if(callee->type!=AST_FIELD_EXPR||callee->child_count!=1)return 0;
    ASTNode* receiver=callee->children[0];ASTNode* type=infer(c,receiver,0);
    ASTNode* base=type&&type->op==TOKEN_KW_REF?type->type_node:type;
    if(!base||!base->name){ast_free(type);return 0;}
    if(strlen(base->name)+callee->name_len+3>=sizeof(link)){ast_free(type);return error(c,call,"method name limit");}
    snprintf(link,sizeof(link),"%s__%s",base->name,callee->name);
    ASTNode* method=declaration(c,link);ASTNode* first=method&&method->child_count?method->children[0]:NULL;
    if(!method||method->type!=AST_FUNCTION||!first||first->type!=AST_PARAM){ast_free(type);return 0;}
    ASTNode* expected=first->type_node;
    if(expected&&expected->op==TOKEN_KW_REF&&type->op!=TOKEN_KW_REF) {
        ASTNode* borrow=ast_create(AST_REF_EXPR);borrow->is_mut=expected->is_mut;ast_add_child(borrow,receiver);receiver=borrow;
    }
    callee->children[0]=NULL;ast_free(callee);
    ASTNode* name=ast_create(AST_IDENT);ast_set_name(name,link,(unsigned)strlen(link));call->children[0]=name;
    unsigned old=call->child_count;ast_add_child(call,receiver);
    memmove(call->children+2,call->children+1,(old-1)*sizeof(*call->children));call->children[1]=receiver;
    ast_free(type);return 0;
}
static int match_argument(Context* c,ASTNode* pattern,ASTNode* actual,ASTNode* parameters,ASTNode** arguments,unsigned depth)
{
    if(!pattern||!actual||depth>64)return -1;
    if(pattern->op==TOKEN_EOF)for(unsigned i=0;i<parameters->child_count;i++)if(same(pattern,parameters->children[i]->name)) {
        if(!arguments[i]){arguments[i]=ast_clone(actual);return 0;}
        char a[KEY_LIMIT]={0},b[KEY_LIMIT]={0};size_t as=0,bs=0;
        return key_type(arguments[i],a,&as,0)||key_type(actual,b,&bs,0)||strcmp(a,b)?-1:0;
    }
    if(pattern->op!=actual->op||pattern->is_mut!=actual->is_mut||pattern->int_val!=actual->int_val||
       pattern->child_count!=actual->child_count)return -1;
    if((pattern->name||actual->name)&&(!pattern->name||!actual->name||strcmp(pattern->name,actual->name)))return -1;
    if(pattern->type_node||actual->type_node)if(match_argument(c,pattern->type_node,actual->type_node,parameters,arguments,depth+1))return -1;
    for(unsigned i=0;i<pattern->child_count;i++)if(match_argument(c,pattern->children[i],actual->children[i],parameters,arguments,depth+1))return -1;
    return 0;
}
static int infer_generic_call(Context* c,ASTNode* call)
{
    if(!call->child_count||call->children[0]->type!=AST_IDENT)return 0;
    ASTNode* callee=call->children[0];ASTNode* fn=declaration(c,callee->name);
    if(!fn||fn->type!=AST_FUNCTION||!fn->generic_parameters)return 0;
    unsigned count=fn->generic_parameters->child_count;
    if(count>64)return error(c,call,"generic parameter inference limit");
    ASTNode* arguments[64]={0};unsigned index=1;int failed=0;
    for(unsigned i=0;i<fn->child_count;i++)if(fn->children[i]->type==AST_PARAM) {
        ASTNode* actual=index<call->child_count?infer(c,call->children[index++],0):NULL;
        if(match_argument(c,fn->children[i]->type_node,actual,fn->generic_parameters,arguments,0))failed=1;
        ast_free(actual);
    }
    if(index!=call->child_count)failed=1;
    for(unsigned i=0;i<count;i++)if(!arguments[i])failed=1;
    ASTNode* instance=failed?NULL:instantiate(c,fn,arguments,count);
    for(unsigned i=0;i<count;i++)ast_free(arguments[i]);
    if(!instance)return error(c,call,"cannot infer consistent generic arguments; supply explicit types");
    ast_set_name(callee,instance->name,instance->name_len);return 0;
}
static int expressions(Context* c,ASTNode* n,ASTNode* result,unsigned depth)
{
    while(n&&n->type==AST_BINARY_EXPR&&n->child_count==2) {
        if(expressions(c,n->children[1],result,depth+1))return -1;
        n=n->children[0];
    }
    if(!n)return 0;
    if(depth>256)return error(c,n,"expression traversal nesting limit");
    if(n->generic_parameters)return 0;
    unsigned saved=c->binding_count;
    if(n->type==AST_FUNCTION) {
        result=n->type_node;
        for(unsigned i=0;i<n->child_count;i++)if(n->children[i]->type==AST_PARAM) {
            if(c->binding_count==BINDING_LIMIT)return error(c,n,"binding limit");
            c->bindings[c->binding_count++]=(Binding){n->children[i]->name,ast_clone(n->children[i]->type_node)};
        }
    }
    if((n->type==AST_LET_STMT||n->type==AST_VAR_STMT)&&n->type_node&&n->child_count)constructor(c,n->children[0],n->type_node);
    if(n->type==AST_RET_STMT&&n->child_count)constructor(c,n->children[0],result);
    if(n->type==AST_MATCH_EXPR&&n->child_count) {
        /* Resolve the scrutinee before inferring it: it may itself be a method
           or an inferred generic call. Payload bindings belong to one arm. */
        if(expressions(c,n->children[0],result,depth+1))return -1;
        ASTNode* t=infer(c,n->children[0],0);
        ASTNode* en=t&&t->name?declaration(c,t->name):NULL;
        int failed=0;
        for(unsigned i=1;i<n->child_count&&!failed;i++) {
            ASTNode* arm=n->children[i];if(!arm->child_count)continue;
            ASTNode* pattern=arm->children[0];constructor(c,pattern,t);
            ASTNode* name=pattern->type==AST_CALL_EXPR&&pattern->child_count?pattern->children[0]:pattern;
            ASTNode* variant=NULL;
            if(en&&en->type==AST_ENUM_DECL&&name->type==AST_FIELD_EXPR&&name->child_count&&same(name->children[0],en->name))
                for(unsigned j=0;j<en->child_count;j++)if(same(en->children[j],name->name))variant=en->children[j];
            unsigned arm_saved=c->binding_count;
            if(variant&&pattern->type==AST_CALL_EXPR&&pattern->child_count==variant->child_count+1) {
                for(unsigned j=1;j<pattern->child_count;j++) {
                    ASTNode* binding=pattern->children[j];
                    if(binding->type!=AST_IDENT||same(binding,"_"))continue;
                    if(c->binding_count==BINDING_LIMIT){failed=error(c,binding,"binding limit");break;}
                    c->bindings[c->binding_count++]=(Binding){binding->name,ast_clone(variant->children[j-1])};
                }
            } else if(pattern->type==AST_IDENT&&!same(pattern,"_")&&!variant) {
                if(c->binding_count==BINDING_LIMIT)failed=error(c,pattern,"binding limit");
                else c->bindings[c->binding_count++]=(Binding){pattern->name,ast_clone(t)};
            }
            for(unsigned j=1;j<arm->child_count&&!failed;j++)failed=expressions(c,arm->children[j],result,depth+1);
            for(unsigned j=arm_saved;j<c->binding_count;j++)ast_free(c->bindings[j].type);
            c->binding_count=arm_saved;
        }
        ast_free(t);return failed;
    }
    for(unsigned i=0;i<n->child_count;i++) {
        ASTNode* child=n->children[i];if(expressions(c,child,result,depth+1))return -1;
        if(child->type==AST_LET_STMT||child->type==AST_VAR_STMT) {
            if(c->binding_count==BINDING_LIMIT)return error(c,child,"binding limit");
            ASTNode* t=child->type_node?ast_clone(child->type_node):child->child_count?infer(c,child->children[0],0):NULL;
            c->bindings[c->binding_count++]=(Binding){child->name,t};
        }
    }
    if(n->type==AST_CALL_EXPR&&(method_call(c,n)||infer_generic_call(c,n)))return -1;
    if(n->type==AST_FUNCTION||n->type==AST_BLOCK||n->type==AST_BLOCK_EXPR) {
        for(unsigned i=saved;i<c->binding_count;i++)ast_free(c->bindings[i].type);
        c->binding_count=saved;
    }
    return 0;
}
int kru_instantiate_generics(ASTNode* program)
{
    Context* c=calloc(1,sizeof(*c));if(!c)return -1;c->root=program;
    /* Option/Result are language core templates, also available without imports. */
    Lexer lexer;lexer_init(&lexer,"enum Option<T>{Some(T),None} enum Result<T,E>{Ok(T),Err(E)}");
    ASTNode* prelude=parse_program(&lexer);
    if(!prelude){free(c);return -1;}
    for(unsigned i=0;i<prelude->child_count;i++)if(!declaration(c,prelude->children[i]->name)) {
        ast_add_child(program,prelude->children[i]);prelude->children[i]=NULL;
    }
    ast_free(prelude);
    int result=methods(c);
    if(!result)result=types(c,program,0);
    if(!result)result=expressions(c,program,NULL,0);
    for(unsigned i=0;i<c->count;i++)free(c->instances[i].key);
    for(unsigned i=0;i<c->binding_count;i++)ast_free(c->bindings[i].type);
    free(c);return result;
}
