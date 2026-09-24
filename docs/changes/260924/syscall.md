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

## 分发表下标加 `confine_array_index`（Spectre V1）（2026-09-24）

**文件**
- `kernel/lib/syscall/src/table.cpp` — `syscall_dispatch()` 在钳位之后再夹一次下标

**症状**
潜伏。`%rax` 完全由用户决定，而钳位是一段 C++ 的 `if`。

**原因**
钳位把越界号改写成哨兵槽的下标，架构上是对的；但它是 C++，编译器可以把它编成条件跳转。
一旦编成跳转并被误预测，下面的 `SYSCALL_TABLE[number]` 会拿原始的 `%rax` 作下标，从表外
用户指定的位置取出一个"处理函数"再间接调用——哪怕只在推测路径上存在片刻，这也是一次跳到
用户所选地址的执行。

**改法**
改前：

```cpp
        auto number = syscall_abi::SYSCALL_COUNT;
        if (frame->ax < syscall_abi::SYSCALL_COUNT) {
            number = frame->ax;
        }
        SYSCALL_TABLE[number](frame);
```

改后：

```cpp
        auto number = syscall_abi::SYSCALL_COUNT;
        if (frame->ax < syscall_abi::SYSCALL_COUNT) {
            number = frame->ax;
        }
        number = arch::confine_array_index(number, TABLE_SIZE);
        SYSCALL_TABLE[number](frame);
```

钳位保留（架构上的语义：越界号落到哨兵槽，回 `BadSyscall`）；`confine_array_index` 只管推测
路径，把任何表外下标变成 0，最坏是推测地开始执行槽 0 的 shim——一个合法的目标。架构上它什么
也不改变，因为走到这里时 `number` 已在范围内。

**参照**
Zircon `kernel/arch/x86/syscall.S`：`cmp $ZX_SYS_COUNT, %rax; jae .Lunknown_syscall` 之后紧跟
`cmovge %r11, %rax`（%r11 为 0），注释说明分支之后只可能在错误路径上越界，而这一步必须是 cmov。

**验证**
- 构建通过；两个镜像启动正常。
- `syscall_dispatch` 反汇编：钳位在这次构建里恰好编成了 `cmovbq`，新加的一道是 `cmovaq`，
  之后才是 `callq *(%rcx,%rax,8)`。钳位编成什么由编译器决定，第二道不依赖它。

**故意没做**
Spectre V2（间接调用的目标注入）不在这一条：Zircon 在这里用 retpoline，本项目还没有这套设施。

## `//kernel/lib/syscall:headers` 只对本库可见，不再转出私有调用号头（2026-09-24）

**文件**
- `kernel/lib/syscall/BUILD.gn` — `:headers` 的 `visibility` 从 `["*"]` 收窄为 `[":*"]`；`source_set("syscall")` 对 `:headers` 的依赖从 `public_deps` 改为 `deps`

**症状**
审查发现：第 4 条给 `//system/public:syscalls.private` 设了 `visibility`，只放行 `//kernel/lib/syscall/*`
和 vDSO，说这是"让调用号保持私有"。但第 6 条让 `:headers`（`visibility = ["*"]`）经 `public_deps`
带上了 `syscalls.private`，而 `source_set("syscall")` 又经 `public_deps` 带上 `:headers`——于是任何依赖
`//kernel/lib/syscall` 的目标（今天是 `kernel/top`）都拿到了私有头的 include 路径。

**原因**
GN 的 `visibility` 只管"谁可以直接依赖我"，挡不住经 `public_deps` 转出去的 config。私有性要在
每一层转发处守住，而不只是在源头声明。

**改法**
改前：

```gn
cpp_interface("headers") {
  public = [ "include/syscall/handlers.hpp" ]
  public_deps = [
    "//kernel/lib/user_copy:headers",
    "//system/public:syscalls.private",
  ]
  visibility = [ "*" ]
}

source_set("syscall") {
  ...
  public_deps = [
    ":headers",
    "//kernel/lib/user_copy",
    "//kernel/object:headers",
  ]
```

改后：`:headers` 的 `visibility = [ ":*" ]`；`source_set("syscall")` 把 `":headers"` 挪进 `deps`。
`handlers.hpp` 只被本库的源文件包含；处理函数经分发表到达，库外没有人按名字调用它们。

**参照**
无对应物（GN 的可见性规则）。

**验证**
- 构建通过；重新生成的 ninja 文件里，带私有路径 `gen/system/public/private` 的只剩
  `kernel/lib/syscall` 自己的 `headers.ninja` 与 `syscall.ninja`（`kernel/top` 不再有）。
- 两个镜像启动正常。

**故意没做**
无。

## 勘误：P1-A 第 1–16 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 第一节**参照**里的 "Zircon 的 kazoo（`zircon/tools/kazoo`）"：本地 Zircon 树里没有这个目录。现在的生成器是
  zither（`zircon/tools/zither`），它的 kernel 后端生成 `lib/syscalls/zx-syscall-numbers.h` 与
  `lib/syscalls/kernel-wrappers.inc`，`legacy_syscall_cdecl` 后端生成 `cdecls.inc`。FIDL 源 `//zircon/vdso/*.fidl` 的路径是对的。
  文中其他写"kazoo 生成"的地方同样应读作 zither。
- 第一节**改法**写"默认工具链本来就只描述 action 和元数据图、不编译任何东西"。错：默认工具链（`kernel-x86`）
  有 `cxx` 规则，也编译一些宿主侧工具和测试（prof、unwind、obit、ustl 的测试）。把生成器放在那里仍然成立，
  理由只是"它是一个固定的工具链，动作只跑一次"；`system/public/BUILD.gn` 的注释已改正。
- 第一节说 `syscalls.private` 的 `visibility` 让调用号保持私有。不完全：`visibility` 只管直接依赖者，第 6 条让
  `//kernel/lib/syscall:headers`（对所有人可见）把它转了出去。已由「`//kernel/lib/syscall:headers` 只对本库可见」一节改正。

## vDSO 校验要求整条 `syscall` 指令都在 vDSO 里（2026-09-24）

**文件**
- `kernel/object/include/ours/object/process_dispatcher.hpp` — `vdso_contains(pc)` 改为 `vdso_contains(addr, len)`，判断一个区间
- `kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp` — `arch::SYSCALL_INSN_SIZE = 2`
- `kernel/lib/syscall/src/table.cpp` — `trapped_from_vdso()` 传 `(frame->ip - 2, 2)`
- `docs/repairs/260923/x86-kernel.md` — 删掉第 2 条

**症状**
潜伏（加固项）。原来比较的是返回地址 `frame->ip` 是否落在 `[base, base + span)`。返回地址是 `syscall`
**之后**的那个字节，于是一条恰好位于 vDSO 下方最后两个字节、返回地址等于 vDSO 第一个字节的 `syscall`
也会被接受。程序映像就映射在 vDSO 下方；今天还没有能映射内存的系统调用，所以够不到，`vmar_map` 之后就能够到。

**原因**
被问的是"这条指令是谁的"，不是"返回地址在哪"。一条指令占 `[ip - 2, ip)`，它的全部字节都得属于 vDSO。

**改法**
改前：

```cpp
    auto vdso_contains(VirtAddr pc) const -> bool {
        return pc - m_vdso_code_base < m_vdso_code_size;
    }
    ...
        return process->vdso_contains(frame->ip);
```

改后：

```cpp
    auto vdso_contains(VirtAddr addr, usize len) const -> bool {
        return len <= m_vdso_code_size && addr - m_vdso_code_base <= m_vdso_code_size - len;
    }
    ...
        return process->vdso_contains(frame->ip - arch::SYSCALL_INSN_SIZE,
                                      arch::SYSCALL_INSN_SIZE);
```

等价于 `base + 2 <= ip <= base + span`。计划里比较过两种看似更简单的写法：`base < ip` 会放过
`ip = base + 1`；"`ip - 2` 落在 `[base, base + span)`" 会放过 `ip = base + span + 1`。区间写法两端都对，
而且不会回绕：`addr` 低于 base 时减法回绕成一个巨大的偏移，第二个比较拒绝它；没有 vDSO 的进程
（大小为 0）对任何长度为 2 的区间都答否。

**参照**
Zircon `VDso::ValidSyscallPC()`：精确比较返回地址是否是某个批准的 `SYSRET` 标签。这里的区间检查是它的
一个真子集；精确白名单要等 `OURS_VDSO_SYSRET_*` 的地址能传给内核（生成器产物 ⑦，P1 后期）。

**验证**
- 构建通过；两个镜像启动正常——所有真正的系统调用仍被接受。
- 用宿主 Python 按同样的算术核对边界（base = 0xa00000，span = 0x2000）：`ip = base+1` 拒、`base+2` 收、
  `base+span` 收、`base+span+1` 拒、`ip = 0`/`1`（回绕）拒。

**故意没做**
精确的 SYSRET 白名单（见参照）。

## 从 vDSO 之外陷入：杀掉进程，而不只是拒绝（2026-09-24）

**文件**
- `kernel/include/ours/task/exception.hpp` — `PolicyCode`（`BadSyscall = 18`，同 Zircon）
- `kernel/object/thread_dispatcher.cpp`、`.hpp` — `on_policy_exception()`：构造合成的 PolicyError 上下文，交给 `on_user_exception()`；报告行加上 `synth_code`、`synth_data`
- `kernel/lib/syscall/src/table.cpp` — `sys_not_from_vdso()` 调 `on_policy_exception()`；没有 ThreadDispatcher 时 ASSERT

**症状**
绕开 vDSO 自己发 `syscall` 的程序只拿到一个 `BadSyscall`，可以忽略它继续跑。"只能经 vDSO 进内核"
这条规则被违反的代价只是一个错误码。

**原因**
伪造陷入的程序，它的下一条指令不值得信任——它已经证明自己在绕开规则。原先只拒绝，是因为那时还没有
异常机制；第 26 条之后有了。

**改法**
改前：

```cpp
    auto sys_not_from_vdso(arch::SyscallFrame *const frame) -> void {
        log::warn("syscall: refused a trap from {:X}, which is outside the vDSO", frame->ip);
        frame->ax = static_cast<usize>(Status::BadSyscall);
    }
```

改后：

```cpp
    auto sys_not_from_vdso(arch::SyscallFrame *const frame) -> void {
        log::warn("syscall: refused a trap from {:X}, which is outside the vDSO", frame->ip);

        auto *const thread = object::ThreadDispatcher::current();
        ASSERT(thread, "a trap from a kernel thread at {:#x}", frame->ip);
        thread->on_policy_exception(task::PolicyCode::BadSyscall, frame->ax, frame->ip, frame->sp);

        frame->ax = static_cast<usize>(Status::BadSyscall);
    }
```

`on_policy_exception()` 在对象层里造一个 `ExceptionContext{PolicyError, "BadSyscall", synth_code = 18,
synth_data = 调用号, frame = nullptr}`，走与 CPU 异常相同的 `on_user_exception()`：报告、杀进程。
lib/syscall 因此仍只 include 对象层的头。线程在返回路上的挂起信号检查处退出（第 21 条），不会 SYSRET 回 ring 3。
`frame = nullptr`：陷入发生在系统调用里，没有异常帧；系统调用帧里的寄存器是程序的参数，不是故障时的状态——
报告打 "no frame to dump"。

**参照**
Zircon `sys_invalid_syscall()`（`kernel/lib/syscalls/syscalls.cc`）：`Thread::Current::SignalPolicyException(ZX_EXCP_POLICY_CODE_BAD_SYSCALL, num)`
后返回 `ZX_ERR_BAD_SYSCALL`；`ZX_EXCP_POLICY_CODE_BAD_SYSCALL` = 18。

**验证**
- 构建通过；两个镜像正常启动。
- 临时在 shell 的 `--- end ---` 之后放一条裸 `syscall`（`movl $0, %eax; syscall`），后面跟一行
  "SURVIVED"（未提交）。两个镜像都依次出现：

  ```
  syscall: refused a trap from 202AB4, which is outside the vDSO
  terminating process 'shell' (koid 23) thread koid 27: type=PolicyError vector=BadSyscall error_code=0x0 rip=0x202ab4 sp=0x11fff40 cr2=0x0 synth_code=18 synth_data=0x0
    no frame to dump: raised from inside a system call
  object: thread (koid 27) of process 'shell' destroyed
  object: process 'shell' (koid 23) destroyed
  ```

  没有 "SURVIVED"——线程没有回到 ring 3；内核继续运行。

**故意没做**
常驻的探针测试（第 30 条）连同第 27 条一起，等 `sync::Mutex` 的丢失唤醒修好再提交（`docs/repairs/260924/sync.md`）。

## lib/syscall 回到只 include 对象层的头：`on_bad_syscall()`（2026-09-24）

**文件**
- `kernel/object/include/ours/object/thread_dispatcher.hpp`、`thread_dispatcher.cpp` — 新增公开的 `on_bad_syscall(number, ip, sp)`；`on_policy_exception()` 挪进私有部分
- `kernel/lib/syscall/src/table.cpp` — 调 `on_bad_syscall()`，删掉 `#include <ours/task/exception.hpp>`

**症状**
审查发现：第 29 条的记录写"lib/syscall 因此仍只 include 对象层的头"，但 `table.cpp` 为了写出
`task::PolicyCode::BadSyscall` 而 include 了 task 层的 `<ours/task/exception.hpp>`——违反了设计 4.7 给 lib/syscall
定的依赖白名单（`kernel/object`、`user_copy`、arch 的 syscall 帧头）。

**原因**
策略码是 task 层的类型，让系统调用层去拼它，就是让系统调用层知道了对象层下面的东西。系统调用层只需要说
"这个线程绕开了 vDSO"，怎么把它变成一个异常是对象层的事。

**改法**
改前：

```cpp
#include <ours/task/exception.hpp>
...
        thread->on_policy_exception(task::PolicyCode::BadSyscall, frame->ax, frame->ip, frame->sp);
```

改后：

```cpp
        thread->on_bad_syscall(frame->ax, frame->ip, frame->sp);
```

```cpp
auto ThreadDispatcher::on_bad_syscall(u64 const number, VirtAddr const ip, VirtAddr const sp)
    -> void {
    on_policy_exception(task::PolicyCode::BadSyscall, number, ip, sp);
}
```

**参照**
Zircon `sys_invalid_syscall()` 直接调 `Thread::Current::SignalPolicyException(ZX_EXCP_POLICY_CODE_BAD_SYSCALL, num)`；
这里是同一个调用，往上挪了一层。

**验证**
构建通过；`grep task/ kernel/lib/syscall` 为空；两个镜像启动正常；临时的裸 `syscall` 探针（未提交）仍然得到
`type=PolicyError vector=BadSyscall ... synth_code=18` 与两行 destroyed。

**故意没做**
无。

## 勘误：P1-A 第 17–29 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「从 vDSO 之外陷入：杀掉进程」一节写"lib/syscall 因此仍只 include 对象层的头"。当时的 `table.cpp` 其实 include 了
  `<ours/task/exception.hpp>`。已由「lib/syscall 回到只 include 对象层的头」一节改正。
