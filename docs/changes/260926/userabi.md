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

## dbg_1 的 shell 用 vmo_read 列出 BootFS，并补测 vmo_read 的跨页、边界与坏缓冲区（2026-09-26）

**文件**
- `examples/shell/shell.cpp` — 新增 `check_bootfs()`；两个坏地址常量 `UNMAPPED`、`KERNEL` 从 `check_bad_pointers()`、`check_vmo_read()` 里提到文件作用域共用

**症状**
无，补测试。设计步骤 K14 的第 ④ 个提交。两件事要等用户程序真正拿到 BootFS 才能从用户态这一侧检查：
K12 的验收（交给用户态的 BootFS 里没有内核文件），和 K13 当时没有 VMO 可读、只测到了"句柄被拒绝"的那些 `vmo_read` 路径。

**原因**
这一节的学习价值在"测试要真能抓到错"。第一版的跨页检查被审查指出两个"永远会通过"的漏洞，都记在改法里。

**改法**
改前：dbg_1 的 shell 只打一行 `shell: bootfs has 4 entries`。

改后，`check_bootfs()` 依次做：

```cpp
    ou::vmo_read(bootfs, &header, 0, sizeof(header));
    ou::vmo_read(bootfs, entries, sizeof(header), entries_size);
    ou::vmo_read(bootfs, names, sizeof(header) + entries_size, header.name_table_size);
    // print every entry with its size; none may be image.elf or code-patches, in any directory
    ...
    auto const boundary = spanning->data_offset + page;   // inside one file, past its first page
    ou::vmo_read(bootfs, across, boundary - 8, sizeof(across));   // two steps in the kernel
    ou::vmo_read(bootfs, before, boundary - 8, sizeof(before));
    ou::vmo_read(bootfs, after, boundary, sizeof(after));
    ...
    ou::vmo_read(bootfs, &byte, size, 0);       // Ok
    ou::vmo_read(bootfs, &byte, size, 1);       // OutOfRange
    ou::vmo_read(bootfs, &byte, 0, size + 1);   // OutOfRange
    // into the kernel half, into unmapped, into the vDSO: InvArgs
```

- **内核文件检查**：每个条目连同大小打出来，并拒绝基名为 `image.elf` 或 `code-patches` 的条目，不管它在哪个目录——这是内核包里内核映像和补丁表的固定名字（`KernelPackage`），与内核里黑名单按文件名的那一半相同。
- **跨页读，第一版的两个漏洞**：
  - 第一版把页边界放在第一个文件的开头，用 ELF 魔数证明"读到了东西"。审查指出这个镜像里四个文件都是 ELF，开头 24 字节完全相同：
    内核第二步若读成了别的文件的首页，比较照样相等。改为把边界放在**一个文件内部**——找第一个至少跨两页的文件（shell），取它第二页的开头，
    两边都是这个文件自己的字节。
  - 边界前 8 字节原本是名字表之后补齐的零，而缓冲区也初始化为零：内核若"返回 Ok 却没写这半边"，也比较相等。改为三个缓冲区各自先填上
    不同的哨兵值（0xa5、0x5a、0x3c），没被写到的那一半不可能与另一个缓冲区相等。
  - 页大小取条目里记录的数据对齐（内核按页对齐文件数据并把它写进每个条目），用户态本身拿不到 `PAGE_SIZE`。
- **边界与坏缓冲区**与 `read_user()` 的文档一一对应：`offset = size` 读 0 字节在范围内、读 1 字节不在；比整个 VMO 多一字节不在；
  目标在内核半区（读第一页之前就被拒）、未映射、只读的 vDSO（拷贝时才发现）都回 InvArgs。

**参照**
Zircon `zircon/system/utest/core/vmo/vmo.cc` 对 `zx_vmo_read` 的同类用例（越过末尾、末尾处长度 0、坏缓冲区）。坏缓冲区的错误码不同：
这里一律 InvArgs（本内核 user_copy 的统一规则），Zircon 是 NotFound 与 AccessDenied（见 `docs/changes/260925/syscall.md` 的 vmo_read 一节）。

**验证**
- dbg_1：shell 读出 4 个条目，名字与大小和同一次启动里内核打出的包清单逐一相同——

  ```
  shell: bootfs entry userboot (1736 bytes)
  shell: bootfs entry vdso (1536 bytes)
  shell: bootfs entry shell (24672 bytes)
  shell: bootfs entry fault_probe (7872 bytes)
  ```

  而内核包里有 8 个文件（另有 `config/user_bootfs`、`vmours/code-patches`、`vmours/image.elf`、`physboot`）：内核自己的文件没有交给用户态，
  这是 K12 第一次从用户态这一侧得到确认。随后各项 `vmo_read` 都得到预期答案，`shell: bootfs checks passed`。
- dev：`shell: bootfs checks skipped (no BootFS here)`（init 启动的 shell 不持有 BootFS），其余与上一个提交相同。
- **变异检查**：临时把内核 `read_user()` 的第二步改为读下一页（`at = offset + done + PAGE_SIZE`），dbg_1 打出 `shell: bootfs checks FAILED`；
  还原后重新构建，两个镜像的哈希与通过的那次相同。
- 审查：成立两条，都是跨页检查的检出力不够（见上），已在本提交改正。

**故意没做**
- 读出整个文件的内容并逐字节核对：没有独立的参照来源；在 K20 的 Slice 与 K21 的映射落地后，可以把同一个文件经映射读出来与 `vmo_read` 对比。

## 内核把 cmdline 中的 `userboot.*` 选项作为 environ 交给第一个进程（2026-09-26）

**文件**
- `kernel/lib/obi/include/obi/command_line_view.hpp` — 新增 `for_each_token()`，按顺序遍历 cmdline 的每个 token
- `kernel/lib/userabi/src/userboot.cpp` — 新增 `collect_userboot_options()`；`make_bootstrap()` 把挑出的 token 写进 processargs 的 environ，消息缓冲按上限算大小

**症状**
第一个进程拿不到 cmdline 里的任何选项：processargs 的 environ 是空的。这使 Zircon 式的 `userboot.next` 做不出来——
`userboot.next=<name>` 由 **userboot 程序**读取，决定它下一个启动谁，而 cmdline 只在内核手里。
于是只能由内核直接用 `kernel.select.userboot=init` 启动 init，userboot 程序本身从未运行，init 顶替了它。

**原因**
这不是某一行写错了，而是缺了一条通道：内核→用户之间只有 processargs 这一条消息，它只带根 job、BootFS 和程序名。
选项属于谁，就应该由谁来解析。`kernel.*` 选项控制的是 ring 0，由 `boot::BootOptions` 解析；
`userboot.*` 选项控制的是 userboot 程序，内核既不知道有哪些 key，也不知道 value 的含义。
所以内核能做的只有**原样转发**。在此之前没有转发的地方，这类选项就无处可放。

**改法**
改前（`make_bootstrap()`）：
```cpp
// The header, two handle_infos and a program name of at most 63
// bytes (boot::BootOptions keeps it in 64) come to 108 bytes.
u8 message[128];
u32 length = 0;
std::string_view const args[] = {program};
if (auto const status = encode_process_args(message, {infos, count}, args, {}, {}, &length);
```
改后：
```cpp
constexpr usize MESSAGE_BYTES =
    sizeof(ProcArgs) + 2 * sizeof(u32) + 64 + USERBOOT_OPTIONS_BYTES;
u8 message[MESSAGE_BYTES];
u32 length = 0;
std::string_view const args[] = {program};
auto const environ = collect_userboot_options();
...
if (auto const status = encode_process_args(message, {infos, count}, args,
                                            {environ.tokens, environ.count}, {}, &length);
```
`collect_userboot_options()` 用新加的 `CommandLineView::for_each_token()` 遍历镜像 cmdline，
只收以 `userboot.` 开头的 token，最多 8 个、合计（含 NUL）不超过 384 字节。消息缓冲按这些上限算出大小（492 字节），
所以编码不会因为空间不够而失败。

几处取舍：
- **前缀白名单，而不是整条 cmdline**：内核自己的选项留在 ring 0，以后新增的 `kernel.*` 选项默认不会泄露到用户态，
  不需要有人记得去过滤它。
- **不做成 `BootOptions` 的一项**：`BootOptions` 的规则是每个选项有唯一的名字、默认值和语法，而这里一个都没有——内核什么也不解析，
  只是转交整个 token。
- **放不下就丢弃并记日志，不截断**：截断的 `userboot.next=` 会启动一个没人要的程序；丢弃则让 userboot 退回默认行为，原因写在日志里。
  这和 `BootOptions` 遇到取不了的值时忽略它、而不是拒绝启动的做法一致。
- **否掉的方案：像现在的 Zircon 那样，把整个启动容器作为 `VmoBootdata` 交给 userboot，由它自己找 CMDLINE 解析。**
  这更贴近今天的 Zircon，但要把 OBI 解析搬进用户态；而且 ours 内核里的 OBI 目前没有现成的 VMO，改动大得多。

**参照**
- `Zircon userboot main.rs` 的 `parse_option()` / `get_options()`：userboot 只认 `userboot.next`、`userboot.root`、`userboot.crc`，
  是从交给它的 ZBI VMO 里的 CMDLINE 项读出来的。ours 按同样的名字划边界，只是改由内核事先挑出来转交。
- Zircon 早期的 userboot 是从 processargs 的环境变量里拿到内核 cmdline 的，本条走的是这条老路。

**验证**
- `prebuilt/ninja/ninja -C out/userboot-next` 通过；clang-tidy 在改动行上没有新的诊断（`text_` 命名那条是原有的）。
- QEMU（dev 镜像）：临时在 cmdline 里加 `userboot.next=probe kernel.x=1`（验证后已还原），日志里只出现
  `userabi: passing userboot.next=probe to init`，`kernel.x=1` 和 `kernel.select.userboot=init` 都没有转发；init 起 shell、ping/pong 照常。

**故意没做**
- 让 userboot 程序读取 `userboot.next` 并启动下一个程序：那是下一步，需要把 userboot 改成链接 runtime。
- 去掉 `kernel.select.userboot`：Zircon 保留了它，用来选第一个进程跑哪个 userboot 程序（eng 镜像换成测试版），ours 也保留。
