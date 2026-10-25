#include <stdlib.h>
#include <string.h>
#include "../include/move_flow.h"

static int bit(const uint64_t* values,unsigned id)
{return (values[id/64]>>(id%64))&1;}
static void put(uint64_t* values,unsigned id,int value)
{
 uint64_t mask=UINT64_C(1)<<(id%64);
 if(value)values[id/64]|=mask;else values[id/64]&=~mask;
}

int kru_reference_validate(KruMoveFlow* f,ASTNode** source)
{
 unsigned count=f->count,words=(f->local_count+63)/64,loans=0;
 for(unsigned i=0;i<count;i++)loans+=f->events[i].action==KRU_REF_BORROW;
 if(!loans||!words)return 0;
 unsigned* successors=malloc((size_t)count*2*sizeof(*successors));
 uint64_t* live=calloc((size_t)count*words,sizeof(*live));
 uint64_t* state=calloc((size_t)count*words,sizeof(*state));
 unsigned* queue=malloc((count+1)*sizeof(*queue));
 unsigned char* reachable=calloc(count,1),*queued=calloc(count,1);
 int result=-1;
 if(!successors||!live||!state||!queue||!reachable||!queued)goto done;
 if(kru_flow_successors(f,successors))goto done;
 /* Reference liveness is distinct from owner availability. A dead reference
    does not prevent later mutation; copying a live reference keeps its source
    live until the copy, including across back edges. */
 int changed;
 do {
  changed=0;
  for(unsigned step=count;step;step--) {
   unsigned i=step-1;KruMoveEvent* e=&f->events[i];
   uint64_t input[KRU_MOVE_LOCALS/64]={0};
   for(unsigned edge=0;edge<2;edge++) {
    unsigned next=successors[2*i+edge];
    if(next<count)for(unsigned w=0;w<words;w++)input[w]|=live[(size_t)next*words+w];
   }
   int used=bit(input,e->local);
   if(e->action==KRU_REF_BORROW||e->action==KRU_REF_CLEAR||e->action==KRU_REF_COPY)put(input,e->local,0);
   if((e->action==KRU_REF_COPY||e->action==KRU_REF_JOIN)&&used)put(input,e->target,1);
   if(e->action==KRU_REF_USE)put(input,e->local,1);
   for(unsigned w=0;w<words;w++) {
    uint64_t* value=&live[(size_t)i*words+w];
    if(*value!=input[w]){*value=input[w];changed=1;}
   }
  }
 }while(changed);
 /* Analyze one loan at a time. This bounds working memory by events*locals,
    instead of multiplying it by the number of loan creation sites. */
 for(unsigned loan=0;loan<count;loan++) {
  KruMoveEvent* origin=&f->events[loan];if(origin->action!=KRU_REF_BORROW)continue;
  memset(state,0,(size_t)count*words*sizeof(*state));
  memset(reachable,0,count);memset(queued,0,count);
  unsigned head=0,tail=1;queue[0]=0;reachable[0]=queued[0]=1;
  while(head!=tail) {
   unsigned i=queue[head];head=(head+1)%(count+1);queued[i]=0;
   uint64_t output[KRU_MOVE_LOCALS/64];memcpy(output,state+(size_t)i*words,words*sizeof(*output));
   KruMoveEvent* e=&f->events[i];
   if(e->action==KRU_REF_COPY)put(output,e->local,bit(output,e->target));
   if(e->action==KRU_REF_JOIN)put(output,e->local,bit(output,e->local)||bit(output,e->target));
   if(e->action==KRU_REF_CLEAR)put(output,e->local,0);
   if(e->action==KRU_REF_BORROW)put(output,e->local,i==loan);
   for(unsigned edge=0;edge<2;edge++) {
    unsigned next=successors[2*i+edge];if(next>=count)continue;
    int update=!reachable[next];reachable[next]=1;
    for(unsigned w=0;w<words;w++) {
     uint64_t* value=&state[(size_t)next*words+w],merged=*value|output[w];
     if(*value!=merged){*value=merged;update=1;}
    }
    if(update&&!queued[next]){queue[tail]=next;tail=(tail+1)%(count+1);queued[next]=1;}
   }
  }
  for(unsigned i=0;i<count;i++) {
   if(!reachable[i])continue;
   KruMoveEvent* e=&f->events[i];int conflict=0;
   if(e->action==KRU_REF_USE&&origin->conditional&&bit(state+(size_t)i*words,e->local)) {
    for(unsigned w=0;w<words;w++) {
     uint64_t others=state[(size_t)i*words+w]&live[(size_t)i*words+w];
     if(w==e->local/64)others&=~(UINT64_C(1)<<(e->local%64));
     if(others){*source=e->source;result=1;goto done;}
    }
   }
   if(e->local==origin->target) {
    if(e->action==KRU_REF_END)conflict=2;
    if(e->action==KRU_MOVE_TAKE)conflict=1;
    if(e->action==KRU_REF_ACCESS&&(e->conditional||origin->conditional))conflict=1;
   }
   if(e->action==KRU_REF_BORROW&&e->target==origin->target&&(e->conditional||origin->conditional))conflict=1;
   if(!conflict)continue;
   for(unsigned w=0;w<words;w++)if(state[(size_t)i*words+w]&live[(size_t)i*words+w]) {
    *source=e->source;result=conflict;goto done;
   }
  }
 }
 result=0;
 done:free(successors);free(live);free(state);free(queue);free(reachable);free(queued);return result;
}
