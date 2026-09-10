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

## 名字只在一处成形：`ktl::format_name()`，空闲线程也只命名一次（2026-09-10）

**文件**
- `kernel/lib/ktl/include/ktl/name.hpp` — 新增 `format_name()`
- `kernel/kernel/task/thread.cpp` — `become_idle()` 不再重命名
- `kernel/kernel/task/idle_thread.cpp`、`kernel/kernel/task/dpc.cpp` — 改用 helper

**症状**
不是 bug。上一节把三处 `format_to` 都改对了，但改法是**同样的四行抄三遍**，
其中两处的注释还写着"见 `Thread::Current::become_idle()` 里的说明"——
一个跨文件的、随时会因为改名而失效的引用。

**原因**

**1. 同一个不变量抄了三份。** `sizeof(buf) - 1`、`.out`、`*end = '\0'`——
三处各自维护。第四个命名点出现时，抄错的是哪一份全看运气；而抄错的后果是
`ktl::Name::set()` 拿 `strlen` 越界读栈，不是编译错误。

**2. 空闲线程被命名了两次。** `IdleThread::init()` 起名 `"Idle/{}"`，
`become_idle()` 又起一次 `"Idle[{}]"`。第二次**总是**覆盖第一次
（`become_idle()` 就跑在那个 idler 上，函数开头的断言明说了），
所以第一个名字从来没有被人看见过，而同一个约定有了两种拼法。

真正该命名的地方是 `IdleThread::init()`：它是唯一能给**还没起来的 CPU** 的
idler 命名的地方（`task::init()` 在 BSP 上遍历 `cpu_possible_mask()` 建所有 idler），
`become_idle()` 只能给自己命名。

**改法**
```cpp
// ktl/name.hpp —— "为什么"只写这一遍
template <usize N, typename... Args>
auto format_name(char (&buffer)[N], std::format_string<Args...> fmt, Args &&...args)
    -> char const * {
    static_assert(N > 1u, "A name buffer must hold at least one character and a NUL");
    auto const end = std::format_to_n(buffer, N - 1, fmt, static_cast<Args &&>(args)...).out;
    *end = '\0';
    return buffer;
}
```
```cpp
char buffer[24];
auto const name = ktl::format_name(buffer, "Dpc/{}/{}", cpu, LEVEL_NAMES[i]);
```

放在 `ktl::Name` 旁边，因为它就是"怎么造一个能交给 `Name` 的字符串"。
缓冲区长度由 `char (&)[N]` 带进来，调用方连 `sizeof` 都不用写。

`become_idle()` 里那一段整个删掉，只留一句注释说明为什么这里**不**命名。

**参照**
无，本项目自有设计。

**验证**
四核引导：cpu0 8589 / cpu1 601 / cpu2 83915 / cpu3 83848，无断言失败。
DPC 线程名实测为 `Dpc/0/Low`、`Dpc/0/Normal`、`Dpc/0/High`（上一节验证过）。

**故意没做**
没有给 `Thread::Current::set_name()` 加一个收长度的重载，也没有让
`Thread::spawn_etc()` / `DpcQueue::start()` 收 `string_view`。那要动三个签名，
而 `format_name()` 已经把重复消掉了；等哪天名字真的需要带长度传递再说。
