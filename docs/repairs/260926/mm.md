# kernel/mm 待修项（2026-09-26）

## 1. `Thread::Current::get()` 分两次读：先读 `%gs` 算出槽的地址，再读槽里的线程指针

**现象**：潜伏，今天触发不到；一旦就绪线程可以跨 CPU 迁移（负载均衡、work stealing）就会出现，
而且后果很重：拿到的是**别的线程**，对它的抢占计数、地址空间、锁上下文做操作。

**原因**：`Thread::Current` 继承 `CpuLocal::Current<Thread *>`，`get()` 是
`CpuLocal::access(CURRENT_INSTANCE)`——先 `movq %gs:CURRENT_CPU_OFFSET, %rcx`，加上模板地址，
再 `movq (%rcx), %rcx` 读槽。两条指令之间被抢占、并在另一个 CPU 上恢复，读到的就是原来那个
CPU 此刻在跑的线程。`CpuLocal::Pinned` 的反汇编里能直接看到这个序列（抢占计数的读-改-写之前
那三条）。

今天安全，只因为没有任何东西会移动一个被抢占的线程：`Scheduler::steal_work_from` 是
`return 0;`（`scheduler.cpp:492-494`），`migrate_thread` 是空分支（:479-490），
`Migrator::move_queued_thread` 没有调用者；被抢占的线程由 `put_prev_thread` 放回本 CPU 的队列
（`rr.cpp:111-123`）。

**照谁改**：Zircon `arch_get_current_thread()`（`zircon/kernel/arch/x86/include/arch/current_thread.h`）
用 `READ_PERCPU_FIELD(current_thread)` 直接从 gs 读，注释说的就是这个问题：经
`x86_get_percpu()->current_thread` 分两步读，中间可能发生上下文切换，读到别的 CPU 的当前线程。
Linux x86 `get_current()`（`arch/x86/include/asm/current.h`）同样是一条 `%gs` load
（`this_cpu_read_stable()`）。这里对指针大小的
`T`，`CpuLocal::Current<T>` 应提供经 `CpuLocal::read()` 的单指令读取，`Thread::Current::get()`
改用它。当前线程指针对调用线程来说在哪个 CPU 上读都一样，所以不需要 `CpuLocal::Pinned`。

## 2. 没有「取当前 CPU 时抢占必须已关」的调试检查

**现象**：所有在开着抢占时取 `current_cpu()`/`access()`/`PerCpu::ref_mut()` 并继续使用结果的地方，
今天都不会被任何断言发现。

**原因**：`CpuLocal::current_cpu()`、`access()`、`with_current()`、`PerCpu::ptr_mut()/ref_mut()`、
`Current<T>::get()` 都只检查 handle/unit 有效，不检查抢占或中断状态（`cpu_local.hpp`）。

**照谁改**：Linux `debug_smp_processor_id()` → `check_preemption_disabled()`
（`lib/smp_processor_id.c`）：抢占开着、中断开着、又没有绑死在单个 CPU 上时报警；
`raw_smp_processor_id()` 是明确不要检查时的出口。这里应在调用点逐个换成 `CpuLocal::Pinned`
（或确认属于「在哪个 CPU 读都一样」的一类、改用 `read()`）之后，再给这些接口加检查，
并留一个 raw 版本；先加检查会在大量现有路径上直接触发。

## 3. `gktl::Counter` 的 `max()`/`min()` 是非原子的读-比较-写

**现象**：潜伏：树里所有计数器都是 `GKTL_COUNTER`（`Type::Sum`），没有 Min/Max 计数器，
也没人调 `.max()`/`.min()`。

**原因**：`kernel/lib/gktl/include/gktl/counter.hpp:76-100` 经 `CpuLocal::with_current` 取本 CPU 的槽，
然后 `atomic_ref` 读、比较、再单独写，中间不关抢占也不关中断。同一个 CPU 上：线程 A 调 `max(10)`
读到 5，被时钟中断抢占；线程 B 调 `max(20)` 写入 20；A 回来写入 10，20 丢了。中断上下文的调用者
同理。对比 `add()`：relaxed `fetch_add` 在 x86 上是 `lock xadd`，对中断原子，迁移后落在旧 CPU
的槽上也不影响求和。

**照谁改**：在 `atomic_ref` 上用 `compare_exchange` 循环。Linux 没有 per-CPU max，
最接近的是 `this_cpu_cmpxchg()` 循环。

**来源**：以上各条出自 2026-09-26 per-CPU 访问审计，已对照代码复核。
