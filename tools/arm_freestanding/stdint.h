#ifndef _FREESTANDING_STDINT_H
#define _FREESTANDING_STDINT_H
typedef signed char int8_t; typedef unsigned char uint8_t;
typedef short int16_t; typedef unsigned short uint16_t;
typedef int int32_t; typedef unsigned int uint32_t;
typedef long long int64_t; typedef unsigned long long uint64_t;
typedef __INTPTR_TYPE__ intptr_t; typedef __UINTPTR_TYPE__ uintptr_t;
#define INT8_MAX 127
#define INT16_MAX 32767
#define INT32_MAX 2147483647
#define UINT8_MAX 255u
#define UINT16_MAX 65535u
#define UINT32_MAX 4294967295u
#define INT32_MIN (-2147483647-1)
#define INT64_MAX 9223372036854775807LL
#define INT64_MIN (-9223372036854775807LL-1)
#endif
