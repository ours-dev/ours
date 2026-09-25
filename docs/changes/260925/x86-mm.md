# arch/x86/mm 改动记录（2026-09-25）

## 页表锁关着中断拿（2026-09-25）

**文件**
- `kernel/arch/x86/mm/page_table.cpp` — `PageTable` 的七处 `Guard<sync::SpinLock>` 改为 `Guard<sync::SpinLock, IrqSave>`
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp` — `m_lock` 声明处写明为什么

**症状**
潜伏，当前没有观察到。条件：线程在 `map_pages()`/`unmap_pages()`/`protect_pages()`/`destroy()` 里持着一个地址空间的页表锁
被抢占，同一颗 CPU 上下一个线程对同一个地址空间映射、或在里面缺页——自旋在"本 CPU 持有"的锁上：dbg_1 断言
`SpinLock::acquire()` 的 `!is_held()`，release 版自旋到被抢占为止。

**原因**
`Guard<sync::SpinLock>` 显式写出了"没有选项"：`lockdep::Guard` 为每个选项构造一个 saver，这里一个都没有，
`SpinLock::acquire()` 本身只做 CAS。改之前：

```cpp
    auto result = [&] {
        Guard<sync::SpinLock> guard(m_lock);
        return Engine::map(m_pgd_phys, phys_to_virt, alloc_table, va, pa, n, flags);
    }();
```

缺页路径在 #PF 处理函数里、中断还关着时经 `VmMapping::fault()` 进到这里，所以那一侧"碰巧"关着中断；从系统调用、
`VmAspace::destroy()` 进来的线程上下文持有者则开着中断和抢占。同一把自旋锁的持有者一旦有一个会让出 CPU，就会有人等一个
不会再跑的持有者。

**改法**

```cpp
    auto result = [&] {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        return Engine::map(m_pgd_phys, phys_to_virt, alloc_table, va, pa, n, flags);
    }();
```

不用 PreemptDisable：`init_vmm()` 在 `InitLevel::Threading` 之前就经这把锁重设内核映像映射的权限，那时启动 CPU 还没有线程，
`AutoPreemptDisabler` 无处可去；缺页一侧本来就关着中断。

目前通常在它外面还有地址空间锁（`VmAspace::m_lock`，已经同样改为 IrqSave），这里嵌套一层 IrqSave 只是一次 pushf/cli/popf，
换来这把锁自己的规则不依赖调用者。

**参照**
- Zircon `X86PageTableBase::lock_` 是可睡眠的 `Mutex`（`zircon/kernel/arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`，
  `Guard<Mutex> a{AssertOrderedLock, &lock_, LockOrder()}`），没有这个问题；这里在自旋锁换成可睡眠的锁之前，IrqSave 是
  唯一在所有阶段都不会自锁的选项。
- Linux `mm->page_table_lock` / split PTE lock 用 `spin_lock()`（关抢占）：Linux 从不在中断里改用户页表，也没有启动早期
  "没有线程"的阶段需要照顾，所以关抢占就够；这两点这里都不成立。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- QEMU：dev 到 `init: shell closed its channel`、`--- end ---`；dbg_1 到 `shell: probes done (11 killed, 0 survived)`、
  `--- end ---`，无断言。

**故意没做**
- 没有换成可睡眠的锁；见 `docs/repairs/260925/mm.md` 第 1 条（同一思路适用于地址空间锁）。
