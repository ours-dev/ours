# ours 用户层接口与启动环境：最终设计（2026-09-23）

> 输入：task、memory、ipc、resource、object_model、startup 六个方向的设计，加三位评审（最小闭环、一致性与安全、与代码的契合度）的意见。评审核实为错误的状态判断已经改正；互相冲突的地方给出了统一方案；超出需求的部分降了级或删掉，理由写在第 8 节。“现状”一栏以代码为准，给出文件路径。

## 1 总览

内核是为用户层服务的，所以接口从“第一个用户程序要做成什么”倒推。结果是 9 个对象集、约 90 个系统调用，分成三档。

**P0 只做一条最小闭环。** 内核直接把 init 起成第一个进程，交给它根 job 句柄。init 建一对 channel，用一个明确标为临时的内核调用 `ours_process_spawn_boot` 从常驻的内核包里起 shell，把 channel 一端交过去。双方交换一条 ping/pong，init 用 `ours_object_wait_one` 阻塞等回复。

P0 只新增 6 个系统调用，但工作量主要在它们下面：句柄表、信号核心、Channel，还有几处今天就存在的入口缺陷，第一个阻塞调用一出现就会暴露。这些缺陷是：用户 RSP 只存在 per-CPU 槽里；FMASK 不清 TF/DF/AC；内核直接解引用用户指针，而支持 SMAP 的 CPU 上 SMAP 其实是开着的。

完整的 Zircon 式对象接口放在 P1，包括用户态加载器、进程生命周期、Vmo/Vmar、Port/Futex、时间与日志。顺序是：边界 → 生命周期 → 加载器 → 多路与同步。加载器落地的时候删掉临时调用。

P0 期间有三条不变量：
- 只运行内核包里的受信程序；
- 每个用户进程只有一个线程；
- 用户态出错仍然 panic，进程不退出。

| 对象集 | 成员对象 | 背后内核对象 | ours 现状 | 优先级 |
|---|---|---|---|---|
| A 句柄与对象核心 | Handle、HandleTable、Dispatcher 信号核心、Object 通用调用 | 全局 Handle arena；每进程 HandleTable；`Dispatcher::m_signals` + `SignalObserver` | stub：`Handle::make/clone` 没有定义，`HandleTable` 没有存储（`kernel/object/include/ours/object/handle_table.hpp:39-55`）；`attach_observer` 不做电平检查（`kernel/object/dispatcher.cpp:24-36`） | P0 close / wait_one / 信号核心；P1 其余 |
| B 任务 | Job、Process、Thread；Exception、SuspendToken、Profile、JobPolicy | `task::Thread`、`mm::VmAspace` | Job/Process/Thread 为 stub：内核内部的构造能用，面向用户的操作全缺。其余 missing | P0 只用内核内部路径；P1 create/start/exit/kill；P2 其余 |
| C 内存 | Vmo、Vmar（现名 VmaDispatcher）、vDSO VMO、BootFS VMO、Pager、Stream | `VmObjectPaged/Physical`、`VmArea/VmMapping`、`VmAspace` | Vmo/Vmar/Pager 为 stub；vDSO VMO、BootFS VMO、Stream missing | P1；Pager/Stream P2 |
| D IPC 与同步 | Channel（PeerHolder、MessagePacket）、Event、EventPair、Port、Futex、Socket、Fifo | PeerHolder 共享锁、`sync::Event`、`task::OwnedWaitQueue` | Channel 为 stub（`channel_dispatcher.hpp` 只有 `create()` 一个声明）；其余 missing | P0 Channel（只传字节）；P1 带句柄、Event/Port/Futex；P2 Socket/Fifo |
| E 时间 | 单调时钟调用、Timer、Clock | `time::SteadyClock`、`time::Timer` + `ours::Dpc` | missing（内核侧已实现，用户侧没有入口） | P1；Clock P2 |
| F 资源与设备 | Resource、Interrupt、IoPorts | 每个种类一个区间分配器、`irq::IrqObject`、TSS io_bitmap | Resource 为 stub（`create()` 内部检查自相矛盾，也不构造对象）；其余 missing | P1 交付根资源；P2 其余 |
| G 诊断与系统信息 | debug_write、command_list、Log、cprng、系统常量 | logz4、kshell 注册表、新 CPRNG | debug_* 已实现；其余 missing | P0 debug_write；P1 其余 |
| H 系统调用边界 | 入口（`syscall.S`/`vdso.S`）、UserPtr、Status、ABI 生成器 | `arch::SyscallFrame`、`kernel/lib/syscall/src/table.cpp` | 入口有缺陷；UserPtr missing；Status 已实现但要修订；ABI 靠手工同步四处 | P0 入口 + UserPtr；P1 生成器 |
| I 启动（临时） | `process_spawn_boot`、`userabi::launch`、常驻内核包 | elfload Loader/Binder、KernelPackage | missing：`userboot_init` 是一整个函数，包视图是局部变量 | P0，P1 删除 |

## 2 对象集与接口

表格约定：
- `HandleValue` 是公共句柄值类型（u32，是否改名见 D4）。
- `Instant` 是 i64 纳秒。
- “rights”一栏写的是调用所需的句柄权限。
- ou 入口是 `system/lib/ou` 里的具名函数。P1 起在这些函数之上加 RAII 类型（`ou::Handle`、`ou::Channel`、`ou::Process` 等）。
- 所有新对象都派生自现有的 `SoloDispatcher`/`PeeredDispatcher`，或组合进现有 dispatcher，**不新增基类**。

### 2.1 A 句柄与对象核心

- **Handle / HandleTable**：把寄存器里的 u32 解析成 (Dispatcher, Rights)，权限只在这里检查一次。每个进程一张表，P0 用 `sync::Mutex` 保护，因为 `sync::RwLock` 是空壳（`kernel/kernel/sync/rwlock.cpp` 里只有一个空命名空间）。编码和出参句柄规则见 4.1。
- **Dispatcher 信号核心**：沿用已有的基类，要补的东西如下。
  - `ObjectType m_type`：存储字段，由 `SoloDispatcher` 从 `Derived::TYPE` 写入，`downcast<T>()` 比较它；
  - `update_state(clear, set)`；
  - `attach_observer` 的电平检查；
  - `detach`、`cancel(Handle const *)`；
  - `allowed_user_signals()`、`related_koid()`、名字。
  - 同时修掉一个 bug：`m_canary.verify<Self>()` 放在任何真实对象上都必然断言失败（`dispatcher.cpp:26/39`）。
  - 信号只能在线程上下文里更新，因为 `m_lock` 是 Mutex。
- **WaitSignalObserver**（object 层内部类）：栈上的 observer 加一个 `sync::Event`。`sync::Event` 要先补构造函数，今天 `kernel/include/ours/sync/event.hpp:35-47` 的 `m_flags` 没有初始化。“挂上 → 睡 → 摘下 → 读当前信号”整套流程封在 `object::wait_one()` 里，lib/syscall 只搬参数。

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_handle_close(HandleValue handle)` | `HANDLE_INVALID` 回 Ok。持表锁摘除，放锁后销毁；句柄数归零时调 `on_zero_handles`。经这个句柄挂着的等待以 Cancelled 结束 | 无 | P0 | `ou::handle_close` |
| `Status ours_object_wait_one(HandleValue handle, Signals signals, Instant deadline, Signals *observed)` | 电平触发，条件已满足就立即回 Ok。否则可中断地睡眠，满足回 Ok，超时回 TimeOut，句柄被关回 Cancelled（observed 带 HANDLE_CLOSED）。P0 的 deadline 只接受 0 和 `INFINITE_TIME`，其余回 NotSupported，因为用户态还读不到时钟 | Wait | P0 | `ou::object_wait_one` |
| `Status ours_handle_duplicate(HandleValue handle, Rights rights, HandleValue *out)` | rights 必须是 `SameRights` 或源权限的子集，否则 InvArgs。顺序是预留槽 → 写出 → 发布；arena 满回 NoResource | Duplicate | P1 | `ou::handle_duplicate` |
| `Status ours_handle_replace(HandleValue handle, Rights rights, HandleValue *out)` | 原子地换权。除了 BadHandle，其余情况源句柄一律失效；rights 越权时也关掉源句柄并回 InvArgs | 无 | P1 | `ou::handle_replace` |
| `Status ours_object_get_info(HandleValue handle, InfoTopic topic, void *buffer, usize buffer_size, usize *actual, usize *avail)` | topic 高 4 位是版本号。单记录 topic 缓冲不够回 BufferTooSmall；数组类 topic 写回 actual/avail。topic 列表见 4.4 | HandleValid/HandleBasic 不要求；其余要 Inspect；枚举类另要 Enumerate | P1 | `ou::object_get_info` |
| `Status ours_object_get_property(HandleValue handle, Property property, void *value, usize value_size)` | Name（`MAX_NAME_SIZE` 32 字节，含 NUL）；ProcessVdsoBaseAddress（只读，launcher 绑定导入时要用） | GetProperty | P1 | `ou::object_get_property` |
| `Status ours_object_set_property(HandleValue handle, Property property, void const *value, usize value_size)` | Name 截断到 31 字节。RegisterFs/RegisterGs 只能设调用线程自己，否则 AccessDenied；这是 TLS 的唯一入口 | SetProperty | P1 | `ou::object_set_property` |
| `Status ours_object_signal(HandleValue handle, Signals clear_mask, Signals set_mask)` | 先清后置，一次 `update_state` 完成。只允许 USER0..7 和该类型声明的 ExtraSignals（Event/EventPair 的 SIGNALED），否则 InvArgs | Signal | P1 | `ou::object_signal` |
| `Status ours_object_signal_peer(HandleValue handle, Signals clear_mask, Signals set_mask)` | 同上，作用在对端。对象不是 peered 回 NotSupported，对端已关回 PeerClosed | SignalPeer | P1 | `ou::object_signal_peer` |
| `Status ours_object_wait_many(WaitItem *items, usize count, Instant deadline)` | `WaitItem{HandleValue handle; Signals waitfor; Signals pending;}`。count ≤ `WAIT_MANY_MAX_ITEMS`(64)；任一项满足就返回，每一项都写回 pending | 每项都要 Wait | P1 | `ou::object_wait_many` |
| `Status ours_handle_close_many(HandleValue const *handles, usize count)` | 逐个关闭，遇错不中止；只要有无效值，最终回 BadHandle | 无 | P2 | `ou::handle_close_many` |
| `Status ours_object_get_child(HandleValue handle, KoId koid, Rights rights, HandleValue *out)` | 按 koid 取子 job、进程或线程的句柄，权限不超过父句柄 | Enumerate | P2 | `ou::object_get_child` |

### 2.2 B 任务

共同规则：
- **状态机**：Process 是 `Initial → Running → Dying → Dead`；Thread 是 `Initial → Running ⇄ Suspended → Dying → Dead`，把 Zircon 的 INITIAL/INITIALIZED 合成了一个。
- **一把进程锁**（`Dispatcher::get_lock()`）同时保护 state、线程表和 suspend_count，删掉独立的 `m_thread_list_lock`。“线程变为可运行、挂进线程表、进程 Initial→Running”这三件事必须在同一段锁内完成。
- **`task::Thread` 延迟到 start 时才创建。** ours 没有 Zircon 的 `Forget()`，一个从未启动的核心线程无法回收。tid 用 dispatcher 的 koid（改用 `spawn_etc`）。
- **锁序**：Job → Process → ThreadDispatcher → `task::Thread::m_pi_lock` → runqueue。不引入全局 thread_lock。进程从 job 摘除（`remove_child_process`）必须在进程锁外进行，这是 Zircon fxbug 42105735 那个死锁的教训。
- **最后一个引用**：运行中的进程由自己的线程保活（`task::Thread::m_user_thread → ThreadDispatcher → ProcessDispatcher`）；job 对子进程只持弱引用。
- **退出码**是 i64，被杀码沿用 Zircon 的数值，命名为 `TASK_RETCODE_*`。
- **用户态故障**（P1）：`faults.cpp` 的用户分支经 `task::Thread` 上调到 `ThreadDispatcher::on_user_exception()`。它打印“进程 / 线程 / koid / 异常类型 / rip / cr2”，然后以 `TASK_RETCODE_EXCEPTION_KILL` 杀掉进程。非 vDSO 陷入走同一条路径。

各对象的现状、rights 与 signals：

| 对象 | 现状 | 默认 rights | signals |
|---|---|---|---|
| Job | stub：`job_dispatcher.hpp:13` 的默认 rights 是 `Rights::None`，类里只有 `root_job()` | 见 4.2 | TERMINATED、JOB_NO_JOBS、JOB_NO_PROCESSES |
| Process | stub：`process_dispatcher.cpp:39-85` 的 `create()` 能用；`:87-101` 的 `attach_thread` 在锁外执行，`detach_thread` 是 Unimplemented；State 从未迁移过 | 见 4.2 | TERMINATED |
| Thread | stub：`thread_dispatcher.cpp` 的 spawn/start 能进 ring 3；State 只有 Running；`on_*` 是空的，`on_exit` 不清 `m_kernel_thread`；`kernel/lib/syscall/src/task.cpp` 的 `sys_thread_*` 全是 Unimplemented，也没进表 | 见 4.2 | TERMINATED、THREAD_RUNNING、THREAD_SUSPENDED |

Exception、SuspendToken、Profile、JobPolicy 都是 missing（`faults.cpp:66-136` 对用户态故障一律 panic）。

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_process_create(HandleValue job, char const *name, usize name_size, ProcessCreateOptions options, HandleValue *process_out, HandleValue *root_vmar_out)` | 在 job 下建一个 Initial 进程，同时返回 root VMAR（`ProcessDispatcher::create` 已经是这样）。返回前由对象层映射 vDSO，并把范围登记到 `VmAspace`（D2）。名字截断到 31 字节；job 已 Dying/Dead 回 BadState；options 只接受 None | job: ManageProcess | P1 | `ou::process_create` |
| `Status ours_process_start(HandleValue process, HandleValue thread, VirtAddr entry, VirtAddr stack, HandleValue arg1, usize arg2)` | 只用于 Initial 进程的首线程；进程没登记 vDSO 回 BadState；entry/stack 必须是规范的用户地址。arg1 一进调用就从调用者的表里摘下，成败都被消耗，装进目标表后新值放 rdi。arg2 原样放 rsi，约定为 vDSO 基址。上面三件事在同一段进程锁内完成。内核起 userboot 也走同一个 `ProcessDispatcher::start` | process: Write；thread: Write；arg1: Transfer | P1 | `ou::process_start` |
| `[[noreturn]] void ours_process_exit(i64 retcode)` | 第一次进入 Dying 时记下 retcode，对其余线程发 Kill，当前线程退出。最后一个线程摘下后进程转 Dead，在锁外执行 finish_dead_transition：清句柄表、`VmAspace::destroy`、置 TERMINATED、从 job 摘除。vDSO 桩在 syscall 之后跟一条 `ud2` | 无 | P1 | `ou::process_exit` |
| `Status ours_task_kill(HandleValue task)` | job/process/thread 通用，异步执行，以 TERMINATED 置位为完成。进程的 retcode 为 `TASK_RETCODE_SYSCALL_KILL`；Initial 状态的进程直接转 Dead；根 job 被 kill 就停机 | Destroy | P1 | `ou::task_kill` |
| `Status ours_thread_create(HandleValue process, char const *name, usize name_size, ThreadCreateOptions options, HandleValue *out)` | 只建一个 Initial 状态的 ThreadDispatcher，不建 `task::Thread`；没启动就关掉最后一个句柄时，对象直接析构 | process: ManageThread | P1 | `ou::thread_create` |
| `Status ours_thread_start(HandleValue thread, VirtAddr entry, VirtAddr stack, usize arg1, usize arg2)` | 启动非首线程，进程必须已是 Running；arg1/arg2 是原始值，不是句柄。前置条件：x86 TLB shootdown、缺页路径取 VMO 锁 | ManageThread | P1 | `ou::thread_start` |
| `[[noreturn]] void ours_thread_exit()` | Dying → Dead → 清 `m_kernel_thread` → `remove_thread`；最后一个线程退出时进程随之转 Dead | 无 | P1 | `ou::thread_exit` |
| `Status ours_job_create(HandleValue parent, JobCreateOptions options, HandleValue *out)` | 子 job 持有 `Arc<parent>`，parent 只记弱链接；高度 ≤ `JOB_MAX_HEIGHT`(32)；子 job 继承 policy 且只能收紧 | ManageJob | P1 | `ou::job_create` |
| `Status ours_thread_read_state(HandleValue thread, ThreadStateKind kind, void *buffer, usize buffer_size)` / `ours_thread_write_state(…)` | 只在线程 Suspended 或阻塞在异常里时可用；GeneralRegs 包含 fs_base/gs_base | Read / Write | P2 | `ou::thread_read_state` / `ou::thread_write_state` |
| `ours_task_create_exception_channel(HandleValue task, ExceptionChannelOptions options, HandleValue *out)`、`ours_exception_get_thread`、`ours_exception_get_process` | Exceptionate 以组合方式放进 Thread/Process/Job；投递顺序 thread → process → job → kill | Inspect+Duplicate+Transfer+ManageThread；job/process 另要 Enumerate | P2 | 同名 |
| `Status ours_task_suspend(HandleValue task, HandleValue *token_out)` | SuspendToken 活着，目标就停着；关掉令牌即恢复。前置：`suspend_or_kill` 补上 Initial 状态的处理 | Write | P2 | `ou::task_suspend` |
| `Status ours_job_set_critical(HandleValue job, JobCriticalOptions options, HandleValue process)` | 进程死则 kill 整个 job。P1 只做内核内部版：init 对根 job 是 critical，init 死时打印名字和 retcode 后 panic | job: Destroy；process: Wait | P2 | `ou::job_set_critical` |
| `Status ours_job_set_policy(HandleValue job, JobPolicyOptions options, JobPolicyTopic topic, void const *policy, u32 count)` | 只能在 job 没有子节点时设置；子 job 只能更严 | SetPolicy | P2 | `ou::job_set_policy` |
| `ours_profile_create(HandleValue system_resource, ProfileCreateOptions options, ProfileInfo const *info, HandleValue *out)`、`ours_thread_set_profile(HandleValue thread, HandleValue profile, ThreadSetProfileOptions options)` | 只暴露 priority 和 affinity；`ProfileInfo` 定长，保留字节必须为 0（字段有效位叫 `ProfileInfoOptions`）。经 `task::Thread::override_base_profile` 与 `set_cpu_affinity_soft` 生效，不碰 fair.* | System 资源；thread: ManageThread；profile: ApplyProfile | P2 | 同名 |
| `ours_process_read_memory` / `ours_process_write_memory` | 经目标进程的 aspace 读写，不经当前 CR3 | Read+Write / Write | P2 | 同名 |

### 2.3 C 内存

规则：
- **命名**：对象层和 ABI 统一叫 Vmar（`VmarDispatcher`、`ours_vmar_*`、`RIGHTS_DEFAULT_VMAR`），`mm::VmArea` 不改名。改名放在第一个 vmar 系统调用之前做（P1-C）。
- **dispatcher 签名不再暴露 mm 类型**：`VmarDispatcher::map(VmOptions, usize vmar_offset, VmoDispatcher &, u64 vmo_offset, usize length, Rights vmar_rights, Rights vmo_rights) -> Result<VirtAddr>`。翻译成 MmuFlags/VmPlacement 的工作在 dispatcher 内完成。偏移一律是字节偏移。
- **提交策略**：用户的 `vmo_create` 一律按需提交（Lazy）；`create_paged(Pinned)` 只留给内核内部使用（userboot 的栈、vDSO、BootFS）。
- **映射上限**：`vmar_map` 从第一天起就保存 per-mapping 上限，即 vmar rights ∩ vmo rights ∩ 区域的 CanMap*。`vmar_protect` 必须和这个上限在同一个提交里上线。今天 `vm_area.cpp:136-143` 直接拿区域的 `m_vmaf` 当上限，只读 BootFS 会因此被 protect 成可写。
- **W^X**：系统调用层拒绝 PermWrite+PermExecute 的映射；launcher 为每个 PT_LOAD 建一个 VMO，按 p_flags 映射。`RIGHTS_DEFAULT_VMO` 暂时带 Execute，作为临时偏离记录在案。
- **共享 VMO 整体提交**：多个进程共享的 VMO（vDSO、BootFS）在创建时整体提交。缺页路径不取 VMO 锁（`vm_mapping.cpp:391-500`），惰性共享的 VMO 会被并发地往 page map 里插页。

现状：
- Vmo：stub。`vmo_dispatcher.cpp:22-65` 只有内核内部的 create；`vm_object_paged.cpp:103-113` 的 read/write 是 Unimplemented；没有 `size()`。
- Vmar：stub。`vma_dispatcher.cpp:24-68` 有 create 和 map，但 map 暴露 mm 类型、没有 vmo 偏移，rights 推导还是 TODO（`:38`）。
- vDSO VMO：missing。`loader.cpp:108-146` 给每个进程复制一份。
- BootFS VMO：missing。解压缓冲走 vmalloc，物理上不连续。
- Pager：stub，只有一个头文件。
- Stream：missing。

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_vmo_create(u64 size, VmoCreateOptions options, HandleValue *out)` | 按需提交。size 按页取整，超过 `VMO_MAX_SIZE` 或取整溢出回 OutOfRange。options 只接受 None，Resizable 回 NotSupported | 无（P2 加 job policy） | P1 | `ou::vmo_create` |
| `Status ours_vmo_get_size(HandleValue vmo, u64 *size)` | 返回按页取整后的大小；依赖新增的 `VmObject::size()` | 无 | P1 | `ou::vmo_get_size` |
| `Status ours_vmo_read(HandleValue vmo, void *buffer, u64 offset, usize length)` / `Status ours_vmo_write(HandleValue vmo, void const *buffer, u64 offset, usize length)` | 字节偏移；越界或溢出回 OutOfRange；未提交的页读出为零；不在自旋锁下拷贝；physical VMO 回 NotSupported | Read / Write | P1 | `ou::vmo_read` / `ou::vmo_write` |
| `Status ours_vmo_op_range(HandleValue vmo, VmoOp op, u64 offset, u64 size, void *buffer, usize buffer_size)` | Commit / Decommit / Zero。VmoOp 是枚举，不是位掩码，所以不叫 *Options。malloc 释放内存靠 Decommit | Write | P1 | `ou::vmo_op_range` |
| `Status ours_vmar_map(HandleValue vmar, VmOptions options, usize vmar_offset, HandleValue vmo, u64 vmo_offset, usize length, VirtAddr *mapped_addr)` | 7 个参数，第 7 个走 r12。PermWrite 和 PermExecute 都要求同时有 PermRead；拒绝 Write+Execute；每一位 Perm 都必须被两边句柄允许，交集存为映射上限。带 Specific 时 vmar_offset 相对 vmar 起点。vmo_offset 必须页对齐，且 `vmo_offset + length ≤ size`，否则 OutOfRange。自动加 `MmuFlags::User`，不预先提交 | vmo: Map；每位 Perm 两边都要对应的 Read/Write/Execute | P1 | `ou::vmar_map` |
| `Status ours_vmar_unmap(HandleValue vmar, VirtAddr addr, usize length)` | addr 未对齐回 InvArgs（mm 会悄悄对齐，所以 dispatcher 先拦）；与 vDSO 区间相交回 AccessDenied（由 mm 执行）；只部分覆盖子区域时拒绝 | 整块覆盖子区域时需 OpChildren | P1 | `ou::vmar_unmap` |
| `Status ours_vmar_allocate(HandleValue parent, VmOptions options, usize offset, usize size, HandleValue *child, VirtAddr *child_addr)` | 只接受 CanMapR/W/X 与 Specific；子区域 rights = 默认值 + 由 CanMap 位推出的 R/W/X。用于栈 guard 和 PIE 整体预留 | 每个 CanMap 位都要求 parent 有对应 rights | P1 | `ou::vmar_allocate` |
| `Status ours_vmar_protect(HandleValue vmar, VmOptions options, VirtAddr addr, usize length)` | 新权限必须 ⊆ 映射上限且 ⊆ vmar 句柄的 rights；对 vDSO 拒绝。**只能和 per-mapping 上限同一个提交上线** | 对应的 Read/Write/Execute | P1 | `ou::vmar_protect` |
| `Status ours_vmar_destroy(HandleValue vmar)` | 拆掉所有映射和子区域，并把区域标为失效；root 回 NotSupported；包含 vDSO 的拒绝。前置：`VmArea::destroy` 改为公开并持锁 | OpChildren | P1 | `ou::vmar_destroy` |
| `ours_vmo_set_size`、`ours_vmo_create_child`（Slice，Snapshot 先用急切复制）、`ours_vmo_create_physical(HandleValue resource, PhysAddr paddr, usize size, HandleValue *out)`、`ours_vmo_set_cache_policy`、`ours_vmo_replace_as_executable`、`ours_vmo_create_contiguous`、`ours_vmar_op_range` | 形状照 Zircon。physical 要求 Mmio 资源覆盖所请求的范围；cache policy 要求 arch 先有 WC/UC 位 | 同 Zircon | P2 | 同名 |
| `ours_pager_*`、`ours_stream_*` | 依赖 Port 和 PageSource 的读请求协议；`pager_dispatcher.hpp` 当作脚手架保留 | 同 Zircon | P2 | 同名 |

### 2.4 D IPC 与同步

规则：
- **Channel 默认 rights** 照 Zircon，**不含 Duplicate**：一个端点只能有一个持有者。startup 设计里带 Duplicate 的写法作废。
- **信号**：READABLE（队列非空）、WRITABLE（只表示对端存活，不表示背压）、PEER_CLOSED、USER0..7。
- **P0 的锁**：两端共享 PeerHolder 里的一把 `sync::Mutex`，锁序固定为 PeerHolder → 端点的 `m_lock`，`update_state` 在后者之下执行。把锁指针挪进 Dispatcher 基类（Zircon 的共享锁形态）推迟到 EventPair/Socket 出现时再做。
- **MessagePacket**：P0 里包头和字节分两次 kmalloc，`CHANNEL_MAX_MSG_BYTES = 4096`（等于 `heap::KMALLOC_MAX_SIZE`），不走 vmalloc。原因是 vmalloc 全系统只有 256 个活跃区间，而且 `vfree` 没有 TLB shootdown（`docs/repairs/260908/heap.md #2`）。P1 换成 BufferChain 定长块链后升到 65536，放宽上限是兼容变更。每个端点最多挂 `CHANNEL_MAX_PENDING_MESSAGES = 64` 条消息。消息的销毁放在放锁之后。
- **txid 约定**现在就写进文档：消息前 4 字节是 txid，用户自选的 txid 必须 < 0x8000_0000。内核只在 P1 的 channel_call 里用它。

现状：Channel 为 stub（只有 `create()` 声明，第三个出参类型写成了 `Signals*`，BUILD.gn 里没有 .cpp）。Event、EventPair、Port、Futex、Socket、Fifo 都是 missing。另外 `sync::Semaphore` 是桩，`OwnedWaitQueue` 的退避协议是占位代码。

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_channel_create(ChannelCreateOptions options, HandleValue *out0, HandleValue *out1)` | 建一对共享 PeerHolder 的端点，两端初始都置 WRITABLE。options 必须为 None。两个出参都写出成功后才发布 | 无（P2 加 job policy） | P0 | `ou::channel_create` |
| `Status ours_channel_write(HandleValue channel, ChannelWriteOptions options, void const *bytes, u32 num_bytes, HandleValue const *handles, u32 num_handles)` | 原子地排进对端队列，并给对端置 READABLE；字节先拷进 MessagePacket 再取锁。超过 `CHANNEL_MAX_MSG_BYTES` 回 OutOfRange；对端已关回 PeerClosed；对端队列满回 NoResource，不清 WRITABLE。**P0：num_handles > 0 回 NotSupported。** P1：handles 一进调用就摘下，成败都被消耗；handles 含 channel 自身回 NotSupported；同一值出现两次回 BadHandle | channel: Write；P1 起每个被传的句柄要 Transfer | P0 | `ou::channel_write` |
| `Status ours_channel_read(HandleValue channel, ChannelReadOptions options, void *bytes, HandleValue *handles, u32 num_bytes, u32 num_handles, u32 *actual_bytes, u32 *actual_handles)` | 非阻塞地取队首消息。队空且对端存活回 ShouldWait；队空且对端已关回 PeerClosed。缓冲不够回 BufferTooSmall，写出所需的 actual_*，消息留在队首。读空后清 READABLE。8 个参数，第 7、8 个走 r12/r13。P0 的 actual_handles 恒为 0 | Read | P0 | `ou::channel_read` |
| `Status ours_channel_call(HandleValue channel, ChannelCallOptions options, Instant deadline, ChannelCallArgs const *args, u32 *actual_bytes, u32 *actual_handles)` | 内核生成 txid（≥ 0x8000_0000）写进前 4 字节，按 txid 把回复定向投递给调用者，并经 OwnedWaitQueue 把调用者的调度属性继承给服务端。被挂起后能重入（拆成 call_noretry + call_finish）。在它落地前，`ou::channel_call` 用 write + wait_one + read 组合实现，单线程、单个未决请求时两者等价 | Read+Write | P1（后期） | `ou::channel_call` |
| `Status ours_event_create(EventCreateOptions options, HandleValue *out)` | 状态就是 `m_signals`，用 object_signal 置或清 SIGNALED | 无 | P1 | `ou::event_create` |
| `Status ours_eventpair_create(EventPairCreateOptions options, HandleValue *out0, HandleValue *out1)` | 生命期令牌：一端最后一个句柄关闭，对端置 PEER_CLOSED | 无 | P1 | `ou::eventpair_create` |
| `Status ours_port_create(PortCreateOptions options, HandleValue *out)` | 包队列加等待者队列（补完 `sync::Semaphore`，或直接建在 `task::WaitQueue` 上）；在途包数有上限 | 无 | P1 | `ou::port_create` |
| `Status ours_port_queue(HandleValue port, PortPacket const *packet)` / `Status ours_port_wait(HandleValue port, Instant deadline, PortPacket *packet)` | PortPacket 与 Zircon 一样是 48 字节。queue 强制 type=User，超过上限回 ShouldWait | Write / Read | P1 | `ou::port_queue` / `ou::port_wait` |
| `Status ours_object_wait_async(HandleValue handle, HandleValue port, u64 key, Signals signals, WaitAsyncOptions options)` / `Status ours_port_cancel(HandleValue port, HandleValue source, u64 key)` | 挂一个一次性的 PortObserver，电平触发。锁序：对象锁 → port 锁。observer 超过上限回 NoResource。Edge/Timestamp 选项放 P2 | handle: Wait；port: Write | P1 | `ou::object_wait_async` / `ou::port_cancel` |
| `Status ours_futex_wait(Futex const *value_ptr, Futex current_value, HandleValue new_futex_owner, Instant deadline)` / `Status ours_futex_wake(Futex const *value_ptr, u32 wake_count)` | Futex = i32。每个进程一个 FutexContext（桶用自旋锁，值里放 `OwnedWaitQueue`），FutexState 由线程创建时各贡献一个。前置：`OwnedWaitQueue` 的 ChainLock 式退避协议（今天是占位，`docs/repairs/260915/sync-mutex.md #1`），以及持桶锁时不会缺页睡眠的用户读 | 无 | P1（与 thread_start 同批） | `ou::futex_wait` / `ou::futex_wake` |
| `ours_futex_requeue`、`ours_futex_wake_single_owner`、`ours_futex_get_owner` | condvar broadcast 与 PI mutex 交接 | 无 | P2 | 同名 |
| `ours_socket_*`、`ours_fifo_*`、`ours_channel_write_etc` / `ours_channel_read_etc` | 形状照 Zircon；Socket 先只做 Stream | 同 Zircon | P2 | 同名 |

### 2.5 E 时间

现状：`time::SteadyClock`、`time::Timer`/`TimerQueue` 在内核里已经实现，用户侧没有任何入口；`time::current_mono_time()` 只有声明没有定义；唯一登记的时钟源是 HPET（MMIO，用户态读不到）。

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Instant ours_clock_get_monotonic()` | 自启动以来的单调纳秒。先以陷入实现，经 `object::monotonic_now()` 门面返回 `SteadyClock::now()`。P2 改为在 vDSO 里用 invariant TSC 计算，前提是先规定“用户态启动后时钟源冻结”，因为 ours 的 Clock 会在更好的时钟源登记时重新锚定 | 无 | P1 | `ou::clock_get_monotonic` |
| `Instant ours_deadline_after(i64 nanoseconds)` | 当前时间加时长，溢出时饱和到 `INFINITE_TIME`。vDSO 能编 C++ 之前也以陷入实现 | 无 | P1 | `ou::deadline_after` |
| `Status ours_nanosleep(Instant deadline)` | 经 `ThreadDispatcher::sleep_until` 执行；deadline 已过就让出 CPU；被 kill 时可中断返回 | 无 | P1 | `ou::nanosleep` |
| `Status ours_timer_create(TimerCreateOptions options, ClockId clock, HandleValue *out)` / `Status ours_timer_set(HandleValue timer, Instant deadline, i64 slack)` / `Status ours_timer_cancel(HandleValue timer)` | 内嵌一个 `time::Timer` 加一个 `ours::Dpc`。Timer 回调在中断上下文里只负责排 Dpc，Dpc 持锁置 SIGNALED。`on_zero_handles` 时先 `Timer::cancel()`，再取消 Dpc。ClockId 挪进 system/public | create 无；set/cancel: Write | P1 | `ou::timer_create` / `ou::timer_set` / `ou::timer_cancel` |
| `ours_clock_get_boot`、`ours_ticks_get`、`ours_ticks_per_second`、`ours_clock_create` / `ours_clock_read` / `ours_clock_update`（UTC 由用户态的 timekeeper 推进） | 形状照 Zircon；clock 的参数带版本号或 size | 同 Zircon | P2 | 同名 |

### 2.6 F 资源与设备

规则：
- **删掉 `RsrcKind::Root`。** 种类只有 Mmio、Irq、IoPort、System，枚举写 `: u32` 并给显式值，编号 3 留作空洞（与 Zircon 相同）。
- **区间分配器**：每个种类一个，在 heap 初始化之后由平台钩子建立。建立时按独占方式扣掉内核自用的范围：RAM、内核映像、HPET/LAPIC/IOAPIC 的 MMIO、COM1 0x3F8-0x3FF、PIT 0x40-0x43、PIC 0x20/0xA0、IRQ0 和 COM1 的 IRQ 线。
- **System 槽位**：System 种类按 base 切成 size=1 的槽位，包括 Debug、Debuglog、Info、Cpu、Power、Vmex、Cprng。
- **P1 要做的非系统调用工作**：重写 `ResourceDispatcher::create`（今天它自相矛盾，而且不构造对象）；补完 `userabi::make_resource_handle`（今天是空函数）；把四个 ranged root 放进给 init 的 bootstrap 消息。

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_resource_create(HandleValue parent, ResourceKind kind, ResourceCreateOptions options, u64 base, usize size, HandleValue *out)` | parent 必须与 kind 同种类，并完整包含 [base, base+size)，回绕回 InvArgs。带 Exclusive 时在分配器里独占登记该区间。System 种类的 size 必须为 1。最后一个句柄关闭时释放独占区间 | parent: Write | P2 | `ou::resource_create` |
| `ours_interrupt_create` / `_wait` / `_ack` / `_destroy` / `_trigger` / `_bind` | InterruptDispatcher 以组合方式持有一个 `irq::IrqObserver`，自身状态由自己的 SpinLock 保护。前置：IrqObserver 的回调要带上下文指针（今天固定传 nullptr）、irq 层要有 mask/unmask/ack、GSI 映射要成为接口 | resource: Irq 种类且覆盖 vector | P2 | 同名 |
| `Status ours_ioports_request(HandleValue resource, u16 io_addr, u32 length)` / `ours_ioports_release(…)` | 进程级的 IoBitmap，上下文切换时装进 TSS | resource: IoPort 种类且覆盖区间 | P2 | 同名 |

### 2.7 G 诊断与系统信息

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_debug_write(char const *buffer, usize length)` | 已实现。P0：改走 UserPtr，并按 `ProcessDispatcher::current()` 给每行加上“<进程名>: ”前缀，否则日志里分不清 init 和 shell。P1：改为经 `logz4::ring::append` 写入，让用户输出出现在 panic dump 里，并放到启动选项 `kernel.enable-debugging-syscalls` 之后 | 无（受启动选项控制） | P0 | `ou::debug_write` |
| `Status ours_command_list(char *buffer, usize length)` | 已实现，kshell 专用，不进入稳定 ABI（生成器里给它打 debug 属性）。P2 改名为 `ours_debug_command_list(HandleValue resource, …)` 并要求 System/Debug 资源，或者随 shell 演示一起删掉 | 今天无 → System/Debug | 维持现状 | `ou::command_list` |
| `Status ours_debuglog_create(HandleValue resource, LogCreateOptions options, HandleValue *out)` | resource 必须是 System/Debuglog 槽位；带 Readable 选项时额外授予 Read。init 的第一个 Log 句柄由 userboot 直接放进 bootstrap 消息，不走这个调用 | resource: 种类与槽位匹配 | P1 | `ou::debuglog_create` |
| `Status ours_debuglog_write(HandleValue log, LogWriteOptions options, void const *buffer, usize length)` | 附上 severity、调用者的 process/thread koid 和单调时间，写进 `logz4::ring`；超长截断，不报错；读端的唤醒经 Dpc | Write | P1 | `ou::debuglog_write` |
| `Status ours_debuglog_read(HandleValue log, LogReadOptions options, void *record, usize record_size)` | 按句柄上的游标读下一条；没有记录回 ShouldWait；被覆盖跳过的条数写进 dropped。前置：logz4 ring 支持按 ticket 读、append 有通知钩子、记录带 pid/tid | Read | P1 | `ou::debuglog_read` |
| `void ours_cprng_draw(void *buffer, usize length)` | vDSO 按 256 字节分块，循环调用内部的 `ours_cprng_draw_once`。后者不进 `<ours/vdso.hpp>`，但内核照公开 ABI 校验它的参数。参数非法时终止线程。前置：内核 CPRNG（ChaCha20 构造，用 RDRAND/RDSEED/TSC 抖动播种） | 无 | P1 | `ou::cprng_draw` |
| `u32 ours_system_get_num_cpus()`、`u32 ours_system_get_page_size()`、`u64 ours_system_get_physmem()` | 先以陷入实现，结果写进 `frame->ax`；以后改成读 vDSO 常量，程序无感。physmem 需要新增 `pmm::total_managed_bytes()` | 无 | P1 | 同名 |
| `ours_system_get_version_string`、`ours_system_get_features`、`ours_system_powerctl`（System/Power）、`ours_cprng_add_entropy`（System/Cprng）、`ours_debug_read` / `ours_debug_send_command`（System/Debug） | 形状照 Zircon；add_entropy 要求资源凭证，这是把 Zircon 在 fidl 文档里自述的 BUG 落实下来 | 见左栏括号 | P2 | 同名 |

### 2.8 I 临时启动调用

| 签名 | 语义 | rights | 优先级 | ou 入口 |
|---|---|---|---|---|
| `Status ours_process_spawn_boot(HandleValue job, char const *name, usize name_size, HandleValue bootstrap)` | **临时。** bootstrap 一进调用就从调用者的表里摘下，成败都被消耗。name 最长 63 字节且不含 `/`（这样就排除了 `vmours/image.elf`），在常驻内核包里按名查找，找不到回 NotFound。之后走 `userabi::launch`，与内核起 init 是同一个函数：映射映像和 vDSO、绑定导入、登记 vDSO 范围、建栈、把 bootstrap 装进子进程的表并把值放进 rdi、启动。不返回进程句柄，因为 P0 没有 wait/kill 的需求 | job: ManageProcess；bootstrap: Transfer | P0（P1-C 删除） | `ou::process_spawn_boot` |

**这是对硬规则“系统调用处理函数只能碰 kernel/object”的显式违背。** 它会让 lib/syscall 新增一条到 `//kernel/lib/userabi` 的 GN 依赖，而 userabi 使用的 elfload 又直接调 `mm::get_frames_n` 和 PhysMap。

理由有三：
1. 这是唯一不要求先修好 `VmObjectPaged::write`、也不要求先做出用户态 libc、加载器和 Binder 移植的路径。
2. 处理函数本身只解析 dispatcher 句柄，装载代码与 `userboot_init` 是同一份，没有新增任何绕过对象层的代码。elfload 的绕行是已登记的欠账（`docs/repairs/260919/elfload.md`）。
3. 它只接受内核包里的文件，ring 0 不解析任何用户提供的字节。

约束：
- `vdso.hpp` 的注释里标明它是临时的；
- 在 `docs/repairs` 里登记删除条件：P1-C 的加载器落地后，删掉这个调用和那条 GN 依赖。

## 3 启动与运行环境

### 3.1 P0 时序

1. **内核**：`labour_routine → userabi::userboot_init()`。解压内核包，并把包视图**常驻**为 `userabi::KernelPackage` 单例（今天是局部变量，函数返回后就找不到了）。
2. 按 `kernel.select.userboot` 选程序：dev 镜像改为 `init`，dev.dbg_1 保持 `shell` 用作回归。
3. 调 `userabi::launch(root_job, "init", HandleOwner{root_job, RIGHTS_DEFAULT_JOB})`，依次做：
   - `ProcessDispatcher::create`；
   - `Loader::map` 映像与 vDSO，`Binder::bind` 绑定导入；
   - `set_vdso_code_range`；
   - 建 64 KiB 栈；
   - `ThreadDispatcher::spawn`；
   - `HandleTable::add` 把根 job 句柄装进新进程的表；
   - `start(entry, sp = ((top & ~15) - 8), arg1 = 句柄值, arg2 = vDSO 基址)`。
4. **init** 的 `_start(HandleValue job, VirtAddr vdso_base)`：调 `ou::channel_create(&a, &b)`。
5. init 调 `ou::process_spawn_boot(job, "shell", b)`。内核从 init 的表里摘下 b，在常驻包里找到 `shell`，调 `launch(job, "shell", b)`。
6. init 调 `ou::channel_write(a, "ping")`，再调 `ou::object_wait_one(a, READABLE+PEER_CLOSED, INFINITE_TIME)`。这是第一次真正睡眠的系统调用。
7. **shell** 的 `_start(HandleValue bootstrap, VirtAddr vdso_base)`：
   - `channel_read(bootstrap)` 回 ShouldWait 时，先 `wait_one` 再读；
   - 读到 "ping" 后回 `channel_write(bootstrap, "pong")`；
   - read 回 BadType 说明 shell 是被内核直接选中的（rdi 是 job 句柄），这时照旧跑 command_list 演示。
8. init 被唤醒，`channel_read` 读到 "pong"，打印出来，然后自旋（P1 之前没有 exit）。

### 3.2 P1 目标时序（内核 → userboot → init → shell）

1. **内核**：
   - 从常驻包建 BootFS VMO：只读，创建时整体提交，需要 `VmObjectPaged::write` 的内核缓冲版。
   - 建唯一的 vDSO VmoDispatcher，由内核持有、不外发，登记到对象层。
   - 调 `ProcessDispatcher::create(root_job, "userboot")`，它在内部映射 vDSO。
   - 内核 elfload 装载 userboot。
   - `channel_create`，写一条 processargs 消息，带上 PROC_SELF、THREAD_SELF、VMAR_ROOT、JOB_DEFAULT、VMO_BOOTFS、Log、四个 ranged root resource；environ 里放 OBI 命令行中所有 `userboot.*` 项。
   - 调 `ProcessDispatcher::start(arg1 = 用户端 channel, arg2 = vdso_base)`。
2. **userboot**（与内核同构建的小程序）：
   - runtime 读出消息，从 environ 取 `userboot.next`（默认是 init）；
   - 只读映射 BootFS；
   - 用 `system/lib/launch` 启动 init，把系统能力集整体转交；
   - 调 `process_exit(0)`。
3. **init**：持有根 job 和根资源；为服务建子 job；读 BootFS 里的服务清单，依次启动 logger、console、shell；用 `object_wait_one(TERMINATED)` 加 `get_info(Process)` 回收子进程，必要时重启。
4. **launcher 算法**（`system/lib/launch`）：
   1. `process_create`、`thread_create`；
   2. 对每个 PT_LOAD：`vmo_create` → `vmar_map` 进 launcher 自己（R+W）→ memcpy 段内容；
   3. 用 `get_property(ProcessVdsoBaseAddress)` 读子进程的 vDSO 基址；
   4. Binder 按 BootFS 里 `vdso` 文件的 .dynsym，在 launcher 自己的临时视图里写 GOT；
   5. `vmar_unmap` 临时视图，再按 p_flags `vmar_map` 进子进程。因为绑定已经完成，子进程直接以最终权限映射，RELRO 段也不需要 protect；
   6. 栈：`vmar_allocate` 留 guard，再 `vmo_create` + `vmar_map(R+W)`；
   7. `channel_create`，`channel_write` 写 processargs 并随消息转移句柄；
   8. `process_start(proc, thread, entry, sp, child_end, vdso_base)`；
   9. 关掉 child_vmar 和 parent_end；进程句柄用 `handle_replace` 降到 Wait+Inspect+Destroy。

### 3.3 引导协议

- **P0（临时）**：rdi 是一个句柄，含义由启动方与被启动方约定（init 拿到根 job，shell 拿到 channel 端）；rsi 是 vDSO 基址。
- **P1 起统一为**：rdi 是 bootstrap channel，rsi 是 vDSO 基址；channel 上的第一条消息是 processargs。**arg2 永远不放句柄值。** 全系统只有这一种格式、一个解析器，内核发给 userboot 的消息也用它。
- **格式**，定义在 `system/public/ours/process_args.hpp`：
  - 头部：`struct ProcArgs { u32 protocol, version, handle_info_off, args_off, args_num, environ_off, environ_num, names_off, names_num; }`，字段顺序与 `zx_proc_args` 相同；`PROCARGS_PROTOCOL` 用 ours 自己的 magic，避免让人误以为与 Zircon 线上兼容。
  - 每个句柄对应一个 u32：`handle_info(HandleType type, u16 arg) = u32(type) | u32(arg) << 16`，位布局同 `PA_HND`。
  - 字节布局：`[ProcArgs][u32 handle_info × nh][argv][environ][names]`，所有字符串以 NUL 结尾、首尾相接。消息里的句柄数组与 handle_info 按下标一一对应。上限就是 channel 的上限。
- **`enum class HandleType : u8`** 沿用 Zircon 的数值：ProcSelf 0x01、ThreadSelf 0x02、JobDefault 0x03、VmarRoot 0x04、VmarLoaded 0x05、VmoStack 0x13、VmoExecutable 0x14、VmoBootdata 0x1A、VmoBootfs 0x1B、NsDir 0x20、Fd 0x30、DirectoryRequest 0x3B、LogSink 0x3D、Resource 0x3F、MmioResource 0x50、IrqResource 0x51、IoportResource 0x52、SystemResource 0x54、User0..2 0xF0..0xF2。ours 自有的类型（如 Debuglog）放在 0xE0–0xEF。VmoVdso 0x11 保留不用，因为 vDSO VMO 不外发。

### 3.4 进程初始环境

| 项 | 约定 | 今天 |
|---|---|---|
| rip | ELF 的 e_entry | 同 |
| rdi | bootstrap 句柄值；0 表示没有任何句柄 | 恒为 0（`userboot.cpp:455`） |
| rsi | vDSO 基址，给 runtime 回溯和 P2 的 crt0 自绑定用 | 恒为 0 |
| rsp | `((top & ~15) - 8)`，即入口处 ≡ 8 (mod 16)，所以 `_start` 可以是 C++ 函数（与 Zircon 的 `compute_initial_stack_pointer` 相同）；crt0 用汇编写时再防御性地对齐一次 | 16 字节对齐（`userboot.cpp:209`），SSE 关着才没出事 |
| 其余通用寄存器 | 0 | `IrqFrame{}` 已做到 |
| rflags | 只置 IF；IOPL=0；DF=0 | IOPL=1（`kernel/arch/x86/kernel/include/ours/arch/entry.hpp:28`） |
| fs/gs base | 0；TLS 用 `set_property(RegisterFs)`（P1） | FSGSBASE 只在 CPU 支持时才开（`init.cpp:73`） |
| x87/SSE | 未启用，用户代码用 `-mno-sse` 编译 | 打开 SSE 之前必须先修好 rsp |
| 栈 | 64 KiB；ELF 的 PT_GNU_STACK p_memsz 非零时取它；栈下方一页不映射，作 guard | 一页（`userboot.cpp:210`） |
| 句柄表 | 只有启动方转进来的句柄；值是每进程不透明的 u32，低两位恒为 1 | 没有句柄表 |
| vDSO | 每个进程只映射一次，R+X，不可写、不可拆、不可改；范围记在 VmAspace | 每个进程拷一份，范围记在 ProcessDispatcher |

### 3.5 ELF 加载归属的分阶段路线

| 阶段 | 谁加载 | 谁把导入绑到 vDSO | 用到的接口 |
|---|---|---|---|
| P0 | 内核 `userabi::launch`。init 由内核直接起，shell 经 `process_spawn_boot` 起；只从内核包里取文件 | 内核 `elfload::Binder` | 不需要新的加载接口 |
| P1 | 父进程的 launcher，从 BootFS VMO 映射后自己解析 | 父进程 launcher 用移到 `system/lib/elfload` 的 Binder，在自己的临时视图里写 GOT | process_create/thread_create/process_start、vmo_create、vmar_map/unmap/allocate、get_property、channel_* |
| P2 | 同 P1 | 子进程的 crt0 用 rsi 里的基址自己绑定（先跑一段不需要重定位的自举代码） | 同 P1 |
| 之后 | userboot 改由 phys 加载（`HandoffEnd::Elf`），内核不再解析任何 ELF | — | — |

### 3.6 服务组织与用户库分层

- **userboot**（P1）：随内核一起构建的 launcher，是唯一理解内核引导消息的程序；把系统能力集转交出去后退出。
- **init**：放在 `system/service/init`，是第一个策略进程。P1 为服务和应用建子 job，负责启动、回收和重启服务。P1 后期提供 `/svc`：消息为 `{u32 ordinal = OPEN; u32 name_size; name}` 加一个 channel 的服务端句柄；init 维护“名字 → 提供者”的表，把请求转发到提供者的 outgoing 目录（DirectoryRequest 句柄）。IDL 放 P2，与系统调用描述共用生成器。
- **最小权限**：P0 的 init 不给 shell 任何 job 句柄；`job_create` 落地后再给子 job。
- **用户侧库分层**（由下到上）：
  1. `system/public/ours`：ABI 头。
  2. `system/lib/ou`：每个系统调用一个具名入口；P1 加 RAII 类型。
  3. `system/lib/libc`：把 `kernel/lib/libc/string.cpp` 挪过来，内核与用户共用，并补齐只声明未定义的函数（`docs/repairs/260919/libc.md`）。
  4. `system/lib/runtime`（新建）：汇编 crt0、processargs 解析、`ou::take_startup_handle(HandleType, u16)`、bump 分配器、调 main，main 返回后调 process_exit。它不并进 ou，因为 ou 的约定是“一个调用一个入口”。
  5. `system/lib/elfload`（Binder 与 Loader 算法，去掉对内核 logz4 胶水的依赖）、`system/lib/bootfs`、`system/lib/launch`。
  6. 程序：userboot、init、shell。`user_executable()` 默认链接 runtime 和 libc。

## 4 横切

### 4.1 句柄编码与 koid

- **编码**：句柄值 = `((base_value << 2) | 0b11) ^ (random << 2)`。base_value 的布局是 [31:30]=0、[29:18]=generation、[17:0]=arena 下标。`HANDLE_INVALID = 0`，公开 `HANDLE_FIXED_BITS_MASK = 0x3`。
- **解析过五道关**，失败统一回 BadHandle；类型不符回 BadType；权限不足回 AccessDenied：
  1. 低两位为 1；
  2. 异或去掉随机数；
  3. 下标落在已提交范围内；
  4. base_value 全等比较（P1 改成常数时间比较）；
  5. 句柄上记的 table koid 等于本表的 koid。用表 koid 而不是进程 koid，是为 create_shared 留余地。
- **random 必须预先左移 2 位**，这一点照 Zircon `handle_table.cc:50`。`docs/architecture/object/handle-lookup.md` 没写明，照字面做会把两个固定位翻掉；该文档要补一句，并把里面的 kCamelCase 常量改成 SCREAMING_SNAKE_CASE。
- **P0**：arena 固定 2^12 槽，一次提交；random = 0；每表一把 `sync::Mutex`。
- **P1**：预留 2^18 槽的虚拟地址，在 arena 锁下按页显式 commit+map（写法同 `Stack::init`）；random 从 CPRNG 取（QEMU 默认的 Nehalem 没有 RDRAND）；`sync::RwLock` 补完后换成读写锁。
- **出参句柄**：先预留槽算出值，再把所有出参 copy_to_user，全部成功后持表锁一次发布；任何一步失败都销毁预留。这样不存在“句柄已可见又被撤回”的窗口。
- **跨表转移**：在源表锁下摘除 → 改 table koid → 在目标表锁下装入，从不同时持有两把表锁。
- **koid**：P1 改成 u64，要赶在 get_info、ExceptionInfo、LogRecord 这些结构冻结之前。今天是 u32，`KoIdGenerator` 会回绕（`system/public/ours/types.hpp:36`）。tid 统一用 koid。

### 4.2 Rights

- **位值整体照搬 Zircon**：Duplicate 0、Transfer 1、Read 2、Write 3、Execute 4、Map 5、GetProperty 6、SetProperty 7、Enumerate 8、Destroy 9、SetPolicy 10、GetPolicy 11、Signal 12、SignalPeer 13、Wait 14、Inspect 15、ManageJob 16、ManageProcess 17、ManageThread 18、ApplyProfile 19、ManageSocket 20、OpChildren 21、Resize 22、AttachVmo 23、ManageVmo 24；`Rights::SameRights` 为 bit 31。删除 `Io` 和 `Default`。
- 下表中 BASIC = Transfer+Duplicate+Wait+Inspect，IO = Read+Write，PROPERTY = GetProperty+SetProperty。内核发出的句柄显式给出 rights，不依赖 `default_right()`。

| 对象 | RIGHTS_DEFAULT_* | 与 Zircon 相比 |
|---|---|---|
| Channel | BASIC − Duplicate + IO + Signal + SignalPeer | 相同 |
| Process | BASIC + IO + PROPERTY + Enumerate + Destroy + Signal + ManageProcess + ManageThread | 相同（今天是 Signal+Io） |
| Thread | BASIC + IO + PROPERTY + Destroy + Signal + ManageThread | 相同 |
| Job | BASIC + IO + PROPERTY + GetPolicy + SetPolicy + Enumerate + Destroy + Signal + ManageJob + ManageProcess + ManageThread | 相同（今天是 None） |
| Vmo | BASIC + IO + PROPERTY + Map + Signal **+ Execute** | 偏离：没有 vmex 之前临时带 Execute |
| Vmar | BASIC − Wait + OpChildren，再加由区域 flags 推出的 R/W/X | 相同 |
| Event / EventPair | BASIC + Signal；EventPair 再加 SignalPeer | 相同 |
| Port | BASIC − Wait + IO | 相同 |
| Timer / Log | BASIC + Write + Signal；Log 以 Readable 创建时加 Read | 相同 |
| Resource | BASIC − Wait + Write + GetProperty | 相同 |

### 4.3 Signals

| bit | 名字 | 适用对象 |
|---|---|---|
| 0 | READABLE | channel、socket、fifo、log |
| 1 | WRITABLE | channel（只表示对端存活）、socket、fifo |
| 2 | PEER_CLOSED | 所有 peered 对象 |
| 3 | SIGNALED | event、eventpair、timer；在 job/process/thread 上叫 TASK_TERMINATED |
| 4 | THREAD_RUNNING、JOB_NO_JOBS、CLOCK_STARTED、SOCKET_PEER_WRITE_DISABLED | 按对象类型复用 |
| 5 | THREAD_SUSPENDED、JOB_NO_PROCESSES、SOCKET_WRITE_DISABLED | 按对象类型复用 |
| 10 / 11 | SOCKET_READ_THRESHOLD / SOCKET_WRITE_THRESHOLD | socket |
| 23 | HANDLE_CLOSED | 只出现在 observed 里，从不存进对象状态，object_signal 不能动它 |
| 24–31 | USER0..USER7 | object_signal 可写 |

- `Kill`/`Suspend` 挪进内核私有的 `task::ThreadSignals`（名字见 D5），Process/Thread 的 ExtraSignals 改为 None。**这三件事必须在同一个提交里完成**：拆分、按新布局重排、改模板实参。否则在中间状态下，用户可以对一个进程 object_signal 出 READABLE。
- 同一对象类型内的别名位用 static_assert 保证互不重叠。

### 4.4 get_info 与 property

- **InfoTopic** 编号照搬 Zircon，高 4 位是版本号。
  - P1：HandleValid(1)、HandleBasic(2)、Process、Thread、Vmo、ProcessThreads、JobChildren、JobProcesses、HandleCount。
  - P2：Vmar、HandleTable、ProcessMaps、TaskStats、KmemStats。
- **记录结构**：`InfoHandleBasic{KoId koid; Rights rights; ObjectType type; KoId related_koid; u32 reserved; u8 padding[4];}`。所有公共 ABI 结构体都用 static_assert 钉住 sizeof 和 offsetof。
- **ObjectType** 编号照搬 Zircon：Process=1、Thread=2、Vmo=3、Channel=4、Event=5、Port=6、Resource=15、EventPair=16、Job=17、Vmar=18，其余照 `zircon/types.h`。
- **分派**：syscall 层按 ObjectType switch，downcast 后调具名方法，不给 dispatcher 加虚函数。
- **Property** 编号照 Zircon：RegisterGs 2、Name 3、RegisterFs 4、ProcessVdsoBaseAddress 6。

### 4.5 错误模型

- **编码**：`Status : u32`，Ok=0，其余都是正数。每一项写死显式数值，只追加、不改号。别名 `Unsupported`、`OutOfMem` 只为兼容保留，设计和新代码只用 NotSupported、NoMemory。
- **新增** BufferTooSmall、PeerClosed。
- **内部码隔离**：`InternalIrqKilled`、`InternalIrqRetry`、`Delayed` 移进内核私有的枚举，生成的 shim 用 DEBUG_ASSERT 保证它们不漏到用户态。唯一要跨过边界的是 blocking 调用的重试码，由 vDSO 桩自己消化。
- **越界调用号和非 vDSO 陷入**：P0 都回 BadSyscall（今天分别回 OutOfRange 和 AccessDenied，`table.cpp:113-115`、`:79-82`）。P1 的 fault-kill 落地后，非 vDSO 陷入改为打印报告并杀掉进程。
- **错误优先级**：BadHandle > BadType > AccessDenied > InvArgs，即先解析句柄、再检查参数。
- **出参**只在 Ok 时写入；唯一例外是 handle_replace 会让源句柄失效。
- **跨 ABI 的枚举**一律写 `: u32` 并给显式值。

### 4.6 用户指针拷贝

- **新库** `kernel/lib/user_copy`（include/src/test 结构），提供 `UserInPtr<T>`、`UserOutPtr<T>`、`UserInOutPtr<T>`、`UserOutHandle`，以及 `copy_from_user`、`copy_to_user`、`copy_array_*`。
- **做法**：先检查整个 [p, p+len) 都在用户半区且不回绕，否则回 InvArgs。`stac`/`clac` 只包住拷贝本身。（2026-09-24 修订：本内核不带 `.code-patches` 构建，所以不用代码补丁换 nop，而是 `user_copy.S` 里一个宏展开出带 `stac`/`clac` 与不带的两个变体，按 `feature_has_smap` 选。）
- **P0 不做故障恢复**（只跑受信程序），但缺页处理要把“内核态访问一个 P=1 的用户页”直接 panic，报 SMAP 违规。今天这种情况是活锁：`vm_mapping.cpp:443` 看到 PTE 已经存在就回 Ok，指令重试后再次缺页。改成 panic 至少失败得响亮。
- **P1 加故障恢复**：线程的 arch 状态里加 `data_fault_resume`；`x86_handle_page_fault` 遇到内核态访问用户地址时先按需调页，失败且设置了 resume 就把 ip 改到 resume 点。大约 30 行。（2026-09-24 修订：改 ip 还要求出错指令落在拷贝例程自己的区间 `[x86_user_copy_begin, x86_user_copy_end)` 里。只看槽的话，在没有 SMAP 的 CPU 上，拷贝途中来的中断若在处理函数里碰了用户地址，也会被送到拷贝的出口——带着中断的栈，等于一次跳到任意返回地址。）
- **使用规则**：
  - 持有 `VmAspace::m_lock`（自旋锁）或缺页路径会拿的任何锁时，不得拷贝用户内存；
  - 对象锁下也不拷贝：写入先拷进内核缓冲再取锁，读出先出队、放锁、再拷出；
  - futex 需要“持桶锁时读用户字”，用不会睡眠的 capture-faults 变体（P1）。
- **顺手改掉** `debug.cpp:35-37` 里“SMAP 没开”的过时注释：`mmu.cpp:41` 在 CPU 支持时就会打开 SMAP。

### 4.7 系统调用 ABI 的描述与生成

- **调用号**：用户只能通过 vDSO 导出的 `ours_*` 符号进入内核；调用号降为内核与 vDSO 之间的私有约定（D8）。`system/public/ours/syscall.hpp` 里“用户程序把号放进 rax”的注释作废。
- **寄存器**：参数依次是 rdi、rsi、rdx、r10、r8、r9、r12、r13。
  - 4 个及以上参数的桩要先 `mov %rcx, %r10`，因为 SYSCALL 会覆盖 rcx；
  - 第 7、8 个参数由桩先 push r12/r13，再从调用者栈上装入，syscall 之后 pop 回来。`SyscallFrame` 已经保存了这两个寄存器，`syscall.S` 的头注释也已写明这个约定；
  - 超过 8 个参数，生成器直接拒绝。
- **返回形态**有三种：Status（eax）、原始值（`Instant`、`u32` 等）、`[[noreturn]]`（syscall 之后跟 `ud2`）。
- **生成器**是 P1-A 的第一件事；P0 的 6 个桩手写。
  - 描述文件是 `//system/public/ours/syscalls.gni`，一个 GN scope 列表，每条包含 name、brief、args{name, type, dir, count, release}、attrs{blocking, noreturn, debug}；
  - 经 `generated_file(output_conversion="json")` 转成 JSON，交给只用标准库的 `//build/syscall/gen_syscalls.py`（宿主 python3 是 3.9，没有 tomllib）。
  - 产物七样：
    1. 私有调用号头；
    2. vDSO 桩汇编；
    3. `<ours/vdso.hpp>` 包含的 extern "C" 声明；
    4. 内核侧带类型的 `sys_*` 声明。处理函数的定义与描述不一致时会链接失败，今天四处手工同步之间缺的编译期关联由此补上；
    5. 帧拆解 shim 与分发表；
    6. `libours.ifs` 加 ABI 比对，`user_executable` 改为链接它；
    7. 链接后读 vDSO 的 `.symtab`，生成 `VDSO_SYSRET_OFFSET[]` 和 build-id。
  - 此后新增一个能力，人手只写三样：描述里的一条、`sys_<name>` 函数体、ou 入口。
- **精确的 vDSO 入口校验**（P1 后期，随生成器一起做）：判断 `ip - vdso_base == VDSO_SYSRET_OFFSET[min(ax, COUNT)]`。一次比较同时挡住越界调用号、跳进 vDSO 中间陷入、借 A 的 syscall 指令发 B 号调用这三种情况。在它落地之前，vDSO 的“内部调用”不构成安全边界，所以内核按公开 ABI 校验所有参数，生成器也暂不提供 vdso_only 属性。
- **lib/syscall 的依赖白名单**：`//kernel/object`、`//kernel/lib/user_copy`、arch 的 syscall 帧头文件。时钟、cprng、系统信息、日志这类无状态服务，经 kernel/object 里的薄门面访问（D15）。P0 的 spawn_boot 是唯一登记在案的例外。
- **这条白名单暂时无法机械检查**：`//kernel/object:headers` 的 public_deps 里有 `//kernel/mm:headers`，`process_dispatcher.hpp` 又 include 了 task 的头文件。要等对象层的公开头不再暴露 mm/task 类型之后才能检查。

### 4.8 锁序与资源记账

- **总序**：Job → Process → HandleTable → PeerHolder → Dispatcher `m_lock` → Port → VmAspace → VmCowPages → `task::Thread::m_pi_lock` → runqueue。这份总序写进 `docs/architecture/object/`，并用现有的 `lockdep::Guard` 给各锁分类，调试构建里就能抓到违规。
- **硬规则**：
  1. Handle 销毁和 `on_zero_handles` 不在表锁或进程锁下执行；
  2. 不在 VmAspace 锁或任何自旋锁下拷贝用户内存；
  3. 中断上下文里的生产者（Timer、Interrupt、Log）只投递 Dpc，由 Dpc 去调 `update_state`；
  4. 不引入全局 thread_lock，PI 链用 OwnedWaitQueue 的退避式加锁。
- **记账**（P1-C，与“句柄随消息转移”同批）：每个存活的 Handle（包括在途的）都记在创建它的进程名下，设 `HANDLE_MAX_PER_PROCESS` 上限。`CHANNEL_MAX_PENDING_MESSAGES`、`VMO_MAX_SIZE`、`WAIT_MANY_MAX_ITEMS` 都在公共头里给出具体数值。
- **P0 的“每进程单线程”约束**要写进 ou 与 runtime 的文档：今天没有 TLB shootdown（`docs/repairs/260919/x86-mm.md #1`），缺页路径也不取 VMO 锁。

## 5 第一条垂直切片（P0）

开工前先改写 `.agents/tasks/syscall-object-layer/DESIGN.md` 的 §2 和 §5：那份在途计划把 channel 放在 P2，不改的话，另一个会话会照旧顺序各做一半。

节奏：下表每一行是一次 build → 两个镜像都 boot → 一次提交 → 一条 `docs/changes/<YYMMDD>/<MODULE>.md` 记录。

| 步 | 内容 | 做完怎么验证 |
|---|---|---|
| 1a | FMASK 加 TF、DF、AC（`kernel/arch/x86/kernel/init.cpp:47-54`；对照 Linux `syscall_init` 的 SYSCALL_MASK） | dev 和 dev.dbg_1 都照旧启动，shell 打出命令表 |
| 1b | 用户 RSP 在换栈后压进 `SyscallFrame`（共 18 槽，16 字节对齐），出口从帧里恢复；同时关掉 `docs/repairs/260908/x86.md #1` | 同上；`syscall_dispatch` 里 `DEBUG_ASSERT(rsp % 16 == 0)` 不触发 |
| 1c | `arch_build_uspace_irqframe` 的 IOPL 改为 0 | 同上 |
| 1d | 分发前 `sti`，恢复用户 RSP 之前 `cli`（Zircon `do_syscall_pre`）；必须在 1b 之后做 | 同上；SMP 下 shell 连续多次 debug_write 没有异常 |
| 2 | 新建 `kernel/lib/user_copy`（范围检查 + stac/clac 补丁）；`debug.cpp`、`command.cpp` 改用它；缺页处理对 SMAP 违规 panic | 默认 Nehalem 和带 SMAP 的 CPU（qemu_boot.py 换成 `-cpu max`）下，shell 都能打出命令表。今天后者会在 debug_write 里活锁 |
| 3a | 拆分 Signals：公共 `ours::Signals` 改成 Zircon 布局，`task::ThreadSignals` 接收 Kill/Suspend，ExtraSignals 改 None | 构建通过、启动日志不变，SMP 调度行为不变 |
| 3b | Rights 改用 Zircon 位值；按 4.2 表修订各对象的 `RIGHTS_DEFAULT_*` | 构建并启动 |
| 3c | Status 显式编号，新增 BufferTooSmall、PeerClosed，内部码移到私有枚举，越界调用号回 BadSyscall | 构建并启动 |
| 3d | `Instant` 改为 i64，ABI 常量 `INFINITE_TIME` 改为 Instant 类型（chrono 版挪进内核 `time::`），加 `HANDLE_INVALID`、`CHANNEL_MAX_*`，`HandleValue` 改名（D4），改掉 `syscall.hpp` 的过时注释 | 构建并启动 |
| 4a | Dispatcher 加 `ObjectType` 字段和 `downcast`；修 attach/detach 的 canary 校验；`KernelHandle` 析构调 `on_zero_handles`，修 `reset()`（`docs/repairs/260906/object.md #1`） | 构建并启动 |
| 4b | 定义 `Handle::make/dup`；全局 arena（2^12 槽）；HandleTable（Mutex、五道关、random=0）；`ProcessDispatcher::handle_table()/current()`、`ThreadDispatcher::current()`；`table.cpp` 改经 `current()` 取当前进程 | 启动日志不变 |
| 4c | 给 `ProcessDispatcher` 加 `name()` 访问器；`sys_debug_write` 加“<进程名>: ”前缀 | shell 输出的每一行变成 `shell: …` |
| 5a | userabi 把根 job 句柄装进首个进程的表，rdi 放句柄值，rsi 放 vDSO 基址 | 内核日志出现 `userabi: root job -> shell as handle 0x…`；默认的 userboot 程序和 shell 都照旧运行 |
| 5b | `ours_handle_close` 全套（处理函数、桩、`vdso.hpp` 声明、`ou::handle_close`）；shell 的 `_start` 接收 bootstrap 参数，bootstrap 不是 channel 时做两次 close | dev.dbg_1 日志出现 `shell: close(bootstrap) = Ok` 和 `shell: close again = BadHandle` |
| 6a | `sync::Event` 构造函数；`Dispatcher::update_state`、电平检查、detach、`cancel(handle)`；`object::wait_one` | 构建并启动（功能在第 7 步由用户态验证，不为此加内核自测代码） |
| 6b | PeerHolder、`PeeredDispatcher` 构造函数、`ChannelDispatcher`（只传字节）、MessagePacket（两次 kmalloc，每端 ≤ 64 条）；`on_zero_handles` 给对端置 PEER_CLOSED 并清 WRITABLE | 同上 |
| 7 | `ours_channel_create/write/read`、`ours_object_wait_one` 的处理函数、桩（r10 以及 r12/r13）、声明和 ou 入口 | dev.dbg_1 里 shell 做自环测试，日志依次出现：`loopback ping ok`、`read on empty = ShouldWait`、`wait_one(READABLE) immediate ok`、`close peer -> PEER_CLOSED ok` |
| 8a | 把 `userboot_init` 的装载序列抽成 `userabi::launch(JobDispatcher &, name, HandleOwner bootstrap)`；KernelPackage 改为常驻单例；栈改 64 KiB，rsp ≡ 8 (mod 16) | 两个镜像的启动日志与改动前一致 |
| 8b | `ours_process_spawn_boot`（临时）全套，并在 `docs/repairs` 登记删除条件 | 构建通过；实际运行在第 9 步验证 |
| 9 | 新建 `system/service/init`（`user_executable`），加进 dev 镜像的 `kernel_package`，cmdline 改为 `kernel.select.userboot=init`；shell 增加 channel 分支 | 见下面的完成判据 |

**新增系统调用**（按实现顺序，共 6 个）：`ours_handle_close`(5b) → `ours_channel_create`、`ours_channel_write`、`ours_channel_read`、`ours_object_wait_one`(7) → `ours_process_spawn_boot`(8b)。

**新增或改动的对象体**（按实现顺序）：
1. UserPtr(2)；
2. 公共 Signals、Rights、Status、types(3)；
3. Dispatcher 类型标签、KernelHandle(4a)；
4. Handle arena、HandleTable、`ProcessDispatcher::handle_table()/current()/name()`、`ThreadDispatcher::current()`(4b-4c)；
5. `sync::Event` 构造函数、信号核心、`object::wait_one`(6a)；
6. PeerHolder、ChannelDispatcher、MessagePacket(6b)；
7. `userabi::KernelPackage`、`userabi::launch`(8a)；
8. init 程序(9)。

**P0 完成判据**：dev 镜像的日志按顺序出现下面三行；dev.dbg_1 仍然直接跑 shell 的命令表演示；SMP 下连续启动 5 次结果一致（用来抓用户 RSP 竞态）。

```
init: spawned shell
shell: bootstrap says "ping"
init: shell says "pong"
```

如果主人否决 D1-A，第 1–7 步不变，第 8–9 步换成 P1-C 里 process_create、thread_create、process_start、vmo_create、vmo_get_size、vmar_map 这组子集。

**P0 之后的 P1 批次**：

| 批次 | 内容 | 前置 | 完成判据 |
|---|---|---|---|
| P1-A 边界 | 系统调用生成器；UserPtr 的 `data_fault_resume`；fault-kill（`on_user_exception`；Kill 分支先开中断再 `Thread::Current::exit`；sysret 前检查挂起信号）；非 vDSO 陷入改为杀进程；KoId 改 u64；句柄查找的常数时间选择（Spectre V1）。arena 惰性提交推迟到 P1-C，每表随机数推迟到 P1-E 的 CPRNG（2026-09-24 定） | 无（随机数暂取 0） | 把坏指针交给系统调用得到 `InvArgs`；自己解引用坏指针、#GP、ud2、从 vDSO 外陷入的程序被杀掉；两种情况内核都继续运行。计划见 `.agents/tasks/syscall-boundary/PLAN.md` |
| P1-B 生命周期 | 进程状态机、`process_exit`、线程 `on_exit` 摘表并清 `m_kernel_thread`、`VmAspace::destroy`、TERMINATED、get_info(HandleValid/HandleBasic/Process)、`task_kill`、job 子表与 `job_create`、内核内部的 init critical | P1-A | init 读到 shell 的退出码；循环起停 100 次，内存不增长 |
| P1-C 加载器专线 | D2、D3 定案；Vmar 改名；process_create/thread_create/process_start；vmo_create/get_size；vmar_map；handle_duplicate/replace；句柄随消息转移与每进程记账；processargs；独立的 userboot；runtime/crt0；用户侧 libc；Binder 和 bootfs::View 移到 system/lib；BootFS VMO；**删除 spawn_boot** | P1-B；mm 的 `size()`、边界检查、偏移透传、`write()` | init 在用户态从 BootFS 装载 shell；lib/syscall 不再依赖 userabi |
| P1-D 多路与同步 | wait_many、event、eventpair、object_signal/signal_peer、port、get/set_property；thread_start/exit；futex；内核版 channel_call；消息上限升到 64 KiB | TLB shootdown、缺页路径的 VMO 锁、OwnedWaitQueue 退避协议、Semaphore | 一个多线程服务用 port 同时服务两个客户端 |
| P1-E 时间与诊断 | clock/deadline/nanosleep、timer、debuglog、cprng、系统信息、ResourceDispatcher 重写与 ranged root 交付、debug_write 收口 | P1-C 的 processargs | shell 用 Log 句柄输出；带 deadline 的 wait_one 按时回 TimeOut |
| P1-F 内存补全 | vmo_read/write、op_range、vmar_unmap/allocate/protect/destroy（protect 与 per-mapping 上限同一提交）、修 OOM 死循环（heap.md #3） | P1-C | malloc 能 Decommit 回收内存；对只读 BootFS 映射 protect 成可写会被拒绝 |

## 6 下层阻塞项

P0 必须先修的：

| 缺口 | 位置 | 阻塞什么 | 做法 |
|---|---|---|---|
| 用户 RSP 只存在 per-CPU 槽里 | `kernel/arch/x86/kernel/syscall.S:133,202`；`SyscallFrame` 没有 sp | 所有会 park 的调用（wait_one，以及任何 Mutex 竞争） | 压进帧（Linux `entry_SYSCALL_64` 的 pt_regs->sp；Zircon `x86_syscall`） |
| FMASK 不清 TF、DF、AC | `init.cpp:47-54` | 安全：AC=1 让 SMAP 失效；TF=1 让 #DB 在用户 GS 下执行 | 加上这三位 |
| 系统调用全程 IF=0 | `syscall.S`（没有 sti） | spawn_boot 装载期间该 CPU 丢时钟 tick；阻塞调用 | 1d |
| 内核直接解引用用户指针，SMAP CPU 上活锁 | `debug.cpp:35-42,64`、`command.cpp:57-86`、`mmu.cpp:41`、`vm_mapping.cpp:443`、`faults.cpp:70` | 所有带指针参数的调用 | 4.6 |
| `Handle::make/clone` 没有定义；`reset()` 调用不存在的 `on_zero()` | `handle.hpp:19-35,104-112` | 句柄层 | 4a/4b |
| HandleTable 无存储；`sync::RwLock` 是空壳 | `handle_table.hpp:39-55`、`kernel/kernel/sync/rwlock.cpp` | 所有带句柄的调用 | 改用 Mutex |
| 信号核心不完整，canary 校验必然断言失败 | `dispatcher.cpp:24-50` | wait_one | 6a |
| PeeredDispatcher 没有构造函数、没有 peer | `dispatcher.hpp:87-102` | Channel | 6b |
| `sync::Event` 没有构造函数 | `event.hpp:35-47` | WaitSignalObserver | 6a |
| 公共 Signals 被 task 当线程控制位使用 | `signals.hpp:12-18`、`kernel/include/ours/task/thread.hpp:348-356`、`process_dispatcher.hpp:21-22` | 对象信号 ABI | 3a |
| Rights 只有 5 位，Job 默认为 None；Status 隐式编号；Instant 是 usize | `rights.hpp:12-23`、`job_dispatcher.hpp:13`、`status.hpp`、`types.hpp:25-27` | ABI | 3b–3d |
| `table.cpp` 直接碰 `task::Thread` | `table.cpp:60-70` | 分层规则 | 4b |
| kmalloc 上限 4 KiB；vmalloc 只有 256 个区间且没有 shootdown | `kmalloc.hpp:30`、`vmalloc.cpp:54-56`、`heap.md #2` | MessagePacket | 4 KiB 上限 |
| 内核包视图是局部变量；栈只有一页且 16 字节对齐 | `userboot.cpp:209-210,316-469` | spawn_boot | 8a |
| vDSO 桩不搬 r10，也不处理 r12/r13 | `kernel/lib/userabi/vdso/vdso.S` | channel_* | 第 7 步手写 |

P1 的前置：

| 缺口 | 位置 | 阻塞什么 | 做法 |
|---|---|---|---|
| 用户态故障一律 panic | `faults.cpp:66-136` | 监督 | `on_user_exception` 后 kill |
| Kill 分支为空，而且是在 IF=0 下被调用；`kill()` 的断言写反；`suspend_or_kill` 不处理 Initial | `kernel/kernel/task/thread.cpp:617-651,236-304` | process_exit、task_kill、SuspendToken | 先开中断再 exit（Zircon `ProcessPendingSignals`） |
| sysret 前不检查挂起信号 | `syscall.S:181-217` | 被 kill 的阻塞调用会把 InternalIrqKilled 带回用户态 | 出口检查 |
| 线程退出后 ThreadDispatcher 仍挂在 SafeLink 链上，析构时 trap | `process_dispatcher.cpp:87-101`、`thread_dispatcher.cpp:125-142`、`list_hook.hpp:63-64` | 第一次 process_exit 就 trap | 在进程锁下 `remove_thread`，并清掉指针（Zircon `ResetCoreThread`） |
| 进程死后什么都回收不了：aspace 与 root VMA 的 Arc 成环、`~VmAspace` 是空函数、没有 ArchVmAspace::destroy、内核栈和 elfload 取的帧都泄漏 | `vm_area_or_mapping.hpp:159-160`、`vm_aspace.hpp:127`、`vm_aspace.cpp:84-86`、`stack.cpp:23-55`、`loader.cpp:108-146` | 服务重启 | `VmAspace::destroy()`：拆 root VMA、摘出 `ALL_ASPACE_LIST`、页表释放推迟到 CR3 不再指向它（Zircon `VmAspace::Destroy`） |
| `VmObjectPaged::read/write` 是 Unimplemented；没有 `VmObject::size()` | `vm_object_paged.cpp:103-113` | BootFS VMO、vmo_read/write/get_size | 内核缓冲版 + 用户拷贝版 |
| 建映射不检查 VMO 边界；`map_with_vmo` 把偏移写死为 0；create 丢掉 `commit_range` 的返回值 | `vm_area.cpp:117-157,267,274`、`vm_cow_pages.cpp:39-41`、`vm_object_paged.cpp:40` | vmar_map | dispatcher 检查、偏移透传、错误传出 |
| 映射的权限上限直接取区域的 `m_vmaf` | `vm_area.cpp:136-143` | vmar_protect | `create_mapping` 加 ceiling 参数 |
| 缺页路径不取 VMO 锁；aspace 锁是自旋锁 | `vm_mapping.cpp:391-500`、`vm_aspace.hpp:82-84` | 共享惰性 VMO、多线程进程 | D12 |
| 没有 TLB shootdown | `docs/repairs/260919/x86-mm.md #1` | thread_start、unmap/protect/decommit | 先修 |
| OwnedWaitQueue 的退避协议是占位；Semaphore 是桩 | `owned_wait_queue.hpp:63-78`、`sync-mutex.md #1`、`semaphore.cpp` | futex、channel_call 的 PI、port | 补完 |
| OOM 时 `alloc_frames_slowpath` 死循环 | `heap.md #3` | 运行不受信程序 | 修 |
| `current_mono_time` 没有定义；只有 HPET 一个时钟源 | `kernel/include/ours/time/mod.hpp`、`hpet.cpp:379` | 用户态时钟 | 先以陷入实现 |
| `ResourceDispatcher::create` 自相矛盾；`make_resource_handle` 是空函数 | `resource_dispatcher.cpp`、`kernel/lib/userabi/src/resource.cpp` | 根资源 | 重写 |
| 用户工具链没有 libc 实现；Binder 依赖内核 logz4 | `system/lib/libc`（只有头文件）、`kernel/lib/elfload/BUILD.gn` | 用户态 launcher | 挪 `string.cpp`，去掉 logz4 胶水依赖 |

## 7 需要主人拍板的决策

| # | 问题 | 选项 | 推荐 |
|---|---|---|---|
| D1 | P0 用什么方式起第二个程序 | A 临时的 `process_spawn_boot`（6 个新调用）；B 直接做用户态加载器（约 12 个新调用，还要先修 `VmObjectPaged::write`、做用户 libc、移植 Binder、定 vDSO 归属） | **A**。先跑通，B 的全部工作在 P1-C 照做，届时删掉 A。它违背分层规则，理由见 2.8 |
| D2 | 用户创建的进程由谁映射 vDSO，范围记在哪 | A 由内核在 `ProcessDispatcher::create` 内映射，vDSO VMO 不外发，范围记在 `mm::VmAspace`；B 照 Zircon 发 vDSO VMO，由 vmar_map 按对象身份识别 | **A**。不变式在 Initial 状态下就成立；只有 mm 能在 unmap/protect/destroy 路径上拒绝对 vDSO 的改动；不必分发一个还得防 create_child、防“只读映射后再 protect”的特权 VMO。基址通过 `get_property(ProcessVdsoBaseAddress)` 读出 |
| D3 | P0 之后 ELF 由谁加载 | A 用户态 launcher，父进程用共享的 Binder 绑定导入，P2 改为 crt0 自绑定；B 保留一个永久的内核加载调用 | **A**。否则 ring 0 要为每次启动解析不可信的 ELF |
| D4 | 公共句柄值类型叫什么 | A 改名为 `ours::HandleValue`，用户侧 `ou::Handle` 做 RAII；B 保留 `ours::Handle` | **A**。系统调用层会同时出现 `ours::Handle` 和 `object::Handle` |
| D5 | 从公共 Signals 挪出的线程控制位叫什么 | A `task::ThreadSignals`；B `task::PendingActions`；C `task::ThreadRequests` | **A**，便于对照 Zircon 的 `THREAD_SIGNAL_*` |
| D6 | Rights 位布局 | A 整体换成 Zircon 位值；B 保留现有 5 位，按需追加 | **A**。现在还没有用户态依赖，换的成本最低，之后可以直接对照 Zircon 的权限文档 |
| D7 | 系统调用描述用什么格式 | A GN scope 列表 + 只用标准库的 Python 生成器；B `.pyl` 文件；C FIDL 子集 + fidlc/kazoo | **A**。它就在构建图里，不用写解析器，也能直接喂给 `generated_ifs_file` |
| D8 | 调用号是否算公共 ABI | A 私有，由生成器按位置分配；B 公共，手工编号 | **A**。用户只能经 vDSO 进入内核，调用号没有别的消费者 |
| D9 | userboot 和 init 是一个进程还是两个 | A P1 起分成两个，P0 用配置合并；B 永久合并 | **A**。内核引导消息里带根资源这类特权对象，其格式应该只被一个随内核构建的小程序理解 |
| D10 | ProcessDispatcher 上的 `m_parent`/`m_children`/`CREATED_PROCESS_LIST` 怎么处理 | A 新代码不读写它们，删除前问主人；B 做成 POSIX 式进程树 | **A**。层级由 job 表达（Zircon 同样没有进程父子关系）；这些成员可能是预留的脚手架 |
| D11 | Channel 队列满时怎么办 | A 回 NoResource，不清 WRITABLE；B 真正背压，清 WRITABLE 并回 ShouldWait；C 照 Zircon 抛策略异常 | **A**。等异常机制落地后改成 C；B 会改变 WRITABLE 的含义 |
| D12 | aspace 锁和 VMO 锁用什么类型 | A 都换成 Mutex，锁序 aspace → VMO；B 都用 SpinLock | **A**。前提是先让用户态来源的缺页在开中断的状态下处理，并规定持自旋锁时禁止缺页 |
| D13 | 怎么保证内核与 vDSO 一致 | A 继续从 BootFS 加载，userboot 核对 build-id；B 像 Zircon rodso 那样把 vDSO 嵌进 vmours 映像 | **A**。改动小；等 vDSO 需要数据页时再考虑 B |
| D14 | 根资源 | A 删掉 Root，交出四个按种类的 ranged root；B 保留单一 Root | **A**。一个能派生一切的句柄就是环境权威，Zircon 也已移除 ROOT 种类 |
| D15 | lib/syscall 能否直接调时钟、CPRNG、logz4 这类无状态服务 | A 在 kernel/object 里做薄门面，lib/syscall 按依赖白名单只依赖它；B 允许直接调用 | **A**。这样主人的规则将来能由 GN 依赖图机械检查 |

## 8 被否掉或推迟的方案

| 方案 | 出处 | 处置 | 理由 |
|---|---|---|---|
| 出参由 vDSO 桩从寄存器写回用户指针 | memory | 否决 | channel_read 仍然要拷贝用户缓冲区；桩写 out 时缺页会漏掉已入表的句柄；与 r12/r13 约定冲突 |
| 出参句柄“先装表、失败再回滚” | task、ipc | 否决 | 回滚之前别的线程可以 dup 或转走它，失败的调用不再没有副作用 |
| 内核装载调用 `ours_vma_load_elf` | task | 否决 | 要在 ring 0 解析用户提供的 ELF；已由只接受包内文件的临时 spawn_boot 取代 |
| arg2 装 BootFS 句柄值的捷径 | task | 否决 | arg2 约定为 vDSO 基址；init 不该认两套启动协议 |
| `ours_thread_set_fs_base` | startup | 否决 | 与 `set_property(RegisterFs)` 重复；设别的线程改用 thread_write_state |
| 按 BootFS 文件名 spawn 的永久调用 | task | 否决 | 会把 BootFS 写死进系统调用 ABI |
| 用户 `vmo_create` 走 Pinned 立即提交 | startup | 否决 | 一次调用就能吃光物理内存；改为 Lazy 加上限 |
| P0 映射可执行时不查 Execute，用单个 RWX 镜像 VMO | startup | 否决 | rights 交集检查从第一天就做，否则以后收紧就是改语义 |
| 把 HPET 的 MMIO 页映射给用户读时钟 | resource | 否决 | 暴露设备寄存器，Linux 也已放弃这种做法 |
| 全局 thread_lock；虚函数 `get_type()`/`get_lock()` | Zircon | 否决 | 违反主人的规则；不新增虚函数 |
| P0 的用户态加载器、Vmo/Vmar、processargs、独立 userboot | memory、startup | 推迟到 P1-C | 压着四处下层阻塞，P0 的成本被低估了 |
| P0 的 process_create/start/exit 与 fault-kill | task | 推迟到 P1-A/B | P0 的程序都是受信的，交换完消息后自旋即可；bring-up 期 panic 更响亮 |
| P0 的生成器、sysret 白名单、KoId u64、get_info、handle_duplicate | object_model | 推迟到 P1 | 手写 6 个桩可控；生成器是 P1 第一件事 |
| P0 的 UserPtr 故障恢复 | object_model、评审 2 | 推迟到 P1-A（与 fault-kill 同批） | P0 只跑受信程序；stac/clac 已经消除 SMAP 活锁，缺页 panic 能让违规暴露出来 |
| P0 的消息带句柄、Peered 锁指针重构、txid 强制 | ipc | 推迟到 P1 | 切片只传字节 |
| 64 KiB 消息 | ipc | 推迟到 P1-D | 要等 BufferChain；vmalloc 有区间数限制，vfree 没有 shootdown |
| exception channel、thread_read_state | task | 推迟到 P2 | init 用 TERMINATED、ProcessInfo 的 retcode 加内核打印的故障报告就能监督；异常通道是给调试器和崩溃报告器的 |
| SuspendToken、Profile、JobPolicy、job_set_critical 系统调用 | task | P2 | 服务于调试、安全加固、实时性 |
| Vmar 的 CanMapSpecific / SpecificOverwrite | memory | P2 | `VmPlacement::at` 遇到已占用地址一律拒绝 |
| 在 ABI 里给 Profile 预留 Deadline 字段 | task | 否决（暂不进 ABI） | 调度器还没有这些参数，不预留可能写错的字段 |
| 删除 ProcessDispatcher 的进程树成员 | task | 暂缓 | 可能是预留的脚手架，先问主人 |
| 用 vdso_only 属性或隐藏符号当安全边界 | object_model、resource | 否决 | 精确的 sysret 校验落地之前，跳进桩就能陷入 |
| P0 就给 Log 和 resource 预留 bootstrap 槽位 | resource | 推迟到 P1 | P0 没有 processargs，槽位无处落脚 |

## 9 与 Zircon 的偏离汇总

| 条目 | Zircon | ours | 理由 |
|---|---|---|---|
| 用户进程的 vDSO | 启动者拿 vDSO VMO 自己映射 | 内核在 `ProcessDispatcher::create` 里映射，VMO 不外发；范围同样记在 VmAspace | vDSO-only 规则必须在任何线程运行之前成立；少一个需要防护的特权 VMO |
| 临时的 `process_spawn_boot` | 无 | P0 按内核包里的文件名起程序 | 先跑通；只接受包内文件；P1-C 删除 |
| `task::Thread` 创建时机 | thread_create 时就建，没启动的用 `Forget()` 丢弃 | start 时才建 | ours 没有 `Forget()`；`m_kernel_thread` 只在 start 到 exit 之间非空 |
| 线程状态 | INITIAL 与 INITIALIZED 分开 | 合并为 Initial | ours 没有 futex 池预留这一步；P1 引入 FutexState 贡献时再复核 |
| 进程锁 | 进程锁之外还有线程表相关的锁 | 一把 `Dispatcher::get_lock()` | 状态迁移与线程表成员必须原子地一起变化 |
| 线程与调度锁 | 全局 thread_lock（现在是 ChainLock） | 细粒度锁 + OwnedWaitQueue 退避 | 主人明确拒绝全局 thread_lock |
| 对象类型标签 | 虚函数 `get_type()` | 基类里的 `ObjectType` 存储字段 | 不新增虚函数，写法与 canary 的 MAGIC 一致 |
| Peered 对象的共享锁 | 虚函数 `get_lock()` 返回 PeerHolder 的锁 | P0 用两把锁加固定锁序；P1 视需要改成锁指针 | 先不做基类重构；没有虚调用 |
| Channel 队列满 | 超过 3500 条抛策略异常 | 回 NoResource，不清 WRITABLE | ours 没有异常机制；不改变 WRITABLE 的含义 |
| 消息上限 | 64 KiB | P0 4 KiB，P1 64 KiB | vmalloc 区间数有限，vfree 没有 shootdown |
| Status 编码 | 负的 i32 | 正的 u32，显式编号 | 调用方都经 ou 与 Ok 比较；改成有符号会波及整棵树 |
| 系统调用描述 | FIDL + fidlc/kazoo | GN scope 列表 + Python 生成器 | 树里没有 IDL 工具链；列表由构建生成 |
| sysret 白名单 | 每个调用可以有多个 VIA 调用点 | 每个调用号一个偏移 | ours 的 vDSO 内部函数不自己发系统调用，真出现了再扩成列表 |
| 出参句柄的发布顺序 | 新的 Rust 实现先入表后写出 | 预留 → 写出 → 发布 | 写出失败时不会漏句柄，也没有可见窗口 |
| 句柄表校验 | 第五关用进程 koid；表锁是 BrwLock | 用表 koid；P0 用 Mutex | 给 create_shared 留余地；`sync::RwLock` 还没实现 |
| 引导消息 | 内核给 userboot 发两条只含句柄的消息 | 全系统只有一种 processargs 消息，magic 用 ours 自己的 | 只有一个 runtime、一个解析器；不同的 magic 避免让人误以为线上兼容 |
| BootFS | userboot 在用户态解压 ZBI | 内核解压后交出一个只读 VMO | lz4 解码器只在内核里，而且内核加载 userboot 时已经解压过一遍 |
| VMO 的 Execute 权限 | 要经 vmex 换成可执行 | 在 P2 之前默认带 Execute；系统调用层拒绝 W+X | 先跑通；P2 引入 vmex 时从默认值里去掉 |
| Snapshot 子 VMO | COW | 先急切复制 | 语义不变，COW 放 P2 |
| resource_create 的形状 | kind 塞在 options 低位，带 name，共 7 个参数 | kind 与 options 分成两个参数，不带 name | *Options 只放位掩码；名字走 set_property |
| cprng_add_entropy | 不检查权限（fidl 文档自述为 BUG） | 要求 System/Cprng 资源 | 落实 Zircon 自己的结论 |
| Log 的底层实现 | DLog + notifier 线程 | logz4 ring + Dpc 唤醒 | 复用已有的设施 |
| clock_get_monotonic | 在 vDSO 里计算 | 先以陷入实现 | ours 只有 HPET（MMIO）一个时钟源，vDSO 也没有数据页 |
| Profile | 支持多种调度 discipline，入口是 object_set_profile | 只暴露 priority 与 affinity，入口是 thread_set_profile | 调度器今天只表达得了这两项 |
| 句柄数量限制 | 靠 job policy 与内存压力兜底 | 每进程句柄上限，在途句柄也计入 | ours 这两样都还没有 |