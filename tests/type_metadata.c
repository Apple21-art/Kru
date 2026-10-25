#include <assert.h>
#include <string.h>
#include "../include/types.h"

int main(void)
{
    const char *names[] = {"i8","u8","i16","u16","i32","u32","i64","u64","i128","u128"};
    const unsigned sizes[] = {1,1,2,2,4,4,8,8,16,16};
    for (unsigned i=0;i<10;i++) {
        KruTypeId id=kru_type_named(names[i],strlen(names[i]));
        const KruPrimitive *type=kru_primitive(id);
        assert(type && type->size==sizes[i] && type->align==sizes[i]);
        assert(type->category==KRU_INTEGER && type->is_signed==(i%2==0));
    }
    assert(kru_type_named("int",3)==KRU_I32);
    assert(kru_type_named("usize",5)==KRU_USIZE);
    assert(kru_primitive(KRU_USIZE)->size==8);
    assert(KRU_USIZE!=KRU_U64 && KRU_ISIZE!=KRU_I64);
    assert(kru_primitive(KRU_CHAR)->size==4 && KRU_CHAR!=KRU_U32);
    assert(kru_primitive(KRU_BOOL)->size==1);
    assert(kru_primitive(KRU_STR)->size==16);
    assert(kru_primitive(KRU_F32)->size==4 && kru_primitive(KRU_F64)->size==8);
    assert(kru_type_named("missing",7)==KRU_BAD);
    assert(kru_integer_fits(KRU_I8,127,0));
    assert(!kru_integer_fits(KRU_I8,128,0));
    assert(kru_integer_fits(KRU_I8,128,1));
    assert(!kru_integer_fits(KRU_I8,129,1));
    assert(kru_integer_fits(KRU_U8,255,0));
    assert(!kru_integer_fits(KRU_U8,256,0));
    assert(!kru_integer_fits(KRU_U8,1,1));
    assert(kru_integer_fits(KRU_I64,UINT64_C(9223372036854775808),1));
    assert(!kru_integer_fits(KRU_I64,UINT64_C(9223372036854775808),0));
    assert(kru_integer_fits(KRU_U64,UINT64_MAX,0));
    assert(!kru_integer_fits(KRU_BOOL,1,0));
    assert(kru_integer_fits128(KRU_U128,UINT64_MAX,UINT64_MAX,0));
    assert(!kru_integer_fits128(KRU_U128,1,0,1));
    assert(kru_integer_fits128(KRU_I128,0,UINT64_C(1)<<63,1));
    assert(!kru_integer_fits128(KRU_I128,0,UINT64_C(1)<<63,0));
    assert(!kru_integer_fits128(KRU_U64,0,1,0));
    return 0;
}
