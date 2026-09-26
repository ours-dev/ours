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
