# sync 待修项（2026-09-08）

## 1. `sync::Mutex` 在竞争时不排他：`acquire()` 丢掉了「没拿到锁」这个返回值

来源：2026-09-08 对 vmalloc series 做对抗式评审时查出，六个维度里的并发维度报出，
三个反驳者中两个确认。

### 症状

`Guard guard{some_mutex}` 在锁已被别的线程持有时**照样进入临界区**。

### 证据

```cpp
// kernel/include/ours/sync/mutex.hpp:91
inline auto Mutex::acquire(Duration spining_duration) -> void {
    m_canary.verify<Self>();
    acquire_common(spining_duration, TimesliceExtension<false>{});   // 返回值被丢弃
}
```

`acquire_common()` 的 `bool` 语义是**「没有拿到锁」**：CAS 成功时 `return false`，
否则尾调 `acquire_slowpath()`。

```cpp
// kernel/kernel/sync/mutex.cpp:66
auto const deadline_ticks = now_ticks; // FIXME(SmallHuaZi)
```

`deadline_ticks` 等于 `now_ticks`，所以 `do { ... } while (now_ticks < deadline_ticks)`
的循环体**只跑一遍**。锁真被持有时 CAS 失败、`old_mutex_state` 带 `Contended`、
`break`，然后：

```cpp
// kernel/kernel/sync/mutex.cpp:103
    // Failed to wait a lock release. Make current thread sleep

    // Every exit above either returns false on a successful CAS or breaks
    // here: contended by another thread, self-owned, or the spin deadline
    // passed. None of those acquired the mutex.
    return true;
```

那句 "Make current thread sleep" 底下**没有代码**。`m_waiters` 从来没有人挂上去。
`true`（没拿到）一路传回 `acquire()`，被丢掉，`Guard` 的构造函数返回，调用方继续。

### 后果

释放侧同样错乱：非持有者的 `Guard` 析构会调 `release()`，`try_release()` 拿它自己的
线程指针去 CAS，失败后走 `release_slowpath()`；`m_waiters` 为空于是
`publish(State::Available)`——**它把别人的锁解开了**，顺带在进入时
`reset_initial_assigned_cpu()` 覆盖掉了真正持有者的 `m_cpunum`。

单 CPU 也会发生，抢占就够：A 持锁后在 `VmObjectPaged::create()` 里被抢占，B 进来，
两者都在临界区里。

### 影响面

`sync::Mutex` 已经是 `VmObjectPaged::m_mutex`、`MemCache::CACHE_LOCK`、
`Dispatcher::m_lock`、`ProcessDispatcher::m_thread_list_lock` 的类型。
2026-09-08 的 vmalloc series 把它又放到了**每一次超过 `KMALLOC_MAX_SIZE` 的
`operator new` / `operator delete`** 的路径上（`heap::LOCK`）。

vmalloc 这一侧目前还安全，但**靠的是调用者恰好都是单线程的**（启动自检、prof 的 init
hook、`userboot_init()`），不是靠锁。第一个在运行期线程里 `new` 出超过 4 KiB 的调用者
就会打破它。

### 修法方向

要么让 `acquire_slowpath()` 真的把线程挂到 `m_waiters` 上并让出（这需要和调度器的
阻塞/唤醒对接），要么在此之前让 `acquire()` **至少自旋到真的拿到锁**再返回——后者对
"锁内会睡眠"的用法是错的，所以只能是权宜。无论哪条都是 `kernel/kernel/sync` 的 lane。

在它修好之前，任何依赖 `sync::Mutex` 做真实排他的新代码都是不安全的。
