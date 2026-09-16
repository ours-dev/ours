# kernel/sync — 待修

---

## 1. `Mutex` 阻塞时不做优先级继承

`OwnedWaitQueue::park_and_assign_owner()` 现在真的 park 了（`docs/changes/260915/sync-mutex.md`
第 1 条），但**只 park，不 assign owner**：PI 链没有接上。

缺的是 `lock_for_park_and_assign_owner_or_backoff()` 所预留的 ChainLock 式退避协议——
`assign_owner()` → `begin_propagate()` 断言属主的 PI 锁已被持有，而本内核不要全局
`thread_lock`，所以取一串线程锁必须能退避重来。

`lock_for_owner_replacement()`（空函数）和 `lock_for_park_and_assign_owner_or_backoff()`
（直接返回、不加锁）都是同一个协议的占位符。传播那一半（`begin_propagate`、
`complete_propagate`、`assign_owner`、`reset_owner`、`override_thread_base_profile`）已经是实的，
等的就是加锁协议。

后果：持低优先级锁的线程可以无限期挡住高优先级线程。当前只有优先级相同的线程在跑，所以看不出来。

## 2. `Mutex::try_release()` 的快路径是死的

`try_release()` 构造的期望值是**不带** `Contended` 的裸线程指针，而 `acquire` 存进
`m_state` 的总是 `thread | Contended`，所以那个 CAS 永远失败，`release()` 每次都走
`release_slowpath()`。不是正确性问题（队列为空时 `publish(State::Available)` 收尾），
但「无竞争时的快速释放」这条路名存实亡，读代码的人会以为它生效。
