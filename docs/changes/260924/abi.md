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

## `<ours/vdso.hpp>` 的声明改由生成器产出（2026-09-24）

**文件**
- `build/syscall/gen_syscalls.py` — 新增 `--vdso-decls`
- `system/public/BUILD.gn` — 生成动作多一个输出 `public/ours/vdso_decls.inc`；新增 `group("vdso_decls")`，由 `:public` 经 `public_deps` 带上
- `system/public/ours/vdso.hpp` — 手写外壳保留，`extern "C"` 块里改为 `#include <ours/vdso_decls.inc>`
- `system/user_executable.gni` — deps 加 `//system/public`
- `kernel/lib/userabi/src/userboot.cpp` — 删掉没用到的 `#include <ours/vdso.hpp>`
- `docs/repairs/260923/syscall.md` — 删除清单里的声明一项并入"随条目消失"

**症状**
没有运行时症状。一个调用的用户侧签名写在 `vdso.hpp`，内核侧写在描述里，二者之间没有关联；
`vdso.hpp` 里 `ours_debug_write` 的注释还指错了 `MAX_DEBUG_WRITE` 的位置（说在 `debug.cpp`，
其实在 `<ours/syscall.hpp>`）。

**原因**
这是第四处、也是最后一处手工同步：vDSO 桩（第 5 条）、内核声明与 shim（第 6 条）都已生成，
程序调用的声明还是手写。程序和内核对同一个调用的参数理解不一致时，没有任何东西会报错——
桩只是把寄存器原样交给内核。

**改法**
改前（`vdso.hpp`，每个调用一段手写注释加声明）：

```cpp
extern "C" {
/// Writes |length| bytes of |buffer| to the kernel's debug console.
///
/// ... (see MAX_DEBUG_WRITE in kernel/lib/syscall/src/debug.cpp) ...
auto ours_debug_write(char const *buffer, usize length) -> ours::Status;
...
}
```

改后：

```cpp
extern "C" {
#include <ours/vdso_decls.inc>
}
```

生成的声明带着描述里的 `brief` 和 `doc`——文档是写给调用者的，这里是调用者唯一会读它的地方。
类型映射：原生类型不带命名空间，其余加 `ours::`；`dir = "in"` 的指针是 `T const *`，
`"out"` 是 `T *`；`void` 数据保持 `void`（内核侧才换成 `u8`）。生成的 8 个签名与原来手写的
逐个比对，完全相同。

**依赖边。** 用户程序拿公共头走的是工具链配置 `//system:user.headers`，不是依赖——所以它们
能 include `<ours/vdso.hpp>`，却没有任何边让编译排在生成器之后，干净构建里会随机找不到
`vdso_decls.inc`。于是：

- 生成目录的 include 路径挂在 `group("vdso_decls")` 的 `public_configs` 上，而不是塞进
  `public.config`：没有依赖的目标会确定地找不到头文件，而不是偶尔找不到；
- `:public` 经 `public_deps` 带上这个 group，`ou` 的头本来就依赖 `//system/public`；
- `user_executable()` 给每个程序加上 `//system/public` 依赖，补上那条边。

内核工具链也经 `BUILDCONFIG.gn` 直接拿到 `public.config`，同样没有边。内核里唯一 include
`<ours/vdso.hpp>` 的是 `userboot.cpp`，而它根本没用到——删掉。

**参照**
Zircon：`<zircon/syscalls.h>` 包含 kazoo 生成的 `<zircon/syscalls/internal/cdecls.inc>`，
外壳手写、声明生成，同样的切分。

**验证**
- 生成的 8 个声明与原手写声明逐个相同（名字、参数类型与顺序、返回类型）。
- 干净构建通过；干净构建的 ninja 图里，`init.cpp` 的编译对 `phony/system/public/syscalls.gen`
  有 order-only 依赖。
- 两个镜像启动正常。

**故意没做**
`user_executable()` 改为链接生成的 `libours.ifs` 而不是 vDSO 本体，不在 P1-A（计划"明确不在 P1-A"一节）。
