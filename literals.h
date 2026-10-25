#ifndef KRU_LITERALS_H
#define KRU_LITERALS_H
#include <stddef.h>
#include <stdint.h>
int kru_decode_char(const char* token,size_t length,uint32_t* scalar);
/* Output capacity must be at least the token length. Embedded NUL is retained. */
int kru_decode_string(const char* token,size_t length,unsigned char* output,size_t* output_length);
#endif
