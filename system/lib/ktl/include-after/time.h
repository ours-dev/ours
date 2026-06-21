#ifndef OURS_SYSTEM_LIB_KTL_INCLUDE_AFTER_TIME_H_
#define OURS_SYSTEM_LIB_KTL_INCLUDE_AFTER_TIME_H_

typedef long time_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

#endif // OURS_SYSTEM_LIB_KTL_INCLUDE_AFTER_TIME_H_
