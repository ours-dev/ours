# x86-mm 改动记录（2026-09-24）

## SRAT 的 CPU→节点绑定改用 `set_nid_early`（2026-09-24）

**文件**
- `kernel/arch/x86/mm/srat.cpp` — `bind_node_to_cpu()` 里 `set_nid` → `set_nid_early`

**症状**
只在 `mm_numa = true` 且机器给了 SRAT 时出现（默认构建不编译 srat.cpp）。
用 `NUMA=1`（QEMU 4 节点、8 CPU）启动，两个镜像都在把内存交给 buddy 之后立刻缺页：

```
Released 16 regions, 255699 frames (998 MiB) to the buddy allocator
Unhandled page fault: vector=14, error_code=0x2
  cr2=0xffffff8800016fff
  rip=0xffffff8840025605 (+0x25605)      -> memmove + 0x65
  rcx=0x17000  rdx=0x17000  rdi=0x0
```

**原因**
`PmNode::Current` 有两张 CPU→节点表：`STATIC_CPU_TO_NODE`（固件说的"家"，用来建
每个节点的 hard affinity）和 `DYNAMIC_CPU_TO_NODE`（运行时 CPU 当前归属）。两者都以
`NUMA_MAX_NODE` 作"未知"哨兵。

- `set_nid_early()` 两张表一起写，不碰任何节点对象——给启动早期用的。
- `set_nid()` 是运行时把一个 CPU 从一个活着的节点挪到另一个：只写动态表，然后去
  旧节点、新节点上改 `m_work_cpus` 并发布每 CPU 指针。

SRAT 解析跑在 `PlatformInitLevel::PrePmm + 1`，此时一个 `PmNode` 都还没建。调用
`set_nid()` 的结果是：动态表写上了，`PmNode::node(nid)` 为空所以后半段全跳过，**静态表
留在哨兵上**。接着 `PmNode::bootstrap()` 的 `claim_cpu` 看动态表已经不是哨兵，认为
"有拓扑源认领过了"，也不写静态表。`PmNode::init()` 按静态表建 hard affinity，于是每个
节点的 hard affinity 都是空的；`CpuLocal::Mapper::build_map_info()` 拿不到任何亲和信息，
只能把 CPU 单元轮转分到各节点组里。

从"hard affinity 全空"到那次 `memmove` 写到 physmap 基址附近，中间的调用链没有追完：
故障地址 `physmap + 0x16fff`、长度 `0x17000`、`rdi=0` 像是把一段模板拷到一个从空分配
换算出来的地址上，但没有回溯可以证实。能确定的是：两次运行只差这一行，改前两个镜像
都在这里崩，改后都越过了这里。

这是"早期"与"运行时"两个接口被用错时机的典型：`set_nid()` 自身没有错，它的"新旧相同
就返回""节点不存在就不管"在运行时都是对的；错在它被拿来做它不负责的首次登记。

**改法**
改前：

```cpp
            mm::PmNode::Current::set_nid(static_cast<CpuNum>(count), NnId{binding.domain});
```

改后：

```cpp
            mm::PmNode::Current::set_nid_early(static_cast<CpuNum>(count), NnId{binding.domain});
```

注释写明为什么这里只能用 early 版本。

**参照**
`Linux acpi_numa_processor_affinity_init()` 只把 `apicid → node` 记进 `__apicid_to_node[]`，
`init_cpu_to_node()` 随后用 `numa_set_node()` 写进 `early_per_cpu(x86_cpu_to_node_map)`——
都是纯表写入，不碰任何 `pg_data_t`。本项目的 `set_nid_early()` 对应的就是这一层。

**验证**
- 默认构建（`out/bringup.x86`，`mm_numa = false`）：构建通过，两个镜像启动，init↔shell 照常。
- NUMA 构建（`out/numa.x86`，`mm_numa = true`，`NUMA=1`）：
  - 改前：两个镜像都在 buddy 释放之后缺页（见上）。
  - 改后：越过该点，8 个 CPU 全部上线，到达 `kshell>`；随后在 userboot 之前停住，
    80 秒内没有进一步输出。这是被原来的崩溃挡在后面的另一个问题，已记入
    `docs/repairs/260924/x86-mm.md` 第 1 条，没有在这一条里修。

**故意没做**
- `count`（SRAT 条目顺序）被直接当成 CPU 号，没改。CPU 号由 MADT 解析分配，而 MADT 在
  `InitLevel::Irq` 才跑，比这里晚得多；正确做法是先记 `apicid → node`，等 MADT 分好号
  再映射，属于另一件事（待修项第 2 条）。
- 没有处理 SRAT 指向无内存节点的 CPU（待修项第 3 条）。

## 内核态在 user_copy 之外碰用户地址：不论有没有 SMAP 都拒绝（2026-09-24）

**文件**
- `kernel/arch/x86/mm/fault.cpp` — 调页之前的拒绝条件从"SMAP 开、AC 清、用户地址"改为"内核态、用户地址、不在用户拷贝里"；`inside_user_copy()` 与 `resume_user_copy()` 拆开

**症状**
潜伏。没有 SMAP 的 CPU 上（这里 QEMU 的默认型号 Nehalem 就是），内核代码若绕过 `user_copy`
直接解引用一个用户指针、而那一页尚未提交，缺页处理会替它把页调进来，访问成功，什么也不报。
同样的 bug 在有 SMAP 的 CPU 上会被当场拒绝。一个 bug 抓不抓得到取决于 CPU 型号。

**原因**
原来的拒绝条件是 SMAP 报告这类错误的方式（`SMAP 开 && AC 清 && 用户地址`），不是这类错误本身的
定义。本内核的规则是"ring 0 只在 user_copy 里碰用户内存"，它不依赖 CPU 特性；检查也不该依赖。

今天还没有惰性提交的用户映射（程序、栈、vDSO 都是 `Commit` 映射的），所以这个差别暂时触发不到；
P1-C 的 `vmar_map` 不带 commit 之后就会有。

**改法**
改前：

```cpp
    auto const supervisor = (error_code & PAGE_FAULT_USER) == 0;
    auto const access_open = frame->flags.get<arch::ArchCpuState::AC>() != 0;
    if (supervisor && feature_has_smap && !access_open && !mm::is_kernel_address(addr)) {
        log::error("potential SMAP violation: ...", addr);
        return Status::AccessDenied;
    }
    ...
    if (resume_user_copy(frame, addr, supervisor)) {
        return Status::Ok;
    }
    return status;
```

改后：

```cpp
    auto const supervisor = (error_code & PAGE_FAULT_USER) == 0;
    auto const user_address = !mm::is_kernel_address(addr);
    if (supervisor && user_address && !inside_user_copy(frame)) {
        auto const access_open = frame->flags.get<arch::ArchCpuState::AC>() != 0;
        if (feature_has_smap && !access_open) {
            log::error("potential SMAP violation: ring 0 touched user address {:#x} outside user_copy", addr);
        } else {
            log::error("page fault in kernel mode on user address {:#x} outside user_copy", addr);
        }
        return Status::AccessDenied;
    }
    ...
    if (supervisor && user_address) {
        resume_user_copy(frame);
        return Status::Ok;
    }
    return status;
```

"在用户拷贝里"= 槽非零且出错指令在拷贝例程区间内（`inside_user_copy()`），与上一条恢复路径用同一个
判断。于是调页之后的恢复不必再重复检查：走到那里的内核态用户地址故障，按前面的拒绝，一定在拷贝里。
SMAP 能解释时仍打原来的 "potential SMAP violation"，否则打新的消息。

内核自己地址上的内核态缺页照旧交给 `page_fault()`——这与 Zircon 不同：Zircon 对任何没有
`page_fault_resume` 的内核态缺页都停机，因为它的内核从不按需调入自己的内存；本内核还没有这个前提。

改之前查过 profiler：采样器的内存读取器限定在内核栈窗口内，明确不展开用户栈（`sampler.cpp`），
所以没有合法的 user_copy 之外的用户内存访问会被这一条误伤。

**参照**
- Zircon `x86_pfe_handler()`（`kernel/arch/x86/exceptions_c.cc`）：内核态缺页没有 `pfr` 就
  `exception_die(frame, "page fault in kernel mode\n")`，SMAP 能解释时先打 "potential SMAP failure"。
- Linux `do_user_addr_fault()` 对 SMAP 的同类检查（`X86_PF_USER` 清且 `EFLAGS.AC` 清时直接报错：较新的内核调 `page_fault_oops()`，较早的是 `bad_area_nosemaphore()`）；
  Linux 没有 SMAP 时不拒绝这类访问，这里比它更严。

**验证**
- 构建通过；两个镜像在 Nehalem 与 `CPU=max` 下都跑到 `shell: bad-pointer checks passed` 和 `--- end ---`
  （拷贝里的故障仍然走恢复路径）。
- Nehalem 下在 `sys_debug_write` 里临时裸读一次 `0x7fff'ffff'e000`（未提交）：
  `page fault in kernel mode on user address 0x7fffffffe000 outside user_copy`，随后内核停机。
  注意裸读一个**已在**页表里的用户页在没有 SMAP 时根本不缺页，这一条管不到——那只能靠 SMAP。

**故意没做**
没有把内核自己地址上的内核态缺页也改成停机：要先确认内核不再按需调入自己的内存（vmalloc 等）。

## 勘误：P1-A 第 17–29 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「内核态在 user_copy 之外碰用户地址」一节写"内核自己地址上的内核态缺页照旧交给 `page_fault()`"，并以此为理由
  不在内核地址上直接停机。前提是错的：`Thread::Current::fault()` 对任何内核地址立即返回 `NotFound`，调用者随即 panic——
  内核地址上的内核态缺页本来就会停机，与 Zircon 一致。"故意没做"里那条理由因此不成立，也不需要做。`fault.cpp` 的注释已改正。
- 同一节新增的停机消息 "user copy faulted on ..., outside the user range it was admitted for" 只看槽是否被设。拷贝进行中
  来一个中断、其处理函数碰了用户地址时，也会被归到拷贝头上。现在还要求出错指令在拷贝例程里。

## 从不可执行的用户页取指：缺页交给映射判定，不再当伪缺页无限重试（2026-09-24）

**文件**
- `kernel/mm/include/ours/mm/vm_fault.hpp` — `VmfCause` 加 `Instruction`
- `kernel/arch/x86/mm/fault.cpp` — `x86_handle_page_fault()` 把错误码第 4 位（I/D）翻译成 `VmfCause::Instruction`
- `kernel/mm/vm_mapping.cpp` — `VmMapping::fault()` 对没有 `MmuFlags::Executable` 的区域拒绝取指缺页

**症状**
用户程序跳进自己的栈（`((void(*)())stack_buffer)()`）后既不返回、也不被杀：日志停在 `--- end ---`，
没有 `terminating process`。线程在 #PF（`error_code=0x15`，P|U|I）里原地打转；它仍可被抢占，所以外部 kill
还有效，但故障本身永远不会杀掉它。P1-A 的规则"地址空间解决不了的 #PF 杀进程"（`x86_dispatch_exception()`）
从未被触发。

**原因**
缺页的原因有四个维度：页在不在（P）、读还是写（W）、哪个特权级（U）、是不是取指（I/D）。翻译成 `VmfCause`
时只留了前三个，于是一次取指到了映射那里就和一次读没有区别。

读在一个"存在但不可执行"的页上是合法的，所以 `VmMapping::fault()` 一路放行：写检查不适用；接着查页表，发现
PTE 已经在、权限（含 NX）与区域的记录一致，按"另一个 CPU 已经解决了 / 本 CPU 还在用旧 TLB"处理，回 `Ok`。
`iretq` 回去重做同一次取指，PTE 没有变，NX 还在，再缺页——而每一次都被当成已解决。伪缺页捷径本身没错，错在它
假定"PTE 与区域一致"就意味着"这次访问是区域允许的"，而这个推论只对区域允许的访问类型成立；把访问类型丢掉，
推论就对一切访问都成立了。

userboot 的栈是 `User|Readable|Writable`、没有 `Executable`，PTE 带 NX，EFER.NXE 在 BSP 和 AP 上都无条件打开，
所以今天就能复现。程序映像整体映射为 RWX，只有栈（以及将来任何不可执行的映射）会撞上。

**改法**

改前：

```cpp
// kernel/mm/include/ours/mm/vm_fault.hpp
enum class VmfCause {
    None,
    Write = BIT(0),
    Absent = BIT(1),
    User = BIT(2),
    Software = BIT(3),
};

// kernel/arch/x86/mm/fault.cpp
cause |= (error_code & PAGE_FAULT_PRESENT) ? VmfCause::None : VmfCause::Absent;
cause |= (error_code & PAGE_FAULT_WRITE) ? VmfCause::Write : VmfCause::None;
cause |= (error_code & PAGE_FAULT_USER) ? VmfCause::User : VmfCause::None;

// kernel/mm/vm_mapping.cpp, VmMapping::fault()
bool const will_write = !!(vmf->cause & VmfCause::Write);
if (will_write && !(mmuf & MmuFlags::Writable)) {
    return {Status::AccessDenied, 0};
}

auto &arch_aspace = m_aspace->arch_aspace();
// ... query(): present and permissions match -> return {Status::Ok, 0};
```

改后：

```cpp
// kernel/mm/include/ours/mm/vm_fault.hpp
    Software = BIT(3),
    Instruction = BIT(4),   // Zircon: VMM_PF_FLAG_INSTRUCTION

// kernel/arch/x86/mm/fault.cpp
constexpr usize PAGE_FAULT_INSTRUCTION = 1u << 4;
...
cause |= (error_code & PAGE_FAULT_INSTRUCTION) ? VmfCause::Instruction : VmfCause::None;

// kernel/mm/vm_mapping.cpp, VmMapping::fault(), right after the write check
bool const will_execute = !!(vmf->cause & VmfCause::Instruction);
if (will_execute && !(mmuf & MmuFlags::Executable)) {
    return {Status::AccessDenied, 0};
}
```

检查放在查页表之前，和写检查同一位置：不论页在不在，答案都一样。`AccessDenied` 经
`Thread::Current::page_fault()` 返回到 `x86_dispatch_exception()`，按 P1-A 的规则变成 `FatalPageFault` 杀进程。

被否掉的方案：在 arch 处理函数里见到 P|I 就直接判死。取指缺页在页已存在时并不总是违规——`protect()` 给区域加上
`Executable` 之后，页表里可能还是旧的 NX 项，这时的缺页是伪缺页，`VmMapping::fault()` 里"权限不一致就 `protect()`
一次"的分支正是为它准备的。页该有什么权限只有映射知道，arch 层只负责如实翻译。

**参照**
- Zircon `x86_pfe_handler()`（`kernel/arch/x86/exceptions_c.cc`）：`PFEX_I` → `VMM_PF_FLAG_INSTRUCTION`，只翻译不判定。
- Zircon `VmMapping::PageFaultLockedObject()`（`kernel/vm/vm_mapping.cc`）：`VMM_PF_FLAG_INSTRUCTION` 时把
  `ARCH_MMU_FLAG_PERM_EXECUTE` 加进所需权限，在任何查找之前一次性比较，不满足即 `ZX_ERR_ACCESS_DENIED`。
- Linux `do_user_addr_fault()` → `access_error()`（`arch/x86/mm/fault.c`）走的是另一条路：它不看 `VM_EXEC`，而是把
  "非写且 `X86_PF_PROT`（页已存在）"一律判错，经 `bad_area_access_error()` 发 SIGSEGV；不存在的页先按 VMA 的
  `vm_page_prot`（带 NX）调入，重新取指时再以 PROT 缺页被判错。它敢这样做，是因为 `mprotect()` 返回前已改好 PTE，
  用户页上不存在"页表还是旧的"非写伪缺页。本内核的伪缺页捷径恰恰是为 `protect()` 没改到的项准备的，
  "存在即违规"在这里不成立，所以照 Zircon 按区域权限判定。

**验证**
- 构建通过（`prebuilt/ninja/ninja -C out/bringup.x86`）；clang-format 已跑；clang-tidy 在改动行上无新告警。
- 临时探针（未提交）：`examples/shell/shell.cpp` 的 `--- end ---` 之后，经函数指针调用栈上一个只含 `ret`（0xc3）的缓冲区。
  - 改前（`kernel.dev.obi`，40 s）：停在 `--- end ---`，无 `terminating`、无后续输出，探针之后的 "PROBE: returned" 也没有打出。
  - 改后 `kernel.dev.obi`：
    `terminating process 'shell' (koid 35) thread koid 39: type=FatalPageFault vector=page fault error_code=0x15 rip=0x11fff70 sp=0x11fff38 cr2=0x11fff70`，
    随后 `object: thread (koid 39) of process 'shell' destroyed`、`object: process 'shell' (koid 35) destroyed`，无 panic。
  - 改后 `kernel.dev.dbg_1.obi`：同样的三行（koid 23/27，cpu1），内核继续运行到 60 s 观察结束（`I'm Thread 1/2` 持续输出）。
  - `rip == cr2 == 栈地址`，确认是取指本身缺页，不是 `ret` 之后的某个访问。

**故意没做**
- 没有补读权限检查。Zircon 对非写缺页还要求 `ARCH_MMU_FLAG_PERM_READ`；本内核 x86 上"存在即可读"，
  一个有 `Executable` 无 `Readable` 的区域目前无法在页表里表达，补上也触发不到。等有架构能表达只执行页时再加。
- 没有碰 `docs/repairs/260924/x86-kernel.md` #1（所有叶子 PTE 被强制 `Writable|Global`）：那条不清 NX，
  不影响本问题，已交给另一个会话。
