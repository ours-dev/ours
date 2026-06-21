#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_TIME_H_
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_TIME_H_

#include <stddef.h>

#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1

typedef long clock_t;
typedef long time_t;

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

#ifdef __cplusplus
extern "C" {
#endif

clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm* timeptr);
time_t time(time_t* timer);
char* asctime(const struct tm* timeptr);
char* ctime(const time_t* timer);
struct tm* gmtime(const time_t* timer);
struct tm* localtime(const time_t* timer);
size_t strftime(char* __restrict str, size_t maxsize,
                const char* __restrict format,
                const struct tm* __restrict timeptr);
int timespec_get(struct timespec* ts, int base);

#ifdef __cplusplus
}
#endif

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_TIME_H_
