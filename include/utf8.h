#ifndef KRU_UTF8_H
#define KRU_UTF8_H
#include <stddef.h>
#include <stdint.h>
/* Validate bounded bytes; embedded zero is an ordinary Unicode scalar. */
static inline int kru_utf8_valid(const unsigned char* data,size_t size)
{
    for(size_t i=0;i<size;) {
        uint32_t scalar=data[i++],minimum;unsigned rest;
        if(scalar<0x80)continue;
        if(scalar>=0xC2&&scalar<=0xDF){scalar&=31;rest=1;minimum=0x80;}
        else if(scalar>=0xE0&&scalar<=0xEF){scalar&=15;rest=2;minimum=0x800;}
        else if(scalar>=0xF0&&scalar<=0xF4){scalar&=7;rest=3;minimum=0x10000;}
        else return 0;
        if(rest>size-i)return 0;
        while(rest--){unsigned byte=data[i++];if((byte&0xC0)!=0x80)return 0;scalar=(scalar<<6)|(byte&63);}
        if(scalar<minimum||scalar>0x10FFFF||(scalar>=0xD800&&scalar<=0xDFFF))return 0;
    }
    return 1;
}
#endif
