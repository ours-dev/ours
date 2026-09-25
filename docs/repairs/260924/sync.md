# sync 待修项（2026-09-24）

## 1. `Mutex` 没有析构函数，释放者放队列锁时 mutex 可能已被销毁

`Mutex::release_slowpath()` 在队列锁下发布新状态（`Available` 或新主人），然后才放队列锁。
发布 `Available` 的那一刻起，别的线程就能走快路径拿锁、放锁、再销毁这个 mutex（比如引用计数
归零的 dispatcher）；释放者随后那一下 `m_lock.store(UNLOCKED)` 就写进了已释放的内存。
交接给新主人的路径同理：新主人一被唤醒就可能放锁并销毁。

潜伏：今天没有观察到。触发条件是"最后一次释放与销毁在另一颗 CPU 上紧挨着发生"。

照 Zircon `Mutex::~Mutex()` 改：析构时自旋，直到 `m_waiters` 的队列锁 `is_unlocked()`，
再断言状态是 `Available`。它的注释把这个窗口讲得很清楚（`ReleaseContendedMutex()` 末尾）。

## 2. 递归获取 `Mutex` 静默死锁

持有者再 `acquire()` 同一把锁，会在 `acquire_slowpath()` 里打上 `Contended` 然后 park——
等的是自己，永远不会醒，也没有任何诊断。

照 Zircon `Mutex::AcquireContendedMutex()`：阻塞之前
`if (LK_DEBUGLEVEL > 0 && IsHeld()) panic(...)`。本树对应 `KERNEL_DEBUG_LEVEL`，
`holder() == current_thread` 即可判断。

## 3. `acquire_common<true>` 的返回值和 `acquire()` 的循环含义冲突

`Mutex::acquire()` 把 `acquire_common()` 的返回值读作"还没拿到，再来一次"。但时间片延长那条
实例化（`TimesliceExtension<true>`）在拿到锁时返回的是 `set_timeslice_extension()` 的结果——
Zircon 里这个 bool 的意思是"延长设上了，释放时要清"，与"没拿到"无关。一旦有人走 `true` 这条路，
拿到锁、设上延长，就会被当成没拿到，再去抢一次自己持有的锁（见第 2 条）。

潜伏：`acquire()` 只实例化 `false`，`CriticalMutex` 还是空类。实现 `CriticalMutex` 之前要先把
这两个含义拆开（Zircon 的 `Acquire()` 丢掉返回值，`CriticalMutex::Acquire()` 才用它）。
