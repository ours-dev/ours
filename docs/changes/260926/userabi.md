# userabi 改动记录（2026-09-26）

## 第一个进程收到 processargs：根 job 与 BootFS；init、shell 迁到 runtime（2026-09-26）

**文件**
- `kernel/object/message_packet.cpp`、`include/ours/object/message_packet.hpp` — 新增从内核字节建消息的 `create(std::span<u8 const>, ...)`；两个 `create` 的公共部分抽成私有的 `assemble()`
- `kernel/lib/userabi/src/userboot.cpp` — 新增 `make_bootstrap()`；`userboot_init()` 用它代替"把根 job 放进 rdi"；改掉文件头早已过时的"只做了第一步"
- `kernel/lib/userabi/include/userabi/userboot.hpp` — `userboot_init()` 的说明
- `system/service/init/init.cpp`、`BUILD.gn` — `_start` 换成 `main`；读 BootFS 头部；`start_shell()` 给 shell 写 processargs
- `examples/shell/shell.cpp`、`BUILD.gn` — `_start` 换成 `main`；按手里的启动句柄决定做什么；`answer_bootstrap()` 换成 `answer_init()`；新增 `describe_bootfs()`
- `system/lib/runtime/BUILD.gn`、`test/runtime_smoke.cpp` — 注释：除 userboot 外都已迁移

**症状**
无故障，是设计步骤 K14 的第 ③ 个提交（本体）。今天第一个进程的 `rdi` 就是根 job 句柄；K12 建好的 BootFS VMO 一直留在内核里、交不出去；
init 和 shell 靠手写的 `_start` 猜 `rdi` 是什么（shell 甚至用"对它做 channel_read 回不回 BadType"来判断自己是被谁启动的）。

**原因**
这一节的学习价值在"为什么内核、init、shell 必须在同一个提交里切换"和"内核怎么给自己写一条消息"。

- **三方必须一起切**：启动协议的写方和读方要同时换。内核只有一种写法，它同时决定 dev 的第一个程序（init）和 dbg_1 的第一个程序（shell）
  怎么启动；shell 换到 runtime 之后，又要求它的另一个启动方 init 给它写 processargs。三条边连成一片，拆开的中间状态跑不起来：runtime
  程序若拿到的是一个 job 而不是 channel，会在"等 READABLE 或 PEER_CLOSED"里永远睡下去（job 两个信号都不会出现）。
- **内核要给自己写消息**：`MessagePacket::create` 只收用户指针，而用户指针会先检查"是否在用户半区"，内核栈上的缓冲区过不了。
  所以加一个收 `std::span<u8 const>` 的重载，对应 Zircon 收 `const char *data` 的那个 `MessagePacket::Create`。
- **句柄记在谁名下**：这些句柄都在启动内核线程上造出来，这个线程不属于任何进程，`Handle::make()` 找不到可记账的进程，所以都不占任何
  进程的配额——和以前内核直接铸根 job 句柄时一样。

**改法**
改前（`userboot_init()`）：

```cpp
    auto bootstrap =
        object::Handle::make(ustl::Arc<object::Dispatcher>(object::JobDispatcher::root_job()),
                             object::JobDispatcher::default_right());
    ...
    log::info("userabi: handing {} the root job", program);
    auto const status = launch(object::JobDispatcher::root_job(), program, ustl::move(bootstrap));
```

改后：

```cpp
    auto const has_bootfs = static_cast<bool>(resident_bootfs.dispatcher());
    auto bootstrap = make_bootstrap(program);
    ...
    log::info("userabi: handing {} its processargs: the root job{}", program,
              has_bootfs ? " and the BootFS" : "");
    auto const status = launch(object::JobDispatcher::root_job(), program, ustl::move(bootstrap));
```

`make_bootstrap()`（节选）：

```cpp
        handles[count] = object::Handle::make(
            ustl::Arc<object::Dispatcher>(object::JobDispatcher::root_job()),
            object::JobDispatcher::default_right());
        infos[count++] = handle_info(HandleType::JobDefault, 0);
        if (resident_bootfs.dispatcher()) {
            handles[count] = object::Handle::make(
                object::KernelHandle<object::Dispatcher>(ustl::move(resident_bootfs)),
                resident_bootfs_rights);
            infos[count++] = handle_info(HandleType::VmoBootfs, 0);
        }
        std::string_view const args[] = {program};
        encode_process_args(message, {infos, count}, args, {}, {}, &length);
        object::MessagePacket::create({message, length}, handles, count, &packet);
        kernel_end.dispatcher()->write(ustl::move(packet));
        return object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(user_end)),
                                    channel_rights);
```

- **只放存在的句柄**：BootFS 可能没建成（make_user_bootfs 失败不致命），这时消息里只有一个句柄、一个 handle_info，两边位置一一对应。
  `MessagePacket` 相信内核不会给它空句柄——没有 DEBUG_ASSERT 的构建里没人检查——所以空的根本不放进去。
- **BootFS 是移交，不是复制**：`resident_bootfs` 移进消息后就空了，第一个进程决定还给谁。照 Zircon userboot 把 VMO 句柄 move 出去的做法。
- **内核这一端写完就丢**：作为 KernelHandle 丢掉，它的析构会告诉 channel"最后一个持有者没了"。消息留在队列里照样可读，进程同时看到
  PEER_CLOSED，runtime 正是等"READABLE 或 PEER_CLOSED"再读，所以没问题。Zircon 也丢掉它那一端，但先用 `release()` 从 KernelHandle 里
  取出裸引用（`BootstrapChannel::Create`），跳过了那个通知，所以它的 userboot 在 bootstrap 上永远看不到 PEER_CLOSED。（第一版这里写成
  "Zircon 留着那一端"，审查指出不对，已改。）
- **init**：取 JobDefault（没有就停下）与 VmoBootfs，用 `vmo_read` 读出 BootFS 头部，打印 `init: bootfs has 4 entries`——
  这是用户态第一次读到内核在 K12 为它建的 BootFS，也是第一次对真正的 VMO 做 `vmo_read`。然后 `start_shell()`：先编码 processargs
  （argv {"shell"}，一个启动句柄：ping/pong 用的 channel 远端，作为 User0），先写后 spawn。不把根 job 交给 shell：回答 ping 用不着它，
  dev 镜像里也没有 fault_probe 可以起（最小权限，设计 3.6）。BootFS 也不给：init 自己还要用，转交得等 `handle_duplicate`（K15）。
- **shell**：按手里的启动句柄决定做什么——有 User0 就回答 init，有 JobDefault 就跑探针，有 VmoBootfs 就读头部。dev 与 dbg_1 是同一个程序，
  走不同的分支。`check_handle_close()` 关的是 init 的 channel（有的话）——关掉它就是 init 那行 `init: shell closed its channel` 的由来，
  这一点与以前相同——否则关探针跑完后的根 job。日志措辞改为 `shell: init says "ping"`、`shell: close(startup handle) = Ok`，因为那已经不是 bootstrap 了。

**参照**
- 内核一侧：Zircon `zircon/kernel/lib/userabi/userboot.cc`（先把所有启动消息写进 bootstrap channel，最后才 Start；VMO 句柄 move 出去）。
  注意 Zircon 现在给 userboot 发的是两条"只有句柄"的消息，不再是 processargs；这里按设计 3.3 仍用 processargs，全系统一种格式、一个解析器。
- `MessagePacket::create(std::span<u8 const>, ...)`：Zircon `MessagePacket::Create(const char *data, ...)`（`object/message_packet.h`）。

**验证**
两个镜像构建、启动：
- dev：`userabi: handing init its processargs: the root job and the BootFS`、`init: started with the root job`、`init: bootfs has 4 entries`
  （与内核那行 `bootfs vmo 40960 bytes, 4 files` 一致）、`init: spawned shell`、`shell: init says "ping"`、`shell: started without a BootFS`、
  `init: shell says "pong"`、`shell: close(startup handle) = Ok`、`close again = BadHandle`、`init: shell closed its channel`，六组检查全部 passed，
  `512 channels made before the quota`，`--- end ---`；没有 `runtime:` 行（没有程序在 main 之前启动失败）。
- dbg_1：`userabi: handing shell its processargs: the root job and the BootFS`、`shell: started by the kernel; no one to answer`、
  `shell: bootfs has 4 entries`；探针与上一个提交相同（13 行 `fault_probe:`、11 个 `retcode -1028`、对照探针 `retcode 2`）；
  `512 channels`；shell 以 `retcode 0` 结束（main 返回 0，由 runtime 调 process_exit），`dead, 1 handles closed`——关掉的是它没有交出去的 BootFS；
  没有 `runtime:` 行，没有 `retcode -2147483649`。

**故意没做**
- dbg_1 的 shell 列出 BootFS 条目、补测 `vmo_read` 的跨页/越界/坏缓冲区：K14 的第 ④ 个提交。
- 默认的 userboot 程序仍是手写 `_start`、忽略 `rdi`：两个 bringup 镜像都不启动它；它在 U8 变成 launcher 时迁移。
- init 仍然停在 `pause` 循环里而不退出：它是其他程序的启动方，还没有什么会注意到它不在了。

审查：三个方向（内核一侧、init 与 shell 的每条路径、注释准确性）并行审查、逐条反驳核实。代码没有发现问题；成立的五条都是注释，已在本提交改正：
runtime 里"init 先 spawn 后写"的说法因本提交而过时（现在三个启动方都先写后启动）；shell 的 `check_handle_close` 把"句柄怎么进到本进程的表里"说错了
（是 runtime 读消息时装进去的，值由本进程的表决定）；以及上面那条关于 Zircon 丢弃 bootstrap 端的说法。
