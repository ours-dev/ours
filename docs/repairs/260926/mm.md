# kernel/mm 待修项（2026-09-26）

## 1. 没有「取当前 CPU 时抢占必须已关」的调试检查

**现象**：所有在开着抢占时取 `current_cpu()`/`access()`/`PerCpu::ref_mut()` 并继续使用结果的地方，
今天都不会被任何断言发现。

**原因**：`CpuLocal::current_cpu()`、`access()`、`with_current()`、`PerCpu::ptr_mut()/ref_mut()`、
`Current<T>::get()` 都只检查 handle/unit 有效，不检查抢占或中断状态（`cpu_local.hpp`）。

**照谁改**：Linux `debug_smp_processor_id()` → `check_preemption_disabled()`
（`lib/smp_processor_id.c`）：抢占开着、中断开着、又没有绑死在单个 CPU 上时报警；
`raw_smp_processor_id()` 是明确不要检查时的出口。这里应在调用点逐个换成 `CpuLocal::Pinned`
（或确认属于「在哪个 CPU 读都一样」的一类、改用 `read()`）之后，再给这些接口加检查，
并留一个 raw 版本；先加检查会在大量现有路径上直接触发。

## 2. `gktl::Counter` 的 `max()`/`min()` 是非原子的读-比较-写

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
