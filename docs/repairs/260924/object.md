# object 待修项（2026-09-24）

## 1. 从未启动的线程，其内核线程泄漏

`ThreadDispatcher::spawn()` 先建内核线程（`task::Thread::spawn`，Initial 状态，挂在全局线程表上，带内核栈），
`start()` 才让它运行。若 ThreadDispatcher 在 `start()` 成功之前被销毁，那个内核线程永远留着：没有东西释放它，
它的 trampoline 参数指向已释放的 dispatcher（不会被解引用，因为它永远不会被 resume）。

今天够不到：`userabi::launch()` 对每个 spawn 出来的线程都 start，中间没有失败路径。将来的触发点：进程在首线程
启动前被杀（`attach_thread` 回 BadState），或 P1-B 的 `thread_create` 之后不 start 就关掉句柄。

改法照 Zircon `Thread::Forget()`：销毁一个从未启动的线程时把它从全局表摘下、释放栈与线程结构。

## 3. AMD 上 `lfence` 不保证串行化派发

`kernel/arch/x86/kernel/user_copy.cpp` 在范围检查与拷贝之间用 `lfence` 做 Spectre V1/V1.1 缓解。Intel 上 `lfence`
是派发串行化的；AMD Fam10h–Zen1 上要先把 `MSR_AMD64_DE_CFG`（0xC0011029）第 1 位置上才是，内核从没设过。

改法照 Linux `init_amd()` 里的 `msr_set_bit(MSR_AMD64_DE_CFG, MSR_AMD64_DE_CFG_LFENCE_SERIALIZE_BIT)`，
或 Zircon 在 AMD 上的同类设置；需要 CPU 厂商识别。
