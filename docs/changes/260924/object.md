# object 改动记录（2026-09-24）

## 句柄 arena 的查找不再有分支（Spectre V1）（2026-09-24）

**文件**
- `kernel/object/handle.cpp` — `HandleArena::lookup()` 改为 `confine_array_index` + `conditional_select_nospec_eq`；占用中的槽记录加 `HANDLE_LIVE_MARK`；`reserve()` 不再跳过 generation 0
- `docs/architecture/object/handle-lookup.md` — 常量名改为本项目的写法；补上随机数须预左移；第 3、4 道关按现状重写，改正第 4 道关的理由

**症状**
潜伏。句柄查找在每个带句柄参数的系统调用入口执行，输入是用户给的数字，而查找里有三个
由它决定的条件跳转：generation 是否为 0、下标是否越界、槽里的值是否相等。

**原因**
条件跳转会被预测。比较结果出来之前，CPU 沿预测的一侧执行：预测"相等"时，后面的代码会拿着
用户随便编的数字所指的那个槽——越界时甚至是 arena 之外的内存——当作 `Handle` 用，读它的对象
指针、它的权限，这些访存在缓存里留下的痕迹在回滚之后仍可测得。这就是 Spectre V1
（bounds check bypass）。原来的 TODO 把这个比较的风险说成"程序可以靠计时探测哪些槽被占用"，
理由写错了：占用情况不是值得设防的秘密，要防的是推测路径上对用户所选对象的使用。

**改法**
改前：

```cpp
        auto lookup(u32 const base_value) -> Handle * {
            if ((base_value & HANDLE_GENERATION_MASK) == 0) {
                return nullptr;
            }
            auto const index = base_value & HANDLE_INDEX_MASK;
            if (index >= HANDLE_ARENA_SLOTS) {
                return nullptr;
            }
            // TODO(P1): a constant-time comparison, ...
            if (m_live_base_value[index].load(std::memory_order_acquire) != base_value) {
                return nullptr;
            }
            return std::launder(reinterpret_cast<Handle *>(&m_storage[index]));
        }
```

改后：

```cpp
        auto lookup(u32 const base_value) -> Handle * {
            static_assert(USER_ASPACE_BASE == MB(2), ...);
            auto const index =
                arch::confine_array_index(base_value & HANDLE_INDEX_MASK, HANDLE_ARENA_SLOTS);
            auto const live = m_live_base_value[index].load(std::memory_order_acquire);
            auto const address = arch::conditional_select_nospec_eq(
                live, base_value | HANDLE_LIVE_MARK, reinterpret_cast<usize>(&m_storage[index]), 0);
            auto *const handle = reinterpret_cast<Handle *>(address);
            return handle ? std::launder(handle) : nullptr;
        }
```

三个分支分别这样消掉：

- **越界**：不检查，而是夹住。越界的下标变成 0，接下来的比较自然失败——探针里仍是原下标，
  槽 0 里任何记录都不可能等于它。
- **相等比较**：用条件传送选出"槽地址或 0"，比较未决时结果不可能是槽地址。
- **generation 为 0**：原来要拒绝它，是因为空槽记录 0，而下标 0、generation 0 的 base 值也是 0。
  现在占用中的槽记 `base_value | HANDLE_LIVE_MARK`（第 31 位——base 值只有 30 位，这一位
  永远空闲），探针也带这一位，空槽的 0 永远比不中。于是 generation 0 不必再保留，`reserve()`
  回绕时不再跳过它，与 Zircon 的 `NewHandleValue()` 一致。

失败时返回的 0 在推测路径上被解引用也安全：调用者的 `if (!handle)` 本身又是一个分支，误预测
时可能按小偏移读空指针；`USER_ASPACE_BASE`（2 MiB）以下在任何地址空间都不映射，读不到数据、
也不会填缓存行。`static_assert(USER_ASPACE_BASE == MB(2))` 把这个前提钉在代码里。

`std::launder` 之前的空指针判断是对**选择结果**的判断，推测路径上看到的只能是 0；编译器也
把它折掉了（见验证）。

**参照**
Zircon `Handle::FromU32()`（`kernel/object/handle.cc`）：`arena_.Confine(handle_addr)` 夹住地址，
`fbl::conditional_select_nospec_eq(handle->base_value(), value, handle_addr, 0)` 选出结果。
Zircon 把 base 值存在槽内对象里；这里存在槽外的 `m_live_base_value`，所以用"活标记"区分空槽，
而不是靠 generation 0。

**验证**
- 两个内核（dbg.0、dbg.1）里 `Handle::from_base_value` 的反汇编，从取下标到返回没有任何条件跳转：

  ```
  andl  $0x3ffff, %eax
  cmpq  %rax, %rdx          ; 0x1000 vs index
  cmovaq %rax, %rsi         ; confine
  movl  0x6088(%rax,%rsi,4), %edx   ; live record
  orl   $0x80000000, %edi   ; probe | LIVE_MARK
  ...
  cmpq  %rdx, %rdi
  cmovneq %rcx, %rax        ; slot address or 0
  retq
  ```

- 两个镜像启动正常，shell 的 `close again = BadHandle` 与通道检查照常（关闭后的陈旧值、
  以及正常查找都走的是这条新路径）。

**故意没做**
- 第 5 道关（句柄的表 koid 等于本表 koid）仍是普通比较：到那一步时拿到的已是一个真实存在的
  `Handle`，推测路径上最多看到"别的进程的一个合法句柄"，这与 Zircon 相同。
- arena 惰性提交推迟到 P1-C（2026-09-24 定）。

## 进程的状态与线程表由同一把进程锁保护（2026-09-24）

**文件**
- `kernel/object/process_dispatcher.cpp` — `attach_thread()` 在进程锁下完成状态检查、启动线程、入表、Initial→Running
- `kernel/object/include/ours/object/process_dispatcher.hpp` — 删掉 `m_thread_list_lock`；`m_state` 与 `m_thread_list` 注明由 `get_lock()` 保护

**症状**
潜伏。原来的 `attach_thread()` 先启动线程（`activate(false)`），再拿一把只管线程表的锁把它入表；
进程的 `m_state` 从未被任何锁保护，也从未离开 Initial。

**原因**
接下来的杀进程（第 25 条）要"在锁下遍历线程表，逐个杀掉"。两把锁、两个步骤之间有缝：
- 线程已经在跑、还没入表时，一次杀进程遍历看不到它，它就活了下来；
- 进程已经开始死亡时，仍然可以有新线程被加进来。

只有把"检查状态、启动线程、入表"放进同一把锁里，任何拿这把锁的观察者看到的进程才要么还没有这个
线程、要么这个线程已在运行且在表里。

**改法**
改前：

```cpp
auto ProcessDispatcher::attach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();
    thread.mark_active();

    thread.activate(false);

    Guard guard{m_thread_list_lock};
    m_thread_list.push_back(thread);
    return Status::Ok;
}
```

改后：

```cpp
auto ProcessDispatcher::attach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();

    Guard guard{get_lock()};

    auto const initial_thread = m_thread_list.empty();
    auto const expected = initial_thread ? State::Initial : State::Running;
    if (m_state != expected) {
        return Status::BadState;
    }

    thread.mark_active();
    if (auto const status = thread.activate(false); Status::Ok != status) {
        return status;
    }
    m_thread_list.push_back(thread);

    if (initial_thread) {
        m_state = State::Running;
    }
    return Status::Ok;
}
```

用 dispatcher 自带的 `get_lock()` 而不是另开一把：状态本来就该由对象锁保护，两把锁只会带来锁序问题。
锁序：进程锁在前，线程锁在后（`activate()` 拿线程的锁）。两把都是 `sync::Mutex`，`activate()` 里
`resume()` 会拿 PI 自旋锁，持 Mutex 再拿自旋锁没有问题。

**参照**
Zircon `ProcessDispatcher::AddInitializedThread()`（`kernel/object/process_dispatcher.cc`）：`Guard<CriticalMutex> guard{get_lock()}` 下
判断是否首线程、按状态拒绝、`MakeRunnable`、`thread_list_.push_back`、首线程时 `SetStateLocked(State::RUNNING)`。

**验证**
构建通过；两个镜像启动正常，init 和 shell 各自的首线程经新路径启动（进程 Initial→Running），
坏指针检查与通道检查照常。

**故意没做**
`detach_thread()` 仍是 `Unimplemented`，下一条实现。

## 线程退出时，ThreadDispatcher 离开它的进程（2026-09-24）

**文件**
- `kernel/object/include/ours/object/thread_dispatcher.hpp` — `State` 加 `Dying`、`Dead`；新增 `mark_dead()`；注明 `m_kernel_thread` 由 `get_lock()` 保护、退出后置空
- `kernel/object/thread_dispatcher.cpp` — `on_exit()` 调 `detach_thread()`；`mark_dead()`
- `kernel/object/process_dispatcher.cpp` — `detach_thread()` 从 `Unimplemented` 变为真实实现

**症状**
潜伏，下一条就会触发。一个用户线程退出后，它的 ThreadDispatcher 仍挂在进程的侵入式线程表上，
`m_kernel_thread` 仍指向一个即将被 DPC 销毁的 `task::Thread`：
- 杀进程遍历线程表时会找到一个背后什么都没有的线程，再经悬空指针去"杀"它；
- ThreadDispatcher 最终析构时仍被链在表里，侵入式链表钩子的自检会在析构时报错（在 DPC worker 里）。

**原因**
`on_exit()` 什么也不做，`detach_thread()` 是 `Unimplemented`。对象的生命周期（由句柄和引用决定）
比它所代表的内核线程长，两者之间的指针必须在线程退出那一刻就断开，而不是等对象析构。

**改法**
改前：

```cpp
auto ThreadDispatcher::on_exit() -> Status {
    verify_canary();
    return Status::Ok;
}

auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();
    return Status::Unimplemented;
}
```

改后：

```cpp
auto ThreadDispatcher::on_exit() -> Status {
    verify_canary();
    return m_process->detach_thread(*this);
}

auto ThreadDispatcher::mark_dead() -> void {
    verify_canary();

    Guard guard{get_lock()};
    m_state = State::Dead;
    m_kernel_thread = nullptr;
}

auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();

    Guard guard{get_lock()};
    thread.mark_dead();
    static_cast<void>(m_thread_list.erase(m_thread_list.iterator_to(thread)));
    return Status::Ok;
}
```

置 Dead、清指针、出表在进程锁下一次完成（线程锁在内，与 `attach_thread()` 同序）：杀进程在进程锁下
遍历时，每个线程要么还在表里且活着，要么已经完全不在——不会有"在表里但已没有内核线程"的中间态。
`detach_thread()` 保留了原名（主人同意），没有另起 `remove_thread`。

**参照**
Zircon `ThreadDispatcher::ExitingCurrent()`（`kernel/object/thread_dispatcher.cc`）：在线程锁下
`SetStateLocked(DEAD)` 与 `ResetCoreThread()`，随后 `process_->RemoveThread(this)`（进程锁下出表）。
Zircon 分两段加锁；这里合成一段，更严格。

**验证**
构建通过；两个镜像启动正常。用户线程退出这条路径要到下一条之后（杀进程）才会真正走到，届时由
dispatcher 销毁日志（第 24 条）确认整条析构链。

**故意没做**
`Dying` 状态这一条只声明、不设置——由下一条的 `ThreadDispatcher::kill` 设置。

## ThreadDispatcher 与 ProcessDispatcher 的销毁计数并打日志（2026-09-24）

**文件**
- `kernel/object/process_dispatcher.cpp`、`kernel/object/include/ours/object/process_dispatcher.hpp` — 新增 `~ProcessDispatcher()`、`name()`
- `kernel/object/thread_dispatcher.cpp`、`kernel/object/include/ours/object/thread_dispatcher.hpp` — `~ThreadDispatcher()` 从 `= default` 改为有定义

**症状**
没有运行时症状。`DISPATCHER_THREAD_DESTROY_COUNT`、`DISPATCHER_PROCESS_DESTROY_COUNT` 两个计数器早已声明，
从来没人加过；进程和线程对象死掉时什么痕迹也不留。

**原因**
接下来的杀进程要能验证"整条析构链都放手了"：线程 → ThreadDispatcher → ProcessDispatcher → 句柄表。
这条链上任何一处多持了一个引用，进程就不会被析构，而今天没有父进程会等它（`process_exit`、get_info 在 P1-B），
所以没有别的东西能发现。销毁那一刻的一行日志，就是测试（和读日志的人）的观察点。

**改法**
改前：

```cpp
    ~ThreadDispatcher() = default;
```

`ProcessDispatcher` 没有显式析构函数。

改后：

```cpp
ThreadDispatcher::~ThreadDispatcher() {
    DISPATCHER_THREAD_DESTROY_COUNT.add(1);
    log::info("object: thread (koid {}) of process '{}' destroyed", koid(), m_process->name());
}

ProcessDispatcher::~ProcessDispatcher() {
    DISPATCHER_PROCESS_DESTROY_COUNT.add(1);
    log::info("object: process '{}' (koid {}) destroyed", name(), koid());
}
```

ThreadDispatcher 自己不存名字（名字给了它的内核线程，而那时内核线程早已销毁），所以按 koid 和所属进程名来称呼。
析构发生的位置：最后一个引用放手处，可能是 DPC worker（线程销毁时放掉它对 ThreadDispatcher 的引用），不会在持锁路径上。

**参照**
Zircon 的 `ThreadDispatcher::~ThreadDispatcher()` 与 `ProcessDispatcher::~ProcessDispatcher()` 同样做
`kcounter_add(dispatcher_*_destroy_count, 1)`。

**验证**
构建通过；两个镜像启动正常，日志里没有 `destroyed`——今天还没有进程或线程对象会死。第 26 条之后由杀进程触发。

**故意没做**
没有给 ThreadDispatcher 加名字成员：只为一行日志不值得。

## `ProcessDispatcher::kill` 与 `TASK_RETCODE_*`（2026-09-24）

**文件**
- `system/public/ours/types.hpp` — `TASK_RETCODE_*` 六个常量（数值同 Zircon）
- `kernel/object/process_dispatcher.cpp`、`.hpp` — `kill(i64)`、`m_retcode`
- `kernel/object/thread_dispatcher.cpp`、`.hpp` — `kill()`

**症状**
没有运行时症状。对象层还没有"结束一个进程"的操作，下一条（用户态异常杀进程）需要它。

**原因**
杀进程的语义要落在对象层：它要在进程锁下改状态、遍历线程表、给每个线程发 Kill，而这些都是
对象层的状态。内核线程（`task::Thread`）只负责接收信号、在回 ring 3 的路上自行退出（第 20、21 条）。

**改法**
新增，没有改前。

```cpp
auto ProcessDispatcher::kill(i64 const retcode) -> void {
    verify_canary();

    Guard guard{get_lock()};
    if (m_state == State::Dead || m_state == State::Dying) {
        return;
    }
    m_retcode = retcode;

    if (m_thread_list.empty()) {
        m_state = State::Dead;
        return;
    }

    m_state = State::Dying;
    for (auto &thread: m_thread_list) {
        thread.kill();
    }
}

auto ThreadDispatcher::kill() -> void {
    verify_canary();

    Guard guard{get_lock()};
    switch (m_state) {
        case State::Running:
            if (!(m_flags & Flags::Active)) {
                break;
            }
            static_cast<void>(m_kernel_thread->kill());
            m_state = State::Dying;
            break;
        case State::Dying:
        case State::Dead:
            break;
    }
}
```

- 只发信号，不 exit：线程在自己回 ring 3 的路上处理 Kill（异常尾部、SYSRET 之前）。所以 `kill()` 可以
  持着会睡眠的进程锁运行，也可以由一个线程对自己的进程调用。
- 只有第一次 kill 的 retcode 算数：已在死亡中的进程保留它死亡的原因（Zircon 同）。
- 没有线程的进程直接进 Dead；Zircon 随后的 `FinishDeadTransition()`（TERMINATED 信号、拆地址空间）在 P1-B。
- 从没启动过的线程（`Active` 未置）不动，与 Zircon 对 INITIAL/INITIALIZED 线程的处理一致。
- 锁序与 `attach_thread()`、`detach_thread()` 相同：进程锁在前，线程锁在后。

`TASK_RETCODE_*` 取 Zircon 的数值（-1024 … -1029）：它们是 ABI，没有理由不同。

**参照**
Zircon `ProcessDispatcher::Kill()`、`KillAllThreadsLocked()`（`kernel/object/process_dispatcher.cc`）、
`ThreadDispatcher::Kill()`（`kernel/object/thread_dispatcher.cc`）、`ZX_TASK_RETCODE_*`（`zircon/syscalls/object.h`）。

**验证**
构建通过；两个镜像启动正常。`kill` 还没有调用者，下一条接上。

**故意没做**
- 进程在最后一个线程退出时转 Dead、`finish_dead_transition`、`VmAspace::destroy`、TERMINATED 信号：P1-B。
- 对另一颗 CPU 上正在运行的线程不发 IPI（`suspend_or_kill` 的 Running 分支只置信号）：P1-A 里只有进程杀自己，
  不受影响；多线程进程要等目标线程下一次中断才会死。

## 线程第一次进 ring 3 之前也检查挂起信号（2026-09-24）

**文件**
- `kernel/object/thread_dispatcher.cpp` — `ThreadDispatcher::trampoline()` 在 `arch_enter_uspace()` 之前调 `process_pending_signals()`

**症状**
审查发现（潜伏，P1-A 里够不到）：一个线程被启动（`attach_thread` 已让它就绪）、还没真正运行时，它的进程被杀了。
这个线程照样从入口点开始执行用户程序，直到它的第一次中断或系统调用才死——在一个已经在死亡中的进程里跑了用户代码。

**原因**
回 ring 3 有三条路：异常尾部、SYSRET 之前、以及线程**第一次**进用户态。前两条都检查挂起信号（第 19、21 条），
第三条没有。

**改法**
改前：

```cpp
    arch::local_irq_disable();
    arch_enter_uspace(&frame);
```

改后：

```cpp
    arch::local_irq_disable();
    task::Thread::Current::process_pending_signals();
    arch_enter_uspace(&frame);
```

在关中断之后检查，与另外两条路一致：之后再到来的信号不会落在"检查之后、进入之前"的空隙里。Kill 就在这里退出线程。

**参照**
Zircon `ThreadDispatcher::StartRoutine()`：`arch_disable_ints(); arch_iframe_process_pending_signals(&iframe); arch_enter_uspace(&iframe);`。

**验证**
- 构建通过；两个镜像启动正常。
- 临时在 trampoline 里对名为 shell 的进程调 `kill()`，放在这次检查之前（未提交）：dbg_1 日志只有
  `userabi: 'shell' starts ...`，接着两行 destroyed，shell 的横幅一次也没打出——它没有执行任何用户指令；没有断言。

**故意没做**
无。
