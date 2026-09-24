# abi 改动记录（2026-09-24）

## `KoId` 改为 u64（2026-09-24）

**文件**
- `system/public/ours/types.hpp` — `using KoId = u64;`，并注释为什么
- `kernel/include/ours/koid.hpp` — `KoIdGenerator::generate()` 的 count 形参改为 `usize`
- `kernel/object/handle.cpp` — `static_assert(sizeof(Handle) == 40)`

**症状**
潜伏。koid 是 32 位，用户程序循环 `channel_create` 就能把它耗尽并回绕。

**原因**
koid 的契约只有一条：在系统的整个生命周期里唯一，永不复用。句柄表的第五项检查
（句柄上记的 table koid 等于本表的 koid）、将来的 `object_get_child(koid)`、调试器
按 koid 找对象，都建立在这条契约上。

32 位守不住它。每个 dispatcher、每张句柄表各取一个，一次 `channel_create` 取两个；
按每微秒一个算（单核上紧循环创建），2^32 在 72 分钟内用完。回绕之后：

- 先发出 0，也就是 `KOID_INVALID`，同时也是 `Handle::table_koid()` 表示"不在任何表里"
  的值——一个对象的 koid 和"无"撞在一起；
- 再往后发出仍在使用的 id：一个句柄被移进另一张表，而那张表恰好与原表同 koid，
  本该失败的表检查会通过。

这个值会进 ABI（P1-B 冻结 `InfoHandleBasic` 时 koid 是其中的字段），所以必须在那之前改。
按同样的速率，2^64 能用五十多万年。

**改法**
改前：

```cpp
using KoId = u32;
...
    static auto generate(KoId count = 1) -> KoId {
```

改后：

```cpp
using KoId = u64;
...
    static auto generate(usize count = 1) -> KoId {
```

`generate` 的形参是"要几个"，是数量而不是 id，所以用 `usize`，与 KoId 的宽度脱钩。

`Handle` 的大小没有变：原来 `m_table_koid`（4 字节）之后有 4 字节填充，为的是让
`m_dispatcher` 的指针按 8 对齐；现在 8 字节的 koid 正好占满。仍是 8 + 8 + 4 + 4 + 16 = 40。
句柄 arena 是静态的 `HANDLE_ARENA_SLOTS × sizeof(Handle)`，给 Handle 加一个成员就是给每个
槽加，所以用 `static_assert` 把 40 钉住，让以后的增长是一个明确的决定而不是副作用。

**参照**
`Zircon zx_koid_t`（`uint64_t`），以及 `KernelObjectId::Generate()`。

**验证**
- 构建通过；触碰 `types.hpp` 和 `koid.hpp` 后全量重编，没有新增的转换或窄化告警。
- 两个镜像启动正常，init↔shell 对话与 shell 的句柄检查照常。

**故意没做**
- `prof` 的 `current_tid()` 仍返回 u32，`prof_pmu.cpp` 里是显式 `static_cast<u32>`：
  采样记录的格式是 prof 自己的，改宽属于 prof 的事。线程 id 什么时候改用 dispatcher
  的 koid 是 P1-B 的事。
- `Thread::spawn()` 里 `KOID_INVALID == tid` 的检查保留：u64 下实际不会触发，但它表达的是
  "生成器不发 0"这条约定。

## 调用号离开公共头（2026-09-24）

**文件**
- `system/public/ours/syscall.hpp` — 删掉 `OURS_SYSCALL_COUNT`、`OURS_SYS_*`、`ours::SYSCALL_COUNT`、`SyscallNumber`；重写头注释；去掉 `#ifdef __cplusplus`
- `kernel/arch/x86/kernel/syscall.S` — 删掉不再用到的 `#include <ours/syscall.hpp>`
- `kernel/lib/syscall/src/table.cpp` — 私有常量 `DISPATCH_SLOTS = 256` 取代公共的 `SYSCALL_COUNT`
- `docs/repairs/260923/syscall.md` — 删除清单里的 `OURS_SYS_PROCESS_SPAWN_BOOT` 一项改为随条目消失

**症状**
没有运行时症状。公共头的注释写着"用户程序把其中一个号放进系统调用号寄存器"——这在
vDSO 规则之下不成立，却以公共 ABI 的身份印在每个用户程序都能 include 的头里。

**原因**
这个项目的规则是：用户程序只能经 vDSO 进内核，`syscall` 指令只存在于 vDSO 里，内核拒绝
来自别处的陷入。于是调用号从来不是程序看得到的东西，它是 vDSO 的桩和内核分发表之间的私有
约定（设计 D8）。把它放在公共头里，等于把 vDSO 要隐藏的那样东西公开发布——一个程序拿到号，
唯一能做的就是绕开 vDSO 自己发陷入，而那正是规则禁止的。

放在公共头里还有一个后果：一旦用户程序依赖了某个号，调整顺序就成了 ABI 变化。私有之后，
号只在 vDSO 和内核之间，两边同时从 `syscalls.gni` 生成，重排只是一次重新构建。

此前前三条提交已经让内核和 vDSO 都改用生成的 `syscall_abi::`，这一条删掉的只是没有人再
用的旧定义。

**改法**
改前（节选）：

```cpp
// The system-call ABI: the contract between a user program and the kernel's
// entry stub. ... a user program puts one of them in the syscall-number register.
#define OURS_SYSCALL_COUNT 256
#define OURS_SYS_DEBUG_WRITE 0
...
#ifdef __cplusplus
namespace ours {
inline constexpr auto SYSCALL_COUNT = usize{OURS_SYSCALL_COUNT};
...
enum class SyscallNumber : usize { DebugWrite = OURS_SYS_DEBUG_WRITE, ... };
}
#endif
```

改后：头里只剩两侧共享的常量——`INSTANT_INFINITE`、`CHANNEL_MAX_MSG_BYTES`、
`CHANNEL_MAX_MSG_HANDLES`、`MAX_DEBUG_WRITE`、`PROCESS_BOOT_NAME_MAX` 和三个 `*Options` 枚举；
头注释说明号为什么不在这里、在哪里。汇编不再 include 它，所以 `#ifdef __cplusplus` 的分层
也一起去掉。

`table.cpp` 的表长原来取自公共的 `SYSCALL_COUNT`（256）。本条只挪位置不改行为：

```cpp
    constexpr auto DISPATCH_SLOTS = usize{256};
    static_assert(syscall_abi::SYSCALL_COUNT <= DISPATCH_SLOTS);

    constexpr auto TABLE_SIZE = DISPATCH_SLOTS + 1;
```

按生成的计数定表长是下一条的事，它会改变 8..255 号的返回值，值得单独一条。

表外的名字一律写全限定的 `syscall_abi::SYSCALL_COUNT`，不用 using 指令：否则删掉
`ours::SYSCALL_COUNT` 之后，同名的生成常量会悄悄顶上，表长在这一条里就变成 9。

**参照**
Zircon：`zx-syscall-numbers.h` 由 kazoo 生成，只给内核和 vDSO 用；公共的 `<zircon/syscalls.h>`
只有 `zx_*` 函数声明，没有号。

**验证**
- `grep -rn "OURS_SYS_\|OURS_SYSCALL_COUNT\|SyscallNumber" kernel system examples`：只剩
  `table.cpp` 注释里的一处历史说明（`OURS_SYSTEM_*` 头文件守卫另计）。
- `grep -rnw SYSCALL_COUNT`：只剩 `syscall_abi::` 限定的。
- 干净构建通过；两个镜像启动正常。

**故意没做**
无。
