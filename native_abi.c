#include "../include/native_abi.h"
unsigned kru_x64_scalar_arguments(const KruTypeId* types,unsigned count,KruAbiLocation* locations)
{
    unsigned gp=0,sse=0,stack=0;
    for(unsigned i=0;i<count;i++) {
        int floating=types[i]==KRU_F32||types[i]==KRU_F64;
        unsigned words=types[i]==KRU_I128||types[i]==KRU_U128?2u:1u;
        if(floating&&sse<8)locations[i]=(KruAbiLocation){KRU_ABI_SSE,sse++,1};
        else if(!floating&&gp+words<=6) {locations[i]=(KruAbiLocation){KRU_ABI_GP,gp,words};gp+=words;}
        else {
            if(words==2)stack=(stack+1u)&~1u;
            locations[i]=(KruAbiLocation){KRU_ABI_STACK,stack,words};stack+=words;
        }
    }
    return stack;
}
