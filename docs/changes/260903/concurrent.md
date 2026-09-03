# system/lib/concurrent — 改动记录

格式见 `docs/changes/TEMPLATE.md`。这个库对着 Zircon 的
`zircon/system/ulib/concurrent`，装的是不依赖运行环境的并发原语。

---

## 1. ChainLock：为拆掉全局 ThreadLock 准备的仲裁原语（2026-09-03）

**文件**
- `system/lib/concurrent/include/concurrent/chain_lock.hpp` — 新增。token、
  token 发生器、锁本体和四态仲裁结果。
- `system/lib/concurrent/test/chain_lock_test.cpp` — 新增。宿主侧单元测试。
- `system/lib/concurrent/BUILD.gn` — 把头文件登记进 `cpp_interface`，加上测试目标。
- `BUILD.gn` — 把测试挂进默认构建。

**症状**

没有症状，这是新设施。它要解决的是一个现在还写不出来的东西：
`OwnedWaitQueue::lock_pi_chain_common()`（`kernel/kernel/task/owned_wait_queue.cpp`）
至今是一个空的 `while (true)`，因为用普通自旋锁根本没法把它写对。

**原因**

PI 链的形态是 `thread -> 它拥有的队列 -> 队列的 owner -> owner 阻塞在的队列 -> ……`，
边是走的时候才发现的，所以「这次操作要拿哪几把锁、按什么顺序拿」在开始之前不知道。
两条从不同线程出发的游走可以用相反的顺序碰到同一对节点——这对任何「只会等」的锁都是死锁。

常规解法是给整张图一把大锁，也就是 Zircon 的全局 `thread_lock`。本项目明确不要它，
那么每个对象自己的锁就必须自带一套仲裁规则。

关键是把锁的状态字从「谁持有」（`sync::SpinLock` 存的是 cpu+1）换成「持有者这次操作有多老」。
一次操作先从全局单调计数器取一个 token，这一组锁**全部**用同一个 token 去 CAS。
CAS 失败时拿到的不只是「被占了」，而是「被谁占了、他和我谁更老」：

- `T < X`（我更老）：自旋等，该让路的是对方。
- `T > X`（我更年轻）：**退避**——放掉这次操作已经持有的每一把链锁，重走。
- `T == X`：游走绕回了已经访问过的节点。

两条性质由此而来，而且都很容易被「优化」掉，所以在头注释里写死了：

- **无死锁**：等待边只从年轻指向年老，token 是全序，等待图不可能成环。
- **无饿死**：退避时**保留原 token**。一个不断重试的操作只会相对越来越老，
  终将持有全系统最老的 token，那之后没有任何东西能再让它退避。
  退避时重新取 token 看起来更整洁，实际上会变成活锁。

协议还有一半这个类管不了，只能写进注释：**只持有部分锁时可以读、绝不能写**。
退避是靠放锁来回滚的，它没有办法回滚已经写下去的东西。

**改法**

`ChainLock` 只做锁本身，token 是显式参数：

```cpp
auto try_acquire(ChainLockToken) -> Result;              // Ok / Retry / Backoff / Cycle
template <typename Relax> auto acquire_or_backoff(ChainLockToken, Relax &&) -> bool;
template <typename Relax> auto acquire_first_in_chain(ChainLockToken, Relax &&) -> void;
auto release(ChainLockToken) -> void;
```

否掉的方案，两个：

1. **照 Zircon 把 `ChainLock` 做成 `template <typename Transaction>`。**
   Zircon 让锁自己去问 `Transaction::Active()` 拿当前 token、做调试计数，
   于是锁可以自己断言「first-in-chain 时确实一把没持」。代价是这个库从此绑死在一个
   环境上，宿主测试要先造一个假 Transaction 才编得动。这里反过来：token 是参数，
   「本 CPU 当前 token 是哪个、持了几把、有没有开始改动」归上层的事务层，
   事务**持有**锁而不是被织进锁里。库因此完全无环境依赖，宿主上就能跑完整协议。
2. **把自旋提示 `relax` 做成类模板参数。** 那会把 ChainLock 按提示的种类裂成若干个
   互不相通的类型，而一把锁的身份和等待者怎么打发时间没有关系。所以它是**函数参数**——
   和 ustl 侵入式算法里策略作为函数参数传入是同一条规矩。

`Cycle` 单独作为一个返回值而不是直接 trap：图的形状是用户态决定的，一条自己绕回自己的
futex 链是内核必须活下来并报告的程序错误，不是内核 bug，怎么处理只有发起操作的调用方知道。

`release()` 收 token 只为断言，`DEBUG_ASSERT` 在 `KERNEL_DEBUG_LEVEL` 没定义的宿主构建里
整个消失，所以那里显式 `static_cast<void>(token)`。

**参照**

- Zircon `zircon/system/ulib/concurrent/include/lib/concurrent/chainlock.h`
  的 `ChainLock::AttemptAcquireCommon()`，设计文档
  `zircon/kernel/lib/kconcurrent/docs/chainlock.md`，
  以及协议的 TLA+ 模型 `zircon/system/ulib/concurrent/models/chainlock`。
- Linux 没有对应物。它的 `rt_mutex_adjust_prio_chain()` 走 PI 链时用的是
  「拿一把、看一眼、放掉、重试」的乐观遍历，靠 `pi_lock`/`rq->lock` 的固定嵌套顺序
  外加重试计数来避免死锁，而不是给锁本身加仲裁。

**验证**

- 宿主单元测试 `chain-lock-test` 通过：token 单调且唯一、无竞争的 acquire/release 往返、
  仲裁四态逐行验证、以及一个手工编排的「两条游走以相反顺序碰同一对节点」场景——
  年轻的那条退避、放锁，年老的那条走完，年轻的那条用**原 token** 重试后拿到两把。
- 确认断言是活的：故意把 `assert(first < second)` 改成 `>` 重编，测试如期 abort
  （宿主构建没有定义 `NDEBUG`），改回后通过。
- 内核工具链下的编译验证：用 `owned_wait_queue.cpp` 那条真实编译命令编一个探针 TU，
  实例化全部模板体并走一遍 acquire/backoff/release，`-Wthread-safety` 开启、
  `KERNEL_DEBUG_LEVEL=0`，**零告警**。这一步是必要的——`-fsyntax-only` 不检查模板体，
  而且 `-Wthread-safety` 只在内核工具链上开。
- 全量 `ninja -C out/bringup.x86` 通过；`clang-format` 与 `gn format` 均已应用。
- **没有 QEMU 实测**：内核里还没有任何代码用到它。

**故意没做**

- 没有事务层。`ChainLockTransaction`（每 CPU 的当前 token、持锁计数、
  退避点上「手上必须一把不剩」的断言、中断关闭的纪律）是下一条。
  没有它，`acquire_first_in_chain()` 的前置条件只能靠调用方自觉。
- 没有做 Zircon 的保留 token 段（它低位留了 10000 个值给保存/恢复事务状态用）。
  本树还没有那个特性，等真需要时再引入，那时是加一个常量的事。
- 没有做 Zircon 的争用状态与「自旋一会儿再挂起」混合等待。现在纯自旋，
  `release()` 因此是一次 store 而不是 Zircon 的三步。
- 没有把 `Thread::m_pi_lock` 和 `WaitQueue::m_lock` 换成 ChainLock，
  也没有动 `lock_pi_chain_common()`。那是第 3、4 步。
