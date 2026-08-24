#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_STDDEF_H_
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_STDDEF_H_

#define NULL __null
#define offsetof(type, member) __builtin_offsetof(type, member)

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef struct {
  long long __ours_ll;
  long double __ours_ld;
} max_align_t;

#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_STDDEF_H_
