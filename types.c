#include <string.h>
#include "../include/types.h"

static const KruPrimitive primitives[KRU_PRIMITIVE_COUNT] = {
    [KRU_I32]={"i32",KRU_INTEGER,4,4,1},
    [KRU_BOOL]={"bool",KRU_BOOLEAN,1,1,0},
    [KRU_STR]={"str",KRU_STRING_SLICE,16,8,0},
    [KRU_LEGACY_I32_PTR]={"*i32",KRU_OTHER,8,8,0},
    [KRU_NULL]={"null",KRU_OTHER,0,1,0},
    [KRU_LEGACY_HANDLE]={"<bootstrap-file>",KRU_OTHER,8,8,1},
    [KRU_VOID]={"void",KRU_OTHER,0,1,0},
    [KRU_LEGACY_ARRAY]={"<bootstrap-array>",KRU_OTHER,0,1,0},
    [KRU_I8]={"i8",KRU_INTEGER,1,1,1},
    [KRU_U8]={"u8",KRU_INTEGER,1,1,0},
    [KRU_I16]={"i16",KRU_INTEGER,2,2,1},
    [KRU_U16]={"u16",KRU_INTEGER,2,2,0},
    [KRU_U32]={"u32",KRU_INTEGER,4,4,0},
    [KRU_I64]={"i64",KRU_INTEGER,8,8,1},
    [KRU_U64]={"u64",KRU_INTEGER,8,8,0},
    [KRU_ISIZE]={"isize",KRU_INTEGER,8,8,1},
    [KRU_USIZE]={"usize",KRU_INTEGER,8,8,0},
    [KRU_I128]={"i128",KRU_INTEGER,16,16,1},
    [KRU_U128]={"u128",KRU_INTEGER,16,16,0},
    [KRU_F32]={"f32",KRU_FLOAT,4,4,1},
    [KRU_F64]={"f64",KRU_FLOAT,8,8,1},
    [KRU_CHAR]={"char",KRU_CHARACTER,4,4,0},
};

const KruPrimitive *kru_primitive(KruTypeId id)
{
    return id>=0 && id<KRU_PRIMITIVE_COUNT ? &primitives[id] : NULL;
}

KruTypeId kru_type_named(const char *name,size_t length)
{
    if(!name)return KRU_BAD;
    if(length==3&&!memcmp(name,"int",3))return KRU_I32;
    for(unsigned i=0;i<KRU_PRIMITIVE_COUNT;i++)
        if(strlen(primitives[i].name)==length&&!memcmp(name,primitives[i].name,length))
            return (KruTypeId)i;
    return KRU_BAD;
}

int kru_type_integer(KruTypeId id)
{
    const KruPrimitive *type=kru_primitive(id);
    return type && type->category==KRU_INTEGER;
}

int kru_integer_fits(KruTypeId id,uint64_t magnitude,int negative)
{
    const KruPrimitive *type=kru_primitive(id);
    if(!type||type->category!=KRU_INTEGER)return 0;
    if(!type->is_signed&&negative&&magnitude)return 0;
    unsigned bits=type->size*8;
    if(bits>64)return 1; /* This API receives only a 64-bit magnitude. */
    if(type->is_signed) {
        uint64_t bound=UINT64_C(1)<<(bits-1);
        return magnitude<=bound-(negative?0u:1u);
    }
    return bits==64 || magnitude<=(UINT64_C(1)<<bits)-1;
}

int kru_integer_fits128(KruTypeId id,uint64_t low,uint64_t high,int negative)
{
    const KruPrimitive* type=kru_primitive(id);
    if(!type||type->category!=KRU_INTEGER)return 0;
    if(type->size<16)return !high&&kru_integer_fits(id,low,negative);
    if(!type->is_signed)return !negative||(!low&&!high);
    uint64_t boundary=UINT64_C(1)<<63;
    return high<boundary||(negative&&high==boundary&&!low);
}

KruTypeId kru_type_compound(KruTypeTable* table,KruCompoundClass category,KruTypeId element,unsigned mutable)
{
    if(element==KRU_BAD||element==KRU_VOID)return KRU_BAD;
    mutable=!!mutable;
    for(unsigned i=0;i<table->count;i++) {
        KruCompoundType* t=&table->items[i];
        if(t->category==category&&t->element==element&&t->mutable==mutable)return (KruTypeId)(KRU_COMPOUND_BASE+i);
    }
    if(table->count==KRU_COMPOUND_LIMIT)return KRU_BAD;
    unsigned i=table->count++;
    table->items[i]=(KruCompoundType){category,element,mutable};
    return (KruTypeId)(KRU_COMPOUND_BASE+i);
}
const KruCompoundType* kru_compound(const KruTypeTable* table,KruTypeId id)
{
    unsigned i=(unsigned)((int)id-KRU_COMPOUND_BASE);
    return i<table->count?&table->items[i]:0;
}
