# ou

每个系统调用在用户态的具名入口。ring 3 的程序经它调用内核，不直接碰原始 ABI。

## 分层

```
程序         ou::debug_write("...")          ← 具名、有类型、知道 ABI 的限制
ou           ours_debug_write(ptr, len)      ← 原始 ABI，<ours/vdso.hpp> 声明
vDSO         mov $n, %eax; syscall           ← 唯一允许陷入内核的代码
内核         syscall::debug_write(ptr, len)
```

程序与内核之间只有一处知道「原始 ABI 长什么样」：本库。每个入口都在这里有名字，哪怕它
只是转发（`ou::command_list`）——程序应该到这里找「怎么调用某个系统调用」，而不是绕过
它直接调 `ours_*`。

This is the rule, not yet the tree: `examples/shell` still includes `<ours/vdso.hpp>` and calls `ours_*` directly at 18 sites, which step 5 of `.agents/tasks/system-abi-surface` moves to `ou`.

两个调用约定由本库而不是调用方负责：

- **ABI 限制**。`ou::debug_write` 按 `ours::MAX_DEBUG_WRITE` 切分，常量来自两边共用的
  `<ours/syscall.hpp>`，不需要调用方照着内核的数字手抄一份。
- **长度与指针配对**。所有入口收 `ou::Str` 或定长数组，长度不会配错指针。**Corrected 2026-10-02:** this holds for text only: those entries take an `ou::Str`, and `ou::command_list` has an array overload; byte buffers and handle arrays (`channel_*`, `vmo_read`, `vmo_write`) and `command_list(char *, usize)` are still a pointer and a count, forwarded unchanged.

## 为什么有 `ou::Str` 而不用 `std::string_view`

不是偏好，是当初链接不上：用户程序 `-nostdlib`，没有 libc；`std::string_view{"text"}` 在运行时
调 `strlen()`，比较调 `memcmp()`。现在每个用户程序都链接了 freestanding libc++ 依赖的那几个
字符串/内存函数（`//system/lib/libc`，由 `user_executable()` 默认带上），剩下的一步是把 `Str`
改成 `std::string_view` 的别名，接口形状不变。

## 构建

只能在用户工具链里构建（BUILD.gn 里有断言）——这些入口会陷入内核，ring 0 里调用它们
不是系统调用。程序用 `//system/user_executable.gni` 构建，再 `deps += [ "//system/lib/ou" ]`。

## 还没有的

线程生命周期（创建、启动、退出、等待）等内核一侧的系统调用出现之后，在这里加
`ou::Thread`。目前内核只有两个系统调用，本库就只有两个入口。

**Outdated (2026-10-02):** typed handles in `lib/zx`'s shape (`ou::Vmo`, `ou::Channel`, ...), and `ou` generated from `syscalls.gni`, are planned in `.agents/tasks/system-abi-surface` (steps 8 and 9). Until then every call in `syscalls.gni` has one hand-written entry here, `thread_create` among them, and a handle goes in and out as a bare `ours::HandleValue`. Of a thread's lifecycle the kernel has only `thread_create`, `process_start` (which starts a process's first thread) and `process_exit` (which ends every thread of the caller's process): there is no `thread_start` or `thread_exit`, and nothing raises TERMINATED on a thread.
