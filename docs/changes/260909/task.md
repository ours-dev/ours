# task 改动记录

## 线程名没有 NUL 结尾，`ktl::Name::set()` 顺着栈往下读（2026-09-09）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::become_idle()` 里的 `Idle[N]`
- `kernel/kernel/task/dpc.cpp` — `DpcRunner::init()` 里的 `Dpc/N/Level`
- `kernel/kernel/task/idle_thread.cpp` — `IdleThread::init()` 里的 `Idle/N`

**症状**
日志里的 DPC 线程名是坏的：

```
XXX unpark |Dpc/0/Low^��| -> CPU[0]
XXX unpark |Dpc/0/Normal| -> CPU[0]
XXX unpark |Dpc/0/Highal| -> CPU[0]
```

`Dpc/0/High` 后面跟着 `al`——上一次循环写进同一段栈的 `Dpc/0/Normal` 的尾巴。

**原因**
三处都是同一个写法：

```cpp
char name[24];
ustl::fmt::format_to(name, "Dpc/{}/{}\0", cpu, LEVEL_NAMES[i]);
```

那个 `\0` 是**无效的**。格式串作为 `const char (&)[N]` 传进
`std::format_string`，转成 `string_view` 走的是 `strlen` 那条构造函数，多出来的那个 NUL
在格式化器看见它之前就被截掉了。而 `std::format_to` 本身既不限长也不补终止符——
它只写它格式化出来的那些字符。

于是 `name` 是一段**未初始化的栈内存上盖了几个字节**的结果，然后被交给
`ktl::Name::set(char const *)`：

```cpp
auto set(Char const *name) -> Status {
    ...
    return set(name, CharTraits::length(name));   // strlen，无上界
}
```

`strlen` 从 24 字节的栈缓冲区一路往下读，读到哪个字节碰巧是 0 才停。名字被截到 31 字节
是后话，越界读已经发生了。循环里三次复用同一段栈，所以第二、三次读到的是前一次的残留，
`Dpc/0/Highal` 就是这么来的。

单核下这个错同样存在，只是没人去看这些名字。是四核引导里为了追"线程被派到哪颗 CPU"
才把名字打出来，顺手撞见的。

**改法**
```cpp
char name[24];
auto const end =
    ustl::fmt::format_to_n(name, sizeof(name) - 1, "Dpc/{}/{}", cpu, LEVEL_NAMES[i]).out;
*end = '\0';
```

`format_to_n` 同时解决两件事：写入有上界（`format_to` 没有），返回的 `out` 就是终止符
该落的位置。`sizeof(name) - 1` 给终止符留位。

**参照**
无，本项目自有设计。Zircon 对应的是 `Thread::set_name()` + `snprintf`——`snprintf`
本来就是限长且自动补 NUL 的，所以那边不会犯这个错。C++ 的 `format_to` 两样都不做，
这个差别是要专门注意的。

**验证**
同一行日志（配合 `logz4.md` 里那处修改，否则数组实参根本打不出来）：

```
[4030306000 cpu0 info] XXX dpc name |Dpc/0/Low|
[4031180000 cpu0 info] XXX dpc name |Dpc/0/Normal|
[4031486000 cpu0 info] XXX dpc name |Dpc/0/High|
```

四核引导照常。

**故意没做**
- 没有给 `ktl::Name` 加一个 `format()` 方法把裸缓冲区彻底藏起来。三处同样的错说明
  这个 idiom 是缺的，但那是接口设计，不是这个 bug。记在
  `docs/repairs/260909/task.md`。
- 没有给 `ktl::Name::set(char const *)` 加长度上界。它的契约就是"给我一个 C 字符串"，
  真正该修的是不给它 C 字符串的调用方。
