## 公共 `Signals` 只表示对象信号；线程控制位挪到 `task::ThreadSignals`（2026-09-23）

**文件**
- `system/public/ours/signals.hpp` — 改为 Zircon 的对象信号布局：Readable/Writable/PeerClosed/Signaled(=Terminated)/HandleClosed/User0..7
- `kernel/include/ours/task/types.hpp` — 新增 `task::ThreadSignals { Suspend, Kill }`
- `kernel/include/ours/task/{thread,wait_queue}.hpp`、`kernel/kernel/task/{thread,wait_queue,owned_wait_queue}.cpp`、`kernel/include/ours/sync/event.hpp`、`kernel/kernel/sync/event.cpp` — 改用 `ThreadSignals`
- `kernel/object/include/ours/object/{process,thread}_dispatcher.hpp` — `ExtraSignals` 由 `Signals::Suspend` 改为 `Signals::None`

**症状**
潜伏。channel 需要 READABLE/WRITABLE/PEER_CLOSED 这些位，而公共 `Signals` 里只有 `Suspend`
和 `Kill`，而且 `Kill` 占着 bit 1——那正是每个对象的 WRITABLE。

**原因**
一个枚举装了两种完全不同的东西：
- **对象信号**：对象的可观察状态（channel 可读了、进程终止了），程序通过句柄等待它，属于 ABI；
- **线程控制位**：内核对自己某个线程下的命令（在下一个安全点停下、去死），在线程睡眠时和回到
  用户态前检查，任何程序都看不到。

放在同一套位里，第一个对象信号一加就会和 Kill 撞上。ProcessDispatcher/ThreadDispatcher 把
`Signals::Suspend` 当成自己的 `ExtraSignals`，也说明这个混淆已经往上层扩散了。

**改法**
改前（`signals.hpp`）：

```cpp
enum class Signals : u32 {
    None,
    Suspend = 0_bit,
    Kill = 1_bit,

    MaxCount,
};
```

改后：公共头只剩对象信号，位置与 Zircon `zircon/types.h` 一一对应（bit 0/1/2 可读/可写/对端关闭，
bit 3 按类型解读为 SIGNALED 或 TERMINATED，bit 23 HANDLE_CLOSED，bit 24..31 留给用户）；
线程控制位原样搬进 task：

```cpp
namespace ours::task {
enum class ThreadSignals : u32 {
    None,
    Suspend = 0_bit,
    Kill = 1_bit,
};
}
```

放在 `task/types.hpp` 里 `Interruptible` 旁边，因为两者讲的是同一件事：什么能打断一个阻塞的线程。
task、wait_queue、sync::Event 里的引用逐个替换，语义不变。

**参照**
Zircon：对象信号是 `ZX_OBJECT_*`/`ZX_*_SIGNALED`（`system/public/zircon/types.h`）；线程控制位是
`THREAD_SIGNAL_KILL`/`THREAD_SIGNAL_SUSPEND`（`kernel/include/kernel/thread.h`，`Thread::signals_`）。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表，dbg_1 的 RR 测试线程照常交替运行。

**故意没做**
- 只命名了树里会用到的信号位，其余按对象落地时再加。
- `thread.cpp` 里 `if (!!(signals & ThreadSignals::None))` 恒为假，是原有代码，与本改动无关，没动。

## Rights 换成 Zircon 的位值，各对象默认权限照 Zircon 设定（2026-09-23）

**文件**
- `system/public/ours/rights.hpp` — 全部 25 个权限位 + `SameRights` + `Basic/Io/Property/Policy` 组合，位值同 `zircon/rights.h`；删掉 `Rights::Default`
- `kernel/object/include/ours/object/*_dispatcher.hpp` — `RIGHTS_DEFAULT_{CHANNEL,RESOURCE,PAGER,JOB,PROCESS,THREAD,VMA,VMO}` 照 Zircon 的 `ZX_DEFAULT_*_RIGHTS`

**症状**
潜伏。今天没有任何系统调用检查权限，所以看不出来。但第一个检查权限的调用一落地，就会碰上：
- job 的默认权限是 `Rights::None`，拿到根 job 句柄也什么都做不了；
- channel/resource/pager 的默认权限是 `Rights::Default`，而它等于 `Duplicate`——一个 channel 句柄
  不能读、不能写、不能等待，只能复制；
- 没有 `Wait`、`Inspect`、`Map`、`Execute`、`ManageProcess` 等位，想检查也无从检查。

**原因**
原来的枚举只有 5 位，顺序是自己定的（`Signal` 占着 bit 4，Zircon 在那里放 `EXECUTE`）；
`Default = Duplicate` 这个名字让人以为它是「合理的默认值」，实际只是一个权限。

**改法**
改前：

```cpp
enum class Rights : u32 {
    None,
    Duplicate = 0_bit,
    Transfer = 1_bit,
    Read = 2_bit,
    Write = 3_bit,
    Signal = 4_bit,

    Io = Read | Write,

    Default = Duplicate,
};
```

改后（节选）：

```cpp
enum class Rights : u32 {
    None = 0,
    Duplicate = 0_bit,
    ...
    Signal = 12_bit,
    SignalPeer = 13_bit,
    Wait = 14_bit,
    Inspect = 15_bit,
    ...
    SameRights = 31_bit,

    Basic = Transfer | Duplicate | Wait | Inspect,
    Io = Read | Write,
    Property = GetProperty | SetProperty,
    Policy = GetPolicy | SetPolicy,
};
```

默认权限，例如 channel：

```cpp
constexpr auto RIGHTS_DEFAULT_CHANNEL =
    (Rights::Basic & ~Rights::Duplicate) | Rights::Io | Rights::Signal | Rights::SignalPeer;
```

直接换成 Zircon 的位值，是方案里的决策 D6：现在还没有任何用户态程序依赖这些值，换的代价
最低；之后 Zircon 的权限文档对这些位的描述可以直接拿来用。

**参照**
Zircon `system/public/zircon/rights.h`：`ZX_RIGHT_*` 与 `ZX_DEFAULT_{CHANNEL,JOB,PROCESS,THREAD,VMAR,VMO,RESOURCE,PAGER}_RIGHTS`。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表。今天没有代码检查权限，所以运行行为不会变，
这一条的正确性要等 `handle_close`/`channel_*` 真正检查权限时由用户态验证。

**故意没做**
`VmaDispatcher` 改名为 `Vmar` 属于 P1-C（D2/D3 一起定）；这里只把它的默认权限设成 Zircon VMAR 的。

## Status 显式编号，新增 `PeerClosed`、`BufferTooSmall`；未知调用号回 `BadSyscall`（2026-09-23）

**文件**
- `system/public/ours/status.hpp` — 每个非别名的枚举值都写出数字（与原来的隐式值逐一相同，0..31），新增 `PeerClosed = 32`、`BufferTooSmall = 33`
- `kernel/lib/syscall/src/table.cpp` — `sys_bad_syscall_number` 回 `BadSyscall`，不再回 `OutOfRange`

**症状**
潜伏。channel 需要「对端已关闭」和「缓冲区太小」这两个结果，原来的枚举里都没有。

**原因**
- 编号是隐式的：在中间插入任何一个新值，后面所有值都会悄悄变号。而这些数字是经 `%rax` 穿过
  系统调用边界的，按旧头文件编译的程序和按新头文件编译的内核会对同一个数字理解不同。
- `NoSpace` 和 `BadState` 都不是 `BufferTooSmall`/`PeerClosed` 的替代：前者讲的是文件系统或设备满了，
  后者讲的是「以目前状态不允许」；channel_read 需要告诉调用者「换个更大的缓冲区重试」，
  channel_write 需要告诉调用者「对面没人了，重试也没用」。
- 调用号越界回 `OutOfRange`，和「某个参数越界」混在一起；调用者分不清是调用本身不存在，还是参数错了。

**改法**
改前：

```cpp
enum class Status : u32 {
    Ok,
    Delayed,
    ...
    InternalIrqRetry,

    MaxCount,
};
```

改后：

```cpp
enum class Status : u32 {
    Ok = 0,
    Delayed = 1,
    ...
    InternalIrqRetry = 31,

    PeerClosed = 32,
    BufferTooSmall = 33,

    MaxCount,
};
```

编号由脚本按原顺序生成，保证与原来的隐式值一致；别名（`Unsupported`、`OutOfMem`……）不单独编号。

**参照**
Zircon `ZX_ERR_PEER_CLOSED`、`ZX_ERR_BUFFER_TOO_SMALL`、`ZX_ERR_BAD_SYSCALL`（`system/public/zircon/errors.h`）；
未知调用号回 `ZX_ERR_BAD_SYSCALL` 见 `unknown_syscall()`（`kernel/lib/syscalls/syscalls.cc`）。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表。

**故意没做**
- 没有把 `InternalIrqKilled`/`InternalIrqRetry` 这类内核内部码挪到私有枚举（方案 3c 的一部分）：它们被 task
  代码广泛使用，挪动是一次独立的重构，与这次要做的系统调用无关。
- 没有改成 Zircon 的负数 i32 编码：调用方都通过 ou 与 `Ok` 比较，改有符号会波及整棵树（方案第 9 节）。
