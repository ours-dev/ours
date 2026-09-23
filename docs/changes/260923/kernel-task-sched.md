## 新线程在 trampoline 里开中断（2026-09-23）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `trampoline_handoff()`：取当前线程改用 `Thread::Current::get()`（主人的改动）；
  释放 PI 锁之后保留 `arch::local_irq_enable()`，并写明为什么这里必须显式开

**症状**
在主人的工作区版本里，`trampoline_handoff()` 末尾的 `arch::local_irq_enable()` 被去掉了，理由是「调用
`reschedule_common()` 的地方一定拿着当前线程的锁、且一定是 `Guard<.., IrqSave>`」。对被切回来的老线程这成立；
对**第一次运行的新线程**不成立。实测（临时探针，已撤）：
- 两个镜像里 17 个新线程（DPC 工作线程、kshell、laborer、Sample1/2、shell）进入线程体时全是 `IF=0`；
- dbg 镜像的 Sample1 死循环打印 71080 次，每次都是 `IF=0`——它所在的 cpu2 整个期间收不到时钟中断、收不到 IPI，
  不会被抢占。
用户线程不受影响：它们经 iretq 进 ring 3，IF 由帧决定。

**原因**
老线程被切回来时，是从 `reschedule_common()` 返回到**它自己的**调用者，那里的 `Guard<.., IrqSave>` 恢复它当初
保存的中断状态。新线程第一次运行时并不「返回」，而是从 `Thread::trampoline` 开始，栈上没有属于它的 Guard；
那个 IrqSave 的状态在切出去的那个线程的栈上。`x86_switch_context()` 只存取被调用者保存的寄存器，不带 RFLAGS。
于是新线程带着 IF=0 进入线程体；此后每次 park，Guard 存下的是 0、恢复的也是 0，一辈子关着。

**改法**
改前（工作区版本）：

```cpp
    Self::complete_context_switch(current_thread);
    current_thread->get_pi_lock().release();
}
```

改后：

```cpp
    Self::complete_context_switch(current_thread);
    current_thread->get_pi_lock().release();

    arch::local_irq_enable();
}
```

顺序不变：先交接锁，再开中断——反过来的话，缺口里来一个 tick 会对这个线程跑 `tick_thread()`，撞上它还被持有
的 PI 锁。新增的注释把「为什么新线程必须在这里显式开」写清楚，免得再被当作多余去掉。

**参照**
Zircon 的线程 trampoline：锁交接之后 `arch_enable_ints()`。

**验证**
构建通过；两个镜像各启动两次正常。复测探针：Sample1 在 cpu2 上打印 69161 次、全部 `IF=1`，Sample2 同样。

**故意没做**
没有改成「给新线程伪造一个 IrqSave 的初始状态」来统一两条路径：一行显式的开中断更直接，也与 Zircon 一致。
