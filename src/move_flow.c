#include <stdlib.h>
#include <string.h>
#include "../include/move_flow.h"
void kru_move_begin(KruMoveFlow* f){f->count=f->local_count=0;f->enabled=1;}
int kru_move_local(KruMoveFlow* f,ASTNode* declaration)
{
 for(unsigned i=0;i<f->local_count;i++)if(f->locals[i]==declaration)return (int)i;
 if(f->local_count==KRU_MOVE_LOCALS)return -1;
 f->locals[f->local_count]=declaration;return (int)f->local_count++;
}
int kru_move_event(KruMoveFlow* f,KruMoveEvent event)
{
 if(!f->enabled)return 0;
 if(f->count==KRU_MOVE_EVENTS)return -1;
 if(f->count&&event.position<f->events[f->count-1].position)return -1;
 f->events[f->count++]=event;return 0;
}
void kru_move_patch(KruMoveFlow* f,uint32_t relocation,uint32_t target)
{
 if(!f->enabled)return;
 for(unsigned i=f->count;i;i--)if(f->events[i-1].action==KRU_MOVE_BRANCH&&f->events[i-1].relocation==relocation){f->events[i-1].target=target;return;}
}
void kru_move_known_branch(KruMoveFlow* f,uint32_t relocation,int taken)
{
 if(!f->enabled)return;
 for(unsigned i=f->count;i;i--)if(f->events[i-1].action==KRU_MOVE_BRANCH&&f->events[i-1].relocation==relocation){f->events[i-1].conditional=taken?0:2;return;}
}
static unsigned target_event(KruMoveFlow* f,uint32_t position)
{
 unsigned low=0,high=f->count;
 while(low<high){unsigned middle=low+(high-low)/2;if(f->events[middle].position<position)low=middle+1;else high=middle;}
 return low;
}
int kru_flow_successors(KruMoveFlow* f,unsigned* successors)
{
 unsigned count=f->count;
 for(unsigned i=0;i<count;i++) {
  KruMoveEvent* event=&f->events[i];
  successors[2*i]=event->action==KRU_MOVE_EXIT?count:i+1;successors[2*i+1]=count;
  if(event->action==KRU_MOVE_BRANCH) {
   if(event->target==UINT32_MAX)return -1;
   unsigned target=target_event(f,event->target);
   if(event->conditional==1)successors[2*i+1]=target;
   else if(!event->conditional)successors[2*i]=target;
  }
 }
 return 0;
}
int kru_move_validate(KruMoveFlow* f,ASTNode** source)
{
 f->enabled=0;
 if(!f->local_count)return 0;
 unsigned count=f->count,words=(f->local_count+63)/64;
 if(!count)return -1;
 uint64_t* state=calloc((size_t)count*words,sizeof(*state));
 unsigned* queue=malloc((count+1)*sizeof(*queue));
 unsigned char* queued=calloc(count,1),*reachable=calloc(count,1);
 unsigned* successors=malloc((size_t)count*2*sizeof(*successors));
 if(!state||!queue||!queued||!reachable||!successors){free(state);free(queue);free(queued);free(reachable);free(successors);return -1;}
 int result=0;
 if(kru_flow_successors(f,successors)){result=-1;goto done;}
 /* A bit means possibly unavailable. Union at joins and a worklist compute
    the least fixed point, including back edges and early-return paths. */
 for(unsigned w=0;w<words;w++)state[w]=UINT64_MAX;
 unsigned head=0,tail=1;queue[0]=0;queued[0]=reachable[0]=1;
 while(head!=tail) {
  unsigned i=queue[head];head=(head+1)%(count+1);queued[i]=0;
  uint64_t output[KRU_MOVE_LOCALS/64];memcpy(output,state+(size_t)i*words,words*sizeof(*output));
  KruMoveEvent* event=&f->events[i];
  if(event->action==KRU_MOVE_INIT)output[event->local/64]&=~(UINT64_C(1)<<(event->local%64));
  if(event->action==KRU_MOVE_TAKE)output[event->local/64]|=UINT64_C(1)<<(event->local%64);
  for(unsigned edge=0;edge<2;edge++) {
   unsigned next=successors[2*i+edge];if(next>=count)continue;
   int changed=!reachable[next];reachable[next]=1;
   for(unsigned w=0;w<words;w++) {
    uint64_t* value=&state[(size_t)next*words+w];uint64_t merged=*value|output[w];
    if(merged!=*value){*value=merged;changed=1;}
   }
   if(changed&&!queued[next]){queue[tail]=next;tail=(tail+1)%(count+1);queued[next]=1;}
  }
 }
 for(unsigned i=0;i<count;i++)if(reachable[i]&&f->events[i].action==KRU_MOVE_USE) {
  unsigned local=f->events[i].local;
  if(state[(size_t)i*words+local/64]&(UINT64_C(1)<<(local%64))){*source=f->events[i].source;result=1;break;}
 }
 done:free(state);free(queue);free(queued);free(reachable);free(successors);return result;
}
