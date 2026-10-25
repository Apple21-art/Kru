#ifndef KRU_NATIVE_ABI_H
#define KRU_NATIVE_ABI_H
#include "types.h"
typedef enum { KRU_ABI_GP, KRU_ABI_SSE, KRU_ABI_STACK } KruAbiClass;
typedef struct { KruAbiClass kind; unsigned index,words; } KruAbiLocation;
/* Scalar System V classification. Aggregate arguments are still passed by the
   migration adapter's private copy convention, not by the public struct ABI. */
unsigned kru_x64_scalar_arguments(const KruTypeId* types,unsigned count,KruAbiLocation* locations);
#endif
