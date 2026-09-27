# system/lib/runtime 改动记录（2026-09-26）

## processargs：格式、唯一的编码器与唯一的解码器（2026-09-26）

**文件**
- `system/public/ours/process_args.hpp` — 新增：消息头 `ProcArgs`、`PROCARGS_PROTOCOL`/`PROCARGS_VERSION`、`HandleType`、`handle_info()` 一组、只有头文件的编码器 `encode_process_args()`
- `system/public/public_headers.gni` — 列入这个头
- `system/lib/runtime/` — 新库（copier 生成骨架）：`ProcessArgs`（解码器）、`StartupHandles`（启动句柄表）、`BUILD.gn`、`.clang-format`、`.gitignore`、`README.md`
- `system/lib/runtime/test/` — 15 个宿主单测
- `BUILD.gn` — 默认构建带上 `//system/lib/runtime:tests`

**症状**
无，新功能。设计步骤 U2 的前半：一个进程今天只从 `rdi` 拿到**一个**句柄，它是什么全靠双方约定（dbg_1 的 shell 拿到根 job，dev 的 shell 拿到一个 channel）。
接下来每个程序都要拿到不止一样东西——根 job、K12 建好的 BootFS VMO、vmex、`/boot` 与 `/svc` 的连接、命令行参数——一个寄存器装不下，也说不清哪个是哪个。

**原因**
这一节的学习价值在"为什么是一条消息"和"为什么解码要比 Zircon 严"。

- **一条消息**：`rdi` 改为一个 channel（bootstrap channel），启动方往里写一条 processargs 消息：句柄一组，每个带一个 `handle_info`
  （低 8 位是类型，如 `JobDefault`、`VmoBootfs`；高 16 位是编号，如 NsDir 在 names 里的下标），外加 argv、环境变量、名字表。
  这就是 Zircon 的 `zx_proc_args`，字段顺序、句柄类型的数值都照搬；只有 protocol 字用我们自己的 "OPAR"，免得被当成能和 Zircon 互通。
  对照：Linux 在 exec 时把 argc/argv/envp/auxv 摆在新栈上，由 libc 的 crt1 整理后交给 main；Zircon 和这里是一条 channel 消息，由 runtime 读。
- **只有一个编码器、一个解码器**：内核给第一个进程发（K14）、launcher 给子进程发（K23），都用 `encode_process_args()`；程序这一侧只用
  `runtime::ProcessArgs`。编码器只写调用方给的缓冲区、不分配，所以内核、用户态、宿主测试三处都能用。
- **解码比 Zircon 严**：Zircon 有两个解码器，各有漏洞——
  - 旧的 musl `processargs_read()` 根本不检查 names 段；
  - 新的 `ld::ProcessargsBuffer::Valid()` 检查 handle_info 数组边界时，拿字节数去减一个**以字为单位**的偏移，
    于是一条 40 字节、声称在偏移 36 处有 5 个 handle_info 的消息能通过（实际只放得下 1 个）；
  - 新版遇到跑出消息末尾的字符串不报错，而是悄悄少报几个。
  全系统只有一个编码器，解码失败只可能是编码器的 bug 或伪造的消息，两种情况都应该让程序停下，而不是带着少了一个的参数启动。

**改法**
改前：没有这个格式；各程序的 `_start(bootstrap, vdso_base)` 各自约定 `bootstrap` 是什么。

改后，格式（节选）：

```cpp
struct ProcArgs {
    u32 protocol;
    u32 version;
    u32 handle_info_off;
    u32 args_off;
    u32 args_num;
    u32 environ_off;
    u32 environ_num;
    u32 names_off;
    u32 names_num;
};
static_assert(sizeof(ProcArgs) == 36);

inline constexpr auto PROCARGS_PROTOCOL = u32{0x5241'504f};   // "OPAR"

constexpr auto handle_info(HandleType const type, u16 const arg) -> u32 {
    return u32(type) | u32(arg) << 16;
}
```

字节布局（与 Zircon `process_builder` 写出的一致，无填充；空段的偏移是它本该开始的位置）：

```
[ProcArgs][u32 handle_info × 句柄数][args][environ][names]
```

- **编码器先算完再写**：先算出每段的偏移和总长，检查句柄数、总长不超过一条 channel 消息的上限（OutOfRange）、字符串里没有 NUL
  （InvArgs——否则读的一方会多读出一个串）、`handle_info` 的保留位（8–15）为 0，最后才检查缓冲区够不够（BufferTooSmall）。
  **只有返回 Ok 才会写任何东西**，缓冲区和长度在失败时原样不动。所有和都小于 4096，所以最后收窄成 u32 是精确的。
- **解码器的检查顺序**：头部够长 → protocol 与 version 完全相等 → handle_info 数组在头部之后、4 字节对齐、到末尾之前放得下全部
  句柄（照 musl 的写法 `(bytes - off) / 4 >= nh`）→ 没有 handle_info 设置保留位 → 每个非空字符串段在头部之后、末尾之前，且剩余字节
  至少够每个串一个 NUL → 每个串的 NUL 都在消息内。任何一条不过都回 InvArgs（Zircon 对格式错误的消息回 ZX_ERR_INVALID_ARGS）。
  所有"不超过末尾"的比较都写成对 `bytes` 做减法、并先确认偏移不在末尾之后，防止无符号减法回绕。未知的句柄类型**不**算错：那是读的一方可以忽略的东西。
- **字符串就地读**：`ProcessArgs::Strings` 迭代出的 `std::string_view` 直接指向消息缓冲，而且每个都以 NUL 结尾，可以直接当 C 字符串交给
  `main` 的 argv。代价是消息缓冲必须活得比这个视图久（下一步的启动代码会把它放在永不返回的栈帧或静态区）。
- **`StartupHandles::take(type, arg)`**：按完整的 `handle_info`（类型和编号都要相等）找第一个，交出去并把该槽清零，第二次取同一个就拿不到——
  每个句柄只有一个主人。没人取的句柄保持打开直到进程结束，和 Zircon 一样。不加锁：P0 每个进程只有一个线程（接口设计 4.8）。

**参照**
- 格式：Zircon `zircon/system/public/zircon/processargs.h` 的 `zx_proc_args`、`PA_HND()` 与 `PA_*`。
- 编码布局：Zircon `src/lib/process_builder/src/process_args.rs` 的 `Message::build()`；单测里的金样字节就是从它的测试移植的。
- 解码：Zircon `sdk/lib/c/startup/musl-processargs/processargs.c` 的 `processargs_read()`、`processargs_strings()`；
  `sdk/lib/ld/include/lib/ld/processargs.h` 的 `ProcessargsBuffer::Valid()`（上面说的那个量纲错误在它的 handle_info 边界式里）。
- 取句柄：Zircon `sdk/lib/c/zircon/take_startup_handle.c` 的 `zx_take_startup_handle()`。

**验证**
- 宿主单测 15 个全部通过（`out/bringup.x86/host/obj/system/lib/runtime/test/process_args`）：
  - 编码器写出的字节与 Zircon 金样**逐字节**相同（三个句柄 User1/0x1234、NsDir/0、NsDir/1，参数 arg1–arg3，环境 FOO=BAR，名字 /data、/pkg；只换了 protocol 与 version）；
  - 解码器把它原样读回；空消息就是 36 字节的头；
  - 编码器的每种拒绝（串里有 NUL、保留位、句柄过多、总长多一个字节、缓冲区少一个字节）都不写任何东西；
  - 解码器的每条检查各造一条"恰好越界"的消息：头部少一个字节、Zircon 的 protocol、版本加一、handle_info 偏移在头部内/不对齐/在末尾之后、
    句柄数多一个、**40 字节消息声称 5 个句柄**（Zircon 新版放过的那一例）、三个字符串段的偏移在头部内/正好在末尾/在末尾之后、
    串的个数过多、最后一个串没有 NUL、保留位——全部被拒；未知的句柄类型被接受；
  - `take()`：类型和编号都要匹配，取过的再取拿不到。
- **变异检查**：把解码器里的 handle_info 边界换成 Zircon 新版那个有量纲错误的式子、把"串没有 NUL 就失败"改成"就停下"，各有对应的测试失败；
  分别删掉"handle_info 偏移不在末尾之后"和"字符串段偏移不在末尾之后"两条判断，也各有测试失败。
- 两个镜像构建、启动，日志不变：dev `init: shell closed its channel`，dbg_1 `shell: probes done (11 killed, 0 survived)`。
- 审查：三个方向（格式与编码器、解码器安全、测试与注释）并行审查、逐条反驳核实。成立的三条都已在本提交处理：`parse()` 文档里列出的检查顺序
  与代码不符；没有用例把偏移放到末尾之后（删掉那两条判断测试照样全过，ASan 下是越界读）；"失败时不写"只在一个用例里检查过。

**故意没做**
- 读 bootstrap channel、调 `main`、`main` 返回后调 `process_exit` 的启动代码：U2 的下一个提交。它是用户态专用的（汇编入口），和这里能在宿主上测的部分分开。
- `runtime::take_startup_handle` 这个全局入口：同上，它需要一张全局的表，随启动代码一起加。
- bump 分配器：设计把它列在 runtime 里，第一个使用者是 I1 的 init，到那时再加。
- 解码器不检查各段是否互相重叠（例如字符串段压在 handle_info 上）：编码器从不这样写，而重叠本身不会让读越界；Zircon 两个解码器也都不查。

## runtime 的启动代码：`_start` → 读 processargs → `main` → `process_exit`（2026-09-26）

**文件**
- `system/lib/runtime/src/crt0.S` — 新增：`_start`（x86-64）与 `runtime_call_main`
- `system/lib/runtime/src/start.cpp` — 新增：`runtime_start()`，以及 `take_startup_handle()`、`process_args()`、`vdso_base()`
- `system/lib/runtime/include/runtime/main.hpp`、`startup.hpp` — 新增：`main` 的 C 名字声明；启动之后的查询接口与 `EXIT_START_FAILED`
- `system/lib/runtime/BUILD.gn` — 只在用户工具链里定义的 `source_set("runtime")` 与 `start.headers`
- `system/lib/runtime/test/runtime_smoke.cpp`、`test/BUILD.gn`、`BUILD.gn` — 只链接、不运行的冒烟程序，进默认构建
- `docs/design/260923/userspace-interface.md` — 3.6：`ou::take_startup_handle` 改为 `runtime::take_startup_handle`
- `docs/design/260924/userspace-filesystem.md` — K14：写明 runtime 会关掉 bootstrap、会先等消息，以及 init 今天的写入顺序
- `docs/repairs/260926/runtime.md` — 新增第 1 条：没有人执行 `.init_array`

**症状**
无，新功能。设计步骤 U2 的后半：有了格式和解码器，还要有一段"在 main 之前运行"的代码去读消息、调 main；今天每个程序都手写自己的 `_start`。

**原因**
这一节的学习价值在 crt0 该做的几件事，以及为什么 `main` 必须由汇编来调。

- **栈对齐**：内核进入程序时 `rsp` 对 16 取余是 8（设计 3.4，等于"刚被 call 进来"）。`_start` 在第一次 call 之前把它对齐到 16，
  被调函数才能拿到 ABI 要求的栈。今天程序用 `-mno-sse` 编译，对齐错了也不出事，但打开 SSE 那天就会在 `movaps` 上出错。
- **`rbp` 清零**：按帧指针回溯的代码看到 0 就停，`_start` 是最外层。
- **`main` 由汇编调**：C++ 规定程序不得调用 `main`（[basic.start.main]）。而且用户程序是 `-ffreestanding` 编译的，对编译器来说 `main`
  只是普通函数，名字会被修饰成 `_Z4mainiPPc`，汇编里的 `main` 找不到它。所以 runtime 提供 `<runtime/main.hpp>`，把 `main` 声明为
  `extern "C"`，程序在定义 `main` 的文件里包含它；真正的调用在 `crt0.S` 的 `runtime_call_main` 里，一条 `jmp main`。Zircon 的程序是
  hosted 编译，`main` 不会被修饰，它的 `crt1.S` 直接写 `main` 就行；hosted 下把 `main` 声明成 `extern "C"` 反而是错误（-Wmain），
  所以这个头不能进宿主构建。
- **argv 放在哪**：argv 里的每个指针都指向消息里的字符串，所以消息缓冲必须活到进程结束——放在静态区。argv 指针数组放在
  `runtime_start()` 自己的栈帧上（`__builtin_alloca`），这个帧永不返回，main 运行期间一直有效。旧版 Zircon 的 `__libc_start_main`
  也是把 argv 放在启动帧的变长数组里。最坏情况是一条 4 KiB 的消息全是空串，约 4060 个指针、32 KiB，在 64 KiB 的栈里放得下。
- **全局变量为什么是 `constinit`**：没有任何代码执行 `.init_array`（加载器不做，runtime 也还没做），需要构造函数的全局对象会停留在零值、
  且不报错。`constinit` 让编译器保证 runtime 自己的全局变量都是常量初始化的。程序的全局变量没有这层保护，记为待修项。

**改法**
改前：每个程序自己写 `extern "C" [[noreturn]] auto _start(HandleValue bootstrap, VirtAddr vdso_base) -> void`，自己约定 `bootstrap` 是什么。

改后，入口：

```asm
_start:
    xorl %ebp, %ebp
    andq $-16, %rsp
    call runtime_start
    ud2

runtime_call_main:
    jmp main
```

`runtime_start()`（节选）：

```cpp
    runtime::vdso = vdso_base;
    if (bootstrap != ours::HANDLE_INVALID) {
        runtime::read_processargs(bootstrap);   // wait, read, close, parse, fill the handle table
    }
    auto const args = runtime::startup_args.args();
    auto **const argv =
        static_cast<char **>(__builtin_alloca(sizeof(char *) * (usize{args.size()} + 1)));
    ...
    ou::process_exit(runtime_call_main(static_cast<int>(args.size()), argv));
```

- **先等再读**：第一版只读一次、读不到就失败，理由是"启动方总是先写消息再启动进程"。审查指出这个前提今天就不成立：init 是先
  `process_spawn_boot` shell、后写消息（`init.cpp:75-81`），shell 在另一个 CPU 上可能抢先读到空队列。今天 shell 自己处理了
  ShouldWait，K14 换成 runtime 后这层处理就没了。改为照 Zircon 新版（`ld::ProcessargsBuffer`）先等 READABLE 或 PEER_CLOSED：
  启动方不写就丢掉自己那端时，等待醒来、读报 PeerClosed，不会挂死；只有启动方一直握着那端又不写，才会一直等——那是启动方的 bug。
- **读完就关 bootstrap**：照 Zircon。消息是它唯一的用途；程序若要和启动方说话，启动方另给一个 channel 作为启动句柄。
  后果是对端在 main 运行之前就看到 PEER_CLOSED，K14 迁移 init、shell、fault_probe 时要照顾到（已写进设计 K14 行）。
- **没有 bootstrap（`rdi` 为 0）也能启动**：设计 3.4 允许；这时没有句柄，argc 为 0，argv 仍是只含结尾 `nullptr` 的合法数组。
- **启动失败的退出码**：读不到消息或消息不合法，打一行 `runtime: ...` 日志，以 `EXIT_START_FAILED` 退出。第一版取 -1，审查指出 main
  返回 -1 时经符号扩展正好也是 -1，父进程分不清"main 没跑"和"main 返回了 -1"。改为 `INT32_MIN - 1`，在 int 范围之外，没有任何
  main 的返回值能得到它。Zircon 在这里直接 panic；这里若制造一个故障，内核会把它当成程序自己的错报告，所以用有序的退出。
- **`take_startup_handle` 在 runtime 而不在 ou**：设计 3.6 原写作 `ou::take_startup_handle`，同一句又说 runtime 不并进 ou。
  ou 是"一个系统调用一个具名入口"，这个函数不是系统调用，而是 runtime 自己的簿记，所以改名为 `runtime::take_startup_handle`，设计同步修订。
- **还没有程序用它**：每个程序都还自己定义 `_start`，把 runtime 设为 `user_executable()` 的默认依赖会重复定义。init、shell、fault_probe
  在 K14 一起迁移；userboot 要到 U8 变成 launcher 时才迁移（第一版注释写成"K14 全部迁移"，审查指出 userboot 不在其列）。

**参照**
- 入口：Zircon `sdk/lib/c/startup/crt1.S`（`_start` 把 `main` 的地址交给 `__libc_start_main`）、musl `ldso/dynlink` 的 x86-64 入口
  `dl-entry.S`（`and $-16,%rsp`、清 `rbp` 后再 call）。
- 读消息：Zircon `sdk/lib/ld/include/lib/ld/processargs.h` 的 `ProcessargsBuffer`（先等 READABLE）、`sdk/lib/c/startup/processargs-get-handles.cc`
  （读完关掉 bootstrap）；旧版 `__libc_start_main.c`（argv 在启动帧的变长数组里，调 `exit(main(argc, argv, envp))`）。

**验证**
- 冒烟程序 `runtime_smoke` 链接通过；`llvm-readelf`/`llvm-objdump` 检查：入口地址就是 `_start`；`main` 是本地符号、以 C 名字在同一次链接里
  解析，`runtime_call_main` 是一条 `jmp main`；没有 `.rela.plt`，只有 vDSO 导入的 GLOB_DAT，与其他程序相同；没有 `.init_array`；
  `runtime_start` 里的顺序是 `object_wait_one` → `channel_read` → `handle_close` → `ProcessArgs::parse`。
- 两个镜像构建、启动，日志不变：dev `init: shell closed its channel`，dbg_1 `shell: probes done (11 killed, 0 survived)`。
- **未实测**：`_start` 到 `main` 这条路径还没有真正运行过——没有程序使用 runtime，内核也还不发 processargs。第一次运行在 K14。
- 审查：三个方向（入口 ABI 与汇编、`runtime_start` 的每条路径、构建与文档）并行审查、逐条反驳核实。成立的四条都已在本提交处理：
  不等待的前提今天就不成立（init 先 spawn 后写）；退出码 -1 与 main 的 -1 撞车；BUILD.gn 注释漏了 userboot 要到 U8 才迁移；
  设计 K14 行对 init 那条日志的说法不准（它今天是 shell 主动关 bootstrap 时触发的，并不表示 shell 退出了）。

**故意没做**
- 执行 `.init_array`：记为 `docs/repairs/260926/runtime.md` 第 1 条。今天没有程序有它；做它需要先查清链接器提供边界符号的条件。
- `environ` 指针数组与 `getenv`：没有使用者；环境变量经 `runtime::process_args().environ()` 读。
- 未被认领的启动句柄保持打开，与 Zircon 一样；何时关掉只有程序自己知道。

## `ProcessArgs::option()`：按键名取环境串的值，最后一个为准（2026-09-26）

**文件**
- `system/lib/runtime/include/runtime/process_args.hpp`、`src/process_args.cpp` — 新增 `ProcessArgs::option(name)`
- `system/lib/runtime/test/process_args_test.cpp` — 新增 `Option.TakesTheLastWholeNameMatch`
- `kernel/lib/userabi/userboot/userboot.cpp` — 删掉 `find_next()`，改用 `option()`
- `system/service/init/init.cpp` — 删掉 `find_autorun()`，改用 `option()`

**症状**
userboot 的 `find_next()` 和 init 的 `find_autorun()` 是同一段代码：遍历 environ，找 `key=` 前缀，以最后一个为准。两份之间只靠注释互相指着。

**原因**
「cmdline 转来的选项以最后一个为准」是这套 environ 的约定，不是哪个程序自己的事。每个读选项的程序各写一遍，迟早会出现一个取第一个、另一个取最后一个的情况，镜像默认值和 bootloader 追加的值谁生效，就要看是哪个程序在读。这个约定应该只有一个主人。放在 runtime 里是因为它本来就拥有 processargs 的解码。

顺带修掉一个潜在的前缀误配：按 `starts_with("userboot.next")` 匹配（不带 `=`）会把 `userboot.next-is-test=` 也算进来。原来两处都把 `=` 写进前缀常量里，所以没出错，但这条规则是靠调用方记得才成立的。`option()` 按完整键名匹配，要求键名后面紧跟 `=`。

**改法**
改前（userboot 和 init 各一份）：
```cpp
auto find_next() -> std::string_view {
    std::string_view next;
    for (auto const entry: runtime::process_args().environ()) {
        if (entry.starts_with(NEXT_OPTION)) {           // NEXT_OPTION = "userboot.next="
            next = entry.substr(NEXT_OPTION.size());
        }
    }
    return next;
}
```
改后：
```cpp
auto ProcessArgs::option(std::string_view const name) const -> std::string_view {
    std::string_view value;
    for (auto const entry: m_environ) {
        if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=') {
            value = entry.substr(name.size() + 1);
        }
    }
    return value;
}
...
    auto const next = runtime::process_args().option(NEXT_OPTION);   // "userboot.next"
```
返回 `std::string_view`，而不是 `std::optional`：找不到、以及 `key=` 后面没有值，这两种情况对现有的两个读者含义相同（都是「没有指名程序」）。等哪天真有读者需要区分它们，再换成 `std::optional`。

取最后一个而不是像 `getenv()` 那样取第一个：这份 environ 是镜像 cmdline 的转发，cmdline 上后面的词覆盖前面的。

**参照**
- `Zircon userboot main.rs` 的 `parse_cmdline()`：遇到一个选项就赋值一次，所以最后一个生效。
- POSIX `getenv()`：取第一个。这里有意与它不同，理由见上。

**验证**
- 主机单测 `process_args` 16 个全部通过，新增的用例覆盖：最后一个生效、`userboot.next` 不会匹配到 `userboot.next-is-test`、不带 `=` 或值为空都读成空、键名前缀（`oth`）不会匹配到 `other`、找不到时返回空。
- clang-tidy 在改动的三个源文件上没有诊断。
- QEMU dev（cmdline 临时加 `ours.autorun.boot=nosuch ours.autorun.boot=fault_probe+none`）：两条都转到了 init，init 只运行了后一条（`init: autorun fault_probe+none`，然后 `fault_probe: unknown probe none`），shell 照常回答 pong。
- QEMU dbg_1：userboot 照常起 shell，shell 的各组检查全部通过。

**故意没做**
- 三个程序各自的上限（内核 8 条/384 字节、userboot 的 `MAX_HANDED_ON`、init 的 512 字节缓冲）收拢到一处：下一条。
