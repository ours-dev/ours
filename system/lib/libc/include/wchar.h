#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_WCHAR_H_
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_WCHAR_H_

#include <stddef.h>

#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif

typedef unsigned int wint_t;
typedef struct {
    unsigned int __ours_state;
} mbstate_t;

#define WEOF ((wint_t) - 1)

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_WCHAR_H_
