# build — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 属性宏改为单独占一行：`.clang-format` 补 `Macros:`，并按新规则重排 127 个文件（2026-09-04）

**文件**
- `.clang-format` — 新增 `Macros:` 段；`AttributeMacros` 补上遗漏的 `INIT_CONST`
- 127 个 `.cpp`/`.hpp` — 按新规则重排（纯格式，无语义改动）

**症状**

`.clang-format` 里写着 `BreakAfterAttributes: Always`，可是 `INIT_CODE`、`FORCE_INLINE`
这类属性宏始终和声明留在同一行，看上去像配置没生效。

同一份配置、同一个文件，两种写法的结果并排（clang-format 23.1.0-rc3，即 `prebuilt` 里那个）：

```
[[gnu::section(".init.text")]]
static auto b() -> void {}         ← 真属性，断了

INIT_CODE static auto a() -> void {}    ← 属性宏，没断
FORCE_INLINE auto d() -> void {}        ← 同上
```

最能说明问题的是第三个例子：`[[nodiscard]] INIT_CODE static auto c() -> int` 里，
`[[nodiscard]]` 断到了自己一行，紧跟着的 `INIT_CODE` 留在了声明那一行。
同一处声明、同一道规则，一个断一个不断。

**原因**

配置一直是生效的，`AttributeMacros` 的职责比它的名字窄：它只让 clang-format 别把这些宏
**误读**成返回类型或独立声明——原来那段注释写的就是这件事，没有写错，只是不完整。
而 `BreakAfterAttributes` 只认真正的 `[[...]]` 记号。clang-format 看到 `INIT_CODE` 时
拿到的是一个不透明的标识符，它不知道这个名字最终会展开成
`INIT_SECTION("text")` → `LINK_SECTION(".init.text")` → `[[gnu::section(".init.text")]]`。
两个选项各管各的一半，中间那一段——"这个标识符其实是个属性"——在配置里没有地方可说。

`Macros:` 就是那个地方：告诉 clang-format 一个宏展开成什么，它会按展开后的形状排版，
再把结果映射回宏的写法。

**改法**

`AttributeMacros` 里除 `PACKED` 外的每一项，都在 `Macros:` 里补一条展开式。
另外把 `INIT_CONST` 补进 `AttributeMacros`——它和 `INIT_CODE`/`INIT_DATA` 是同一族，
之前漏了。

三处需要写在配置注释里、也值得在这里说清楚：

1. **展开式是给排版看的虚构，不是给预处理器看的。** 只要形状是个属性就够了：
   `INIT_CODE` 真实的链条要经过 `INIT_SECTION()` 和 `LINK_SECTION()`，
   `KERNEL_API` 用的是 `__attribute__((...))` 而不是 `[[...]]`，两者都照写成
   `[[...]]` 也一样工作。代价是这份清单和 `system/public/ours/compiler.hpp`
   之间有一处需要人记着的耦合：那些宏哪天不再是属性了，这里不会自动跟着变。

2. **`PACKED` 故意留在原行。** 它读起来是所修饰类型的一部分（`PACKED struct S`），
   单独一行会把它和它修饰的东西分开。

3. **`FORCE_INLINE_ATTR` 和 `NO_RETURN` 排除在外，因为 clang-format 会死循环。**
   触发条件是属性宏出现在 lambda 位置：

   ```cpp
   [] NO_RETURN() -> void { ... }        // clang-format 23.1.0-rc3 挂死，不返回
   [] [[noreturn]] () -> void { ... }    // 字面属性不挂
   ```

   所以挂死的是宏展开这条路径，不是 lambda 本身。`FORCE_INLINE_ATTR` 存在的**唯一**理由
   就是 lambda 位置（见它定义处的注释），排除它零成本；`NO_RETURN` 全树 13 处，
   只有一处在 lambda 上（`kernel/kernel/task/idle_thread.cpp:34`），
   也就是说它离进这份清单只差改一行源码——**没有改**，因为不该让排版工具的缺陷
   反过来指挥它所排版的代码。等 clang-format 修了，或者那个 lambda 因为别的原因没了，
   再把它加进来。

**重排的范围是算出来的，不是扫全树。** 对每个受版本控制的 C++ 文件，分别用旧配置和新配置
格式化一遍：两者输出相同就跳过；不同，再看文件当前内容是否已经等于旧配置的输出——
是，说明它此前是干净的，重排的差异可以完全归因于这次配置改动，于是应用；
否，说明它本来就有格式漂移，重排会把两件事混在一起，跳过并列出来。

结果：受影响且此前干净的 137 个，其中 10 个正被其它会话改着（这个 worktree 有并发会话），
一并跳过，实改 **127** 个。

留给后来的两批：

- **格式本就漂移、这次没动**（5 个）：`kernel/include/ours/task/owned_wait_queue.hpp`、
  `kernel/lib/eftl/include/eftl/elf/load.hpp`、
  `kernel/lib/eftl/include/eftl/elf/relocations.hpp`、`kernel/mm/cpu_local.cpp`、
  `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp`。
- **被其它会话占着、这次没动**（10 个）：`kernel/arch/x86/kernel/time/hpet.cpp`、
  `kernel/include/ours/task/sched/scheduler.hpp`、`kernel/kernel/task/sched/scheduler.cpp`、
  `kernel/lib/arch/include/arch/paging/visitors.hpp`、`kernel/mm/object_cache.cpp`、
  `kernel/mm/pm_node.cpp`、`kernel/mm/vm_cow_pages.cpp`、`kernel/mm/vm_mapping.cpp`、
  `kernel/mm/vm_object_paged.cpp`、`kernel/phys/memory.cpp`。

这两批都在各自的持有者提交之后跑一次 `clang-format -i` 就会归位。

**参照**

无，本项目自有配置。`Macros:` 是 clang-format 自 17 起提供的选项，用途正是"让格式化器
看穿一个宏"；官方文档把它归在实验性一档，本次在 23.1.0-rc3 上按上述范围实测可用，
唯一的坑就是上面第 3 条那个死循环。

**验证**

两个镜像构建通过。QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到
线程调度，`kernel.dev.obi` 到 `prof: no architectural PMU`——与重排前逐行一致。

纯格式改动本不该影响行为，但属性的位置**是**会影响语义的（属性贴在哪个声明上决定它作用于谁），
所以这一轮启动是必要的确认而不是走过场。

**故意没做**

- 没有扫全树。见上面「重排的范围是算出来的」：格式已经漂移的文件重排会混进无关差异。
- 没有把 `MEM_INIT_CODE`/`MEM_INIT_DATA`/`MEM_INIT_CONST` 加进任何一份清单。
  它们在 `CONFIG_MEM_HOTPLUG` 下展开成**空**，声称它们是属性在那个配置里是假话；
  眼下它们在源码里已经是手写单独一行的，不改也不难看。
- 没有为了让 `NO_RETURN` 进清单而去改 `idle_thread.cpp` 那个 lambda。见上面第 3 条。

---

## `NO_RETURN` 删掉，直接写 `[[noreturn]]`（2026-09-04）

**文件**
- `system/public/ours/compiler.hpp` — 删掉 `#define NO_RETURN`
- `.clang-format` — `AttributeMacros` 去掉 `NO_RETURN`；随之改写 `Macros:` 的排除说明
- 11 个 `.cpp`/`.hpp` — 13 处 `NO_RETURN` 改写成 `[[noreturn]]`

**症状**

不是缺陷，是一条规则：**语言语法层面已经有的东西不该再用宏包一层**，
只有 `[[gnu::]]`、`[[msvc::]]` 这类厂商定制属性才需要宏，因为它们要么不可移植、
要么需要按编译器分支。

`NO_RETURN` 展开就是 `[[noreturn]]`——一个 C++11 起就在标准里的属性，无条件、无分支、
无可移植性问题。这一层包装什么也没换来：读代码的人多一次跳转才能确认它就是标准属性，
而 grep `[[noreturn]]` 找不到这 13 处。

全树扫下来，包装标准属性的宏只有两个：`NO_RETURN` 和 `NO_UNIQUE_ADDRESS`
（另有 `NO_MANGLE` 包着 `extern "C"`，那是语言语法而非属性，同一条规则适用）。
本条只处理 `NO_RETURN`，另外两条各自一条。

**原因**

除了"多一层无谓的间接"，宏包装标准属性还有两处实际代价，这次都碰上了：

1. **工具链看不穿它。** 上一条改动（属性宏单独占一行）为此专门加了 `Macros:` 段，
   把每个属性宏的展开式再告诉 clang-format 一遍——`NO_RETURN` 那一行现在不需要了，
   因为 `[[noreturn]]` 本来就是 clang-format 认识的记号。同理，任何按属性做检索或
   分析的工具（clang-tidy 的检查、IDE 的跳转）对标准拼法是开箱即用的，对宏则不是。

2. **它把 clang-format 的一个死循环带了进来。** 上一条改动里 `NO_RETURN` 被迫排除在
   `Macros:` 之外，因为属性宏出现在 lambda 位置会让 clang-format 23.1.0-rc3 挂死
   （`[] NO_RETURN() -> void { ... }` 不返回，而字面 `[] [[noreturn]] ()` 正常）。
   那条排除说明里写着"它离进清单只差改一行源码，但不该让排版工具的缺陷指挥源码"——
   这条规则把那个两难直接取消了：不是为了迁就工具去改源码，而是源码本来就该这么写，
   工具的问题顺带消失。

**改法**

13 处逐字替换，宏定义删除，`.clang-format` 两处跟着改。替换是纯文本的，
因为 `NO_RETURN` 和 `[[noreturn]]` 在每一个出现位置都是同一个记号。

多属性并列的写法照旧可读：

```cpp
NO_MANGLE [[noreturn]] INIT_CODE USED
auto x86_start_nonboot_cpu(CpuNum cpunum) -> void;
```

**有一处变难看了**，如实记下来。`kernel/kernel/task/idle_thread.cpp:34` 是全树唯一
把这个属性用在 lambda 上的地方，而 `BreakAfterAttributes: Always` 对 lambda 的属性
无论放在参数表前还是后都会断出难看的行：

```cpp
    constexpr auto dummy_trampoline_routine = [] [[noreturn]]
                                              () -> void {
```

没有为此改源码。顺带查证：这个属性在这里**只是文档**——lambda 是当函数指针交给
`Thread::spawn_etc()` 的，而 `[[noreturn]]` 不属于函数类型，穿不过指针；
函数体调的 `panic()` 本身就带 `[[noreturn]]`（`system/public/ours/panic.hpp:88`），
编译器早就知道这条路不返回。它上面的兄弟 `dummy_start_routine` 是同类东西、
同样 panic，也没带这个属性。所以这一处的属性去留是可讨论的，
但那是另一个问题，不该混进本条。

**参照**

无，本项目自有规则。可以对照的是 Linux 的相反选择：它一直用 `__noreturn`
（`include/linux/compiler_attributes.h`）而不是 `[[noreturn]]`，因为它长期要
同时喂 GCC 和 sparse、且到 C23 之前 C 语言里没有标准属性拼法。C++ 这边没有这个约束。

**验证**

两个镜像构建通过。QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到
线程调度，`kernel.dev.obi` 到 `prof: no architectural PMU`。

`[[noreturn]]` 与 `NO_RETURN` 展开完全相同，所以生成的代码本应逐字节一致；
仍然跑了启动，因为属性贴错声明是会改语义的，而这次动了 13 处声明。

**故意没做**

- 没有动 `NO_UNIQUE_ADDRESS`（包着标准的 `[[no_unique_address]]`，但带着 msvc 与
  汇编两条分支）和 `NO_MANGLE`（包着 `extern "C"`）。同一条规则，各自一条改动。
- 没有动 `USED`、`NAKED`、`PACKED`、`NO_INLINE`、`WEAK_LINK`、`PHYS_EXPORT`、
  `LINK_SECTION`、`INIT_*`、`FORCE_INLINE`、`KERNEL_API`、`TA_*`——它们包的都是
  `[[gnu::]]`／`[[msvc::]]`／`__attribute__` 这类厂商属性，正是规则里保留宏的那一类。
- 没有改 `idle_thread.cpp` 那个 lambda。见上面。

---

## `NO_UNIQUE_ADDRESS` 删掉，直接写 `[[no_unique_address]]`（2026-09-04）

**文件**
- `system/public/ours/compiler.hpp` — 删掉 `NO_UNIQUE_ADDRESS` 那四个分支
- `.clang-format` — `AttributeMacros` 与 `Macros:` 各去掉一行
- `kernel/lib/gktl/include/gktl/radix_tree.hpp` — 3 处改写

**症状**

同上一条的规则：语言里已有的东西不再包宏。区别在于这个宏**看起来**有存在的理由——
它不是无条件的，而是四个分支：

```cpp
#if defined(__ASSEMBLER__) || defined(__ASSEMBLY__)
#define NO_UNIQUE_ADDRESS
#elif __has_cpp_attribute(no_unique_address)
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#elif __has_cpp_attribute(msvc::no_unique_address)
// msvc targets, as used by efi, have a different attribute name, even when
// targeting C++20.
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS
#endif
```

"按编译器分支"正是规则里保留宏的那一类，所以这条要先查证再动，不能照上一条的样子直接删。

**原因**

查证结果是：**这四个分支在本项目里只有一个会走到**。

- `msvc::` 那一支要有 MSVC 目标才有意义。全树第一方代码里 `_MSC_VER` 只出现一处
  （`compiler.hpp` 里 `FORCE_INLINE` 的分支），构建配置（`build/`、各 `BUILD.gn`）
  里没有任何 EFI 或 MSVC 工具链。注释里那句 "msvc targets, as used by efi" 是从
  Zircon 的 `compiler.h` 带过来的——**Fuchsia 确实有 EFI 目标，我们没有**。
- `__ASSEMBLER__` 那一支把它定义成空，是为了让汇编源码 include 这个头时不炸。
  但它的全部三处使用都在 `radix_tree.hpp` 的 C++ 成员声明上，汇编永远看不到，
  所以这一支不保护任何东西。
- 最后那个 `#else` 是为不支持该属性的编译器兜底，clang 23 / `-std=c++2c` 下
  `__has_cpp_attribute(no_unique_address)` 恒为真。

也就是说这不是可移植性垫片，是一层**从上游抄来、在这里已经没有对象的条件编译**。
留着它的代价不只是间接一层：那三个 `#elif` 会让读的人以为本项目在支持 MSVC，
从而在别处也照着做无谓的分支——一个错误的前提比一层多余的宏更贵。

（`empty-classes-may-be-planned-scaffolding` 那条经验在这里不适用：脚手架是"为将来要做的事
预留"，而这几个分支是为**别人的**目标写的，不是为我们计划中的目标。）

**改法**

3 处替换成 `[[no_unique_address]]`，宏连同四个分支一起删，`.clang-format` 两处跟着去掉。

**参照**

`Zircon zircon/system/public/zircon/compiler.h` 是这段代码的出处，它的
`ZX_NO_UNIQUE_ADDRESS` 保留 msvc 分支是对的——Fuchsia 的 `//zircon/kernel/phys/efi`
就是 MSVC ABI 目标。差别在目标集，不在写法。

**验证**

两个镜像构建通过——`[[no_unique_address]]` 生效与否会改变 `RadixTree` 的对象布局，
所以编译通过本身就说明属性确实贴上了（贴丢了也能编过，但布局会变；见下）。
QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到线程调度，
`kernel.dev.obi` 到 `prof: no architectural PMU`。

**没有单独测量 `sizeof(RadixTree)`**。属性丢失会让三个空成员各占一字节，是可观测的，
但替换是逐字面的、且 `radix_tree.hpp` 的三处都在同一屏内肉眼可核对，
再加一次 `static_assert` 只为验证一次文本替换不值当。

**故意没做**

- 没有动 `FORCE_INLINE`。它同样是按编译器分支（`_MSC_VER` → `[[msvc::force_inline]]`），
  但两支都是厂商属性，本来就该有宏。这次只是顺带确认了那处 `_MSC_VER` 是全树第一方代码里
  唯一一处。
- 没有动 `NO_MANGLE`（`extern "C"`）。同一条规则，下一条。

---

## `NO_MANGLE` 删掉，直接写 `extern "C"`（2026-09-04）

**文件**
- `system/public/ours/compiler.hpp` — 删掉 `#define NO_MANGLE`
- `.clang-format` — 改掉那段解释"为什么 NO_MANGLE 不在 AttributeMacros 里"的注释
- 27 个 `.cpp`/`.hpp` — 57 处改写；另有三处注释里提到这个宏，一并改用新拼法

**症状**

同前两条的规则，且这一条最直白：`extern "C"` 不是属性，是**语言的链接规范**，
C++ 从第一版就有，没有任何编译器分支。`NO_MANGLE` 展开就是它，一个字不多。

包装的代价在这条上尤其明显，因为它不是属性：`.clang-format` 里为此专门留了一句
"NO_MANGLE 故意不在 AttributeMacros 里，因为它展开成 `extern "C"`，是链接规范而不是属性"
——一句只为解释某个宏为何不适用某条规则而存在的注释。宏没了，这句注释也不必存在了。

**原因**

除了"标准拼法不该藏在宏后面"，这次还有两个具体的收获，都是把真实语法交还给工具带来的：

1. **块形式终于长得像块。** 有两处写的是 `NO_MANGLE { ... }`，
   clang-format 只把它看成"一个不认识的标识符后面跟一个块"，于是按普通复合语句缩进；
   换成 `extern "C" {` 之后它认出这是链接规范，按链接规范排版。结果与树里**本来就直接写
   `extern "C" {` 的那几处**（`kernel/mm/vmm.cpp:7`、`kernel/lib/ccrt/compiler/itanium.cpp:23`、
   `kernel/lib/libc/cpp_dependencies.cpp:8`）一致了——原先是同一件事有两种排版，
   取决于作者当时用了宏还是没用。

2. **多标注并列的顺序读得出对错了。** 像
   `extern "C" [[noreturn]] INIT_CODE USED` 这种，`extern "C"` 打头是语法要求
   （链接规范在前，被它修饰的声明——连同声明自己的属性——在后）。写成 `NO_MANGLE` 时
   这一点被藏起来了，看上去像四个平级的标注可以随便排。

**改法**

57 处逐字替换，宏定义删除。三处注释里提到这个宏的地方也改用新拼法，
其中 `arch/x86/percpu.hpp` 那句顺手写清楚了理由（`syscall.S` 只能引用未修饰的名字）。

`kernel/mm/pmm.cpp` 正被另一个会话改着（同文件另一处 `InvalidArguments` → `InvArgs`）。
按 `CLAUDE.md` 的规矩，工作区里照改，但提交时只提交自己那一处：
以 HEAD 版本为底加上本条的改动，`git hash-object -w` 之后 `git update-index --cacheinfo`
入库，没有 `git add` 整个文件。

**参照**

无，本项目自有规则。Linux 有对应的 `__BEGIN_DECLS`/`__END_DECLS`（`sys/cdefs.h`），
但那是**给 C 头文件用**的——同一个头要能被 C 和 C++ 同时包含，而 C 里没有 `extern "C"`。
本项目的头文件全是 C++ 专用的，没有这个约束。

**验证**

两个镜像构建通过。QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到
线程调度，`kernel.dev.obi` 到 `prof: no architectural PMU`。

这一条有比"能启动"更直接的证据——链接规范如果丢了，符号名会被修饰，
而汇编和链接脚本按未修饰的名字找它们。`llvm-nm` 查符号表：

```
000000000007f468 b KERNEL_PGD
000000000009d5a8 d KERNEL_STACK_BOTTOM
000000000007f3d8 b MAX_PHYS_ADDR
00000000000890e0 d X86_PCPU
0000000000028fa0 t memcpy
```

五个分别覆盖了改动涉及的几种形态：块形式里的变量（`KERNEL_PGD`、`MAX_PHYS_ADDR`）、
`extern "C" INIT_DATA` 形式（`KERNEL_STACK_BOTTOM`）、块形式里的 `inline` 变量
（`X86_PCPU`，被 `syscall.S` 引用）、`extern "C" WEAK_LINK` 形式（`memcpy`）。
名字全部未经修饰。

**故意没做**

- 没有动 `LINK_NAME(name)`（展开成 `asm(name)`）。它带参数，而且 asm label 是 GNU 扩展、
  不是标准语法，属于规则里该保留宏的那一类。
- 没有为 `extern "C" {` 块加 `IndentExternBlock: Indent`。查过树里原有的四处
  `extern "C" {` 本来就是不缩进的，clang-format 现在的输出与它们一致——
  不缩进不是这次改动带来的新样子，改配置反而会去动那四处。

至此 `compiler.hpp` 里包装标准语法的宏清零；剩下的全部包着
`[[gnu::]]`／`[[msvc::]]`／`__attribute__`，正是规则里该有宏的那一类。
