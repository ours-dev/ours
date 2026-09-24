# sync 待修项（2026-09-24）

## 1. `sync::Mutex` 争用时丢失唤醒（已交接给独立会话）

`Mutex::acquire_slowpath()` 里，CAS 失败且看到 `State::Contended` 的线程跳出自旋，调用
`OwnedWaitQueue::park_and_assign_owner()`。后者只拿等待者自己的 PI 锁就 park，在等待队列锁下
**不再复查**互斥锁的状态。持有者的 `release_slowpath()` 若恰好落在"等待者 CAS 失败之后、入队之前"，
会看到队列为空、发布 `State::Available`；等待者随后 park 在一把没人持有、也没人会释放的锁上，永远睡下去。

发现经过：P1-A 第 27 条的探针测试（dbg_1 镜像里 shell 逐个启动 `fault_probe` 看它被杀）。被杀进程的
句柄表在 DPC worker 里销毁，关闭通道一端，在**持有**通道 dispatcher 的 `sync::Mutex` 时给 shell 的 Event
发信号；shell 醒来立刻 `detach_observer()` 去拿同一把锁。临时日志在约一半的启动里停在
"DBG detaching from koid 31"，QEMU monitor 显示 shell 不在任何运行队列上。

改法照 Zircon `Mutex::AcquireContendedMutex()`：在 `ReleaseContendedMutex()` 也会拿的锁下复查互斥锁字再阻塞。

**阻塞的工作**：P1-A 第 27、30 条（探针测试）。它们的代码已写好、验证过一轮，暂存在会话外，等这一条修好再提交——
否则 dbg_1 镜像约一半的启动会停在第一或第二个探针之后。
