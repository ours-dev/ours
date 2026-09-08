# syscall-object-layer

补充基本的系统调用接口，完成 **用户 → 内核 → 用户** 的完整调用链测试。
硬规矩：**syscall 层只能碰 `kernel/object` 的对象**，不允许直接 touch `task::Thread`、
`mm::VmAspace` / `VmArea` / `VmObject` 或任何 arch 细节。凡是 syscall 需要而 dispatcher
没有的能力，去补 dispatcher，不绕。

这份文档先"扩散"再"收敛"：每一节先把可选项摆开，再说选哪个、为什么。
实现由 `user/boot` 会话按最后一节的顺序进行，每步一提交。

---

## 0. 一次系统调用的形状（不变量）

```
用户 syscall 指令
  → x86_syscall（syscall.S，压 arch::SyscallFrame，钳制 %rax）          [arch]
  → 分发 shim（table.cpp，拆帧 → 带类型参数）                            [lib/syscall]
  → 解析句柄（当前进程的 HandleTable，5 道关 + rights 检查）             [object]
  → dispatcher 方法（ProcessDispatcher / VmoDispatcher / ...）           [object]
  → 结果写回：Status 进 frame->ax；出参句柄/数据经 user_ptr 写回用户内存 [lib/syscall]
  → sysretq                                                              [arch]
```

syscall 层在这条链上能碰的东西只有三种：`arch::SyscallFrame`（拆帧）、`user_ptr`
（读写用户内存的设施，见 §3.6）、`object::*`。前两者是**设施**不是内核对象；第三者是
唯一允许碰的对象。`ProcessDispatcher::aspace()` 返回 `mm::VmAspace *`——那是给
kernel/task 的缺页路径用的，**syscall 层不许调**。

---

## 1. 现状盘点

| 对象 / 设施 | 有什么 | 缺什么（本任务需要的） |
|---|---|---|
| `Dispatcher` 基类 | canary、lock、handle_count、observers | **`m_koid` 从未被赋值**（构造函数漏了；`KoIdGenerator::generate()` 已存在，`task::Thread` 在用） |
| `Handle` / `HandleTable` | 设计已定（`docs/architecture/object/handle-lookup.md`）；`Handle::make/clone` 只有声明 | **全部实现**：arena、`base_value`/generation、XOR、5 道关、add/remove/lookup/dup |
| `JobDispatcher` | root job 单例 | 子对象跟踪（本任务不需要） |
| `ProcessDispatcher` | `create()`、`attach_thread()`、`m_aspace`/`m_job`/`m_name` | **`handle_table()` 成员**、**`current()`**、`exit()`/状态机、`start()` |
| `ThreadDispatcher` | `spawn()`/`start()`/`trampoline` | **`current()`**、**退出路径**（`on_exit()` 现在是空的） |
| `VmoDispatcher` | `create/create_paged/create_physical` | `size()`；`read/write`（卡在 mm `VmObjectPaged::write()`） |
| `VmaDispatcher` | `create/map` | `unmap`、`protect`；VMO 偏移（卡在 mm `map_with_vmo` 写死 0） |
| `ChannelDispatcher` / `PagerDispatcher` / `ResourceDispatcher` | 头文件或半成品 | 本任务不碰 |
| `Signals` / `SignalObserver` | 骨架在 | `Signals` 只有 `Suspend`/`Kill`，无 wait 语义（P1） |
| 用户指针设施 | **无**。`sys_debug_write` 裸读用户指针 | `user_ptr<T>` + `copy_from/to_user` |
| 分发层 | 一个手写 shim（`sys_debug_write_shim`） | 一种不用每个 syscall 手抄一遍的拆帧方式 |
| ABI 头 | `system/public/ours/syscall.hpp`：编号总数 + 1 个编号 | 全部编号；用户侧 stub |
| 用户侧运行时 | userboot 手写内联 `syscall` | 一份可复用的 stub 头（不是 libc） |
| 测试设施 | 无。`kernel/tests:kernel.tests` 是空 source_set，`UNITTESTS_ENABLED` 无框架 | 宿主侧 boot-test 脚本；对象层纯逻辑的宿主单测 |
| 观测通道 | `debug_write` ✓ | 进程退出码要能到达串口 |

**mm 侧已交回、本任务依赖的三条**（`docs/repairs/260906/mm.md`）：`create_mapping()`
不理会 `Commit`；`map_with_vmo()` 偏移写死 0；`VmObjectPaged::write()` 未实现。
第三条决定了 `vmo_read/write` 排在 P1。

**另一条要先验证的 mm 事实**：内核地址空间**不支持缺页填充**
（`Thread::Current::fault` 对 `is_kernel_address()` 直接 `NotFound`）。所以 handle
arena 不能像 Zircon 那样"一次预留、惰性提交"，见 §3.2。

---

## 2. 「基本」指哪些 syscall（扩散 → 收敛）

先把候选摆开，按"需要什么"分层：

| 层 | 候选 | 需要 | 备注 |
|---|---|---|---|
| **T0 无句柄** | `debug_write` ✓ | — | 已有 |
| | `thread_exit()` | 退出路径 | 无参数，Zircon 同 |
| | `process_exit(code)` | 退出路径 + 退出码上报 | 测试的"结束信号" |
| | `ticks_get()` / `clock_get_monotonic()` | 只读时钟 | 最干净的 用户→内核→用户 往返，零对象 |
| | `nanosleep(deadline)` | 定时器 + 可中断睡眠 | P1 |
| **T1 句柄自身** | `handle_close(h)` | HandleTable | |
| | `handle_duplicate(h, rights, *out)` | HandleTable + rights 收窄 | 第一个"出参句柄" |
| | `handle_replace` | 同上 | P1 |
| **T2 内存** | `vmo_create(size, opts, *out)` | VmoDispatcher ✓ | |
| | `vmo_get_size(h, *size)` | `VmoDispatcher::size()` | 第一个"出参数据" |
| | `vmo_read/write(h, buf, off, len)` | mm `write()` | **P1**（mm 卡） |
| | `vmar_map(vmar, opts, vmar_off, vmo, vmo_off, len, *addr)` | VmaDispatcher ✓；**7 个参数** | 见 §3.8 的 ABI 讨论 |
| | `vmar_unmap(vmar, addr, len)` | `VmaDispatcher::unmap()` → mm `VmArea::unmap` ✓ | |
| | `vmar_protect` / `vmar_allocate` | mm | P1 |
| **T3 任务** | `process_create(job, name, *proc, *vmar)` | `ProcessDispatcher::create` ✓ + job 句柄 | userboot 起第二个进程的那一刻 |
| | `thread_create(proc, name, *out)` | ✓ | |
| | `thread_start(th, entry, sp, a1, a2)` | ✓ | |
| | `process_start(proc, th, entry, sp, h, a2)` | 句柄跨进程转移 | P1 |
| | `object_wait_one(h, signals, deadline, *observed)` | Signals + Observer + 等待 | P1，`Signals` 要先长出 `Terminated` 等 |
| | `object_get_info` | 各 dispatcher | P1 |
| **T4 IPC** | `channel_create/write/read` | ChannelDispatcher、消息队列、句柄搬运 | **P2**，明确不在本任务 |

**收敛：P0 = 9 个**

```
ticks_get                      ← 零对象往返，最先做，用来校准分发层和 ABI
handle_close, handle_duplicate ← 句柄表本身
vmo_create, vmo_get_size       ← 入句柄 / 出句柄 / 出数据
vmar_map, vmar_unmap           ← 多参数、两个入句柄、出地址
thread_exit, process_exit      ← 退出路径 + 测试结束信号
```

选这 9 个的理由不是"最常用"，而是**它们合起来覆盖了 ABI 的每一种形状**：无句柄、
入句柄、出句柄、出数据（用户指针写回）、超过 6 个参数、以及每一类错误路径（坏句柄、
陈旧句柄、权限不足、坏指针、越界编号）。跑通这 9 个，之后每加一个 syscall 都只是"填表"。
并且它们**不依赖 mm 的三条待修**。

**P1**（依赖 mm 或 Signals）：`vmo_read/write`、`process_create` + `thread_create` +
`thread_start`（userboot 起第二个进程）、`object_wait_one`、`nanosleep`。
**P2**：channel。

---

## 3. 要补的对象与接口（按依赖顺序）

### 3.1 `Dispatcher::m_koid`

构造函数漏了。`Dispatcher(u32 magic)` 里加 `m_koid(KoIdGenerator::generate())`。
一行，但没有它 HandleTable 的第 5 道关（表 koid 比对）无从谈起，且每个对象的 koid 都是
未初始化内存。

### 3.2 `Handle` + `HandleTable`

设计已定，这里只记实现层面的取舍。

**arena 怎么来**——两条路，因为内核地址空间不支持缺页填充（§1）：

- (a) 一次性 `commit` 一小块（如 4096 个句柄 = 4096 × sizeof(Handle)），并把
  `kMaxHandleCount` 的 2 的幂约束保留在 `base_value` 编码上，让容量之后可以只改一个常量；
- (b) 分块增长：arena 是若干个各自连续的块，下标 = 块号 × 块大小 + 块内偏移。

选 (a)。(b) 让"地址 ↔ 下标"不再是一次纯算术，恰恰破坏了 handle-lookup.md 选 arena 的
理由。等 mm 支持内核区惰性提交后，(a) 平滑地变成 Zircon 的形态。

**`Handle` 的成员**（对齐 Zircon `Handle`）：`Arc<Dispatcher>`、`Rights`、
`base_value`（u32，含 generation）、`handle_table_id`（原子，表 koid；转移时改写）。
`sizeof(Handle)` 要 `static_assert` 出来，因为它就是 arena 的步长。

**`HandleTable` 的接口**（syscall 层要用的那几个）：

```cpp
auto add_handle(ustl::Box<Handle>) -> HandleValue;       // 装表，返回用户可见值
auto remove_handle(HandleValue) -> ustl::Box<Handle>;    // 卸表，close 用
auto get_dispatcher_with_rights<T>(HandleValue, Rights required, Arc<T> *out) -> Status;
auto get_handle_locked(HandleValue) -> Handle *;         // dup 用
```

**5 道关里的常数时间比较**：需要一个 `arch::conditional_select_nospec_eq()`
（Zircon 同名），x86 上是 `cmov`。这是本任务对 `lib/arch` 的唯一新要求。

**验证**（§4.6）：编码/解码是纯函数，可以在宿主工具链上单测；这是本任务里最值得有单测
的一块，因为错了不会崩，只会把别人的对象给错人。

### 3.3 `ProcessDispatcher`

- `handle_table()` 成员：`ustl::Arc<HandleTable>`（Zircon 放在 shareable state 里，为
  `create_shared` 留的；本任务先直接做成员，`create_shared` 时再抽）。
- `static auto current() -> ProcessDispatcher &`：经 `ThreadDispatcher::current()`。
  这是 syscall 层找到"我的句柄表"的**唯一入口**。
- `exit(code)`：状态 Running → Dying → Dead；记录退出码；把所有线程 kill；最后一个线程
  退出时释放句柄表（这一步把表里所有 `Handle` 的 `Arc<Dispatcher>` 放掉——**这是
  dispatcher 生命周期的终点**，也是 §4.4 计数器平衡的依据）。
- 退出码要能到达串口：`log::info("process {} ({}) exited with code {}")`。这是测试的
  结束信号（§4.1）。

### 3.4 `ThreadDispatcher`

- `static auto current() -> ThreadDispatcher &`：`task::Thread::Current::get()` 的
  `m_user_thread`。需要 `task::Thread` 暴露一个 `user_thread()` 访问器——**这是本任务
  向 kernel/task 提的唯一要求**，且是只读访问器。
- `exit()`：`Thread::Current::exit()` 已经会调 `on_exit()`，现在 `on_exit()` 是空的。
  它要做：从进程的线程表摘掉自己、通知进程"少了一个线程"、最后一个走的时候触发
  `ProcessDispatcher` 进入 Dead。注意 `on_exit()` 是在**退出中的线程自己的栈上**被调的，
  它不能释放自己正踩着的东西——`Arc<ThreadDispatcher>` 由 `task::Thread::m_user_thread`
  持有，随内核线程被 DPC 回收时释放，那是 kernel/task 的既有路径。

### 3.5 `VmoDispatcher` / `VmaDispatcher`

- `VmoDispatcher::size()`：`mm::VmObject` **没有** `size()` 访问器（查过 `vm_object.hpp`，连 `m_size` 都不在基类上，大小散在 `VmObjectPaged` 的 cow pages 和 `VmObjectPhysical::size_` 里）。这是向 mm 提的第四条：基类上一个 `size() const`。P0 里 `vmo_get_size` 因此排在这条之后，或者先只对 `create_paged` 出来的 VMO 支持——由 `VmoDispatcher` 自己记住创建时的 size，不问 mm。**选后者起步**：大小是创建参数，dispatcher 记一份不算越界，等 mm 有了再换。
- `VmaDispatcher::unmap(addr, len)` → `mm::VmArea::unmap()`（vm-unmap 任务已做）。
- `VmaDispatcher::map()` 的 rights 检查：这是**第一个真正的 rights 检查落点**——
  映射 `Writable` 要求 VMO 句柄有 `Rights::Write`，`Executable` 要求 `Rights::Execute`。
  `Rights` 枚举现在没有 `Execute`，要加（`rights.hpp` 是 ABI，加一位）。

### 3.6 用户指针设施：`user_ptr<T>`（新 `kernel/lib/user_copy`）

现在 `sys_debug_write` 裸读用户指针。P0 里 `handle_duplicate` / `vmo_create` /
`vmo_get_size` / `vmar_map` 都要**写回**用户内存，裸写更危险。

三个选项：
- (a) 继续裸读裸写，每个 syscall 自己做 `is_kernel_address` 检查；
- (b) `user_ptr<T>` 类型：构造时只存地址，`copy_from/copy_to` 做范围检查 + 访问；
      签名里 `user_out_ptr<HandleValue>` 和 `HandleValue *` 在类型上不可混淆；
- (c) (b) + 缺页可恢复：访问期间另一个线程解映射了那页，要能把 #PF 变成 `Status` 而不是
      panic——需要"当前线程正在做用户拷贝"的标记 + 缺页处理里的 fixup 表。

选 (b)，明确 (c) 是欠的。理由：(a) 的检查会散落到每个 syscall 里而且必然有人漏；(c) 需要
异常路径改造（arch + task 两个 lane），不该卡住 P0。**(b) 是类型层面的保证，(c) 是运行
时层面的**——先把类型立住，运行时的窗口写在 repairs 里。

`user_ptr` 是设施不是对象，和 `arch::SyscallFrame` 同类，所以 syscall 层可以碰。

### 3.7 分发层：拆帧怎么写

现在一个 syscall 一个手写 shim。9 个还能忍，30 个就是抄写错误的温床。三个选项：

- (a) 继续手写；
- (b) 模板：`dispatch<&sys_vmo_create>(frame)`，按被调函数的参数类型逐个从
      `frame->di/si/dx/r10/r8/r9` 取值并转换（`HandleValue`、`usize`、`user_ptr<T>`），
      返回值写 `ax`；
- (c) 从一份描述文件生成（Zircon 的 FIDL → kazoo）。

选 (b)。(c) 是 Zircon 的规模才值得的；(b) 的好处是**参数类型即 ABI**：一个 syscall 收
`user_out_ptr<HandleValue>`，拆帧代码就知道那个寄存器是用户指针，不需要另一张表。
超过 6 个参数（`vmar_map` 有 7 个）：`syscall.S` 的注释已经把 `r12`/`r13` 标为 arg7/arg8
且帧里保存了它们——**用 `r12`/`r13`**，不走"参数打包进用户内存的结构体"那条路（那会让
一个 syscall 多一次用户拷贝）。这一点要写进 `syscall.hpp` 的 ABI 说明。

### 3.8 ABI 头与用户侧 stub

- `system/public/ours/syscall.hpp`：全部编号（宏 + `enum class SyscallNumber` 镜像）、
  寄存器约定（含 arg7/8）、`HandleValue` 类型、`HANDLE_INVALID`。
- 用户侧 stub：`system/public/ours/syscall_stubs.hpp`（header-only，`static inline` +
  内联 asm）。放 public 是因为它就是 ABI 的另一半；不做 vDSO，那是 P2。
- `Rights` 加 `Execute`。

### 3.9 userboot 怎么拿到自己的句柄

三个选项：
- (a) `ThreadDispatcher::start()` 的 `arg1`/`arg2` 直接传两个句柄值（self-process、
      root-vmar）；
- (b) 内核映射一页"启动块"到进程里，放一张 {process, vmar, vmo, job …} 表，`arg1` 传地址；
- (c) Zircon 式：bootstrap channel，句柄随消息搬运。

选 (a) 起步：P0 只需要 process 和 vmar 两个，正好两个寄存器；(b) 在需要第三个句柄时
（P1 的 job 句柄）再换，且 (a)→(b) 只改 userboot 两端。(c) 等 channel。

---

## 4. 测试

### 4.1 观测通道

只有两条：`debug_write`（过程）和 `process_exit(code)`（结果）。内核在进程退出时打
`userboot exited with code N`；宿主脚本以此判定。

### 4.2 userboot 变成自检程序

每个用例一行 `PASS <name>` 或 `FAIL <name>: expected X got Y`，最后
`userboot: <n> passed, <m> failed`，`process_exit(m == 0 ? 0 : 1)`。断言宏由 stub 头
旁边的一个测试小头提供（`expect_eq(name, got, want)`），全是内联，不需要 libc。

### 4.3 用例（正向 + 负向，负向才是这个任务的价值）

| 用例 | 验证的是 |
|---|---|
| `ticks_get` 两次单调递增 | 零对象往返；分发层与 ABI 基本正确 |
| `handle_duplicate(self_vmar)` 后两个值不同且都能用 | 出参句柄写回；同对象多句柄 |
| `handle_close` 后再用 → `BadHandle` | **generation 生效**（陈旧值不解析） |
| `handle_duplicate` 收窄为 `Read` 后 `vmar_map` 要 `Write` → `AccessDenied` | rights 落在句柄上 |
| 传别的进程的句柄值（P1 有第二个进程时）→ `BadHandle` | 第 5 道关（表 koid） |
| `vmo_create(0)` / 超大 size → `InvArgs`/`OutOfRange` | 参数校验 |
| `vmo_get_size` 出参是内核地址 → `InvArgs`；是回绕指针 → `InvArgs` | `user_ptr` 范围检查 |
| `vmar_map` 一个 VMO 两次到两个地址，都能读 | VMO/VMA 分家的意义 |
| `vmar_unmap` 后访问 → **缺页**（用户态缺页目前 = 进程死；先记为"预期崩溃"用例，P1 有 exception 再改成可恢复） | unmap 真的拆了 PTE |
| `syscall(0x1000)` → `OutOfRange` | 钳制（已实测一次，纳入回归） |
| `syscall(<有编号无实现>)` → `Unimplemented` | 分发表填充 |
| `thread_exit` 在第二个线程里（P1） | 退出路径不带走进程 |
| 最后 `process_exit(0)` → 串口 `exited with code 0` | 结束信号 |

### 4.4 内核侧不变量

- 进程 Dead 之后：`dispatcher.*.create` 与 `*.destroy` 计数**平衡**（GKTL_COUNTER 已在
  每个 dispatcher 里），句柄表为空。做成 `KERNEL_DEBUG_LEVEL=1` 下的 `DEBUG_ASSERT` +
  一行日志，宿主脚本也 grep 它。这是"没泄漏"的证据，比任何用例都硬。
- `dbg_1` 与 `dev` 两个镜像都跑：断言在一个里开、在另一个里关，一个 bug 在两边表现不同
  正是要抓的。

### 4.5 宿主侧：`scripts/boot-test.sh` + ninja 目标

对标 Zircon 的 `kernel_zbi_test` / `boot_test_success_string`：
- 启动镜像，超时 60 s；
- 成功串：`userboot exited with code 0` **且** 计数器平衡那一行；
- 任何 `FAIL`、`Unhandled`、`panic`、或 QEMU 自行退出（三重故障）→ 失败；
- 退出码给 ninja。GN 里一个 `boot_test("userboot")` 模板，挂到 `//:default` 之外的
  `//:boot_tests`（默认不跑，CI 跑）。
- 日志含转义与 NUL，**grep 一律带 `-a`**（已踩过）。

### 4.6 宿主单测：句柄编码

`HandleTable` 的 `base_value` 编解码、generation 递增、5 道关是纯函数。把它们抽成
`handle_value.hpp`（不依赖 Dispatcher），在 host 工具链下用 `tests/` 已有的 smoke 形式
跑。这是本任务唯一值得写宿主单测的地方——错了不崩、只会把对象给错人，运行时测不出来。

内核内单测框架（`UNITTESTS_ENABLED`）是空壳，**本任务不建**；记入 repairs。

### 4.7 每一步的可观察结果

| 步 | 提交后能看到什么 |
|---|---|
| koid + HandleTable | 宿主单测通过；内核无行为变化 |
| `current()` + 挂表 | 无行为变化；DEBUG_ASSERT 不炸 |
| `user_ptr` | `sys_debug_write` 改用它，串口输出不变 |
| 分发层重构 + `ticks_get` | userboot 打出两个递增的 tick |
| 退出路径 | 串口出现 `exited with code N`，之后计数器平衡 |
| 启动句柄 + close/dup | `PASS handle_*` 各行 |
| vmo/vmar | `PASS vmo_*`/`vmar_*`；unmap 用例按预期缺页 |
| boot-test 脚本 | `ninja boot_tests` 绿 |

---

## 5. 顺序（每步一提交，每步 build + 两镜像 boot）

1. `Dispatcher::m_koid` 初始化。
2. `handle_value.hpp`（纯编码）+ 宿主单测。
3. `Handle` + `HandleTable`（arena 一次性提交；add/remove/lookup/dup）。
4. `ThreadDispatcher::current()` / `ProcessDispatcher::current()` / `handle_table()`；
   向 kernel/task 要 `Thread::user_thread()` 只读访问器。
5. `user_ptr<T>` + `copy_from/to_user`（范围检查版）；`sys_debug_write` 改用。
6. 分发层模板 + ABI 头全量编号 + 用户侧 stub；`ticks_get` 作为第一个客户。
7. `thread_exit` / `process_exit` + 退出路径 + 退出日志 + 计数器平衡断言。
8. userboot 启动句柄（方案 a）；`handle_close` / `handle_duplicate` + 用例。
9. `vmo_create` / `vmo_get_size` / `vmar_map` / `vmar_unmap` + `Rights::Execute` + 用例。
10. `scripts/boot-test.sh` + `boot_test()` GN 模板 + `//:boot_tests`。
11. （P1）等 mm：`vmo_read/write`；`process_create`/`thread_create`/`thread_start`——
    userboot 起第二个进程，跨进程句柄用例。
12. （P1）`Signals::Terminated` + `object_wait_one`。

---

## 6. 交给别的 lane 的

| lane | 事项 | 为什么是他们的 |
|---|---|---|
| mm | `VmObjectPaged::write()`；`map_with_vmo()` 偏移；`create_mapping()` 的 `Commit`；`VmObject::size()`；内核区惰性提交（arena 将来用） | 前三条已在 `docs/repairs/260906/mm.md`，后两条本任务新提 |
| task | `Thread::user_thread()` 只读访问器；`Thread::m_aspace` 与 `process().aspace()` 双真相 | Thread 是他们的 |
| arch/x86 | `x86_syscall` 调用点栈对齐（`docs/repairs/260908/x86.md`）；`conditional_select_nospec_eq` | 帧与 stub 是 arch 的 |
| build | `boot_test()` 模板；内核内单测框架 | — |

---

## 7. 风险与开放问题

- **退出路径与 DPC 回收的交互**：`on_exit()` 跑在退出线程自己的栈上，`ProcessDispatcher`
  进入 Dead 时不能释放正在用的内核栈。要和 kernel/task 确认 `Thread::Current::exit()`
  之后的回收时序（哪一刻 `m_user_thread` 被放掉）。
- **常数时间比较**：`cmov` 在 -O2 下能否保证不被优化成分支，要看生成的汇编，不能只看源码。
- **`vmar_unmap` 用例的"预期崩溃"**：用户态缺页现在等于进程死。P0 里这个用例只能验证
  "确实死了"，要 P1 的用户态异常处理才能变成"返回错误"。
- **arena 一次性提交的容量**：4096 个句柄够 P0/P1；选这个数是为了和 4 KiB 页整除，
  `sizeof(Handle)` 定下来后再算。
- **Rights 的 `Execute`** 是 ABI 变更，要和 `rights.hpp` 里已有的位不冲突。
