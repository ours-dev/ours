// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// Minimal freestanding <errno.h> for libc++'s <cerrno> wrapper. The kernel
// does not use errno today; this exists so libc++ headers that include
// <cerrno> can compile. `errno` itself is declared but never defined: any TU
// that reads/writes errno will fail at link time, which is the intended loud
// failure until a real error-reporting mechanism is wired in.
#ifndef OURS_SYSTEM_LIB_LIBC_INCLUDE_ERRNO_H
#define OURS_SYSTEM_LIB_LIBC_INCLUDE_ERRNO_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define ENOTBLK 15
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ETXTBSY 26
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34

// NOLINTBEGIN(readability-identifier-naming)
// `__errno_location` and the `errno` macro are mandated verbatim by the POSIX
// / C library contract (glibc uses the same names); they cannot be renamed.
extern int *__errno_location(void);

#define errno (*__errno_location())
// NOLINTEND(readability-identifier-naming)

#ifdef __cplusplus
}
#endif

#endif // OURS_SYSTEM_LIB_LIBC_INCLUDE_ERRNO_H
