# syscall 改动记录（2026-09-24）

## 系统调用有了唯一的描述：`syscalls.gni` 与生成的私有调用号头（2026-09-24）

**文件**
- `system/public/ours/syscalls.gni`（新建）— 8 个调用的描述，条目位置即调用号
- `build/syscall/gen_syscalls.py`（新建）— 读描述的 JSON，全部校验，写出 `syscall_abi/numbers.hpp`
- `system/public/BUILD.gn` — 默认工具链里的 `generated_file` + `action`，以及各工具链共用的 `group("syscalls.private")`
- `kernel/lib/syscall/BUILD.gn` — 依赖 `syscalls.private`
- `kernel/lib/syscall/src/table.cpp` — 生成的号与手写的 `OURS_SYS_*` 逐个 `static_assert`

**症状**
没有运行时症状，是结构问题：一个调用的形状写在四处——`syscall.hpp` 的调用号宏、
`vdso.S` 的桩、`vdso.hpp` 的 extern "C" 声明、`handlers.hpp` 的 `sys_*` 声明加
`table.cpp` 的 shim——彼此之间没有任何编译期关联。改错一处，结果是一个调用的参数被送进
另一个调用的处理函数，或者寄存器错位，而构建照常通过。

**原因**
这是 P0 为了先跑通而手写的代价（设计 4.7 当时就写明"P0 的 6 个桩手写，生成器是 P1-A 的
第一件事"）。四处手工同步能维持多久取决于改的人记不记得全部四处；每加一个调用，漏改的
机会就多一次。Zircon 用 kazoo 从 FIDL 生成全部这些东西，出发点相同。

**改法**
一个描述文件，一个生成器，本条先只接出第一样产物（调用号头），其余在第 5–10 条逐个接上。

描述是 GN scope 列表而不是另起一种格式：GN 是这棵树已有的唯一解析器，`generated_file`
加 `output_conversion = "json"` 把它变成 JSON，生成器只用标准库读 JSON（宿主 python 是
3.9，没有 tomllib）。条目示例：

```gn
  {
    name = "channel_create"
    brief = "Makes a channel and writes a handle to each end into |out0| and |out1|."
    doc = [ ... ]
    args = [
      { name = "options"  type = "ChannelCreateOptions" },
      { name = "out0"     type = "HandleValue"  dir = "out" },
      { name = "out1"     type = "HandleValue"  dir = "out" },
    ]
    attrs = []
    ret = "Status"
  },
```

生成器在每次运行时做全部校验，不论要求写哪些产物——坏条目应该被第一个看到它的构建拒绝，
而不是等到某个产物恰好绊倒：参数超过 8 个；恰好 7 个（vDSO 还没有 7 参数桩，第一个需要的
是 P1-C 的 `vmar_map`）；调用重名或参数重名；`count` 指向不存在的参数或指向一个指针
（长度若经指针读，读和拷贝之间可以被改）；未知的 attr、dir、键；`release` 用在按值传的
`HandleValue` 以外。

产物只在默认工具链里生成一次。它不依赖工具链，而默认工具链本来就只描述 action 和元数据图、
不编译任何东西。其他工具链通过 `group("syscalls.private")` 拿到：`public_deps` 跨工具链指向
生成动作，`public_configs` 给出生成目录的 include 路径。这个 group 的 `visibility` 只放行
`//kernel/lib/syscall/*` 和 `//kernel/lib/userabi/vdso/*`——调用号是内核与 vDSO 之间的私有
约定（D8），用户程序依赖它就是依赖了 vDSO 存在的意义所要隐藏的那样东西。

生成器只在内容变化时重写输出，配合 GN 给 action 的 `restat = 1`，描述里只改了注释不会
触发重编。没有写 depfile：生成器只读 GN 已知的那一个 JSON，没有隐藏输入。

生成的头：

```cpp
namespace ours::syscall_abi {
inline constexpr auto SYSCALL_COUNT = usize{8};

inline constexpr auto SYS_DEBUG_WRITE = usize{0};
...
inline constexpr auto SYS_PROCESS_SPAWN_BOOT = usize{7};

inline constexpr std::string_view SYSCALL_NAMES[SYSCALL_COUNT] = { "debug_write", ... };
}
```

`table.cpp` 在过渡期逐个断言新旧两套号相等：

```cpp
static_assert(syscall_abi::SYS_DEBUG_WRITE == OURS_SYS_DEBUG_WRITE);
...
```

下一步（第 6 条）表由生成的 shim 填，这些断言随手写的宏一起删除。

**参照**
Zircon 的 kazoo（`zircon/tools/kazoo`）从 `//zircon/vdso/*.fidl` 生成 `zx-syscall-numbers.h`、
vDSO 桩、内核 wrapper 与分发；设计 4.7。

**验证**
- 增量构建、干净构建（新建 `out/clean.x86`，复制 args.gn，全部目标）都通过。
- 干净构建的 ninja 图里，`table.cpp` 的编译对 `phony/system/public/syscalls.gen` 有 order-only
  依赖，生成头一定先于使用者。
- 本地对调描述里前两条的名字（未提交）：
  `error: static assertion failed due to requirement 'syscall_abi::SYS_DEBUG_WRITE == 0'`。
- 逐条喂坏输入给生成器，全部以非零退出并说明原因：9 个参数、7 个参数、调用重名、
  悬空 count、未知 attr、u32 上的 release、out 指针上的 release、未知 dir。
- 两个镜像启动正常，init↔shell 对话照常。

**故意没做**
- 生成器暂不校验 `type` 是否真有其类型：类型名写错会在编译生成的声明时报错（第 6、9 条），
  比在生成器里维护一份类型表更不容易与代码脱节。
- 描述里的 `doc` 这一步还没有被任何产物用到；第 9 条生成 `vdso.hpp` 的声明时用。

## `sys_*` 声明与拆帧 shim 改由生成器产出（2026-09-24）

**文件**
- `build/syscall/gen_syscalls.py` — 新增 `--kernel-decls`、`--kernel-shims`；校验"ret 为 void 当且仅当 noreturn"
- `system/public/BUILD.gn` — 生成动作多两个输出
- `kernel/lib/syscall/include/syscall/handlers.hpp` — 手写的 8 个声明换成 `#include <syscall_abi/kernel_decls.inc>`
- `kernel/lib/syscall/src/argument.hpp`（新建）— `syscall::from_register<T>()`，寄存器到参数类型的唯一转换
- `kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp` — `arch::syscall_arg<N>(frame)`，参数寄存器序
- `kernel/lib/syscall/src/table.cpp` — 删掉手写的 8 个 shim，改 include 生成的 `kernel_shims.inc`，表从 `GENERATED_SHIMS` 填；删掉上一条的过渡 `static_assert`
- `kernel/lib/syscall/BUILD.gn` — `:headers` 依赖 `syscalls.private`
- `docs/repairs/260923/syscall.md` — 删除清单里"`table.cpp` 的表项"改为生成物

**症状**
没有运行时症状。一个调用的签名原来写三遍：`handlers.hpp` 的声明、各自 `.cpp` 的定义、
`table.cpp` 里 shim 的拆帧调用。声明和定义之间有编译器把关，但 shim 里"第几个参数从哪个
寄存器取、截成什么类型"全凭手写，写错一个寄存器就是把一个参数当成另一个，编译照过。

**原因**
shim 做的事完全是机械的：按参数序取寄存器、按参数类型转换、把结果写回 `ax`。机械的东西
手写，出错的方式只有"抄错"，而抄错恰恰是编译器看不出来的那一种。

**改法**
改前（`table.cpp`，每个调用一个）：

```cpp
    auto sys_channel_write_shim(arch::SyscallFrame *const frame) -> void {
        auto const status = sys_channel_write(
            static_cast<HandleValue>(frame->di),
            static_cast<ChannelWriteOptions>(static_cast<u32>(frame->si)),
            user_copy::UserInPtr<u8 const>(frame->dx), static_cast<u32>(frame->r10),
            user_copy::UserInPtr<HandleValue const>(frame->r8), static_cast<u32>(frame->r9));
        frame->ax = static_cast<usize>(status);
    }
```

改后（生成，`kernel_shims.inc`）：

```cpp
auto sys_channel_write_shim(arch::SyscallFrame *const frame) -> void {
    auto const result = sys_channel_write(
        syscall::from_register<HandleValue>(arch::syscall_arg<0>(frame)),
        syscall::from_register<ChannelWriteOptions>(arch::syscall_arg<1>(frame)),
        syscall::from_register<user_copy::UserInPtr<u8 const>>(arch::syscall_arg<2>(frame)),
        ...);
    frame->ax = static_cast<usize>(result);
}
...
constexpr SyscallHandler GENERATED_SHIMS[] = { &sys_debug_write_shim, ... };
static_assert(std::size(GENERATED_SHIMS) == syscall_abi::SYSCALL_COUNT);
```

原来散在各 shim 注释里的规则，各自归位到一个地方：

- **寄存器序**进 arch 头的 `arch::syscall_arg<N>()`：rdi、rsi、rdx、r10、r8、r9、r12、r13，
  以及 rcx/r11 为什么永远不能是参数（SYSCALL 进内核前已经用返回地址和 RFLAGS 覆盖了它们）。
  这是架构的事，放在架构的头里。
- **类型转换**进 `argument.hpp` 的 `syscall::from_register<T>()`：指针在这里、也只在这里变成
  `UserPtr`；枚举先截成底层类型再转换；整数截到自身宽度——32 位的句柄、计数、选项字落在
  64 位寄存器的低半，SysV 不保证高半为零，所以截断而不是信任。

声明从描述生成，定义仍然手写，这个分工本身就是检查：定义的签名与描述不符时，它是另一个
没人调用的重载，而生成的 shim 调用的那个声明找不到定义，链接失败。

填表从逐个 `table[OURS_SYS_X] = &..._shim` 变成一个循环：

```cpp
        for (auto number = usize(0); number < syscall_abi::SYSCALL_COUNT; ++number) {
            table[number] = GENERATED_SHIMS[number];
        }
```

`GENERATED_SHIMS` 的下标与调用号出自同一处——条目在 `syscalls.gni` 里的位置。

同时删掉了 `table.cpp` 里一段早已不实的 TODO（"只填了 debug_write，其余等句柄表"）：
句柄表 P0 就有了，八个调用全都填上了。

生成器新增一条校验：`ret` 为 `void` 当且仅当调用带 `noreturn`。结果寄存器是回来的唯一途径，
一个会返回却什么都不返回的调用，没有告诉调用者任何可以检查的东西。

**参照**
- Zircon kazoo 生成的 `wrapper_<name>`（`lib/syscalls/kernel-wrappers.inc`，由 `kernel/lib/syscalls/syscalls.cc` include），
  按 `SafeSyscallArgument<T>::Sanitize()` 逐个清洗参数后调 `sys_<name>`（`kernel/lib/syscalls/include/lib/syscalls/safe-syscall-argument.h`）。
- 设计 4.7 产物 4、5。

**验证**
- 增量构建与干净构建通过；两个镜像启动正常，init↔shell 对话与 shell 的通道检查照常。
- 本地把 `sys_handle_close` 的定义改成 `u16` 形参（未提交）：
  `ld.lld: error: undefined hidden symbol: ours::sys_handle_close(unsigned int)`。
  （先试的 `u64` 在函数体里触发了 `-Wshorten-64-to-32`，没走到链接，所以换了一个能隐式转换的类型。）

**故意没做**
- `sys_not_implemented` 和表长 `SYSCALL_COUNT + 1 = 257` 保留：表长改用生成的计数在第 8 条，
  这一条只换 shim 的来源，行为不变。
- `SyscallHandler`、`sys_bad_syscall_number`、`sys_not_from_vdso` 仍是手写：它们不对应描述里的任何调用。

## 分发表按生成的调用数定长；删掉 `sys_not_implemented`（2026-09-24）

**文件**
- `kernel/lib/syscall/src/table.cpp` — `TABLE_SIZE = syscall_abi::SYSCALL_COUNT + 1`；删掉 `sys_not_implemented` 与上一条的 `DISPATCH_SLOTS`；加"无空槽"的编译期检查

**症状**
行为变化：8..255 号原来回 `Unimplemented`，现在回 `BadSyscall`。今天没有合法程序能发出
这些号——vDSO 的桩只用 0..7，而绕开 vDSO 的陷入在查表之前就被拒——所以这个变化目前观察不到。

**原因**
256 个槽是调用号还在公共头里时定下的：号是公开的，就得给将来的调用预留一段，预留段里的号
回答"这个调用存在，只是还没实现"（`Unimplemented`）。号私有之后没有什么需要预留：加一个调用
就是描述里加一条，vDSO 和内核一起重新生成。于是 8..255 不再是"没实现的调用"，它们根本不是调用，
正确的回答是 `BadSyscall`。

`Unimplemented` 与 `BadSyscall` 的区别对调用者是有意义的：前者说"你问的东西存在，换个版本
也许就有"，后者说"你问的不是一个系统调用"。对一个从不存在的号回前者，是在撒谎。

**改法**
改前：

```cpp
    auto sys_not_implemented(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::Unimplemented);
    }
    ...
    constexpr auto DISPATCH_SLOTS = usize{256};
    constexpr auto TABLE_SIZE = DISPATCH_SLOTS + 1;

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, TABLE_SIZE> table{};
        for (auto &entry: table) {
            entry = &sys_not_implemented;
        }
        table[DISPATCH_SLOTS] = &sys_bad_syscall_number;
        ...
```

改后：

```cpp
    constexpr auto TABLE_SIZE = syscall_abi::SYSCALL_COUNT + 1;

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, TABLE_SIZE> table{};
        table[syscall_abi::SYSCALL_COUNT] = &sys_bad_syscall_number;
        ...
```

表不再先整体填一个默认处理函数，所以漏填的槽会是空指针——而 `%rax` 是用户给的，空槽就是
一次按用户意愿发起的"经零地址的间接调用"。为此加了编译期检查：

```cpp
static_assert(
    [] {
        for (auto const entry: make_syscall_table()) {
            if (entry == nullptr) {
                return false;
            }
        }
        return true;
    }(),
    "every SYSCALL_TABLE slot must have a handler");
```

`sys_bad_syscall_number` 的注释同时改正：钳位早已从桩里的 `cmov` 搬进 `syscall_dispatch()`。

**参照**
Zircon：表长是 kazoo 生成的 `ZX_SYS_COUNT`，越界号走 `unknown_syscall`，回 `ZX_ERR_BAD_SYSCALL`。

**验证**
- 构建通过；两个镜像启动正常。
- `llvm-nm -S`：`SYSCALL_TABLE` 由 257 × 8 字节变为 `0x48` = 9 × 8 字节。
- 本地删掉哨兵槽的赋值（未提交）：`static assertion failed ...: every SYSCALL_TABLE slot must have a handler`。

**故意没做**
钳位仍是 C++ 的 `if`，可能被编译成条件跳转，预测错时会越界读表再间接调用（Spectre V1）。
它归第 13 条，用 `confine_array_index` 修。
