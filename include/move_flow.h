#ifndef KRU_MOVE_FLOW_H
#define KRU_MOVE_FLOW_H
#include <stdint.h>
#include "ast.h"
#define KRU_MOVE_EVENTS 32768u
#define KRU_MOVE_LOCALS 2048u
typedef enum {KRU_MOVE_ENTRY,KRU_MOVE_USE,KRU_MOVE_INIT,KRU_MOVE_TAKE,KRU_MOVE_BRANCH,KRU_MOVE_EXIT,
 KRU_REF_BORROW,KRU_REF_COPY,KRU_REF_JOIN,KRU_REF_CLEAR,KRU_REF_USE,KRU_REF_ACCESS,KRU_REF_END} KruMoveAction;
typedef struct {
 uint32_t position,local,target,relocation;
 KruMoveAction action;
 unsigned conditional;
 ASTNode* source;
} KruMoveEvent;
typedef struct {
 KruMoveEvent events[KRU_MOVE_EVENTS];unsigned count,local_count,enabled;
 ASTNode* locals[KRU_MOVE_LOCALS];
} KruMoveFlow;
void kru_move_begin(KruMoveFlow* flow);
int kru_move_local(KruMoveFlow* flow,ASTNode* declaration);
int kru_move_event(KruMoveFlow* flow,KruMoveEvent event);
void kru_move_known_branch(KruMoveFlow* flow,uint32_t relocation,int taken);
void kru_move_patch(KruMoveFlow* flow,uint32_t relocation,uint32_t target);
/* 0 valid; 1 invalid use (source returned); -1 resource/graph failure. */
int kru_move_validate(KruMoveFlow* flow,ASTNode** source);
int kru_flow_successors(KruMoveFlow* flow,unsigned* successors);
/* 1 alias conflict, 2 storage escape, -1 resource failure. */
int kru_reference_validate(KruMoveFlow* flow,ASTNode** source);
#endif
