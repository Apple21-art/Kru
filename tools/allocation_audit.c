/* Test-only allocation accounting. Link with GNU ld --wrap; never shipped in kru0.
   Counts compiler-owned allocations, not libc internals; no reachability scan. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
void *__real_malloc(size_t), *__real_calloc(size_t,size_t), *__real_realloc(void*,size_t);
void __real_free(void*);
char *__real_strdup(const char*), *__real_strndup(const char*,size_t);
typedef struct {void *pointer,*caller;size_t bytes;} Record;
static Record *records;
static size_t count,capacity;
static int registered;
static void report(void) {
 size_t blocks=0,bytes=0;
 for(size_t i=0;i<count;i++)if(records[i].pointer) {
  if(blocks<30)fprintf(stderr,"allocation-audit: %zu bytes allocated at %p remain unfreed\n",records[i].bytes,records[i].caller);
  blocks++;bytes+=records[i].bytes;
 }
 __real_free(records);
 if(blocks){
  fprintf(stderr,"allocation-audit: FAIL %zu blocks / %zu bytes\n",blocks,bytes);
  const char *path=getenv("KRU_ALLOCATION_AUDIT_LOG");
  if(path){FILE *log=fopen(path,"a");if(log){fprintf(log,"%zu blocks / %zu bytes\n",blocks,bytes);fclose(log);}}
  _Exit(99);
 }
}
static void forget(void *p) {
 if(!p)return;
 for(size_t i=count;i-->0;)if(records[i].pointer==p){records[i].pointer=NULL;return;}
}
static void remember(void *p,size_t bytes,void *caller) {
 if(!p)return;
 if(!registered){registered=1;atexit(report);}
 for(size_t i=0;i<count;i++)if(!records[i].pointer){records[i]=(Record){p,caller,bytes};return;}
 if(count==capacity){size_t next=capacity?capacity*2:256;Record *grown=__real_realloc(records,next*sizeof(*records));if(!grown)_Exit(98);records=grown;capacity=next;}
 records[count++]=(Record){p,caller,bytes};
}
void *__wrap_malloc(size_t n){void *p=__real_malloc(n);remember(p,n,__builtin_return_address(0));return p;}
void *__wrap_calloc(size_t n,size_t size){void *p=__real_calloc(n,size);remember(p,n*size,__builtin_return_address(0));return p;}
void *__wrap_realloc(void *old,size_t n){
 /* Retain the old record on allocation failure; avoid using freed pointer bits. */
 size_t index=count;for(size_t i=count;i-->0;)if(records[i].pointer==old){index=i;break;}
 void *p=__real_realloc(old,n);
 if(p||!n){if(index<count)records[index].pointer=NULL;remember(p,n,__builtin_return_address(0));}return p;
}
void __wrap_free(void *p){forget(p);__real_free(p);}
char *__wrap_strdup(const char *s){char *p=__real_strdup(s);remember(p,strlen(s)+1,__builtin_return_address(0));return p;}
char *__wrap_strndup(const char *s,size_t n){char *p=__real_strndup(s,n);remember(p,p?strlen(p)+1:0,__builtin_return_address(0));return p;}
