#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_STDIO_H_
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_STDIO_H_

#include <stdarg.h>
#include <stddef.h>

typedef struct __ours_file FILE;
typedef long fpos_t;

#define EOF (-1)

#ifdef __cplusplus
extern "C" {
#endif

int fclose(FILE *stream);
int fflush(FILE *stream);
void setbuf(FILE *__restrict stream, char *__restrict buffer);
int setvbuf(FILE *__restrict stream, char *__restrict buffer, int mode, size_t size);
int fprintf(FILE *__restrict stream, const char *__restrict format, ...);
int fscanf(FILE *__restrict stream, const char *__restrict format, ...);
int snprintf(char *__restrict buffer, size_t size, const char *__restrict format, ...);
int sprintf(char *__restrict buffer, const char *__restrict format, ...);
int sscanf(const char *__restrict buffer, const char *__restrict format, ...);
int vfprintf(FILE *__restrict stream, const char *__restrict format, va_list args);
int vfscanf(FILE *__restrict stream, const char *__restrict format, va_list args);
int vsscanf(const char *__restrict buffer, const char *__restrict format, va_list args);
int vsnprintf(char *__restrict buffer, size_t size, const char *__restrict format, va_list args);
int vsprintf(char *__restrict buffer, const char *__restrict format, va_list args);
int fgetc(FILE *stream);
char *fgets(char *__restrict buffer, int count, FILE *__restrict stream);
int fputc(int ch, FILE *stream);
int fputs(const char *__restrict string, FILE *__restrict stream);
int getc(FILE *stream);
int putc(int ch, FILE *stream);
int ungetc(int ch, FILE *stream);
size_t fread(void *__restrict buffer, size_t size, size_t count, FILE *__restrict stream);
size_t fwrite(const void *__restrict buffer, size_t size, size_t count, FILE *__restrict stream);
int fgetpos(FILE *__restrict stream, fpos_t *__restrict position);
int fseek(FILE *stream, long offset, int origin);
int fsetpos(FILE *stream, const fpos_t *position);
long ftell(FILE *stream);
void rewind(FILE *stream);
void clearerr(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void perror(const char *string);

FILE *fopen(const char *__restrict filename, const char *__restrict mode);
FILE *freopen(const char *__restrict filename, const char *__restrict mode,
              FILE *__restrict stream);
int remove(const char *filename);
int rename(const char *old_name, const char *new_name);
FILE *tmpfile(void);
char *tmpnam(char *string);

int getchar(void);
int scanf(const char *__restrict format, ...);
int vscanf(const char *__restrict format, va_list args);
int printf(const char *__restrict format, ...);
int putchar(int ch);
int puts(const char *string);
int vprintf(const char *__restrict format, va_list args);

#ifdef __cplusplus
}
#endif

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_STDIO_H_
