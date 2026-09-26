# kernel/mm 待修项（2026-09-25）

## 1. VMO 的页仓库由两把不同的锁保护：缺页走地址空间的 SpinLock，读写与提交走 VMO 的 Mutex

**现象**：潜伏。一个 VMO 只被一个地址空间映射、且不被 `read()`/`write()`/`commit_range()` 同时访问时触发不到。

**原因**：`VmCowPages::m_pages`（一棵 B 树）被两条路径改写：

- 缺页：`VmAspace::page_fault_internal()` 持 `VmAspace::m_lock`（`sync::SpinLock`，`vm_aspace.cpp:165`），
  经 `VmMapping::fault()` 调 `paged->make_cursor()` 和 `require_page()`（`vm_mapping.cpp:485-495`），分配并插入页；
  `VmMapping::map_paged()` 同样直接用 cursor（`vm_mapping.cpp:191`）。
- 提交、读、写：`VmObjectPaged::commit_range_internal()` 和 `read_write()` 持 `VmObjectPaged::m_mutex`。

两把锁互不相干，所以：同一个 VMO 被两个地址空间映射时，两边同时缺页会并发插入同一棵树；一边缺页、一边
`write()`，同理。文件系统计划里第一个会碰上的是 K16（所有进程共用一个 vDSO VMO），其次是 K17/K21
（launcher `vmo_write` 一个 VMO 再映射它）。

**不能直接在缺页路径里拿 VMO 的 Mutex**：缺页持的是 SpinLock，在 SpinLock 下拿会睡眠的 Mutex 是错的。

**照谁改**：Zircon 的缺页在碰页之前先拿 VMO 的锁：`VmMapping::PageFault()`
（`zircon/kernel/vm/vm_mapping.cc:1376`）对分页 VMO 取 `Guard<CriticalMutex>{paged->lock()}`，再进
`PageFaultLockedObject()`。地址空间的锁和 VmCowPages 的锁都是可睡眠的 `CriticalMutex`
（`zircon/kernel/vm/include/vm/vm_aspace.h:336`，`vm_cow_pages.h:176`）。所以要么把 `VmAspace::m_lock` 换成
可睡眠的锁（缺页本来就在可睡眠上下文里），缺页时再取 VMO 的锁；要么让 VmCowPages 自带一把自旋锁专管
m_pages。前者与 Zircon 一致，推荐。

**必须在 K16 之前修掉。**

## 2. `alloc_frames` 的带节点重载不检查 `MemoryAllocationState`，不带节点的检查

**现象**：潜伏。`MemoryAllocationState` 的 `disable()` 计数当前没有调用者，所以两条路径的
行为暂时一致。

**原因**：不带节点的 `alloc_frames(gaf, ...)` 经 `PmNode::Current::alloc_frames()`，
开头就是

```cpp
if (auto const status = inherit_thread_context(gaf); Status::Ok != status) {
    return ustl::err(status);
}
```

而 `inherit_thread_context()` 在 `!mem_state.is_enabled()` 时回 `Status::AccessDenied`，
也就是尊重「这段区间里不许分配」。带节点的 `alloc_frames(nid, gaf, ...)`
（`kernel/mm/pmm.cpp`）在 `nid < NUMA_MAX_NODE` 时直接
`PmNode::node(nid)->alloc_frames(...)`，绕过整个 `Current` 层，那个检查和线程的
resource lock 都不经过。于是 `CpuLocal::Mapper::build_dense()` 这类指定节点的调用者
完全不受计数器约束。

**要先问清楚**：这个不对称是有意的（「指定了节点就是内核基础设施，不受线程策略管」）
还是漏的。如果是漏的，`inherit_thread_context()` 应该提到两条路径的共同上游；
如果是有意的，`MemoryAllocationState` 的注释要写明它只约束哪一条路径，
否则下一个人会以为 `disable()` 能挡住所有分配。

**顺带**：`MemoryAllocationState::disable()` 目前没有任何调用者
（`kernel/mm/include/ours/mm/memory_allocation_state.hpp`），所以这条不急；
但在第一个 `disable()` 落地之前必须定下来，否则那个调用者会以为自己挡住了而实际没挡住。

## 3. `.init.rodata` 实际是可写的，内核可以自己把它保护成只读

**现象**：潜伏。`llvm-readelf -S` 里这个输出段是 `WA`，所以 physboot 按段权限建立的映射
也是可写的——`.init.rodata` 这个名字在硬件层面不成立。内核的 init hook 表
（`.gktl.init_hook`）和调度类注册表（`.task.schedules`）都在里面，运行期可写。

**原因**：段里的条目含指针（函数指针、字符串指针），内核是 DYN 镜像，所以这些是
`R_X86_64_64` 重定位，工具链必须把段标成可写。让它在重定位之后变只读的标准机制是
`PT_GNU_RELRO`，而 lld 只把它认得的少数段放进 RELRO（`.data.rel.ro`、`.got`、`.dynamic`，
以及按**段类型**认的 `INIT_ARRAY`——`.ccrt_init` 就是这么进去的）。

**这里其实不需要 RELRO**，这是关键：RELRO 存在的理由是「动态链接器要通过最终映射写这些页」。
ours 不是这样——`ElfImage::fix_mapping()` 的注释写着，重定位是通过镜像物理页的**恒等映射**
做的，做完才建立内核映射。所以最终映射从来不需要可写，段被标成 `WA` 只是工具链假设了一个
在这个镜像上不成立的前提。

**试过且不行的两条**（省得别人再试）：

- 输出段属性 `(READONLY)`：`ld.lld: error: kernel.ld:158: symbol not found: READONLY`。
  LLVM 23 的 lld 不认这个 GNU ld 扩展。
- 把输出段改名 `.data.rel.ro.init` 想蹭进 RELRO：段仍是 `WA`，也没多出第四个 `GNU_RELRO`。

**改法**：内核在 `init_vmm()` 里 enrol 完 physboot 的映射清单之后，把
`[__init_rodata_start, __init_rodata_end)` 重新保护成只读。链接脚本已经 `PROVIDE_HIDDEN`
了这两个符号，只差两行 `extern char const[] LINK_NAME(...)` 声明
（照 `kernel/mm/include/ours/mm/vmm.hpp` 里 `KERNEL_INIT_START` 的样子）。那个段独占整页的
PT_LOAD（`0xd0000`，后面紧跟 `ALIGN(MAXPAGESIZE)`），范围干净。

这**不是**在质疑 handoff 清单：清单如实报告了 physboot 建立了什么，而「`.init.rodata` 本该
只读」是只有内核知道的事实。要在改动记录里把这个区别写清楚，否则下一个人会以为
清单又被绕过了。

**要先确认**：`reclaim_init_area()` 之后这些帧会还给 buddy，内核虚拟映射还在
（`EarlyMem::reclaim_init_area()` 的注释说 "the mappings stay"）。重用那些帧的人是走
physmap 别名写的，所以只读的内核映射不挡他们——但这条要实测确认，不要只靠推理。

**严重程度**：低。没有人写它，所以今天没有故障；价值在于把「只读」变成硬件保证而不是命名约定。
