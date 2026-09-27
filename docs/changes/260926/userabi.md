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

## userboot 程序读 `userboot.next`，把根 job 与 BootFS 转交给下一个程序并启动它（2026-09-26）

**文件**
- `kernel/lib/userabi/userboot/userboot.cpp` — 改为跑在 runtime 上：`main()` 在 environ 里找 `userboot.next=`，用 `process_spawn_boot` 启动它，并交出根 job 与 BootFS；没有这个选项时仍只打印一行存活证明
- `kernel/lib/userabi/userboot/BUILD.gn` — 依赖 `//system/lib/ou` 和 `//system/lib/runtime`，注释改成新的职责

**症状**
userboot 程序什么也启动不了：没有 runtime，`_start` 不读 processargs，打一行 hello 就停住。
所以镜像想跑 init，只能让内核用 `kernel.select.userboot=init` 直接启动 init，userboot 被顶替掉。
这和 Zircon 的模型相反：userboot 永远是第一个进程，接下来跑谁由它自己决定。

**原因**
第一个进程能拿到的一切——根 job、BootFS，以及上一条加上的 `userboot.*` environ——都在 processargs 消息里。
不读这条消息，userboot 既不知道该启动谁，也没有可以用来启动的 job。旧的 userboot 是按 Zircon 的
trivial-test-userboot 写的，只证明「能进 ring 3、能答一次调用」，没有为「启动下一个程序」留任何位置。

**改法**
改前（整个程序就是这个入口）：
```cpp
extern "C" [[noreturn]]
auto _start() -> void {
    static_cast<void>(ours_debug_write(MESSAGE, sizeof(MESSAGE) - 1));
    for (;;) {
        __asm__ volatile("pause");
    }
}
```
改后：
```cpp
auto main(int, char **) -> int {
    auto const next = find_next();
    if (next.empty()) {
        static_cast<void>(ou::debug_line("userboot: hello from ring 3"));
        halt();
    }
    auto const job = runtime::take_startup_handle(ours::HandleType::JobDefault, 0);
    ...
    auto const bootfs = runtime::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    if (auto const status = start_next(job, bootfs, next); status != ours::Status::Ok) {
```
`start_next()` 给下一个程序的 processargs 与内核给 userboot 的一样：JobDefault、VmoBootfs（有才给），argv = {name}。
所以 init 被 userboot 启动时，看到的和以前被内核直接启动时完全相同，init 一行都不用改。

几处取舍：
- **先 spawn、后写 processargs**，顺序和内核的 `make_bootstrap()` 相反。原因是 job 句柄既要作为 spawn 的父 job，又要放进消息里交出去；
  而 `channel_write` 会把发送的句柄移出本进程。如果先写，手里就没有 job 可以用来 spawn 了。
  否掉的方案是先 `handle_duplicate` 一份：可行，但 userboot 会白白多握一个根 job 句柄。
  反过来的顺序没有代价——runtime 会等到 READABLE 才读消息。
- **多个 `userboot.next` 时取最后一个**，与 Zircon `parse_cmdline()` 逐项覆盖的行为一致：写在后面的覆盖前面的，
  bootloader 才能在镜像默认值后面追加覆盖。
- **启动之后停住，不退出**：Zircon 的 userboot 会等下一个程序结束再退出，但 ours 还没有进程结束的通知（进程句柄上的 TERMINATED，设计 K23）。
- **没有 BootFS 也照样启动**：内核建不出 BootFS 时就不给，该由下一个程序自己应对，init 本来就会处理。

**参照**
- `Zircon userboot main.rs`：`parse_option()` 认 `userboot.next`，`run()` 用 `launch_program()` 启动它并把系统句柄交出去。
  ours 暂不支持 Zircon 用 `+` 分隔参数的写法（`bin/component_manager+--boot`），也不支持 `userboot.root`。

**验证**
- `prebuilt/ninja/ninja -C out/userboot-next` 通过；clang-tidy 在这个文件上没有诊断。
- QEMU，临时改 dev 镜像的 cmdline（验证后已还原）：
  - 只写 `userboot.x=init`（没有 next）：日志里 `userabi: handing userboot its processargs` → `userboot: hello from ring 3`，之后停住。
  - 写 `userboot.next=init`：`userboot: starting init` → `userboot: started init` → `init: started with the root job` → `init: bootfs has 4 entries`
    → init 起 shell → `init: shell says "pong"` → `init: shell closed its channel`。

**故意没做**
- 改两个镜像的 cmdline：放在下一条单独提交。
- 支持 `+` 分隔的参数、`userboot.root`：还没有需求。
- 内核 `launch()` 的日志前缀仍是 `userboot:`（例如 `userboot: found 'init'`）。现在 userboot 已经是一个独立的程序，这个前缀容易让人误读，但它属于内核侧的命名，本条不动。

## dev、dev.dbg_1 两个镜像改用 `userboot.next`，userboot 固定先跑（2026-09-26）

**文件**
- `kernel/products/image/dev/BUILD.gn` — cmdline 从 `kernel.select.userboot=$_userboot` 改为 `userboot.next=$_userboot`（`_userboot` 变量不动），注释改写
- `kernel/products/image/dev.dbg_1/BUILD.gn` — cmdline 从 `kernel.select.userboot=shell` 改为 `userboot.next=shell`，注释改写

**症状**
两个镜像都让内核直接启动 init（dev）或 shell（dbg_1），第一个用户进程不是 userboot。前两条改动已经让 userboot 能读取并执行 `userboot.next`，但镜像还没用上它。

**原因**
`kernel.select.userboot` 在 Zircon 里选的是**第一个进程跑哪个 userboot 程序**（正常是 `userboot`，eng 镜像换成测试版），而不是「系统跑哪个服务」。
ours 以前借它来指名 init/shell，等于让 init 顶替了 userboot 的位置。结果是：凡是本该由 userboot 做的事（选下一个程序、分发启动句柄），都没有位置可放，
只能被拆散到内核和 init 里去做。

**改法**
改前（dev）：
```gn
args = [
  "kernel.select.userboot=$_userboot",
  "kernel.shell=true",
]
```
改后：
```gn
args = [
  "userboot.next=$_userboot",
  "kernel.shell=true",
]
```
dbg_1 同理改为 `"userboot.next=shell"`。两边都不写 `kernel.select.userboot`，取默认的 `userboot`。
`_userboot` 变量名和 `ours.userboot` 构建参数暂时不动，留给构建会话统一改名为 `next`。
cmdline 上方的注释改用英文写：gn format 会按空格重排注释，把中文段落排出多余的空格（旧注释里的「就跑 默认的」就是这样来的）。

**参照**
- `Zircon lib/boot-options/options.inc` 的 `kernel.select.userboot`：只选 userboot 程序本身。
- `Zircon userboot main.rs` 的 `userboot.next`：决定 userboot 接下来启动谁；`integration_tests.cc` 也是这样在 cmdline 里指名测试程序的。

**验证**
- `prebuilt/ninja/ninja -C out/userboot-next` 通过；`gn format --dry-run` 两个文件都干净。
- QEMU dev：`userabi: passing userboot.next=init to userboot` → `userboot: starting init` → `userboot: started init` → `init: started with the root job`
  → `init: spawned shell` → `init: shell says "pong"` → `init: shell closed its channel`。
- QEMU dev.dbg_1（跑 60 秒）：`userboot: starting shell` → `userboot: started shell` → shell 的 8 组检查全部 `checks passed`（channel、handle transfer、
  handle quota、bad-pointer、vmo_read、bootfs、handle_duplicate、libc），fault_probe 逐个被杀，没有 panic。shell 拿到的根 job 和 BootFS 与以前被内核直接启动时相同。

**故意没做**
- 改 `ours.userboot` / `tools/set.py --userboot` / `build/product/ours_args.gni`：由构建会话统一改名为 `next`。
- `options.hpp` 与内核 `userboot_init()` 中「由镜像指名第一个程序」的注释：放在下一条。

## 注释与设计文档改到「userboot 固定先跑，`userboot.next` 决定下一个」（2026-09-26）

**文件**
- `kernel/include/ours/boot/options.hpp` — `USERBOOT_OPTION` 与 `userboot()` 的说明：只选 userboot 程序本身，下一个程序由 `userboot.next` 决定
- `kernel/lib/userabi/include/userabi/userboot.hpp`、`kernel/lib/userabi/src/userboot.cpp` — `userboot_init()` 的说明与函数内注释
- `kernel/products/image/BUILD.gn` — 共享包里「userboot 是默认启动的程序」一句
- `system/service/init/init.cpp`、`system/service/init/BUILD.gn` — init 由 userboot 启动，而不是内核
- `examples/shell/shell.cpp` — 九处「the shell the kernel starts」改为 userboot；日志 `shell: started by the kernel; no one to answer` 改为 `started by userboot`
- `docs/design/260924/userspace-filesystem.md`、`docs/design/260923/userspace-interface.md` — 标出 U8 / P1 §3.2 已落地和未落地的部分，P0 §3.1 第 2 步标为已被取代

**症状**
前三条改动之后，这些注释和文档仍然写着「镜像用 `kernel.select.userboot` 指名第一个程序」「shell/init 由内核直接启动」。dbg_1 的 shell 日志也还是
`started by the kernel`，而实际上启动它的是 userboot。读注释的人会得到一个错误的启动模型。

**原因**
旧模型写在很多地方：选项说明、userabi 接口、镜像 BUILD、init/shell 的文件头和各个检查函数的前置条件说明。每一处都是在描述同一件事实——
第一个进程由内核按镜像指名启动。这件事实变了，而这些注释不会随代码自动过期。尤其是 shell 里「只有内核直接启动的 shell 才有 BootFS/根 job」这类前置条件，
会误导人去内核里找发放这些句柄的代码，而实际发放者已经换成了 userboot。

**改法**
改前（`options.hpp`）：
```cpp
/// The program the first user process runs, as a file in the kernel
/// package. Zircon's option of the same name, with the same default; see
/// userboot() for why it is the image's to choose.
...
/// Which program runs first belongs to the image being booted, not to the
/// kernel: an image that wants something other than the proof-of-life
/// program says so in its cmdline (//kernel/products/image/dev/BUILD.gn).
```
改后：
```cpp
/// Which build of the userboot program the first user process runs, as a
/// file in the kernel package. Zircon's option of the same name, with the
/// same default; see userboot() for what it does and does not choose.
...
/// The first user process is always a userboot program; this option
/// picks only *which* one. Zircon's images leave it at the default and
/// eng images swap in userboot-test-rust, a userboot built to run tests;
/// neither puts a system service here. What userboot starts next is a
/// separate option, `userboot.next`, which the kernel does not parse: ...
```
其余各处同理：把「the kernel starts」改为「userboot starts」，并写明句柄是 userboot 原样转交的。
设计文档只加进度标注，不改原计划正文：计划本身是历史，读的人需要同时看到「原来打算怎么做」和「现在做到哪一步」。

**参照**
- `Zircon lib/boot-options/options.inc` 第 908 行 `kernel.select.userboot`；`zircon/kernel/image/eng` 用它换成 `userboot-test-rust`。
- `Zircon userboot main.rs` 的 `userboot.next`。

**验证**
- 除一条日志字符串外只改了注释和文档。`prebuilt/ninja/ninja -C out/userboot-next` 通过；clang-format 与 `gn format` 都干净（clang-format 顺手改的 shell.cpp 里两处旧代码格式已剔除，不混进本提交）。
- QEMU dev：`userboot: starting init` → `userboot: started init` → `init: shell says "pong"`。dbg_1：`userboot: starting shell` → `shell: started by userboot; no one to answer` → 8 组检查全部 `checks passed`。

**故意没做**
- `tools/set.py` 的 `--userboot` 检查仍在 pack 响应文件里找 `--entry=kernel.select.userboot=`。镜像已经不写这一项，所以 `--userboot` 现在会报「没有这一项」。
  那个文件由构建会话负责，会和 `ours.userboot` → `next` 的改名一起改。
- `docs/changes/` 与 `docs/repairs-20260830.md` 里的旧说法：前者只增不改，后者是只读归档。

## userboot 启动 next 之后退出，而不是停住（2026-09-26）

**文件**
- `kernel/lib/userabi/userboot/userboot.cpp` — 读到 `userboot.next` 后，所有路径都从 `main()` 返回（成功返回 0，失败返回 1），由 runtime 调 `process_exit`；只有没有 next 的存活证明路径还会停住
- `docs/design/260924/userspace-filesystem.md`、`docs/design/260923/userspace-interface.md` — 进度标注改为「已退出」

**症状**
userboot 启动 init/shell 之后永远在 `pause` 里空转。设计文档 U8 的顺序是「……然后 process_exit(0)」，验证判据也要求日志里出现
`object: process 'userboot' … destroyed`，这两条之前都没有做到。

**原因**
userboot 把根 job 和 BootFS 交出去之后，手里什么也不剩，它的工作就结束了。继续留着它，只会白占一个进程、一个线程、一个地址空间，
还会让「系统里还有谁」这个问题多出一个不做事的答案。上一条改动里让它停住的理由是「还没有进程结束的通知」，但这个理由只适用于
**等待** next 结束（Zircon 的做法）；userboot 自己退出并不需要任何通知。

**改法**
改前：
```cpp
    static_cast<void>(ou::debug_line({"userboot: started ", to_str(next)}));
    halt();
}
```
改后：
```cpp
    static_cast<void>(ou::debug_line({"userboot: started ", to_str(next)}));
    return 0;
}
```
拿不到根 job、或者启动 next 失败时，同样 `return 1` 退出，不再停住：一个没法给下一个程序准备好条件的 userboot，也没有别的事可做。
这和 Zircon 一致——它的 `main()` 记下错误后照样返回。
userboot 的退出不会带走任何东西：next 挂在根 job 下，而不是 userboot 下，而且 userboot 原有的句柄已经全部交给了 next。
没有 `userboot.next` 的存活证明镜像仍然停住，因为那里没有要交出的东西，留在 ring 3 本身就是它要证明的内容。

**参照**
- `Zircon userboot main.rs` 的 `main()`：`run()` 返回后（无论成败）进程结束。Zircon 在 `launch_program()` 之后还会等 next 结束；ours 没有等这一步，因为进程句柄上的 TERMINATED 还没做（设计 K23）。

**验证**
- 构建通过；clang-tidy 在这个文件上没有诊断。
- QEMU dev：`userboot: started init` 之后是 `object: process 'userboot' (koid 26) destroyed`，init 照常起 shell，`init: shell says "pong"`。
- QEMU dbg_1（60 秒）：`object: process 'userboot' (koid 26) terminated, retcode 0` → `destroyed`；shell 的 8 组检查全部通过，11 个探针被杀，对照探针 `none` 答 `unknown`。

**故意没做**
- 等 next 结束再退出（Zircon 的做法）：需要进程句柄上的 TERMINATED（K23）。
- 让存活证明路径也退出：那条路径要证明的正是「从最后一次系统调用返回后，ring 3 还在继续运行」。

## 内核向用户态转发的 cmdline 选项从 `userboot.*` 扩成前缀白名单，加入 `ours.autorun.*`（2026-09-26）

**文件**
- `kernel/lib/userabi/src/userboot.cpp` — `USERBOOT_OPTION_PREFIX` 换成前缀表 `USER_OPTION_PREFIXES`（`userboot.`、`ours.autorun.`），`collect_userboot_options()` 等改名为 `collect_user_options()` / `UserOptions` / `MAX_USER_OPTIONS` / `USER_OPTIONS_BYTES`
- `kernel/include/ours/boot/options.hpp`、`kernel/products/image/dev/BUILD.gn` — 注释里的函数名跟着改

**症状**
无法在开机时指定一个在 shell 之后运行的程序。构建会话已经让 `--with` 只负责把程序打进 BootFS（`69526fd2`），但 cmdline 上没有任何选项能让用户态知道「开机后要跑它」：内核只把 `userboot.*` 转给 userboot，别的 token 根本出不了 ring 0。

**原因**
Zircon 用 `zircon.autorun.boot=<command>` 做这件事，读它的是 console-launcher（也就是起 shell 的那个组件），不是 userboot。在 ours 里对应的角色是 init。所以这个选项既不属于内核，也不属于 userboot，而原来的白名单只认 userboot 自己的前缀，它就没有路可走。

白名单本身是对的，必须保留：内核选项（`kernel.shell`、`kernel.name`……）只管 ring 0，新加一个内核选项不应该因为没人记得过滤就漏到用户态。所以这里不把条件放宽成「除了 `kernel.*` 以外都转发」这种黑名单，而是把单个前缀变成一张表：以后每多一族用户态选项，就在表里加一行，要有人明确决定它可以越过这条边界。

**改法**
改前：
```cpp
    constexpr std::string_view USERBOOT_OPTION_PREFIX{"userboot."};
    ...
            if (!token.starts_with(USERBOOT_OPTION_PREFIX)) {
                return;
            }
```
改后：
```cpp
    constexpr std::string_view USER_OPTION_PREFIXES[] = {
        "userboot.",
        "ours.autorun.",
    };
    ...
    auto is_user_option(std::string_view const token) -> bool {
        for (auto const prefix: USER_OPTION_PREFIXES) {
            if (token.starts_with(prefix)) {
                return true;
            }
        }
        return false;
    }
    ...
            if (!is_user_option(token)) {
                return;
            }
```
选项名定为 `ours.autorun.boot=<name>[+arg...]`，直接照搬 `zircon.autorun.boot` 的形状：`+` 分隔参数，保留 `.boot` 后缀，给将来的 `.system` 留出位置。内核仍然只转发整个 token，不解析；userboot 负责把不属于自己的部分交给 init（下一条），init 负责解析并执行（再下一条）。

函数和常量随之改名：它们收集的已经不只是 userboot 的选项，名字再叫 `userboot_options` 就会误导读的人。

被否掉的方案：
- 叫 `userboot.autorun=`：省了内核这一步，但这个选项并不归 userboot 管，挂在它名下是说错了归属。
- 内核直接把选项放进 init 的 processargs：init 不是内核起的，内核够不着它。

**参照**
- `Zircon userboot main.rs` 的 `parse_option()`：只认 `userboot.*`。Zircon 不在内核里过滤，而是把整个 ZBI 交给下一个程序，由 component_manager 通过 `fuchsia.boot.Arguments` 提供其余选项。
- `Fuchsia console_launcher.cc` 的 `GetArguments()`（`autorun_boot`）、`console-launcher.cml` 的 `fuchsia.zircon.autorun.boot`、`docs/reference/kernel/kernel_cmdline.md` 的 `zircon.autorun.boot`。

**验证**
- 构建通过；clang-tidy 在 `userboot.cpp` 上没有新诊断。
- QEMU dev（另开 `out/autorun`，加 `--with //kernel/lib/syscall/test/fault_probe`，临时往 cmdline 里加 `ours.autorun.boot=fault_probe+hello+world`，构建完即还原）：日志出现 `userabi: passing ours.autorun.boot=fault_probe+hello+world to userboot`；userboot 照常起 init，init 起 shell，`init: shell says "pong"`。
- QEMU dbg_1（60 秒）：只转发 `userboot.next=shell`，shell 自检照常跑。

**故意没做**
- userboot 转交、init 执行：分别是后两条改动。
- 把 `kernel.*` 以外的选项全部转发：理由见「原因」，白名单是有意保留的。

## userboot 把不属于自己的环境串转交给下一个程序（2026-09-26）

**文件**
- `kernel/lib/userabi/userboot/userboot.cpp` — 新增 `collect_handed_on()`，`start_next()` 把 `userboot.*` 以外的 environ 写进下一个程序的 processargs，消息缓冲从 128 放大到 512

**症状**
上一条改动之后，内核已经把 `ours.autorun.boot=...` 交给了 userboot，但 userboot 起 init 时 environ 是空的，init 看不到这个选项。

**原因**
内核只给一个进程发消息，就是 userboot。init 不是内核起的，内核够不着它；userboot 之后的每个程序能拿到什么，全看它的父进程交了什么。所以用户态选项要到达 init，中间必须经过 userboot 转交。userboot 原来只把 argv 和两个句柄交出去，环境串一个也没带。

**改法**
改前：
```cpp
    u8 message[128];
    ...
    if (auto const status =
            ours::encode_process_args(message, {infos, count}, args, {}, {}, &length);
```
改后：
```cpp
auto collect_handed_on() -> HandedOn {
    HandedOn handed_on;
    for (auto const entry: runtime::process_args().environ()) {
        if (entry.starts_with(USERBOOT_OPTION_PREFIX)) {
            continue;
        }
        ...
        handed_on.entries[handed_on.count++] = entry;
    }
    return handed_on;
}
    ...
    u8 message[512];
    ...
    if (auto const status = ours::encode_process_args(message, {infos, count}, args,
                                                      {environ.entries, environ.count}, {},
                                                      &length);
```
`userboot.*` 是 userboot 自己的选项，到它这里就结束，不往下传：`userboot.next` 是什么，与 init 无关。其余的原样转交。这样，以后新增一族用户态选项只需要改内核的白名单，userboot 不用跟着改。

缓冲区大小按内核的上限算：头部、两个 handle_info、64 字节的程序名，再加上内核允许所有转发选项占用的 384 字节（`USER_OPTIONS_BYTES`）。两个上限分属两个程序，没有任何东西把它们绑在一起。万一内核的上限将来超过这个缓冲区，编码会返回 `BufferTooSmall`，next 不会启动。这是有意为之：宁可起不来并留下报错，也不要悄悄丢掉一个选项。条数的上限（`MAX_HANDED_ON = 8`）同理，超出的那一条会在日志里点名后丢弃，和内核丢 token 的做法一致。

被否掉的方案：原样转交全部 environ，包括 `userboot.*`。这样更省事，但会让 init 看到一堆和它无关的键；更重要的是，以后要是有人在 init 里读到了 `userboot.next`，两个程序对同一个键就有了两个主人。

**参照**
- `Zircon userboot main.rs`：Zircon 把整个 ZBI 交给下一个程序（component_manager），由后者通过 `fuchsia.boot.Arguments` 提供选项，其中包括 `zircon.autorun.boot`。ours 没有这层服务，environ 本身就是选项，所以由 userboot 在转交时去掉自己的那部分。

**验证**
- 构建通过；clang-tidy 在这个文件上没有诊断。
- QEMU dev（`out/autorun`，cmdline 临时加 `ours.autorun.boot=fault_probe+hello+world`）：`userboot: passing ours.autorun.boot=fault_probe+hello+world to init`，没有 `userboot.next` 被转交；init 照常起 shell，`init: shell says "pong"`、`init: shell closed its channel`。
- QEMU dbg_1：没有要转交的内容，userboot 照常起 shell；shell 的 8 组检查全部通过，11 个探针被杀，对照探针 `none` 答 `unknown`。约 12 秒时 QEMU 被另一个并发会话发来的 SIGTERM 结束，没有跑满 60 秒。

**故意没做**
- init 解析并执行 `ours.autorun.boot`：下一条。
- 把转交的范围做成可配置：目前只有一族选项需要转交，规则就是「不是自己的就交出去」，没有什么需要配置。
