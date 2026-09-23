## 第一个带句柄的系统调用：`ours_handle_close`（2026-09-23）

**文件**
- `system/public/ours/syscall.hpp` — `OURS_SYS_HANDLE_CLOSE 2`
- `system/public/ours/vdso.hpp` — `ours_handle_close(HandleValue) -> Status`
- `kernel/lib/userabi/vdso/vdso.S` — 桩与 `OURS_VDSO_SYSRET_HANDLE_CLOSE`
- `kernel/lib/syscall/src/handle.cpp`（新）、`handlers.hpp`、`table.cpp` — `sys_handle_close` 与分发 shim
- `system/lib/ou/include/ou/handle.hpp`、`src/handle.cpp`（新）— `ou::handle_close`
- `examples/shell/shell.cpp` — `_start(bootstrap, vdso_base)`；关两次入口句柄并打印结果

**症状**
无——这是新功能。它是第一个走完整条「句柄值 → 句柄表 → 对象」路径的系统调用。

**原因**
句柄表、句柄 arena、第一个进程拿到的根 job 句柄都已就位，但用户态还没有任何办法碰到句柄。
`close` 是最小的一个：不需要 rights，不需要新对象，却完整地检验了编码、五道关、摘表和销毁。

**改法**
每一层各加一处，严格按「系统调用只经 vDSO」的路径：

```
shell        ou::handle_close(bootstrap)
ou           ours_handle_close(handle)            <ours/vdso.hpp>
vDSO         mov $OURS_SYS_HANDLE_CLOSE, %eax; syscall
内核         sys_handle_close_shim → sys_handle_close(HandleValue)
对象层       ProcessDispatcher::current()->handle_table().remove(value)
```

两处值得说明：

- **`HANDLE_INVALID` 回 Ok**，照 Zircon `zx_handle_close()`：清理路径可以把每个槽都关一遍，
  不用先问哪些填过。
- **shim 显式截断**：`static_cast<HandleValue>(frame->di)`。SysV 对 32 位参数所在寄存器的高 32 位
  不作保证，编译器在那里留下垃圾也是合法调用；不截断，一个合法句柄会被当成不存在。
- `remove()` 返回句柄而不在锁内销毁，句柄在 `sys_handle_close` 的语句末尾离开作用域——表锁那时
  已经释放。关掉某对象最后一个句柄会运行它的 `on_zero_handles()`，那可能要拿别的锁。

**参照**
Zircon `sys_handle_close()`（`kernel/lib/syscalls/handle_ops.cc`）；`zx_handle_close` 对
`ZX_HANDLE_INVALID` 的约定（`vdso/handle.fidl`）。

**验证**
两个镜像都绑定了第三个导入，shell 打出：

```
elfload: bound 'ours_handle_close' to A01340
shell: close(bootstrap) = Ok
shell: close again = BadHandle
```

第一次关闭证明内核装进表里的值能被解析并摘除；第二次证明同一个数字在关闭后不再指向任何东西——
这正是句柄表存在的意义，而且是从依赖它的那一侧观察到的。

**故意没做**
经这个句柄挂着的等待应以 Cancelled 结束（Zircon 的 `Dispatcher::Cancel(handle)`）——要等下一步有了
等待机制才有东西可取消。

## `channel_create/write/read` 与 `object_wait_one`：四个系统调用的整条通路（2026-09-23）

**文件**
- `system/public/ours/syscall.hpp` — 调用号 3..6；`ChannelCreateOptions`/`ChannelWriteOptions`/`ChannelReadOptions`
- `system/public/ours/vdso.hpp` — 四个声明
- `kernel/lib/userabi/vdso/vdso.S` — 四个桩：第 4 个参数 `%rcx → %r10`；`channel_read` 的第 7、8 个参数从栈装进 `%r12/%r13`（先保存、后恢复）
- `kernel/lib/syscall/src/channel.cpp`、`object.cpp`（新）、`handlers.hpp`、`table.cpp`、`BUILD.gn`
- `system/lib/ou/include/ou/channel.hpp`、`object.hpp`、`src/channel.cpp`、`object.cpp`（新）、`BUILD.gn`
- `kernel/object/include/ours/object/handle.hpp` — `KernelHandle` 的转换构造改用 `release()`
- `system/BUILD.gn`、`system/public/BUILD.gn` — 用户程序拿到 ustl 与 freestanding libc 的头文件路径

**症状**
无——新功能。但它**尚未在运行时被验证**：shell 的自环测试一调用 `channel_create` 就挂死，原因在 mm，
见本节「验证」。

**原因**
方案 P0 的第 7 步。

**改法**
- **寄存器约定**集中在 `table.cpp` 的 shim 里：第 4 个参数在 `%r10`，第 5、6 个在 `%r8/%r9`，第 7、8 个在
  `%r12/%r13`；每个 32 位的值（句柄、计数、选项字）都从寄存器显式截断。
- **`channel_read` 的桩**与 Zircon `syscalls-x86.S` 的 `m_syscall` 宏逐条对应：压 `%r12`、`%r13`，从
  `0x18(%rsp)`/`0x20(%rsp)` 取第 7、8 个参数，陷入，恢复。
- **`channel_create` 的出参**：两个值都先 `copy_to_user`，再装表（方案 4.1）——值是句柄与表的纯函数，
  装表前就能算出；任何一次拷贝失败，两个句柄就地销毁，从未对程序可见过。
- **`channel_write`**：先查句柄（需 `Write`）再在任何锁之外拷贝消息；`num_handles > 0` 回 NotSupported（P1）。
- **`channel_read`**：`Ok` 与 `BufferTooSmall` 时写回长度（两个出参都可为空，同 Zircon）；消息在 channel
  锁释放后才拷出，拷出失败则消息丢失——与 Zircon `sys_channel_read()` 的取舍相同。
- **`object_wait_one`**：只搬参数给 `object::wait_one`；等待正常结束（Ok/TimeOut/Cancelled）才写回 observed。
- **选项**：每个调用一个枚举类型，目前只接受 `None`，其余回 InvArgs，不静默忽略——程序不能依赖一个什么都不做的位。
- **构建**：`vdso.hpp` 开始提到 `Signals`，用户程序于是第一次经 `<ours/signals.hpp>` 引到 ustl 的
  `enum_bits.hpp`，再经 `<utility>` 引到 libc++ 的 `<cstring>`。用户工具链此前既没有 ustl 的 include 路径，也没有
  freestanding libc 的声明；`//system:user.headers` 现在加上了两者（libc 用的是内核同一个 config，
  头文件本就在 `//system/lib/libc/include`）。
- `KernelHandle` 的转换构造原来直接访问另一个特化的私有成员，只是因为从没以 U≠T 实例化过才编译得过；
  改用 `release()`，语义也更准确：引用转移，而对象不应被告知失去持有者。

`channel_read` 桩（与 Zircon 相同偏移）：

```asm
ours_channel_read:
    push %r12
    push %r13
    mov %rcx, %r10
    mov 24(%rsp), %r12
    mov 32(%rsp), %r13
    mov $OURS_SYS_CHANNEL_READ, %eax
    syscall
OURS_VDSO_SYSRET_CHANNEL_READ:
    pop %r13
    pop %r12
    ret
```

**参照**
Zircon `kernel/lib/userabi/vdso/syscalls-x86.S`（`m_syscall`）；`sys_channel_create()`/`sys_channel_write()`/
`sys_channel_read()`（`kernel/lib/syscalls/channel.cc`）；`sys_object_wait_one()`（`object_wait.cc`）。

**验证**
- 构建通过；两个镜像都启动到 shell 打完命令表，四个新导出都被绑定：
  `elfload: bound 'ours_channel_create' to A01430` …
- **运行时未验证。** shell 的自环测试（`channel_create` → 写 → 轮询等待 → 读 → 读空 → 关对端 → PEER_CLOSED）
  在 `channel_create` 里挂死，定位到 mm：shell 跑在 CPU1 上，`ChannelDispatcher::create()` 的第一次
  `kmalloc(160)` 在 `MemCache::allocate(Gaf, u32, CpuCache *)` 里对一把「锁」空转——那把锁的地址是
  kmalloc-256 的 per-CPU `CpuCache` 在 CPU1 上解析出来的地址，落在内核 `.bss` 的 `KMALLOC_CACHE` 数组内部，
  锁字是一个指针值。启动期所有分配都在 CPU0 上发生，所以此前从未暴露。缩小句柄 arena 的实验排除了本系列
  `.bss` 变大的影响。已作为 mm 的独立问题移交；测试补丁暂存，修复后随下一步一起提交。

**故意没做**
没有为了绕开这个问题把用户线程钉在 CPU0 上：下层有缺陷，要修的是下层，不是在上层绕路。
