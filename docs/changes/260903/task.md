# kernel/task（线程）— 改动记录

格式见 `docs/changes/TEMPLATE.md`。调度器那一层的记录在 `scheduler.md`，这里只放 `Thread` 本身。

---

## 1. `Thread::spawn` 模板的实参顺序反了，一直在调用自己（2026-09-03）

**文件**
- `kernel/include/ours/task/thread.hpp` — 两个 `spawn` 模板体里回传给非模板重载的实参顺序。

**症状**

`task::init()` 跑完之后，`start_kernel()` 一 spawn "laborer" 就停：

```
[... cpu0 error] operator new
```

没有别的信息。内核的全局 `operator new` 是 `kernel/lib/libc/cpp_dependencies.cpp` 里的一个
panic 桩（`ours::panic(__func__)`），所以任何走到它的分配都长这样。

**原因**

非模板重载的声明是「名字、例程、优先级」：

```cpp
static auto spawn(char const *, StartRoutine, usize priority) -> Self *;
static auto spawn(char const *, StartRoutine, sched::BaseProfile const &) -> Self *;
```

模板重载自己的参数表却是「名字、优先级、例程、参数包」，而模板体把 bind 好的
`entry_point` **按自己的顺序**回传：

```cpp
StartRoutine entry_point = ustl::function::bind(entry, ustl::forward<Args>(args)...);
return Self::spawn(name, priority, entry_point);   // (name, usize, std::function)
```

`(char const *, usize, std::function<int()>)` 对非模板重载的 `(char const *, StartRoutine, usize)`
不可行——第二个位置要 `StartRoutine`，给的是 `usize`。唯一可行的候选是模板自己，`F` 推导成
`StartRoutine`。于是它再 bind 一次：`std::function` 套 `std::bind` 套 `std::function`，
然后再调自己，无限递归。

它没真的递归下去，是因为**第二层就分配了**。用宿主 libc++ 复现（`scratchpad/overload.cpp`）：

| 对象 | 大小 |
|---|---|
| `std::function<int()>` 的 SBO 容量 | 32 |
| `std::bind(函数指针)` | 16 —— 塞得下 |
| `std::bind(std::function<int()>)` | 40 —— 塞不下 |

第一层 bind 一个裸函数指针，16 字节，`std::function` 内联持有；第二层 bind 的是上一层的
`std::function`，40 字节，超出小缓冲，`std::function` 去堆上要空间，撞上 panic 桩。
lldb 的栈正好是这个形状：`spawn<int(*)()>` 内联调用 `spawn<std::function<int()>>`，
后者在 `function.h:464` 调 `operator new(40)`。

同一个复现在两种写法下跑：错的写法模板深度打到上限、从未到达非模板重载；对的写法深度 1、
到达。

这类错误的根子是两个重载家族的参数顺序不一致：非模板是「例程在前」，模板是「优先级在前」。
只要顺序一致，回传就不可能选错。顺序不一致本身没改（它牵扯所有调用点），这里只修回传。

**改法**

两个模板体都改成按非模板重载声明的顺序回传：

```cpp
return Self::spawn(name, entry_point, priority);
return Self::spawn(name, entry_point, profile);
```

`(char const *, StartRoutine, usize)` 对非模板是精确匹配；两个模板都不可行（第二个位置
既不是 `usize` 也不是 `BaseProfile`），所以不再有歧义。

**参照**

无，本项目自有设计。这是一个纯 C++ 重载决议问题，不是操作系统设计问题。

**验证**

- 构建通过，`clang-format` 无改动，`clang-tidy` 无新告警。
- 宿主 libc++ 下的独立复现（见上表）证实了机制：错误写法递归、正确写法一次到达。
- **QEMU 未实测到这一步**：树里 mm 的在途改动（`VmArea::reserve()` → `VmMapping::protect()`
  在 `init_vmm()` 里撞 canary，见 `docs/repairs/260903/mm.md` 或对应会话）让启动停在
  `init_vmm()`，走不到 `task::init()`，更走不到 spawn "laborer"。这条改动的证据是上面
  的复现和 lldb 栈，不是启动日志。等 mm 修好要回来补一次实测。

**故意没做**

- 没有统一两个重载家族的参数顺序。那是接口改动，动所有 `Thread::spawn` 调用点，超出本条。
- 没有处理「`Thread::spawn` 绑定的参数一多就会超出 SBO 撞 panic 桩」这个更深的问题。
  `StartRoutine` 是 `std::function`，而这个内核没有可用的全局 `operator new`，所以任何
  绑定超过 ~2 个指针大小参数的 spawn 都会在这里死。已记进 `docs/repairs/260903/task.md`。
