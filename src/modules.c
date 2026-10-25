#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include "../include/modules.h"
#include "../include/parser.h"
#include "../include/diagnostic.h"
#define MODULE_LIMIT 128
#define SYMBOL_LIMIT 4096
#define NAME_LIMIT 512
typedef struct {char path[PATH_MAX],name[NAME_LIMIT];ASTNode* tree;} Module;
typedef struct {ASTNode* node;unsigned module;char original[NAME_LIMIT],link[NAME_LIMIT];} Symbol;
typedef struct {Module modules[MODULE_LIMIT];unsigned count;Symbol symbols[SYMBOL_LIMIT];unsigned symbol_count;int failed;} Session;
static int same(ASTNode* n,const char* name){return n->name&&n->name_len==strlen(name)&&!memcmp(n->name,name,n->name_len);}
static int problem(Session* s,const char* what,const char* name){fprintf(stderr,"[kru] modules: %s: %s\n",what,name);s->failed=1;return -1;}
static int load(Session* s,const char* path,const char* name,ASTNode* root,unsigned* index)
{
    char canonical[PATH_MAX];
    if(!realpath(path,canonical))return problem(s,"cannot open source module",path);
    for(unsigned i=0;i<s->count;i++)if(!strcmp(s->modules[i].path,canonical)){*index=i;return 0;}
    if(s->count==MODULE_LIMIT||strlen(name)>=NAME_LIMIT)return problem(s,"module limit exceeded",name);
    unsigned current=s->count++;*index=current;
    Module* m=&s->modules[current];strcpy(m->path,canonical);strcpy(m->name,name);
    if(root)m->tree=root;
    else {
        struct stat st;if(stat(canonical,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>16*1024*1024)return problem(s,"invalid module source",path);
        FILE* f=fopen(canonical,"rb");if(!f)return problem(s,"cannot read source module",path);
        char* source=malloc((size_t)st.st_size+1);if(!source){fclose(f);return problem(s,"allocation failed",path);}
        size_t n=fread(source,1,(size_t)st.st_size,f);fclose(f);source[n]=0;
        uint32_t previous=diagnostic_current_source();
        if(!diagnostic_add_source(canonical,source)){free(source);return problem(s,"source registry allocation failed",path);}
        Lexer lexer;lexer_init(&lexer,source);m->tree=parse_program(&lexer);free(source);
        diagnostic_use_source(previous);
        if(!m->tree)return problem(s,"module parsing failed",path);
    }
    for(unsigned i=0;i<m->tree->child_count;i++) {
        ASTNode* n=m->tree->children[i];
        if(n->type==AST_USE_DECL) {
            /* First-party modules are shipped in lib alongside the compiler. */
            char relative[NAME_LIMIT];
            if(n->name_len>=sizeof(relative))return problem(s,"module name too long",path);
            memcpy(relative,n->name,n->name_len);relative[n->name_len]=0;
            for(char* p=relative;*p;p++)if(p[0]==':'&&p[1]==':'){*p='/';memmove(p+1,p+2,strlen(p+2)+1);}
            char imported[PATH_MAX];char* slash=strrchr(canonical,'/');size_t dir=(size_t)(slash-canonical);
            if(dir+strlen(relative)+6>=sizeof(imported))return problem(s,"module path too long",relative);
            memcpy(imported,canonical,dir);imported[dir]=0;strcat(imported,"/");strcat(imported,relative);strcat(imported,".kru");
            if(access(imported,R_OK)&&!strncmp(relative,"kru_",4)) {
                char executable[PATH_MAX];ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
                if(length>0) {
                    executable[length]=0;char* end=strrchr(executable,'/');if(end)*end=0;
                    end=strrchr(executable,'/');if(end)*end=0;
                    if(strlen(executable)+strlen(relative)+10<sizeof(imported)){strcpy(imported,executable);strcat(imported,"/lib/");strcat(imported,relative);strcat(imported,".kru");}
                }
            }
            unsigned imported_index;
            if(load(s,imported,n->name,NULL,&imported_index))return -1;
        } else if(n->name&&(n->type==AST_FUNCTION||n->type==AST_STRUCT_DECL||n->type==AST_ENUM_DECL||n->type==AST_TYPE_ALIAS||n->type==AST_CONST_DECL)) {
            if(s->symbol_count==SYMBOL_LIMIT||n->name_len>=NAME_LIMIT)return problem(s,"symbol limit exceeded",name);
            Symbol* sym=&s->symbols[s->symbol_count++];sym->node=n;sym->module=current;strcpy(sym->original,n->name);
            if(current) {
                if(strlen(name)+n->name_len+3>=NAME_LIMIT)return problem(s,"symbol name too long",name);
                snprintf(sym->link,sizeof(sym->link),"%s__%s",name,n->name);
                for(char* p=sym->link;*p;p++)if(*p==':')*p='_';
            } else strcpy(sym->link,n->name);
            if(n->type==AST_ENUM_DECL)for(unsigned v=0;v<n->child_count;v++) {
                ASTNode* variant=n->children[v];
                if(s->symbol_count==SYMBOL_LIMIT||variant->name_len>=NAME_LIMIT)return problem(s,"variant limit",name);
                Symbol* vs=&s->symbols[s->symbol_count++];vs->node=variant;vs->module=current;variant->is_public=n->is_public;
                strcpy(vs->original,variant->name);
                if(strlen(sym->link)+variant->name_len+3>=NAME_LIMIT)return problem(s,"variant name too long",name);
                strcpy(vs->link,sym->link);strcat(vs->link,"::");strcat(vs->link,variant->name);
            }
        }
    }
    return 0;
}
static int imports(Session* s,unsigned module,unsigned target)
{
    ASTNode* tree=s->modules[module].tree;
    for(unsigned i=0;i<tree->child_count;i++)if(tree->children[i]->type==AST_USE_DECL&&same(tree->children[i],s->modules[target].name))return 1;
    return 0;
}
static int resolve(Session* s,ASTNode* n,unsigned module,const char** locals,unsigned* count,unsigned depth)
{
    if(!n)return 0;
    if(depth>512)return problem(s,"resolution nesting limit","source");
    unsigned saved=*count;
    if(n->type==AST_FUNCTION) {
        for(unsigned i=0;i<n->child_count;i++)if(n->children[i]->type==AST_PARAM) {
            if(*count==2048)return problem(s,"binding limit","source");
            locals[(*count)++]=n->children[i]->name;
        }
    }
    if(n->name&&(n->type==AST_IDENT||n->type==AST_TYPE)) {
        int local=0;if(n->type==AST_IDENT)for(unsigned i=*count;i;i--)if(!strcmp(locals[i-1],n->name)){local=1;break;}
        if(!local) {
            Symbol* found=NULL;
            /* Own declarations shadow imports, including private definitions. */
            for(unsigned i=0;i<s->symbol_count;i++)if(s->symbols[i].module==module&&same(n,s->symbols[i].original))found=&s->symbols[i];
            if(!found)for(unsigned i=0;i<s->symbol_count;i++) {
                Symbol* sym=&s->symbols[i];if(!sym->module||!sym->node->is_public||!imports(s,module,sym->module))continue;
                char qualified[NAME_LIMIT];
                size_t module_length=strlen(s->modules[sym->module].name),symbol_length=strlen(sym->original);
                if(module_length+symbol_length+3>=sizeof(qualified))continue;
                memcpy(qualified,s->modules[sym->module].name,module_length);
                memcpy(qualified+module_length,"::",2);
                memcpy(qualified+module_length+2,sym->original,symbol_length+1);
                if((sym->node->type==AST_STRUCT_DECL||sym->node->type==AST_ENUM_DECL)&&n->name_len>strlen(qualified)+2&&!strncmp(n->name,qualified,strlen(qualified))&&!strncmp(n->name+strlen(qualified),"::",2)) {
                    char member[NAME_LIMIT];size_t length=strlen(sym->link)+strlen(n->name+strlen(qualified))+1;
                    if(length>=sizeof(member))return problem(s,"associated name too long",n->name);
                    strcpy(member,sym->link);strcat(member,n->name+strlen(qualified));ast_set_name(n,member,(unsigned)strlen(member));break;
                }
                if(same(n,sym->original)||same(n,qualified)) {
                    if(found&&found!=sym)return problem(s,"ambiguous imported name",n->name);
                    found=sym;
                }
            }
            if(found)ast_set_name(n,found->link,(unsigned)strlen(found->link));
        }
    }
    if(resolve(s,n->type_node,module,locals,count,depth+1))return -1;
    for(unsigned i=0;i<n->child_count;i++) {
        ASTNode* child=n->children[i];
        if(resolve(s,child,module,locals,count,depth+1))return -1;
        if(child->type==AST_LET_STMT||child->type==AST_VAR_STMT) {
            if(*count==2048)return problem(s,"binding limit","source");
            locals[(*count)++]=child->name;
        }
    }
    if(n->type==AST_FUNCTION||n->type==AST_BLOCK||n->type==AST_BLOCK_EXPR)*count=saved;
    return 0;
}
int kru_resolve_modules(ASTNode* program,const char* source_path)
{
    int needed=0;for(unsigned i=0;i<program->child_count;i++)if(program->children[i]->type==AST_USE_DECL)needed=1;
    if(!needed)return 0;
    Session* s=calloc(1,sizeof(*s));if(!s)return -1;
    unsigned first;int status=load(s,source_path,"",program,&first);
    if(!status)for(unsigned i=0;i<s->count;i++) {const char* locals[2048];unsigned count=0;if(resolve(s,s->modules[i].tree,i,locals,&count,0)){status=-1;break;}}
    if(!status)for(unsigned i=0;i<s->symbol_count;i++)if(s->symbols[i].node->type!=AST_ENUM_VARIANT)ast_set_name(s->symbols[i].node,s->symbols[i].link,(unsigned)strlen(s->symbols[i].link));
    if(!status) {
        /* Imported AST ownership transfers to the entry compilation unit. */
        for(unsigned i=1;i<s->count;i++) {
            ASTNode* tree=s->modules[i].tree;
            for(unsigned j=0;j<tree->child_count;j++)if(tree->children[j]->type!=AST_USE_DECL) {ast_add_child(program,tree->children[j]);tree->children[j]=NULL;}
        }
        unsigned out=0;for(unsigned i=0;i<program->child_count;i++) {
            ASTNode* n=program->children[i];if(n->type==AST_USE_DECL)ast_free(n);else program->children[out++]=n;
        }program->child_count=out;
    }
    for(unsigned i=1;i<s->count;i++)ast_free(s->modules[i].tree);
    free(s);return status;
}
