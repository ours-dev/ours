# kshell —— 改动记录（2026-09-10）

## 控制台线程被创建了但从来没被启动过（2026-09-10）

**文件**
- `kernel/lib/kshell/kshell/shell.cpp` —— `spawn()` 补上 `detach()` + `resume()`

**症状**
串口上**从来没有出现过 `kshell> ` 提示符**，敲什么都没有反应，而且没有任何报错。

```
$ grep -ac 'kshell' boot.log
0
```

线程在镜像里是有的（`llvm-nm` 能看到 `kshell::(anonymous namespace)::spawn()` 和
`GKTL_INIT_HOOK_KShellInit`），init hook 也确实跑了（同为 `InitLevel::Platform` 的
`ProfInit` 打出了 `prof: no architectural PMU`）。

**原因**
`Thread::spawn()` **只造线程，不启动线程**。造出来的线程是 parked 的，要 `resume()`
才会进调度器。

树里每一个能正常工作的调用者都是成对写的：

```cpp
// kernel/top/main.cpp:84-87
auto t1 = Thread::spawn("Sample1", gibberish, ..., task::DEFAULT_PRIORITY);
t1->detach();
t1->resume();
```

`kernel/kernel/task/dpc.cpp` 同样。只有 kshell 这一处两个都没写：

```cpp
auto *thread = ours::task::Thread::spawn("kshell", ..., DEFAULT_PRIORITY);
if (thread == nullptr) {
    log::warn("kshell: failed to spawn console thread");
}
// ← 就没了
```

于是线程永远停在那儿。**不出声**正是最坏的部分：`spawn()` 返回了非空，
没有任何一条路径会说"这个线程从来没跑过"。

**改法**

```cpp
if (thread == nullptr) {
    log::warn("kshell: failed to spawn console thread");
    return;                      // ← 早返回，下面两行才有意义
}

thread->detach();
thread->resume();
```

`detach()` 也要：没有人 join 这个线程，它也永远不返回，留成 joinable 只是留一具没人收的尸体。

**怎么发现的**
用户想亲手在串口上试一下刚做好的 `//examples/shell`，先替他确认 kshell 能不能派发命令——
结果连提示符都没有。

**参照**
Linux 的 `kthread_create()` + `wake_up_process()` 是同一对；`kthread_run()` 之所以存在
就是因为这两步分开写太容易漏掉后一半。

**验证**
改前：整份启动日志里 `kshell` 出现 0 次。
改后：

```
kshell> [3357613000 cpu0 info] userboot: kernel package holds 6 files
```

提示符出来了（后面跟着的是并发的启动日志，两者共用 COM1）。

**故意没做**
- **没有给 `Thread::spawn()` 加"未 resume 就析构"的断言**。能防住这一类，但 spawn 之后
  隔一段再 resume 是合法用法（`dpc.cpp` 中间还设了亲和性），断言要区分"还没轮到"和
  "永远不会"，不是一行能做对的事。
- **没有让 kshell 和内核日志分开串口**。两者共用 COM1，提示符会被日志冲散。
  分开需要第二个串口或一个真正的控制台对象，那是输入路径那件事的一部分。
