# object 待修项（2026-09-24）

## 1. 从未启动的线程，其内核线程泄漏

`ThreadDispatcher::spawn()` 先建内核线程（`task::Thread::spawn`，Initial 状态，挂在全局线程表上，带内核栈），
`start()` 才让它运行。若 ThreadDispatcher 在 `start()` 成功之前被销毁，那个内核线程永远留着：没有东西释放它，
它的 trampoline 参数指向已释放的 dispatcher（不会被解引用，因为它永远不会被 resume）。

今天够不到：`userabi::launch()` 对每个 spawn 出来的线程都 start，中间没有失败路径。将来的触发点：进程在首线程
启动前被杀（`attach_thread` 回 BadState），或 P1-B 的 `thread_create` 之后不 start 就关掉句柄。

改法照 Zircon `Thread::Forget()`：销毁一个从未启动的线程时把它从全局表摘下、释放栈与线程结构。

## 2. 死进程的页表页不释放

映射、用户页和只被它们引用的 VMO 已经随 `VmAspace::destroy()` 归还（`docs/changes/260925/mm.md`「VmAspace::destroy」一节），
`~VmAspace()` 也会运行了。剩下的是页表本身：`~ArchVmAspace() = default`、`~PageTable() = default`，unmap 从不释放变空的
中间表，于是每个死进程留下它的 PML4 和用户半区的全部 PDPT/PD/PT。

改法：`~VmAspace()` 里调新增的 `ArchVmAspace::destroy()`，只走用户半区的 PML4 表项（PML4[511] 是别名来的内核表，绝不能碰），
自底向上释放中间表，跳过叶子（叶子指向的帧归 VMO），最后释放 PML4。只能在析构里做、不能在 `destroy()` 里做：`destroy()`
跑在进程最后一个线程上，CR3 还指着这张表。对照 Zircon `X86ArchVmAspace::Destroy()`（`arch/x86/mmu.cc`）。

## 3. AMD 上 `lfence` 不保证串行化派发

`kernel/arch/x86/kernel/user_copy.cpp` 在范围检查与拷贝之间用 `lfence` 做 Spectre V1/V1.1 缓解。Intel 上 `lfence`
是派发串行化的；AMD Fam10h–Zen1 上要先把 `MSR_AMD64_DE_CFG`（0xC0011029）第 1 位置上才是，内核从没设过。

改法照 Linux `init_amd()` 里的 `msr_set_bit(MSR_AMD64_DE_CFG, MSR_AMD64_DE_CFG_LFENCE_SERIALIZE_BIT)`，
或 Zircon 在 AMD 上的同类设置；需要 CPU 厂商识别。
