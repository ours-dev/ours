# kernel-task 待修项（2026-09-24）

## 2. `suspend_or_kill` 的 Parked 分支对同一把 PI 锁加锁两次（已交接给独立会话）

`Thread::suspend_or_kill()` 持着目标线程的 PI 锁调用 `wq->unpark_thread(this)`，后者经
`dequeue_waiter` → `wake_dequeued_thread`（`kernel/kernel/task/wait_queue.cpp`）再拿同一把锁。
自旋锁不可重入：dbg_1 在 `SpinLock::acquire` 的 `!is_held()` 断言失败，dbg_0 关着中断永远自旋。
同一分支还有两处：`wq` 可能为空（`Event::signal` 在队列锁下清掉 `m_waitqueue`、放队列锁之后才拿 PI 锁）；
被唤醒的线程拿到的是 `Status::Ok`，而不是 kill/retry 状态。

早于 P1-A（唤醒侧的 PI 锁来自 d851d9bf）。今天没有调用者走到——唯一的 kill 是线程在 Running 时杀自己；
杀一个阻塞在 `object_wait_one` 里的线程（P1-B 的 `task_kill`、多线程进程被杀）就会走到。
改法照 Zircon `Thread::SuspendOrKillInternal()`：以 `InternalIrqKilled`/`InternalIrqRetry` 唤醒，且不重复拿 PI 锁。

## 3. `suspend_or_kill` 的 Sleeping 分支与睡眠定时器竞争（已交接给独立会话）

Sleeping 分支不看 `is_interruptible()`、以 `Status::Ok` 唤醒；`sleep_etc` 的定时器回调在
`trylock_or_cancel` 成功后不检查线程是否仍在 Sleeping。提前唤醒之后定时器仍挂着，到期时会去
unpark 一个已是 Ready/Running 的线程（dbg_1 触发 `scheduler.cpp` 的状态断言，dbg_0 重复入队）。
改法照 Zircon：不唤醒不可中断的睡眠者；`HandleSleep()` 在 `state() != THREAD_SLEEPING` 时直接返回。

## 4. `Thread::Current::tid()` 读的是 `%gs:0x70`，不是当前线程的 tid（已交接给独立会话）

它把 `&(CURRENT_INSTANCE->m_tid)` 交给 `CpuLocal::read()`；`CURRENT_INSTANCE` 是从不被写入的
每 CPU 模板里的空指针，地址折叠成常量 `offsetof(Thread, m_tid)` = 0x70，于是生成 `movq %gs:0x70, ...`。
每个 prof 样本的 tid 都是垃圾值。改为 `Thread::Current::get()->m_tid`。`PmNode::Current::hard_affinity()`
（`kernel/mm/include/ours/mm/pm_node.hpp`）是同一个错误，属于 mm。
