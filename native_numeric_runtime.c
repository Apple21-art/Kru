/* Freestanding numeric formatting. Compiled only when maintainers regenerate
   the checked-in runtime blob; Kru never compiles this file for a user program.
   Floating values are converted from their exact binary rational value using
   bounded decimal integers, then rounded to 9/17 significant decimal digits. */
#include <stdint.h>
#include "../include/utf8.h"

static void write_bytes(const char* bytes,unsigned length)
{
    while(length) {
        register long number __asm__("rax")=1;
        register long fd __asm__("rdi")=1;
        register const char* source __asm__("rsi")=bytes;
        register unsigned long size __asm__("rdx")=length;
        __asm__ volatile("syscall" : "+a"(number) : "D"(fd),"S"(source),"d"(size) : "rcx","r11","memory");
        if(number==-4)continue;
        if(number<=0)return;
        bytes+=number;length-=(unsigned)number;
    }
}

static unsigned unsigned_decimal(char* output,unsigned value)
{
    char reverse[10];unsigned count=0;
    do {reverse[count++]=(char)('0'+value%10);value/=10;}while(value);
    for(unsigned i=0;i<count;i++)output[i]=reverse[count-1-i];
    return count;
}

static void print_float(double value,unsigned precision)
{
    union { double value; uint64_t bits; } representation={.value=value};
    uint64_t bits=representation.bits;
    unsigned sign=(unsigned)(bits>>63),binary_exponent=(unsigned)((bits>>52)&2047);
    uint64_t mantissa=bits&UINT64_C(0xFFFFFFFFFFFFF);
    char output[400];unsigned at=0;
    if(binary_exponent==2047) {
        if(mantissa) {write_bytes("nan\n",4);return;}
        if(sign)output[at++]='-';
        output[at++]='i';output[at++]='n';output[at++]='f';output[at++]='\n';write_bytes(output,at);return;
    }
    if(sign)output[at++]='-';
    if(!binary_exponent&&!mantissa) {output[at++]='0';output[at++]='\n';write_bytes(output,at);return;}
    int power;
    if(binary_exponent) {mantissa|=UINT64_C(1)<<52;power=(int)binary_exponent-1023-52;}
    else power=-1074;
    uint32_t limbs[120];unsigned count=0;
    do {limbs[count++]=(uint32_t)(mantissa%1000000000);mantissa/=1000000000;}while(mantissa);
    unsigned multiplier=power<0?5u:2u,steps=(unsigned)(power<0?-power:power);
    for(unsigned step=0;step<steps;step++) {
        uint64_t carry=0;
        for(unsigned i=0;i<count;i++) {
            uint64_t product=(uint64_t)limbs[i]*multiplier+carry;
            limbs[i]=(uint32_t)(product%1000000000);carry=product/1000000000;
        }
        if(carry)limbs[count++]=(uint32_t)carry;
    }
    char digits[1080];unsigned length=unsigned_decimal(digits,limbs[count-1]);
    for(unsigned i=count-1;i;i--) {
        unsigned word=limbs[i-1];
        for(unsigned j=9;j;j--) {digits[length+j-1]=(char)('0'+word%10);word/=10;}
        length+=9;
    }
    int exponent=(int)length-(power<0?-power:0)-1;
    unsigned kept=length<precision?length:precision;
    if(length>kept) {
        int round=digits[kept]>'5';
        if(digits[kept]=='5') {
            round=(digits[kept-1]-'0')&1;
            for(unsigned i=kept+1;i<length;i++)if(digits[i]!='0'){round=1;break;}
        }
        if(round) {
            unsigned i=kept;
            while(i&&digits[i-1]=='9')digits[--i]='0';
            if(i)digits[i-1]++;
            else {digits[0]='1';exponent++;}
        }
    }
    while(kept>1&&digits[kept-1]=='0')kept--;
    if(exponent>=-4&&exponent<(int)precision) {
        int point=exponent+1;
        if(point<=0) {
            output[at++]='0';output[at++]='.';
            for(int i=0;i<-point;i++)output[at++]='0';
            for(unsigned i=0;i<kept;i++)output[at++]=digits[i];
        } else {
            for(int i=0;i<point;i++)output[at++]=(unsigned)i<kept?digits[i]:'0';
            if((unsigned)point<kept) {
                output[at++]='.';
                for(unsigned i=(unsigned)point;i<kept;i++)output[at++]=digits[i];
            }
        }
    } else {
        output[at++]=digits[0];
        if(kept>1) {output[at++]='.';for(unsigned i=1;i<kept;i++)output[at++]=digits[i];}
        output[at++]='e';output[at++]=exponent<0?'-':'+';
        unsigned absolute=(unsigned)(exponent<0?-exponent:exponent);
        if(absolute<10)output[at++]='0';
        at+=unsigned_decimal(output+at,absolute);
    }
    output[at++]='\n';write_bytes(output,at);
}

void rt_print_f64(double value) {print_float(value,17);}
void rt_print_f32(float value) {print_float((double)value,9);}

typedef struct {uint64_t low,high;} Wide;
static Wide negate(Wide v)
{ return (Wide){0-v.low,0-v.high-(v.low!=0)}; }
static int compare(Wide a,Wide b)
{return a.high!=b.high?(a.high>b.high?1:-1):a.low!=b.low?(a.low>b.low?1:-1):0;}
static Wide subtract(Wide a,Wide b)
{return (Wide){a.low-b.low,a.high-b.high-(a.low<b.low)};}
static Wide divide(Wide a,Wide b,int remainder,int signed_type)
{
    if(!b.low&&!b.high)__builtin_trap();
    int sign_a=signed_type&&(a.high>>63),sign_b=signed_type&&(b.high>>63);
    if(signed_type&&a.high==UINT64_C(0x8000000000000000)&&!a.low&&b.high==UINT64_MAX&&b.low==UINT64_MAX)__builtin_trap();
    if(sign_a)a=negate(a);
    if(sign_b)b=negate(b);
    Wide rest={0,0},quotient={0,0};
    for(int i=127;i>=0;i--) {
        unsigned carry=(unsigned)(rest.high>>63);
        rest.high=(rest.high<<1)|(rest.low>>63);
        rest.low=(rest.low<<1)|((i>=64?a.high>>(i-64):a.low>>i)&1);
        if(carry||compare(rest,b)>=0) {
            rest=subtract(rest,b);
            if(i>=64)quotient.high|=UINT64_C(1)<<(i-64);
            else quotient.low|=UINT64_C(1)<<i;
        }
    }
    Wide result=remainder?rest:quotient;
    if(remainder?sign_a:sign_a!=sign_b)result=negate(result);
    return result;
}

Wide rt_add128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh)
{uint64_t low=al+bl;return (Wide){low,ah+bh+(low<al)};}
Wide rt_sub128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh)
{return subtract((Wide){al,ah},(Wide){bl,bh});}
Wide rt_mul128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh)
{unsigned __int128 low=(unsigned __int128)al*bl;return (Wide){(uint64_t)low,(uint64_t)(low>>64)+al*bh+ah*bl};}
Wide rt_and128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return (Wide){al&bl,ah&bh};}
Wide rt_or128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return (Wide){al|bl,ah|bh};}
Wide rt_xor128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return (Wide){al^bl,ah^bh};}
Wide rt_udiv128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return divide((Wide){al,ah},(Wide){bl,bh},0,0);}
Wide rt_sdiv128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return divide((Wide){al,ah},(Wide){bl,bh},0,1);}
Wide rt_umod128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return divide((Wide){al,ah},(Wide){bl,bh},1,0);}
Wide rt_smod128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh) {return divide((Wide){al,ah},(Wide){bl,bh},1,1);}
Wide rt_shl128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh)
{
    if(bh||bl>=128)__builtin_trap();
    if(!bl)return (Wide){al,ah};
    if(bl>=64)return (Wide){0,al<<(bl-64)};
    return (Wide){al<<bl,(ah<<bl)|(al>>(64-bl))};
}
Wide rt_ushr128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh)
{
    if(bh||bl>=128)__builtin_trap();
    if(!bl)return (Wide){al,ah};
    if(bl>=64)return (Wide){ah>>(bl-64),0};
    return (Wide){(al>>bl)|(ah<<(64-bl)),ah>>bl};
}
Wide rt_sshr128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh)
{
    if(bh||bl>=128)__builtin_trap();
    if(!bl)return (Wide){al,ah};
    if(bl>=64)return (Wide){(uint64_t)((int64_t)ah>>(bl-64)),(uint64_t)((int64_t)ah>>63)};
    return (Wide){(al>>bl)|(ah<<(64-bl)),(uint64_t)((int64_t)ah>>bl)};
}
int64_t rt_cmp128(uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh,uint64_t signed_type)
{
    if(signed_type&&(int64_t)ah!=(int64_t)bh)return (int64_t)ah>(int64_t)bh?1:-1;
    return compare((Wide){al,ah},(Wide){bl,bh});
}
static void print_wide(Wide value,int signed_type)
{
    char buffer[42];unsigned at=41;buffer[at]='\n';
    int negative=signed_type&&(value.high>>63);
    if(negative)value=negate(value);
    do {
        Wide next=divide(value,(Wide){10,0},0,0);
        unsigned digit=(unsigned)(value.low-next.low*10); /* High product is irrelevant to remainder <10. */
        buffer[--at]=(char)('0'+digit);value=next;
    }while(value.low||value.high);
    if(negative)buffer[--at]='-';
    write_bytes(buffer+at,42-at);
}
void rt_print_i128(uint64_t low,uint64_t high) {print_wide((Wide){low,high},1);}
void rt_print_u128(uint64_t low,uint64_t high) {print_wide((Wide){low,high},0);}
void rt_print_char(uint32_t scalar)
{
    char bytes[5];unsigned size;
    if(scalar<0x80){bytes[0]=(char)scalar;size=1;}
    else if(scalar<0x800){bytes[0]=(char)(0xC0|(scalar>>6));bytes[1]=(char)(0x80|(scalar&63));size=2;}
    else if(scalar<0x10000){bytes[0]=(char)(0xE0|(scalar>>12));bytes[1]=(char)(0x80|((scalar>>6)&63));bytes[2]=(char)(0x80|(scalar&63));size=3;}
    else {bytes[0]=(char)(0xF0|(scalar>>18));bytes[1]=(char)(0x80|((scalar>>12)&63));bytes[2]=(char)(0x80|((scalar>>6)&63));bytes[3]=(char)(0x80|(scalar&63));size=4;}
    bytes[size++]='\n';write_bytes(bytes,size);
}

/* Intrinsics used by the source-distributed core I/O module. */
static long syscall3(long number,long a,long b,long c)
{
    __asm__ volatile("syscall" : "+a"(number) : "D"(a),"S"(b),"d"(c) : "rcx","r11","memory");
    return number;
}
long rt_sys_open(const char* path,long write)
{
    long result;
    do {result=syscall3(2,(long)path,write?577:0,420);}while(result==-4);
    return result;
}
long rt_sys_close(long fd) { return syscall3(3,fd,0,0); }
long rt_sys_write(long fd,const char* text)
{
    unsigned long length=0;while(text[length])length++;
    unsigned long done=0;
    while(done<length) {
        long count=syscall3(1,fd,(long)(text+done),(long)(length-done));
        if(count==-4)continue;
        if(count<=0)return count?count:-5;
        done+=(unsigned long)count;
    }
    return (long)done;
}
extern void* rt_mem_alloc(int size);
extern void* rt_mem_realloc(void* pointer,int size);
extern void rt_mem_free(void* pointer);
const char* rt_checked_read(long fd,long* status)
{
    unsigned size=4096,used=0;char* buffer=rt_mem_alloc((int)size+1);
    if(!buffer){*status=-12;return "";}
    for(;;) {
        if(used==size) {
            if(size>=1u<<29){*status=-27;rt_mem_free(buffer);return "";}
            char* grown=rt_mem_realloc(buffer,(int)(size*2+1));
            if(!grown){*status=-12;rt_mem_free(buffer);return "";}
            buffer=grown;size*=2;
        }
        long count=syscall3(0,fd,(long)(buffer+used),(long)(size-used));
        if(count==-4)continue;
        if(count<0){*status=count;rt_mem_free(buffer);return "";}
        if(!count){buffer[used]=0;*status=0;return buffer;}
        used+=(unsigned)count;
    }
}

typedef struct {char* data;unsigned long length,capacity;} NativeStringBuffer;
void rt_string_push(NativeStringBuffer* buffer,const char* text)
{
    unsigned long added=0;if(text)while(text[added])added++;
    if(added>2147483646UL-buffer->length)__builtin_trap();
    unsigned long needed=buffer->length+added;
    unsigned long offset=0;int alias=buffer->data&&(uintptr_t)text>=(uintptr_t)buffer->data&&(uintptr_t)text<=(uintptr_t)buffer->data+buffer->length;
    if(alias)offset=(unsigned long)(text-buffer->data);
    if(needed+1>buffer->capacity) {
        unsigned long capacity=buffer->capacity?buffer->capacity:32;
        while(capacity<needed+1)capacity=capacity>1073741823UL?needed+1:capacity*2;
        char* data=buffer->data?rt_mem_realloc(buffer->data,(int)capacity):rt_mem_alloc((int)capacity);
        if(!data)__builtin_trap();
        buffer->data=data;buffer->capacity=capacity;
        if(alias)text=data+offset;
    }
    for(unsigned long i=0;i<added;i++)buffer->data[buffer->length+i]=text[i];
    buffer->length=needed;buffer->data[needed]=0;
}
const char* const* rt_string_view(NativeStringBuffer* buffer)
{
    return (const char* const*)&buffer->data;
}
void rt_string_drop(NativeStringBuffer* buffer)
{
    if(buffer->data)rt_mem_free(buffer->data);
    buffer->data=0;buffer->length=buffer->capacity=0;
}
int32_t rt_read_int(const char* prompt)
{
    if(prompt)rt_sys_write(1,prompt);
    uint64_t magnitude=0;int sign=1,started=0,valid=1,digit=0;
    for(;;) {
        unsigned char ch=0;long status=syscall3(0,0,(long)&ch,1);
        if(status==-4)continue;
        if(status<=0||ch=='\n')break;
        if(!started&&(ch==' '||ch=='\t'||ch=='\r'))continue;
        if(!started&&(ch=='-'||ch=='+')) {sign=ch=='-'?-1:1;started=1;continue;}
        started=1;
        if(ch>='0'&&ch<='9') {
            digit=1;magnitude=magnitude*10+(unsigned)(ch-'0');
            if(magnitude>2147483648ULL){valid=0;magnitude=2147483648ULL;}
        } else {valid=0;}
    }
    if(!valid||!digit||magnitude>(sign<0?2147483648ULL:2147483647ULL))return 0;
    return sign<0?(int32_t)(0u-(uint32_t)magnitude):(int32_t)magnitude;
}

/* Checked wide conversions use IEEE bit fields, so no host compiler helper
   library or host floating-point rounding mode is part of their semantics. */
uint64_t rt_int128_to_float(uint64_t low,uint64_t high,uint64_t is_signed,uint64_t width)
{
    typedef unsigned __int128 Wide;
    Wide magnitude=((Wide)high<<64)|low;
    unsigned negative=is_signed&&(high>>63);
    if(negative)magnitude=0-magnitude;
    if(!magnitude)return 0;
    unsigned precision=width==32?24:53,bias=width==32?127:1023;
    unsigned exponent=0;Wide scan=magnitude;
    while(scan>>1){scan>>=1;exponent++;}
    Wide significant;
    if(exponent<precision)significant=magnitude<<(precision-1-exponent);
    else {
        unsigned shift=exponent-(precision-1);
        significant=magnitude>>shift;
        Wide remainder=magnitude&(((Wide)1<<shift)-1),half=(Wide)1<<(shift-1);
        if(remainder>half||(remainder==half&&(significant&1)))significant++;
        if(significant==((Wide)1<<precision)){significant>>=1;exponent++;}
    }
    if(width==32&&exponent>127)__builtin_trap();
    uint64_t fraction=(uint64_t)significant&(((uint64_t)1<<(precision-1))-1);
    return ((uint64_t)negative<<(width-1))|((uint64_t)(exponent+bias)<<(precision-1))|fraction;
}
unsigned __int128 rt_float_to_int128(uint64_t bits,uint64_t width,uint64_t is_signed)
{
    typedef unsigned __int128 Wide;
    unsigned fraction_bits=width==32?23:52,bias=width==32?127:1023;
    unsigned exponent_mask=width==32?255:2047;
    unsigned exponent=(unsigned)(bits>>fraction_bits)&exponent_mask;
    unsigned negative=(unsigned)(bits>>(width-1))&1;
    if(exponent==exponent_mask)__builtin_trap();
    int power=(int)exponent-(int)bias;
    if(power<0)return 0; /* Truncation permits negative fractions above -1. */
    if(power>127)__builtin_trap();
    Wide magnitude=(bits&(((uint64_t)1<<fraction_bits)-1))|((uint64_t)1<<fraction_bits);
    if(power>=(int)fraction_bits)magnitude<<=power-fraction_bits;
    else magnitude>>=fraction_bits-power;
    if(is_signed) {
        Wide bound=(Wide)1<<127;
        if(negative?magnitude>bound:magnitude>=bound)__builtin_trap();
    } else if(negative&&magnitude)__builtin_trap();
    return negative?0-magnitude:magnitude;
}

/* Native str is a borrowed byte span. Private Kru aggregate calls pass the
   descriptor address; results receive caller-owned descriptor storage. */
typedef struct {const char* data;unsigned long length;} NativeStringSlice;
long rt_slice_sys_write(long fd,const NativeStringSlice* text)
{
    unsigned long done=0;
    while(done<text->length) {
        long count=syscall3(1,fd,(long)(text->data+done),(long)(text->length-done));
        if(count==-4)continue;
        if(count<=0)return count?count:-5;
        done+=(unsigned long)count;
    }
    return (long)done;
}
void rt_slice_print(const NativeStringSlice* text)
{
    rt_slice_sys_write(1,text);
    NativeStringSlice newline={"\n",1};rt_slice_sys_write(1,&newline);
}
long rt_slice_len(const NativeStringSlice* text){return (long)text->length;}
int rt_slice_eq(const NativeStringSlice* a,const NativeStringSlice* b)
{
    if(a->length!=b->length)return 0;
    for(unsigned long i=0;i<a->length;i++)if(a->data[i]!=b->data[i])return 0;
    return 1;
}
int rt_slice_index(const NativeStringSlice* text,int index)
{
    if(index<0||(unsigned long)index>=text->length)__builtin_trap();
    return (unsigned char)text->data[index];
}
NativeStringSlice* rt_slice_concat(NativeStringSlice* out,const NativeStringSlice* a,const NativeStringSlice* b)
{
    if(a->length>2147483646UL||b->length>2147483646UL-a->length)__builtin_trap();
    unsigned long size=a->length+b->length;char* data=rt_mem_alloc((int)size+1);
    if(!data)__builtin_trap();
    for(unsigned long i=0;i<a->length;i++)data[i]=a->data[i];
    for(unsigned long i=0;i<b->length;i++)data[a->length+i]=b->data[i];
    data[size]=0;*out=(NativeStringSlice){data,size};return out;
}
long rt_slice_sys_open(const NativeStringSlice* path,long write)
{
    if(path->length>2147483646UL)return -36;
    for(unsigned long i=0;i<path->length;i++)if(!path->data[i])return -22;
    char* terminated=rt_mem_alloc((int)path->length+1);if(!terminated)return -12;
    for(unsigned long i=0;i<path->length;i++)terminated[i]=path->data[i];
    terminated[path->length]=0;long result=rt_sys_open(terminated,write);rt_mem_free(terminated);return result;
}
long rt_slice_file_open(const NativeStringSlice* path){long fd=rt_slice_sys_open(path,0);return fd<0?-1:fd;}
long rt_slice_file_open_write(const NativeStringSlice* path){long fd=rt_slice_sys_open(path,1);return fd<0?-1:fd;}
NativeStringSlice* rt_slice_checked_read(NativeStringSlice* out,long fd,long* status)
{
    unsigned size=4096,used=0;char* buffer=rt_mem_alloc((int)size+1);
    *out=(NativeStringSlice){"",0};
    if(!buffer){*status=-12;return out;}
    for(;;) {
        if(used==size) {
            if(size>=1u<<29){*status=-27;rt_mem_free(buffer);return out;}
            char* grown=rt_mem_realloc(buffer,(int)(size*2+1));
            if(!grown){*status=-12;rt_mem_free(buffer);return out;}
            buffer=grown;size*=2;
        }
        long count=syscall3(0,fd,(long)(buffer+used),(long)(size-used));
        if(count==-4)continue;
        if(count<0){*status=count;rt_mem_free(buffer);return out;}
        if(!count){
            if(!kru_utf8_valid((const unsigned char*)buffer,used)){*status=-84;rt_mem_free(buffer);return out;}
            buffer[used]=0;*status=0;*out=(NativeStringSlice){buffer,used};return out;
        }
        used+=(unsigned)count;
    }
}
/* Transfer the read allocation into an owning String; no copy or extra
   allocation. The null error result is safe for ordinary String cleanup. */
NativeStringBuffer* rt_owned_file_read(NativeStringBuffer* out,long fd,long* status)
{
    NativeStringSlice text;
    rt_slice_checked_read(&text,fd,status);
    *out=(NativeStringBuffer){0,0,0};
    if(*status==0)*out=(NativeStringBuffer){(char*)text.data,text.length,text.length+1};
    return out;
}
NativeStringSlice* rt_slice_file_read(NativeStringSlice* out,long fd)
{
    long status;rt_slice_checked_read(out,fd,&status);
    /* Even the legacy empty-on-error result has releasable backing storage. */
    if(status) {
        char* empty=rt_mem_alloc(1);if(!empty)__builtin_trap();
        *empty=0;*out=(NativeStringSlice){empty,0};
    }
    return out;
}
void rt_slice_free(const NativeStringSlice* text)
{
    rt_mem_free((void*)text->data);
}
extern const char* rt_args_get(int index);
NativeStringSlice* rt_slice_args_get(NativeStringSlice* out,int index)
{
    const char* data=rt_args_get(index);unsigned long length=0;
    while(data[length])length++;
    if(!kru_utf8_valid((const unsigned char*)data,length))__builtin_trap();
    *out=(NativeStringSlice){data,length};return out;
}
int rt_slice_read_int(const NativeStringSlice* prompt)
{rt_slice_sys_write(1,prompt);return rt_read_int(0);}
void rt_slice_string_push(NativeStringBuffer* buffer,const NativeStringSlice* text)
{
    unsigned long added=text->length;
    if(added>2147483646UL-buffer->length)__builtin_trap();
    unsigned long needed=buffer->length+added;
    const char* source=text->data;
    unsigned long offset=0;int alias=buffer->data&&(uintptr_t)source>=(uintptr_t)buffer->data&&(uintptr_t)source<=(uintptr_t)buffer->data+buffer->length;
    if(alias)offset=(unsigned long)(source-buffer->data);
    if(needed+1>buffer->capacity) {
        unsigned long capacity=buffer->capacity?buffer->capacity:32;
        while(capacity<needed+1)capacity=capacity>1073741823UL?needed+1:capacity*2;
        char* data=buffer->data?rt_mem_realloc(buffer->data,(int)capacity):rt_mem_alloc((int)capacity);
        if(!data)__builtin_trap();
        buffer->data=data;buffer->capacity=capacity;
        if(alias)source=data+offset;
    }
    for(unsigned long i=0;i<added;i++)buffer->data[buffer->length+i]=source[i];
    buffer->length=needed;buffer->data[needed]=0;
}

const NativeStringSlice* rt_slice_validate(const NativeStringSlice* value)
{
    if(!kru_utf8_valid((const unsigned char*)value->data,value->length))__builtin_trap();
    return value;
}
