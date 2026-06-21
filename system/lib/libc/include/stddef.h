#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_STDDEF_H_
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_STDDEF_H_

#define NULL __null

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_STDDEF_H_
