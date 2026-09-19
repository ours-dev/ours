# libc 待修项（2026-09-19）

## 1. freestanding `<string.h>` 的查找类函数返回 const 指针，与 C 不一致

`system/lib/libc/include/string.h` 声明：

```c
const void *memchr(const void *ptr, int ch, size_t count);
const char *strchr(const char *str, int ch);
const char *strrchr(const char *str, int ch);
const char *strstr(const char *str, const char *substr);
```

C 标准里这四个都返回**非 const** 指针（`void *memchr(const void *, int, size_t)`
等）——C 靠丢掉 const 来让调用方既能查只读缓冲又能拿到可写指针，C++ 则用一对重载
解决（libc++ `<cstring>` 里 `using ::memchr;` 之后再叠 const/非 const 重载）。

影响：定义侧被迫跟着写成 `void const *` 并且不能标 `extern "C"`（标了就是
conflicting types）；真正按 C 原型链接进来的第三方对象文件会对不上；libc++ 的
`<cstring>` 重载推导也可能跟着走偏。

改法：把这四个声明改成 C 的原型，定义侧同步加 `extern "C"`，返回处用
`const_cast`。要一起过一遍 phys 侧（它也吃这个头）。

## 2. `<string.h>` 里有一批只声明没定义的函数

`strcpy`、`strncpy`、`strcat`、`strncat`、`strcmp`、`strncmp`、`strchr`、
`strrchr`、`strstr` 在 `kernel/lib/libc/string.cpp` 里都没有实现，全树也搜不到别处
的定义。现在没人用，一旦有人（或某个 libc++ 头的某条路径）用上，是链接期才会暴露
的错误。

改法：要么补上实现，要么把声明删掉——让头文件描述的东西和实际提供的东西一致。
