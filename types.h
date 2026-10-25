#ifndef KRU_TYPES_H
#define KRU_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* Stable primitive identity, independent of the emitter's instruction set. */
typedef enum {
    KRU_BAD=-1, KRU_I32, KRU_BOOL, KRU_STR, KRU_LEGACY_I32_PTR,
    KRU_NULL, KRU_LEGACY_HANDLE, KRU_VOID, KRU_LEGACY_ARRAY,
    KRU_I8, KRU_U8, KRU_I16, KRU_U16, KRU_U32, KRU_I64, KRU_U64,
    KRU_ISIZE, KRU_USIZE, KRU_I128, KRU_U128, KRU_F32, KRU_F64, KRU_CHAR,
    KRU_PRIMITIVE_COUNT
} KruTypeId;

typedef enum { KRU_INTEGER, KRU_FLOAT, KRU_BOOLEAN, KRU_CHARACTER,
               KRU_STRING_SLICE, KRU_OTHER } KruTypeCategory;
typedef struct {
    const char *name;
    KruTypeCategory category;
    unsigned size, align, is_signed;
} KruPrimitive;

/* Metadata currently describes the approved Linux x86-64 target. */
const KruPrimitive *kru_primitive(KruTypeId id);
KruTypeId kru_type_named(const char *name, size_t length);
int kru_type_integer(KruTypeId id);
int kru_integer_fits(KruTypeId id, uint64_t magnitude, int negative);
int kru_integer_fits128(KruTypeId id,uint64_t low,uint64_t high,int negative);

#define KRU_COMPOUND_BASE 40000
#define KRU_COMPOUND_LIMIT 2048
/* Canonical compound identity: mutability and pointee are part of the type. */
typedef enum { KRU_REFERENCE, KRU_RAW_POINTER, KRU_SLICE } KruCompoundClass;
typedef struct { KruCompoundClass category; KruTypeId element; unsigned mutable; } KruCompoundType;
typedef struct { KruCompoundType items[KRU_COMPOUND_LIMIT]; unsigned count; } KruTypeTable;
KruTypeId kru_type_compound(KruTypeTable*,KruCompoundClass,KruTypeId,unsigned);
const KruCompoundType* kru_compound(const KruTypeTable*,KruTypeId);
#endif
