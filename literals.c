#include "../include/literals.h"
#include "../include/utf8.h"
static int hex(unsigned char ch)
{return ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:ch>='A'&&ch<='F'?ch-'A'+10:-1;}
int kru_decode_char(const char* token,size_t length,uint32_t* scalar)
{
    if(!token||length<3||token[0]!='\''||token[length-1]!='\'')return -1;
    const unsigned char* bytes=(const unsigned char*)token+1;size_t size=length-2;
    uint32_t value=0;
    if(bytes[0]=='\\') {
        if(size==2) {
            switch(bytes[1]) {
            case 'n':value='\n';break;case 'r':value='\r';break;case 't':value='\t';break;
            case '0':value=0;break;case '\\':value='\\';break;case '\'':value='\'';break;case '"':value='"';break;
            default:return -1;
            }
        } else if(size==4&&bytes[1]=='x'&&hex(bytes[2])>=0&&hex(bytes[3])>=0)
            value=(uint32_t)(hex(bytes[2])*16+hex(bytes[3]));
        else if(size>=5&&size<=10&&bytes[1]=='u'&&bytes[2]=='{'&&bytes[size-1]=='}') {
            for(size_t i=3;i<size-1;i++) {int digit=hex(bytes[i]);if(digit<0)return -1;value=value*16+(unsigned)digit;}
        } else return -1;
    } else {
        unsigned count;uint32_t minimum;
        if(bytes[0]<0x80){count=1;value=bytes[0];minimum=0;}
        else if(bytes[0]>=0xC2&&bytes[0]<=0xDF){count=2;value=bytes[0]&31;minimum=0x80;}
        else if(bytes[0]>=0xE0&&bytes[0]<=0xEF){count=3;value=bytes[0]&15;minimum=0x800;}
        else if(bytes[0]>=0xF0&&bytes[0]<=0xF4){count=4;value=bytes[0]&7;minimum=0x10000;}
        else return -1;
        if(size!=count)return -1;
        for(unsigned i=1;i<count;i++) {if((bytes[i]&0xC0)!=0x80)return -1;value=(value<<6)|(bytes[i]&63);}
        if(value<minimum)return -1;
    }
    if(value>0x10FFFF||(value>=0xD800&&value<=0xDFFF))return -1;
    *scalar=value;return 0;
}

#include <string.h>
static int hex_digit(unsigned char ch)
{
    if(ch>='0'&&ch<='9')return ch-'0';
    if(ch>='a'&&ch<='f')return ch-'a'+10;
    if(ch>='A'&&ch<='F')return ch-'A'+10;
    return -1;
}
int kru_decode_string(const char* token,size_t length,unsigned char* output,size_t* output_length)
{
    const unsigned char* s=(const unsigned char*)token;
    size_t used=0;
    int raw=length>=3&&s[0]=='r'&&s[1]=='"';
    uint32_t start=raw?2u:1u;
    if(length<=start||s[start-1]!='"'||s[length-1]!='"')return -1;
    for(uint32_t i=start;i<length-1;i++) {
        unsigned char ch=s[i];
        if(ch=='\\'&&!raw) {
            if(++i>=length-1)return -1;
            switch(s[i]) {
            case 'n':ch='\n';break;case 'r':ch='\r';break;case 't':ch='\t';break;case '0':ch=0;break;
            case '\\':ch='\\';break;case '"':ch='"';break;case '\'':ch='\'';break;
            case 'x': {
                if(i+2>=length-1||hex_digit(s[i+1])<0||hex_digit(s[i+2])<0)return -1;
                ch=(unsigned char)(hex_digit(s[i+1])*16+hex_digit(s[i+2]));i+=2;break;
            }
            case 'u': {
                if(i+1>=length-1||s[++i]!='{')return -1;
                uint32_t scalar=0,digits=0;
                while(++i<length-1&&s[i]!='}') {
                    int digit=hex_digit(s[i]);if(digit<0||++digits>6)return -1;
                    scalar=scalar*16+(uint32_t)digit;
                }
                if(i>=length-1||!digits||scalar>0x10FFFF||(scalar>=0xD800&&scalar<=0xDFFF))return -1;
                unsigned char utf8[4];uint32_t count;
                if(scalar<0x80){utf8[0]=(unsigned char)scalar;count=1;}
                else if(scalar<0x800){utf8[0]=(unsigned char)(0xC0|(scalar>>6));utf8[1]=(unsigned char)(0x80|(scalar&63));count=2;}
                else if(scalar<0x10000){utf8[0]=(unsigned char)(0xE0|(scalar>>12));utf8[1]=(unsigned char)(0x80|((scalar>>6)&63));utf8[2]=(unsigned char)(0x80|(scalar&63));count=3;}
                else{utf8[0]=(unsigned char)(0xF0|(scalar>>18));utf8[1]=(unsigned char)(0x80|((scalar>>12)&63));utf8[2]=(unsigned char)(0x80|((scalar>>6)&63));utf8[3]=(unsigned char)(0x80|(scalar&63));count=4;}
                memcpy(output+used,utf8,count);used+=count;
                continue;
            }
            default:return -1;
            }
        }
        output[used++]=ch;
    }
    if(!kru_utf8_valid(output,used))return -1;
    *output_length=used;return 0;
}
