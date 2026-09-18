# kernel/task/sched 待修项（2026-09-17）

## 1. 调度周期是 64ms，因为粒度常量取错了值

`kernel/include/ours/task/sched/common.hpp`

```cpp
constexpr auto MIN_SCHED_GRANULARITY = Milliseconds(1);   // 定义了，没人用
constexpr auto TARGETED_LATENCY = Milliseconds(8);
constexpr SchedDuration SCHED_GRANULARITY = to_sched_duration(TARGETED_LATENCY);  // ← 取了 8ms
constexpr usize SCHEDULING_PERIOD_GRANS = TARGETED_LATENCY / MIN_SCHED_GRANULARITY;  // 8
```

于是 `scheduling_period()` = 8ms × 8 = **64ms**，而它本该是 1ms × 8 = 8ms。
这个值是 `RoundRobin::on_thread_init()` 给每个线程的时间片基线
（`timeslice.m_base`），所以**每个线程的时间片都大了 8 倍**。

原来的字段叫 `minimal_granularity`，注释写着「a thread 在单轮里拿到的最小时间片」，
却用 `DEFAULT_MIN_TARGETED_LATENCY` 初始化，而没人用的 1ms 常量就在它上面两行。
常量上移那一轮刻意原样保留了这个值，因为改它是行为变更。

**为什么要单独一个提交**：修掉它会把上下文切换频率提高 8 倍。那是可观测的性能与
时延变化，需要自己的改动记录和自己的实测数字（切换计数、tick 间隔），不能混在
重构里，否则以后没人说得清是哪一下改变了调度行为。

**建议顺序**：排在 Runtime 收敛的注解工作之后。切换频率提高 8 倍会放大任何已存在的
上下文切换竞态，先把那一块钉稳再动它。

## 2. `Fair::unpark_preempt()` 的两个 Thread* 参数写反了

`kernel/kernel/task/sched/fair.cpp:439`

接口是 `unpark_preempt(Runtime &, Thread *curr, Thread *thread, UnparkControl)`，
Fair 的实现把 `curr` 和 `thread` 当成了对方。两个形参同类型，所以这是合法的 override，
`-Werror` 全程沉默。

`fair.cpp` 目前不在构建里，所以现在触发不到；Fair 一旦放回构建就是活的缺陷。

**建议修法**：改参数类型让编译器能分辨，而不是只改实现——
`struct UnparkPair { Thread *curr; Thread *waking; };` 具名字段，零运行时成本，
且对所有纪律一次性生效。单独提交。

## 3. `SchedParams::deadline` 无人写入，却是三个决策的输入

`kernel/include/ours/task/sched/context.hpp:228`

```
写入者：  全树 0 处
读者：    fair.hpp:113        x.deadline() < y.deadline()        ← Fair 运行队列的排序比较器
          fair.cpp:212        ctx.deadline() <= ctx.runtime()    ← 抢占判据
          wait_queue.cpp:143  waiter.m_queue_key.vdeadline = ...deadline()  ← 等待队列排序键
```

镜像对称：`SchedParams::vdeadline` 被 Fair 写四次（fair.cpp:190/216/373/419），**零读者**。

两个字段同型（都是 `ffl::Fixed<i64, 0>`——`SchedTime` 和 `SchedDuration` 是同一个类型，
见 `types.hpp` 里那段「时刻与时长现在是同一个类型」的交代），所以读错哪一个编译器都不吭声。

**今天的后果**：`deadline` 恒为 0，`WaitQueue::QueueKey::operator<` 的第一个比较永远相等，
等待队列退化成按 `seq` 的纯 FIFO。而 `wait_queue.hpp:44-62` 的注释引 EEVDF 论文、声称按
虚拟截止期排序——**注释描述的策略没有生效**。Fair 那两个读点目前不在构建里。

值得注意的是：即使把读者改成读 `vdeadline`，今天也一样恒为 0——RR 从不写它。所以在 RR
当班期间 FIFO 反而是对的；真正的风险是 **Fair 回到构建之后，键仍然是 0，排序继续静默
地是 FIFO 而没人察觉**。

**为什么不修**：`deadline` 与 `vdeadline` 分别该由谁写、等待队列到底该按哪一个排序，
是 Fair/EEVDF 的设计决定，作者明确保留（「别动 Fair」）。此处只记录事实与触发条件。

**修的时候要注意**：`wait_queue.cpp:143` 是在 `WaitQueue::m_lock` 加线程 PI 锁下读的，
不是 rqlock——任何声称该字段归运行队列锁所有的新命名都是假的。

## 4. `Profile::recompute_effective_profile()` 声明而无定义

`kernel/include/ours/task/sched/profiles.hpp:71`，调用点 `scheduler_pi.cpp:45` 与 `:66`。

全树没有定义，链接后的 `vmours` 里也没有这个符号。

**为什么现在链接得过**：两个调用点都在 `Scheduler::Pi::common()` 体内
（scheduler_pi.cpp:22-70），而它是一个**函数模板，从未被实例化**——文件底部
（:87-95）显式实例化的是 `on_upstream_thread_base_profile_overrided`、
`join_node_to_pi_graph`、`split_node_from_pi_graph`，这三个的函数体都是空的
（:79/:82/:85）。模板体没被生成，所以 `recompute_effective_profile` 从未被 odr-use。

（不是 `--gc-sections` 丢掉了死调用者——那个解释是错的。）

**触发条件很精确**：只要有人往那三个空函数体里写下第一行 `Pi::common(...)`，
下一次链接就是未定义引用。

**为什么不修**：`fair.cpp:449-453` 有作者的明文交代——不但知道它没有定义，还写明
「Deliberately not calling ... would trade a working build for a link error」，
并指定由 `notify_base_profile_overrided()` 承接这件事。补定义等于替 PI 的效果 profile
合并规则做决定，是设计，不是修补。

## 5. `Timeslice::evaluate()` 定义在错误的翻译单元

`Timeslice` 声明在 `context.hpp:250`，`evaluate()` 却定义在
`kernel/kernel/task/sched/scheduler.cpp:1042`，函数体是 `return false;`。

纯位置问题，无行为影响。顺带：`Timeslice::m_extension` 全树零写入，所以
`has_timelice_extension()` 和 `is_custom_timeslice()` 永远返回 false，整套 extension
机制目前是惰性的（按存根规矩保留）。同处还有一个拼写失配：`has_timelice_extension`
少了一个 `s`。

## 6. 内核开不了 lockdep：两个运行期钩子从未实现

`lockdep_enable = true`（`kernel/kernel_params.gni:8` 的默认值是 `false`）配出来的树链接失败：

```
ld.lld: error: undefined hidden symbol: lockdep::current_lock_context()
>>> referenced by guard.hpp:157 ... 71 more times
ld.lld: error: undefined hidden symbol: lockdep::report_violation(lockdep::ViolationReport const&)
>>> referenced by lock_context.hpp:128
```

两者都是 lockdep 明文交给使用方实现的——`report.hpp:47` 的注释写着
「Supplied by the environment, like `current_lock_context()`」。全树只有
`system/lib/lockdep/tests/threaded.cpp` 实现了它们，那是 lockdep 自己的主机测试。
`system/lib/lockdep` 目录下没有 `src/`。

**后果**：这棵树的锁定序**从未被任何东西校验过**，而且现在也验不了。所有关于
「某某改动让 lockdep 看见了这把锁」的说法，在实现这两个钩子之前都只是潜在收益，
不是已生效的检查。clang 的编译期 thread-safety 分析是另一回事，它一直开着，
和 lockdep 无关。

**要修的话**：`current_lock_context()` 应当返回当前线程的锁上下文——`Thread` 已经为此
留了位置（见 `lockdep::config.hpp` 里那段「a build that asked for no lockdep still gave
every Thread a lock context」的交代）；`report_violation()` 走内核的 panic/日志。
这是一个独立的功能，不是修补。
