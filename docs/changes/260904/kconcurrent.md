# kernel/lib/kconcurrent — 改动记录

格式见 `docs/changes/TEMPLATE.md`。框架那一半在 `//system/lib/concurrent`，
它的改动记录另计。

---

## 1. 加上 `ChainLockTransaction`，链锁协议的内核特化（2026-09-04）

**文件**
- `kernel/lib/kconcurrent/include/kconcurrent/chain_lock_transaction.hpp` — 新增。
- `kernel/lib/kconcurrent/src/chain_lock_transaction.cpp` — 新增，per-CPU 当前事务槽。
- `kernel/lib/kconcurrent/BUILD.gn` — 新增。
- `kernel/lib/BUILD.gn`、`kernel/kernel/task/BUILD.gn` — 接进构建图。

**症状**

无，这是新增。`concurrent::ChainLock` 单独存在时协议是不完整的：它能比较 token、
能给出三种结果，但答不出「调用者现在用的是哪个 token」和「调用者此刻有没有资格拿这把锁」。

**原因**

这两个问题只能由内核回答，因为答案要用到 per-CPU 槽、中断状态和自旋提示——
而框架那一半是**故意**不碰内核头的，它要能在宿主上编译和跑单测。所以按
`system/lib` 是框架、`kernel/lib/k*` 是特化的分工，这些东西归这里。
Zircon 的 `system/ulib/concurrent` 与 `kernel/lib/kconcurrent` 是同一条线。

三个设计点值得写下来，因为都容易被「优化」掉：

1. **一个事务覆盖的是一整轮「走图 + 退避重试」，不是一把锁、也不是一次成功的遍历。**
   token 在构造函数里取一次，退避是在事务**生命期之内**放锁，所以重试是拿着同一个
   token 重走。操作只会相对整个系统越变越老，直到成为最老的活claim，再没人能挤掉它。
   每次尝试都新建一个事务会每次重新取号，协议就从「无饿死」退化成活锁。
2. **中断在事务的整个生命期里关着，不是每把锁关一次。** 要紧的窗口不是「正在取锁时」
   而是「手上还有锁时」，而事务按定义就是跨多次取锁地持着锁。
3. **当前事务放在 per-CPU 槽而不是每线程**，这只有在第 2 点成立时才是对的：中断关着
   就不会被抢占，运行的线程在事务期间不会变，于是「这个 CPU 在跑的事务」和
   「这个线程在跑的事务」是同一个东西。而从一个没被传入事务的深层断言里，
   per-CPU 槽是两者中更好拿到的那个。

**改法**

`ChainLockTransaction` 是 RAII：构造时取 token、关中断、登记为本 CPU 当前事务；
析构时断言锁已全放、注销、恢复中断。成员声明顺序就是协议要求的顺序——
`arch::AutoInterruptDisabler` 声明在最前，所以它最先构造、最后析构，
中断在事务对外可见之前就已经关掉，在事务消失之后才恢复。

它**持有**一个中断禁用器，不是**继承**自它：事务是「有一份要恢复的中断状态」，
不是「一种中断禁用器」。

`acquire_first()` / `acquire()` / `release()` 三个包装除了记账，还把框架那一半的
capability 注解按调用者的 `lock` 参数重新声明了一遍。不写的话
`-Wthread-safety` 会认为 `acquire_first_in_chain()` 拿走的能力再没还回来、
`release()` 放掉了一把从没拿过的锁——这三条在本次构建里是 `-Werror`。

`begin_mutation()` 标出「走完了、开始写」的那一点，把协议里框架看不见的那半条规则
变成断言：只持有部分锁时可以读、不可以写，因为退避是靠放锁来回退的，它没法撤销写。

**参照**

Zircon `ChainLockTransaction`（`zircon/kernel/lib/kconcurrent/`），
当前事务同样放在 percpu 结构里。

**验证**

- 全量 `ninja -C out/bringup.x86` 通过，`kconcurrent.chain_lock_transaction.cpp.o`
  在 `kernel_x86.dbg.0` 和 `dbg.1` 两个工具链下都产出了。
- QEMU debug 镜像启动，**全程零断言**，跑到线程反复打印 `I'm Thread 1`。
- 注意 `//kernel/lib:standalone-libs` 那个聚合 group 谁都没引用，往里加名字不会让它进构建图；
  真正把这个库拉进来的是 `//kernel/kernel/task` 的 deps。gktl 也是这么进来的。

**故意没做**

- **还没有任何调用者。** `Thread::m_pi_lock` 和 `WaitQueue::m_lock` 还是 `sync::SpinLock`。
  换锁是下一条，单独做。
- 没有做嵌套事务。构造函数直接断言本 CPU 没有活着的事务：嵌套意味着一个 CPU 上有两个
  活 token，内层那个必然更年轻，于是内层可能被要求退避、放掉外层正依赖的锁。
  真需要嵌套要专门设计，不能让它意外发生。保存/恢复 `m_previous` 还是写了——
  只值一个字，而它决定了将来万一嵌套是「行为不对」还是「槽里留了个野指针」。
- 没有做把整条 PI 链一次走完的辅助函数。那要先定下链的形状，属于 kernel/task。
