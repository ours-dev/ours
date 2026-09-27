# x86-mm — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. #PF 在可睡眠时开中断（`docs/repairs/260925/mm.md` 第 1 条的第一步）（2026-09-26）

**文件**
- `kernel/arch/x86/mm/fault.cpp` — 新增 `may_sleep_resolving()` 与 `PREAMBLE_DISABLE_DEPTH`；
  在 `task::Thread::Current::page_fault()` 前后开关中断

**症状**

潜伏。缺页从中断门进来（`idt.cpp` 每个表项都是 `type_attr = 0x8E`），全程关中断，从不重新打开。
当下不出错，但它挡住了 `docs/repairs/260925/mm.md` 第 1 条的修法：那一条要把 `VmAspace::m_lock`
换成可睡眠的 Mutex，好让缺页和 `VmObject::read()/write()/commit_range()` 在同一棵 `VmCowPages::m_pages`
上真正互斥。**关中断下不能睡**，所以顺序上必须先把缺页搬进可睡眠的上下文。

顺带一提，解析一次缺页不是短活——要走地址空间、要分配、接下来还要拿 VMO 的锁——全程屏蔽中断
会把这颗 CPU 上的时钟滴答和 IPI 一起拖住。

**原因**

不是所有缺页都能睡。内核持自旋锁或关抢占时缺页，去拿 Mutex 就是死锁：锁永远不会被释放，
而能释放它的线程又调度不上这颗正在自旋的 CPU。所以要有判据。

判据的第一版拿抢占计数和 0 比，**这是错的**，而且靠一个临时探针才发现：

```
PROBE fault#1: user=true addr=0x10 ip=0x201fb0 if=1 locks=0 preempt=1 eager=1 sleepable=false
```

一次纯粹的 ring-3 缺页，中断开着、没持任何自旋锁，抢占计数却是 1。原因是
`arch_handle_exception()` 在分发之前先调 `platform_start_handling_irq()`，它走到
`irq::handle_irq_preamble()`，那里各做一次 `disable_preemption()` 和 `disable_eager_reschedule()`
（对应 Zircon 的 `int_handler_start()`）。`faults.cpp` 里本来就有一句注释写着
"the preempt count the preamble raised"，只是容易看漏。

也就是说**任何缺页的基线就是 1**，拿 0 去比会拒绝掉每一次缺页——判据永远为假，整个改动变成空操作。

**改法**

把基线显式命名，而不是在比较里塞一个魔数 1：

```cpp
/// arch_handle_exception() 在分发前经 platform_start_handling_irq() 走到
/// irq::handle_irq_preamble()，那里各做一次 disable_preemption() 与
/// disable_eager_reschedule()。所以高于这个数的部分才是缺页上下文自己关的。
constexpr u8 PREAMBLE_DISABLE_DEPTH = 1;
```

判据改前：

```cpp
return task::Thread::Current::preemption_state().preemption_disabled_count() == 0;
```

改后：

```cpp
auto const &preemption = task::Thread::Current::preemption_state();
return preemption.preemption_disabled_count() <= PREAMBLE_DISABLE_DEPTH;
```

用 `<=` 而不是 `==`，是为了哪天前导不再抬这两个计数时，它继续答"是"而不是悄悄反转。
eager 计数故意不看：它管的是能否向别的 CPU 急发重调度请求，不是本线程能不能阻塞，
而且前导同样会抬它。

调用点改前：

```cpp
// Call the top level page fault handling routine.
auto const status = task::Thread::Current::page_fault(addr, cause);
```

改后：

```cpp
auto const sleepable = may_sleep_resolving(frame);
if (sleepable) {
    arch::local_irq_enable();
}

auto const status = task::Thread::Current::page_fault(addr, cause);

if (sleepable) {
    arch::local_irq_disable();
}
```

返回前把中断关回去，而不是留着：上面那几层（`faults.cpp` 的分发和调它的桩）都在关中断的前提下
展开；末尾的 `iret` 反正会从帧里恢复缺页上下文自己的 IF，所以开中断的窗口刚好是这一次调用，不多不少。

另外两个条件：帧里的 IF 必须是 1（清了 IF 的内核代码是为了把什么挡在这颗 CPU 外面，
缺页不能替它撤销——中断门已经把当前的 IF 清了，帧是唯一的记录），以及
`sync::SpinLock::num_held_cpu()` 为 0。

**参照**

`Linux do_user_addr_fault()`（`arch/x86/mm/fault.c`）：同样从保存的帧里读 IF 再
`local_irq_enable()`，并用 `faulthandler_disabled()` 拦住不可睡的场合——它的 `in_atomic()`
把自旋锁和抢占折进同一个 preempt 计数，本内核是分开的两个。
`Zircon x86_pfe_handler()`（`zircon/kernel/arch/x86/faults.cc`）在同一位置、同样理由开中断。
前导对应 `Zircon int_handler_start()`。

**验证**

构建通过；`clang-format` 与 `clang-tidy -p out/bringup.x86` 跑过，改动行上无新告警
（文件里既有的两条是 `x86_user_copy_begin/end` 的命名，必须与汇编一致，未动）。

两个镜像都在 QEMU 里跑满 50 秒、零错误零断言：`kernel.dev.obi` 走到 shell 进程创建又销毁，
`kernel.dev.dbg_1.obi` 到 49.9 秒仍在 cpu2/cpu3 上调度线程。

判据本身用临时探针实测后撤掉。修正基线之后：

```
PROBE fault#1: user=true  addr=0x10           ip=0x201fb0        if=1 locks=0 preempt=1 sleepable=true
PROBE fault#3: user=true  addr=0xa00000       ip=0x201fd0        if=1 locks=0 preempt=1 sleepable=true
PROBE fault#4: user=false addr=0x7fffffffe000 ip=0xffffff884005f4fc if=1 locks=0 preempt=1 sleepable=true
```

八次缺页全部 `sleepable=true`（修正前全部为假）。前三次是 `fault_probe` 这个用户态测试程序
故意解引用坏指针，后五次 `user=false` 且地址在用户栈、ip 在内核，是 user_copy 触发的按需分页——
两类上下文都被走到了。

**故意没做**

- 没有动 `VmAspace::m_lock` 的类型，那是第 2 步。这一步只让缺页**可以**睡，还没有人睡。
- 没有改前导本身。让异常路径不抬抢占计数也许更干净，但那是 kernel/task 的设计决定，
  而且会影响每一个异常向量，不是修 VMO 锁顺手能带的。
- `docs/repairs/260925/mm.md` 第 1 条**没有删**：三步只完成了第一步。

---

## 2. 不可睡的缺页改为停机，不再"拒绝"（2026-09-26）

**文件**
- `kernel/arch/x86/mm/fault.cpp` — `x86_handle_page_fault()` 对不可睡的缺页记下原因后停机；可睡时无条件开中断

**症状**

潜伏。第 1 条之后，不可睡的缺页会带着缺少 `VmfCause::MaySleep` 的 cause 进 VM，由
`VmAspace::page_fault_internal()` 拒绝。对 user_copy 来说，拒绝会一路变成系统调用返回 `InvArgs`：
一个内核 bug（持着自旋锁、关着抢占或中断去碰可分页内存）被悄悄地翻译成了"用户传了坏指针"。

**原因**

解析任何缺页都可能阻塞：`VmAspace` 的锁是 Mutex，第 3 步之后 VMO 的锁也是，补页还可能等回收。
所以从已经承诺"不睡"的上下文里，根本不存在"正确地解析"这个选项，只有"打破承诺"和"不解析"。
而"不解析"里又分两种：调用方事先知道自己在锁里、准备好了重试的（Zircon 的 capture 模式），
和调用方根本没想到会缺页的——后者就是 bug。

对照 Zircon 的 `x86_pfe_handler()`（`zircon/kernel/arch/x86/exceptions_c.cc`）：

- 线程设了 `page_fault_resume` 且没置 `RUN_FAULT_HANDLER` 位（capture 模式），**完全不进 VM**，
  把缺页地址和标志塞进 rdx/rcx，跳回恢复点；
- 否则 `DEBUG_ASSERT(arch_num_spinlocks_held() == 0)`，开中断，进 VM；
- VM 这一侧 `PageFaultInternal()` 第一行就是 `lockdep::AssertNoLocksHeld()`。

也就是说 Zircon 的 VM 里没有"不能睡就拒绝"这个分支，意外的原子缺页一律当 bug 断言。

**改法**

改前：

```cpp
auto const sleepable = may_sleep_resolving(frame);
if (sleepable) {
    cause |= VmfCause::MaySleep;
    arch::local_irq_enable();
}

auto const status = task::Thread::Current::page_fault(addr, cause);

if (sleepable) {
    arch::local_irq_disable();
}
```

改后：

```cpp
if (!may_sleep_resolving(frame)) {
    auto const &preemption = task::Thread::Current::preemption_state();
    log::error("page fault at {:#x} from a context that may not sleep: user={} if={} "
               "spinlocks={} preempt={}", ...);
    if (!supervisor) {
        panic("ring-3 page fault on a CPU whose lock or preemption counts say atomic");
    }
    return Status::BadState;
}

cause |= VmfCause::MaySleep;
arch::local_irq_enable();

auto const status = task::Thread::Current::page_fault(addr, cause);

arch::local_irq_disable();
```

比 Zircon 更严的一处：Zircon 只用 `DEBUG_ASSERT`，release 构建里编译掉之后，意外的原子缺页会照常进 VM、
在自旋锁里去拿 Mutex，可能直接挂死。这里 release 构建也停机。两种"更软"的做法都更糟：
断言被编译掉会挂死；拒绝会把 bug 藏成一个 `InvArgs`。

停机分两路，是为了保住最有用的现场：

- **内核态缺页**只打一行"为什么不能睡"，然后返回错误，并在 user_copy 的 resume 分支之前返回。
  `faults.cpp` 的 `x86_dispatch_exception()` 对内核态的失败缺页调 `panic_exception()`，
  它打印 CR2 和全部寄存器——比这里直接 `panic()` 有用得多。
- **用户态缺页**不可能正当地走到这里：用户代码不持内核锁、中断总是开着。走到了只能说明
  这颗 CPU 的计数已经坏了，是内核自己的错，直接 `panic()`，不该杀一个无辜的进程了事。

**顺带纠正一个说法**

此前对比 Zircon 时，我说"Zircon 在进 VM 前把抢占也打开了，我们解析全程不可抢占、会拖延迟"。
这是错的。Zircon 的前导 `int_handler_start()`（`zircon/kernel/include/kernel/interrupt.h`）
同时做了 `PreemptDisable()` 和 `EagerReschedDisable()`；#PF 里只 `PreemptReenable()`，
eager 计数留着。而按 `EagerReschedDisable()` 自己的注释，eager 计数非零时本地抢占只记账、
不执行，但**阻塞仍然允许**。所以 Zircon 的缺页解析同样不可抢占，只是能睡。本内核只降抢占计数
而不动 eager 的话，`is_preemption_enabled()` 照样为假，行为上没有差别，所以不做。

**参照**

`Zircon x86_pfe_handler()`、`VmAspace::PageFaultInternal()`、`int_handler_start()`、
`PreemptionState::EagerReschedDisable()`；`Linux do_user_addr_fault()` 里
`faulthandler_disabled()` → `bad_area_nosemaphore()`。

**验证**

构建通过；`clang-format` 与 `clang-tidy -p out/bringup.x86` 跑过，改动行无新告警。

两个镜像各跑 50 秒，零错误零断言，日志里没有一条 "may not sleep"——和第 1 条探针测到的
"启动里所有缺页都可睡"一致。

停机分支启动里走不到，用临时探针强行把第一次内核态缺页判为不可睡，`kernel.dev.dbg_1.obi`：

```
page fault at 0x7fffffffe000 from a context that may not sleep: user=false if=1 spinlocks=0 pre...
Unhandled page fault: vector=14, error_code=0x0
  cr2=0x7fffffffe000
  rip=0xffffff884005f7cc (+0x5f7cc)  cs=0x10  rsp=0xffffff880405c928
  rax=... rbx=... （全部寄存器）
```

先是原因，再是 `panic_exception()` 的完整现场，没有走到 user_copy 的 resume。探针已撤掉。
**用户态那一路的 `panic()` 未实测**：要让它触发，得先把每 CPU 计数弄坏。

**故意没做**

- `VmfCause::MaySleep` 和 `page_fault_internal()` 里的拒绝还在。它们现在对硬件缺页已经多余
  （能进 VM 的一定可睡），但 `soft_fault()` 仍然会被那个拒绝挡住。下一条一起处理：
  删掉标志和拒绝，换成"调用方没持锁"的断言，`soft_fault()` 共用它。
- capture 模式的拷贝接口不做，树里还没有在锁里做 user copy 的调用方。
