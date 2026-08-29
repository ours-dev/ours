#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_STRING_H_
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_STRING_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *__restrict dst, const void *__restrict src, size_t count);
void *memmove(void *dst, const void *src, size_t count);
void *memset(void *dst, int value, size_t count);
int memcmp(const void *lhs, const void *rhs, size_t count);
char *strcpy(char *__restrict dst, const char *__restrict src);
char *strncpy(char *__restrict dst, const char *__restrict src, size_t count);
char *strcat(char *__restrict dst, const char *__restrict src);
char *strncat(char *__restrict dst, const char *__restrict src, size_t count);
int strcmp(const char *lhs, const char *rhs);
int strncmp(const char *lhs, const char *rhs, size_t count);
const void *memchr(const void *ptr, int ch, size_t count);
const char *strchr(const char *str, int ch);
const char *strrchr(const char *str, int ch);
const char *strstr(const char *str, const char *substr);
size_t strlen(const char *str);

#ifdef __cplusplus
}
#endif

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_STRING_H_
