<!-- Owner decisions, 2026-09-24: every recommendation in section 4 taken.
  1. a bad pointer handed to a system call returns InvArgs; a program that
     dereferences one itself, #GP, ud2 or a forged trap is killed;
  2. examples/fault_probe/trap.S may hold the tree's one bare `syscall`,
     dbg_1 image only, commented as the counter-example to the vDSO rule;
  3. probes are driven by the dbg_1 shell holding the root job;
  4. commits 22, 23, 25 (P1-B minimum) come forward into P1-A;
  5. a ring-3 #DB kills the process;
  6/7. arena lazy commit and the CPRNG are deferred (P1-C / P1-E); the
     conditional items X1-X5 are not done in P1-A;
  8. both user_copy deviations from design 4.6 are adopted;
  9. sys_not_implemented and the empty `signals & None` branch are deleted;
     detach_thread keeps its name. -->

# P1-A 最终实施计划：系统调用边界

## 1. 摘要

基准是 HEAD `c88d2f8b`，工作区干净。评审说基线是 `9dc29cd7`，这已经过时，其后又进了 `b175d29e` 和 `c88d2f8b`。`userboot.cpp` 上他人的改动已在 `9dc29cd7` 提交，第 9 条不再阻塞。

P1-A 共 30 条提交，分五个阶段，外加一组由主人决定是否做的条件项：
- A 前置小修（1–3）
- B 系统调用生成器（4–10）
- C Spectre V1 加固（11–13）
- D user_copy 故障恢复（14–18）
- E fault-kill（19–30）
- 条件项 X1–X5：arena 惰性提交和 CPRNG

完成判据的理解是：通过系统调用传入的坏指针回 `InvArgs`；程序自己解引用坏指针、#GP、ud2、伪造陷入，则被杀掉，内核继续运行。

测试不再集中放在末尾：
- shell 的坏指针检查紧跟 data_fault_resume（第 16 条），两个镜像都跑；
- 探针测试紧跟对应的内核提交（第 27、30 条）；
- 探针只由 dbg_1 镜像里的 shell 持根 job 驱动。原因是 dev 用 dbg.0 编译，`lk_debug_level = 0`（`build/switch/set/dbg.0/BUILD.gn:5`），DEBUG_ASSERT 全部编掉，放在 dev 里就没有断言覆盖。

评审的意见大多成立，以下几处我查过代码后做了修正：
- **评审 1（NMI 只跳过 postamble）**：只跳过 postamble 会泄漏 preamble 加上的抢占计数（`kernel/kernel/irq/mod.cpp:54-56`），必须连 preamble 一起绕过。
- **评审 18（要配合 restat.gni）**：不需要。GN 已经给每条 action 规则加了 `restat = 1`（`out/bringup.x86/toolchain.ninja:103,112`），`restat.gni` 是给 tool() 编译命令用的。
- **评审 18（validations 可能不支持）**：`prebuilt/gn/gn help validations` 有输出，gn 2530 支持。
- **评审 16（没有持锁计数接口）**：`SpinLock::num_held_cpu()` 其实存在（`kernel/kernel/sync/spin_lock.cpp:196`）。但 pi_lock 会在上下文切换时交接，逐 CPU 的计数是否平衡没有验证过，所以结论不变，不拿它做断言。
- **评审 8 的 grep**：`grep OURS_SYS` 会匹配到 `system/lib/libc` 里的 `OURS_SYSTEM_*` 头文件守卫，要改写（见第 7 条）。

## 2. 提交序列

**通用约定**
- 构建：`prebuilt/ninja/ninja -C out/bringup.x86`。
- 下文的 **B（基线启动）**：两个镜像都启动，日志中不能有 panic 或 ASSERT。
  - `python3 scripts/qemu_boot.py`（dev）：日志要有 `init: shell says "pong"`、`shell: channel checks passed`、`--- end ---`。
  - `IMAGE=out/bringup.x86/kernel.dev.dbg_1.obi python3 scripts/qemu_boot.py`：日志要有 `shell: started by the kernel; no one to answer`、`shell: close again = BadHandle`、`shell: channel checks passed`、`--- end ---`。
  - 第 16 条之后，两个镜像都要多出 `shell: bad-pointer checks passed`；第 27 条之后，dbg_1 要多出 `shell: probes done (...)`。
- **附加变体**：
  - `CPU=max`（有 SMAP）用于第 14–18 条和第 26、27 条；
  - `SMP=1` 用于第 19–21 条和第 26、27 条。先在基线上跑一次 `SMP=1`，确认能启动；如果基线就起不来，记进 repairs，改用 `SMP=2`。
- **干净构建**（第 4–10 条）：新建一个 out 目录，复制 `out/bringup.x86/args.gn`，构建全部目标。要覆盖两个 obi、`kernel_x86.dbg.0`、`kernel_x86.dbg.1` 和 `user-x86`。
- **提交方式**：
  - 先看 `git diff <file>`，确认文件里没有别人的改动；
  - 新建的文件先 `git add <新路径>`，然后 `git commit -- <路径…>`。只用 `git commit -- <路径>` 时，未跟踪的文件会报 pathspec 不匹配。
- **文档**：改动记录写进 `docs/changes/<YYMMDD>/<MODULE>.md`，必须贴出改前和改后的关键代码；repairs 的删改和代码放在同一个提交里。
- **Python**：宿主机是 3.9。写 `from __future__ import annotations`，不用 `match`，运行期不用 `X | Y`；只用标准库；参数用 argparse；写 depfile；内容不变时不重写输出。

### 阶段 A：前置小修

**1. `fix(task): Thread::Current::kill() refuses the idle thread, not every other thread`**
- 文件：`kernel/kernel/task/thread.cpp`、`docs/changes/<YYMMDD>/kernel-task.md`
- 改动：`thread.cpp:650` 的 `DEBUG_ASSERT(current_thread->is_idle_thread(), …)` 条件写反了，取反。对照 Zircon `Thread::Current::Exit` 的 `DEBUG_ASSERT(!IsIdle())`。
- 验证：B。
- 依赖：无。

**2. `fix(task): suspend_or_kill takes the PI lock with interrupts and preemption off`**
- 文件：同上。
- 改动：`:248` 的 `Guard thread_guard{get_pi_lock()}` 改为 `Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable>`，与 `resume()`（:216）一致。
- 注释写明自锁链：线程 kill 自己时持着自己的 pi_lock，时钟中断出口的 `Thread::Current::preempt()`（:428-438）会再拿同一把锁。对照 Linux `try_to_wake_up` 的 `raw_spin_lock_irqsave(&p->pi_lock)` 和 Zircon `SuspendOrKillInternal`。
- 验证：B。这条路径目前没有调用者。
- 依赖：无。

**3. `feat(abi): KoId is u64`**
- 文件：`system/public/ours/types.hpp:42`、`kernel/include/ours/koid.hpp`、`kernel/kernel/task/thread.cpp:184-187`（只改注释）、`kernel/object/handle.cpp`、`docs/changes/<YYMMDD>/abi.md`
- 改动：
  - 改为 `using KoId = u64;`。注释写两条理由：循环 `channel_create` 约 72 分钟就能让 u32 回绕；回绕后发出的 0 与 `Handle::m_table_koid` 表示"不在任何表里"的 0 相撞。对照 Zircon `zx_koid_t`。
  - `KoIdGenerator::generate` 的 count 形参改为 `usize`。
  - `handle.cpp` 加 `static_assert(sizeof(Handle) == 40)`。成员是 atomic KoId 8 字节、Arc 8、Rights 4、base 4、hook 16；u32 时靠填充同样是 40。
  - `prof_pmu.cpp:98` 的 u32 窄化保留。
- 验证：构建不新增 `-Wconversion` 告警；B。
- 依赖：无。必须赶在 P1-B 冻结 `InfoHandleBasic` 之前。

### 阶段 B：系统调用生成器（设计 4.7）

**4. `build(syscall): describe the eight calls in syscalls.gni and generate the private number header`**
- 文件：`system/public/ours/syscalls.gni`、`build/syscall/gen_syscalls.py`、`system/public/BUILD.gn`、`kernel/lib/syscall/BUILD.gn`、`kernel/lib/syscall/src/table.cpp`、`docs/changes/<YYMMDD>/syscall.md`
- 描述文件：
  - 8 条按现有 0..7 排列：`debug_write`、`command_list`、`handle_close`、`channel_create`、`channel_write`、`channel_read`、`object_wait_one`、`process_spawn_boot`。
  - 字段有 `name`、`brief`、`doc`、`args{name,type,dir,count,release}`、`attrs{blocking,noreturn,debug}`、`ret`。
  - 调用文档合并自 `vdso.hpp`、`handlers.hpp` 和 `syscall.hpp:25-98`。
- 生成器：
  - 这一步只产出 `private/syscall_abi/numbers.hpp`：`ours::syscall_abi::SYSCALL_COUNT`、`SYS_<NAME>`（constexpr，不用宏）、`SYSCALL_NAMES`。
  - 全部校验在这一步做完，一律拒绝：参数超过 8 个、nargs==7、重名、count 悬空、未知 attr、`release` 用在非 `HandleValue` 上。
- GN：
  - 默认工具链是 `kernel-x86`（`BUILDCONFIG.gn` 的 `set_default_toolchain(target_toolchain)`），它只描述 action 和元数据图。
  - 在 `if (current_toolchain == default_toolchain)` 守卫下加 `generated_file(… output_conversion = "json")` 和 `action`，生成物只产出一次。
  - `group("syscalls.private")` 在所有工具链里定义：`public_deps` 指向生成 action；include_dirs 通过 `get_label_info(…($default_toolchain), "target_gen_dir")` 取得；`visibility` 只放行 `//kernel/lib/syscall/*` 和 `//kernel/lib/userabi/vdso/*`。
- 内核侧：`table.cpp` 对每个号 `static_assert(syscall_abi::SYS_X == OURS_SYS_X)`。
- 验证：
  - 干净构建和增量构建都通过；
  - 检查生成的头文件；
  - 本地对调两条的顺序，static_assert 应当报错（不提交）；
  - B。
- 依赖：无。

**5. `build(vdso): the stubs come from the generator`**
- 文件：`gen_syscalls.py`、`system/public/BUILD.gn`、`kernel/lib/userabi/vdso/{vdso.S,BUILD.gn}`、`docs/repairs/260923/syscall.md`、`docs/changes/<YYMMDD>/userabi.md`
- 改动：
  - 生成 `vdso_stubs.inc`，每个调用一行 `ours_syscall ours_<name>, OURS_VDSO_SYSRET_<NAME>, <n>, <nargs>`。
  - `vdso.S` 只留一个 `.macro ours_syscall`：
    - 分三支：≤3、≤6、8 参数，其他数目 `.error`；
    - 保持 `.balign 16`、原有指令顺序（先 `mov %rcx,%r10` 再 `mov $n,%eax`；8 参数先 push r12/r13）；
    - sysret 标签保持 `.globl` + `.hidden`；
    - 原先散在各个桩上的 r10 说明和栈图集中到宏上，注明对照 Zircon `syscalls-x86.S` 的 `m_syscall`；
    - 删掉对 `<ours/syscall.hpp>`、`<ours/vdso.hpp>` 的 include，改为 include `vdso_stubs.inc`。
  - `vdso/BUILD.gn` 的 deps 加 `syscalls.private`，删掉 :51-55 那段与 :56-59 矛盾的注释。
  - repairs `syscall.md` #1 的删除清单里，"vdso.S 里的桩"改为"`syscalls.gni` 中的 `process_spawn_boot` 条目"。
- 验证：改动前后各导出 vDSO 的三样东西，必须完全相同，比对结果写进改动记录：
  - `llvm-objcopy --only-section=.text -O binary` 的字节；
  - 按名字排序的 `llvm-readelf --dyn-syms`；
  - `llvm-nm -n`，包括 8 个 `OURS_VDSO_SYSRET_*`。

  然后干净构建，B。
- 依赖：4。

**6. `refactor(syscall): sys_* declarations and frame shims come from the generator`**
- 文件：`gen_syscalls.py`、`kernel/lib/syscall/include/syscall/handlers.hpp`、`kernel/lib/syscall/src/argument.hpp`（新建，手写）、`table.cpp`、`kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp`、`docs/repairs/260923/syscall.md`、`docs/changes/<YYMMDD>/syscall.md`
- 改动：
  - 生成 `kernel_decls.inc`（sys_* 声明）。类型映射：`void`+in → `UserInPtr<u8 const>`；`void`+out → `UserOutPtr<u8>`；`char`+in → `UserInPtr<char const>`；out 的 `HandleValue` → `UserOutPtr<HandleValue>`。
  - 生成 `kernel_shims.inc`：每个调用一个 shim，外加 `GENERATED_SHIMS[]`。
  - `argument.hpp` 放 `from_register<T>`，注释从 `table.cpp:106-122` 搬过来，对照 Zircon `SafeSyscallArgument::Sanitize`。
  - arch 头加 `arch::syscall_arg<N>(frame)`，寄存器序 di、si、dx、r10、r8、r9、r12、r13，写明 rcx 和 r11 为什么不可用。
  - `table.cpp`：删掉 :101-167 的 shim 和 :210-217 的填表，改为从 `GENERATED_SHIMS` 填表；去掉第 4 条加的 static_assert；改正 :196-200 的过时 TODO。
  - repairs 清单里的"table.cpp 表项"改为"生成物，随 `syscalls.gni` 条目消失"。
- 验证：干净构建；本地把某个 `sys_*` 的形参类型改掉，应当得到 undefined `sys_*` 链接错误（不提交）；B。
- 依赖：4。

**7. `abi: syscall numbers leave the public header`**
- 文件：`system/public/ours/syscall.hpp`、`kernel/arch/x86/kernel/syscall.S`、`table.cpp`、`docs/repairs/260923/syscall.md`、`docs/changes/<YYMMDD>/abi.md`
- 改动：
  - 同一提交里删掉四样：`OURS_SYSCALL_COUNT`（:23）、`OURS_SYS_*`（:45-98）、`ours::SYSCALL_COUNT`（:108）、`SyscallNumber`（:168-177）。只删宏不删 :108 会编译失败。
  - 重写 :6-31 的说明：调用号是内核与 vDSO 之间的私有约定（D8）。这个头只剩两侧共享的常量，比如 `INSTANT_INFINITE`、`CHANNEL_MAX_*`、`MAX_DEBUG_WRITE`、`PROCESS_BOOT_NAME_MAX`，以及各 Options 枚举。
  - `table.cpp` 新增私有常量 `DISPATCH_SLOTS = usize{256}`，行为不变。
  - 生成的名字一律写全限定名 `syscall_abi::`，禁止 using-directive。否则删掉 `ours::SYSCALL_COUNT` 后，表长会在这一条里悄悄变成 9。
  - 删掉 `syscall.S:3` 的 include。
  - repairs 清单里的 `OURS_SYS_PROCESS_SPAWN_BOOT` 改为 `syscalls.gni` 条目。
- 验证：
  - `grep -rn "OURS_SYS_\|OURS_SYSCALL_COUNT\|SyscallNumber" kernel system examples` 为空；
  - `grep -rnw SYSCALL_COUNT kernel system examples` 只剩 `syscall_abi::` 限定的；
  - 干净构建；B。
- 依赖：5、6。

**8. `syscall: size the dispatch table by the generated count`**（决策 9a）
- 文件：`table.cpp`、`docs/changes/<YYMMDD>/syscall.md`
- 改动：
  - 表长改为 `syscall_abi::SYSCALL_COUNT + 1`，钳位也用它；
  - 删掉 `sys_not_implemented`（:32-34）；
  - 号 8..255 从 `Unimplemented` 改回 `BadSyscall`，与 Zircon `unknown_syscall` 一致，改动记录要写明这是行为变化。
- 主人不同意就整条跳过，保留 `DISPATCH_SLOTS`。
- 验证：B。
- 依赖：7。

**9. `abi: <ours/vdso.hpp> declarations come from the generator`**
- 文件：`gen_syscalls.py`、`system/public/BUILD.gn`、`system/public/ours/vdso.hpp`、`system/user_executable.gni`、`kernel/lib/userabi/src/userboot.cpp`、`docs/repairs/260923/syscall.md`、`docs/changes/<YYMMDD>/abi.md`
- 改动：
  - 生成 `public/ours/vdso_decls.inc`。`vdso.hpp` 保留手写外壳，`extern "C"` 块里改为 include 它。
  - 新增 group `vdso_decls`，由 `:public` 通过 `public_deps` 带上；`user_executable` 的 deps 加 `//system/public`。
  - 删掉 `userboot.cpp:34` 那个没用到的 `#include <ours/vdso.hpp>`，否则内核工具链会看到生成头却没有依赖边。
  - 改正 `vdso.hpp:44-45` 对 `MAX_DEBUG_WRITE` 位置的指向：它在 `syscall.hpp:141`，不在 `debug.cpp`。
  - 更新 repairs 清单里的声明一项。
- 验证：干净构建；生成的签名与旧的手写声明逐个相同；B。
- 依赖：4、6。

**10. `build(vdso): libours.ifs from the description and an ABI check`**
- 文件：`system/public/BUILD.gn`、`kernel/lib/userabi/vdso/BUILD.gn`、`build/syscall/verify_vdso_abi.py`、`docs/changes/<YYMMDD>/userabi.md`
- 改动：
  - `generated_ifs_file("libours")` 的符号用 foreach 从描述拼出，是纯 GN。
  - 用 `toolchain_utils_action` 调 `llvm-ifs --strip-ifs-target`，提取 vDSO 的实际导出。
  - `verify_vdso_abi.py` 用正则逐行解析 `Symbols:` 列表（.ifs 是 YAML，标准库解析不了），排序后比对两份，通过 `validations` 挂到 vDSO 上。这是树里第一次真正给 `validations` 赋值（`kernel/phys/*.gni` 只是转发）。对照 Zircon 的 `verify-abi`。
- 验证：构建通过、stamp 生成；本地从描述里删一条，或在 vDSO 里多导出一个符号，比对应当失败（不提交）；B。
- 依赖：5。

### 阶段 C：Spectre V1

**11. `feat(arch): conditional_select_nospec_eq and confine_array_index`**
- 文件：`kernel/lib/arch/include/arch/nospec.hpp`（按架构分派，写法同 `arch/random.hpp`）、`kernel/arch/x86/lib/arch/include/arch/x86/nospec.hpp`、`docs/changes/<YYMMDD>/arch.md`
- 改动：
  - 只写 x86：select 是 `cmp; cmovnz`，confine 是 `cmp; cmova`，参数一律 `usize`。
  - ARM 和 RISC-V 分支写 `#error`。它们在 `out/bringup.x86` 里编不到，写了实现也无法按每条提交验证。
  - 注释写明为什么必须用 asm（llvm-23 -O2 会把三元表达式编成 `jne`），对照 Zircon `fbl/confine_array_index.h`。
- 验证：构建通过；cmov 的 objdump 检查放到第 12 条，那里有第一个使用者。
- 依赖：无。

**12. `fix(object): the handle arena lookup selects its slot without branching`**
- 文件：`kernel/object/handle.cpp`、`kernel/object/include/ours/object/handle.hpp`、`docs/architecture/object/handle-lookup.md`、`docs/changes/<YYMMDD>/object.md`
- 改动（`lookup()`，:114-132）：
  - index 用 `confine_array_index` 夹住。
  - `publish` 时存 `base | HANDLE_LIVE_MARK`（bit31，保留位），空槽的 0 永远不会比中，于是删掉 generation==0 和越界两条分支。
  - 结果用 select 从槽地址和 0 里选。加 `static_assert(USER_ASPACE_BASE == MB(2))`（`aspace_layout.hpp:71`），注释写明：推测执行下对 0 的访问落在不会映射的页上。
  - 删掉 :125 的 TODO，改正理由：防的是 Spectre V1，不是计时探测。
  - `handle-lookup.md` 补写 random 要预左移，常量改成 SCREAMING_SNAKE。
- 验证：objdump 确认从读 `m_live_base_value` 到 select 之间没有 jcc；B，`close again = BadHandle` 仍然出现。
- 依赖：11。

**13. `fix(syscall): the dispatch index is confined against speculation`**（新增）
- 文件：`table.cpp`、`docs/changes/<YYMMDD>/syscall.md`
- 改动：`syscall_dispatch` 的钳位（:263-266）现在是 C++ 的 `if`，可能编成条件跳转；预测错时会越界读表项，再拿它做间接调用。保留"越界落到哨兵槽"的语义，再加一道 `arch::confine_array_index(number, TABLE_SIZE)`。对照 Zircon `syscall.S:295-301` 的 `jae` 加 `cmovge`。
- 验证：objdump 确认表下标经过 cmov；B。
- 依赖：11（与第 6–8 条改同一个文件）。

### 阶段 D：user_copy 故障恢复（设计 4.6）

**14. `refactor(x86/user_copy): the copy is one assembly routine with an SMAP and a plain variant`**
- 文件：`kernel/arch/x86/kernel/user_copy.S`（新建）、`user_copy.cpp`、`kernel/arch/x86/kernel/BUILD.gn`、`docs/changes/<YYMMDD>/user-copy.md`
- 改动：
  - 用 `.macro` 展开出 `x86_user_copy` 和 `x86_user_copy_smap`，主体是 `rep movsq` 加 `rep movsb`。
  - 两个变体放进同一个 section，导出 `x86_user_copy_begin` 和 `x86_user_copy_end`，第 15 条只需检查一段区间。
  - plain 变体的任何路径上都不能有 stac/clac，Nehalem 上它们是 #UD。
  - C++ 按 `feature_has_smap` 选择变体；删掉 `UserAccessWindow`，它的说明搬进 .S 的头注释。对照 Zircon `user-copy.S` 和 Linux `copy_user_64.S`。
- 验证：
  - 两个镜像在 Nehalem 和 `CPU=max` 下都跑到 `--- end ---`；
  - objdump 确认只有 `_smap` 变体含 stac/clac；
  - `CPU=max` 下临时加一处裸解引用（不提交），仍然报 `potential SMAP violation`。
- 依赖：无。

**15. `feat(user_copy): a faulting user copy returns InvArgs instead of panicking (data_fault_resume)`**
- 文件：`kernel/arch/x86/kernel/include/ours/arch/{thread.hpp,user_copy.hpp}`、`kernel/arch/x86/kernel/task/thread.cpp`、`user_copy.{S,cpp}`、`kernel/arch/x86/mm/fault.cpp`、`kernel/lib/user_copy/{src/user_copy.cpp,include/user_copy/user_copy.hpp}`、`docs/changes/<YYMMDD>/user-copy.md`
- 改动：
  - `X86Thread` 加 `m_data_fault_resume`。槽按线程存，因为拷贝中途可能被抢占或迁移。
  - 汇编拿不到当前线程（见 `syscall.S:182-188` 的注释），所以槽地址作为第 4 个参数传入。它原在 %rcx，入口处移到 %r10，因为 `rep movs` 要占用 %rcx。对照 Zircon `_x86_copy_to_or_from_user(…, &thr->arch().page_fault_resume, …)`（user-copy.cc:92）。
  - 执行顺序：先写槽，再 stac，再拷贝；fixup 路径在 smap 变体上先 clac，然后清槽，返回 0 或 1。
  - 注释写两条不变式：设槽到清槽之间不 call、不动栈；%r10 由 `interrupt_common` 保存和恢复。
  - `arch_copy_*` 改为返回 `Status`，失败回 `InvArgs`；入口 DEBUG_ASSERT 槽为 0。
  - `fault.cpp` 在 `page_fault` 失败后，只有以下四个条件同时成立才改 `frame->ip`：supervisor、`!is_kernel_address(cr2)`、槽非 0、ip 落在 `[x86_user_copy_begin, end)` 内。这条路径不打日志。对照 Zircon `x86_pfe_handler` 的 DO_FAULTS 路径。
- 验证：两个镜像在 Nehalem 和 `CPU=max` 下都通过；在 shell 里临时调用（不提交）：
  - `ours_debug_write((char const*)0x7fff'ffff'e000, 8)` 应得 `InvArgs`；
  - `channel_read` 的目标设为 `vdso_base`，应得 `InvArgs`；
  - 之后的检查照常通过。
- 依赖：14。

**16. `test(shell): bad user pointers come back as InvArgs and the kernel keeps running`**（提前到这里）
- 文件：`examples/shell/shell.cpp`、`docs/changes/<YYMMDD>/examples-shell.md`
- 改动：新增 `check_bad_pointers(vdso_base)`，在 `check_channel_loopback()` 之后调用，内容见第 3 节。
- 验证：两个镜像，Nehalem 和 `CPU=max` 各跑一次。
- 依赖：15。

**17. `fix(x86/mm): a kernel-mode fault on a user address outside user_copy panics without SMAP too`**
- 文件：`fault.cpp`、`docs/changes/<YYMMDD>/x86-mm.md`
- 改动：supervisor、用户地址、槽为 0 时，不再调页，直接 `page fault in kernel mode outside user_copy`。今天 :43 只在有 SMAP 时拦截。对照 Zircon `if (!pfr) exception_die`。
- 验证：
  - 两个镜像在两种 CPU 下都通过，第 16 条的检查守着 user_copy 的恢复路径；
  - Nehalem 下临时的裸解引用，改前静默成功，改后 panic（不提交）；
  - 如果启动失败，先查 `userabi::launch` 和 elfload 有没有直接写用户虚拟地址。
- 依赖：15。

**18. `harden(x86/user_copy): lfence between the range check and the copy`**
- 文件：`user_copy.cpp`、`user-copy.md`
- 改动：调用汇编之前加 `lfence`，防 Spectre v1 和 v1.1。对照 Zircon `user-copy.cc:80-88`。
- 验证：B，两种 CPU。
- 依赖：14。

### 阶段 E：fault-kill（设计 2.2）

**19. `fix(x86): the exception tail runs after the interrupt epilogue, and NMI/#DF/#MC skip it`**
- 文件：`kernel/arch/x86/kernel/faults.cpp`、`docs/changes/<YYMMDD>/x86-kernel.md`
- 改动（`arch_handle_exception`，:193-209）：
  1. NMI、#DF、#MC：`x86_dispatch_exception` 之后直接返回。preamble、postamble 都不进，不抢占，也不处理信号。
     - 今天 NMI 走完整套：嵌在 IRQ 里的 NMI 在 postamble 把 `CURRENT_FRAME` 清成 null（irq/mod.cpp:77），还可能在 NMI 屏蔽没解除时切换线程。
     - 对照 Zircon 单独的 `x86_nmi_handler` 和 `exceptions_c.cc:570` 的 `DEBUG_ASSERT(vector != NMI)`。
     - 注释不拿 IST 当理由：#DF 的 ist 实际是 0（repairs x86-kernel.md #1）。
  2. 其余向量依次执行：preamble → dispatch → postamble → 需要时 `preempt` → 来自用户态时 `process_pending_signals` → `DEBUG_ASSERT(irqs_disabled)`。对照 Zircon `x86_exception_handler` 的 :568-604。
- 验证：B；两个镜像各加跑一次 `SMP=1`。单核下 init 和 shell 最后都在 ring 3 自旋，时钟抢占必然发生在 ring 3，会走到新的尾部；dbg_1 下出口断言不触发。
- 依赖：无。

**20. `fix(task): the Kill branch of process_pending_signals exits the thread`**
- 文件：`kernel/kernel/task/thread.cpp`、`kernel/include/ours/task/thread.hpp`、`docs/repairs/<YYMMDD>/kernel-task.md`、`docs/changes/<YYMMDD>/kernel-task.md`
- `process_pending_signals`（:618-636）：
  - 入口断言 IF=0；
  - 最先检查 Kill：`local_irq_enable(); Thread::Current::exit(0);`；
  - Suspend 分支照旧；
  - 遇到未知位时 panic。今天 Kill 分支是空的，会在 IF=0 下死循环；
  - 删掉 `& ThreadSignals::None` 那个空分支（决策 9b）。
- `signals()` 改用 `ktl::read_once` 读；:224、:242、:244、:595 的写入改用 `write_once`（`kernel/lib/ktl/include/ktl/rwonce.hpp`）。对照 Zircon 用 relaxed atomic 存 `signals_`。
- `exit`（:654）在调 `on_exit` 之前断言 `!arch::irqs_disabled()` 和 `preemption_state().is_preemption_enabled()`。不断言"不持自旋锁"，原因见摘要。
- repairs 记一条：`do_suspend`（:589）用普通 Guard 拿自己的 pi_lock，再 `Scheduler::Current::reschedule`，与第 2 条同类；P1-A 不会置 Suspend，够不到。
- 验证：B + `SMP=1`。dbg_1 下断言不触发；如果 exit 的新断言在已有的内核线程退出路径上触发，先定位那条路径，不要放宽断言。
- 依赖：19。

**21. `feat(x86): check pending thread signals before SYSRET`**
- 文件：`syscall.S`、`faults.cpp`、`table.cpp`（只改注释）、`docs/changes/<YYMMDD>/x86-syscall.md`
- 改动：
  - 在 `call syscall_dispatch`（:192）之后、第一条 pop 之前，插入 `mov %rsp,%rdi; call x86_syscall_process_pending_signals`。
  - C++ 实现放在 `faults.cpp`，与 Zircon `exceptions_c.cc:607` 的位置一致。
  - 删掉 `table.cpp:275` 注释里的 "not written yet"。
- 验证：B + `SMP=1`，init 阻塞在 `object_wait_one` 后能正确返回。
- 依赖：20。与第 7 条改同一个文件，只是文本冲突，不是逻辑依赖。

**22. `refactor(object): one process lock guards state and the thread list`**（P1-B 最小集）
- 文件：`kernel/object/include/ours/object/process_dispatcher.hpp`、`kernel/object/process_dispatcher.cpp`、`docs/changes/<YYMMDD>/object.md`
- 改动：
  - 删掉 `m_thread_list_lock`（hpp:169），改用 `get_lock()`。两把都是 `sync::Mutex`。
  - `attach_thread`（:103-112）在同一段锁内完成三件事：Initial→Running、`push_back`、`activate(false)`。
  - 注释写明锁序是先进程锁后线程锁，因为 `activate` 会拿 `ThreadDispatcher::get_lock`。对照 Zircon `AddInitializedThread`。
- 验证：B。
- 依赖：无。

**23. `fix(object): a ThreadDispatcher leaves its process when its thread exits`**（P1-B 最小集）
- 文件：`kernel/object/{include/ours/object/thread_dispatcher.hpp,thread_dispatcher.cpp,process_dispatcher.cpp}`、`object.md`
- 改动：
  - `ThreadDispatcher::State` 增加 `Dying` 和 `Dead`。
  - 保留原名实现 `detach_thread`（:114-117，决策 9c）：在进程锁下一次做完三件事——置 Dead；`m_kernel_thread = nullptr`（对照 Zircon `ResetCoreThread`）；从线程表摘下（对照 Zircon `RemoveThread`）。
  - `on_exit`（thread_dispatcher.cpp:141）调用它。
  - kill 遍历线程表时持同一把锁，因此看不到摘到一半的线程。
- 验证：B。
- 依赖：20、22。

**24. `feat(object): count and log dispatcher destruction`**（新增，杀进程测试的观察点）
- 文件：`thread_dispatcher.{hpp,cpp}`、`process_dispatcher.{hpp,cpp}`、`object.md`
- 改动：
  - `~ThreadDispatcher`（今天是 `= default`，hpp:93）和新增的 `~ProcessDispatcher` 里，累加已声明但从来没加过的 `DISPATCHER_THREAD_DESTROY_COUNT` 和 `DISPATCHER_PROCESS_DESTROY_COUNT`（两个 .cpp 的 :12）；
  - 各打一行 `object: thread '<name>' (koid <k>) destroyed` 和 `object: process '<name>' (koid <k>) destroyed`。
- 验证：B。今天没有进程会死，这两行不应出现。
- 依赖：无。

**25. `feat(object): ProcessDispatcher::kill and TASK_RETCODE_*`**（P1-B 最小集）
- 文件：`system/public/ours/types.hpp`、`process_dispatcher.{hpp,cpp}`、`thread_dispatcher.{hpp,cpp}`、`object.md`
- 改动：
  - 公共头加 i64 的 `TASK_RETCODE_*`，数值照 Zircon：`SYSCALL_KILL` -1024、`OOM_KILL` -1025、`POLICY_KILL` -1026、`VDSO_KILL` -1027、`EXCEPTION_KILL` -1028、`CRITICAL_PROCESS_KILL` -1029。
  - `ProcessDispatcher::kill(i64)` 在锁下执行：第一次进入 Dying 时记下 retcode；然后对每个线程调 `ThreadDispatcher::kill`，即 Running→Dying，再 `m_kernel_thread->kill()`。
  - 只发信号，不 exit。对照 Zircon `ProcessDispatcher::Kill` 和 `KillAllThreadsLocked`。
- 验证：B。
- 依赖：2、22、23。

**26. `feat(x86): a CPU exception from ring 3 kills the process instead of the CPU`**
- 文件：`kernel/include/ours/task/exception.hpp`（新建，内核私有）、`faults.cpp`、`kernel/arch/x86/mm/fault.cpp`、`kernel/include/ours/task/thread.hpp`、`kernel/kernel/task/thread.cpp`、`thread_dispatcher.{hpp,cpp}`、`docs/changes/<YYMMDD>/{x86-kernel,object}.md`
- 新头文件：
  - `enum class ExceptionType`，数值同 `ZX_EXCP_*`：`General`、`FatalPageFault`、`UndefinedInstruction`、`SwBreakpoint`、`HwBreakpoint`、`PolicyError` 等；
  - `ExceptionContext{type, vector, error_code, ip, sp, fault_address, synth_code, synth_data, frame}`，其中 frame 可以为 null。
- `faults.cpp`：
  - 寄存器转储抽成 `arch_dump_exception_context(ctx)`，frame 为空时打 "no frame to dump"（对照 Zircon exceptions_c.cc:620）。
  - 来自 CPL3 的向量按 Zircon 映射后进入 kill 路径：
    - #DE、#OF、#BR、#NP、#SS、#AC、#GP → General；
    - #UD → UndefinedInstruction；
    - #PF（缺页未解决）→ FatalPageFault；
    - #DB → HwBreakpoint（决策 5）；
    - #BP → SwBreakpoint；
    - #MF、#XM、#CP → General。
  - 3 号和 4 号向量今天从用户态够不到：IDT 所有门的 DPL 都是 0（`idt.cpp:22` 的 `0x8E`），INTO 在 64 位模式下是 #UD。列进映射只是为了完整。
  - NMI 保持打日志后返回，并且已经被第 19 条绕开；7、8、9、10、18、20、29 号向量不论来自哪一环都 panic。
  - CR2 在 `x86_handle_page_fault` 入口读出并带出来，此时还没有开中断。
- 第 19 条的尾部之后，依次执行：
  1. `DEBUG_ASSERT` frame 在当前线程的内核栈内。这是为以后把 #DB 放上 IST 留的保险，`percpu.hpp:52` 已经预留了 `DEBUG_BREAK_STACK`；
  2. `irq_enable`；
  3. `Thread::Current::on_user_exception(ctx)`，转到 `ThreadDispatcher::on_user_exception`：打印 `terminating process '<p>' (koid <k>) thread '<t>': type=<T> vector=<to_string(IrqVec)> error_code=<e> rip=<> sp=<> cr2=<>` 和寄存器，然后 `process().kill(TASK_RETCODE_EXCEPTION_KILL)`；
  4. `irq_disable`；
  5. `process_pending_signals`，由它 exit；
  6. 如果它返回了，就 panic。

  对照 Zircon `dispatch_user_exception`。
- 验证：B + `SMP=1`。然后临时在 dbg_1 的 shell 里 `--- end ---` 之后加一条 `ud2`（不提交）。日志要依次出现：
  - 带 `type=UndefinedInstruction vector=invalid opcode` 的报告；
  - `object: thread 'shell' … destroyed`；
  - `object: process 'shell' … destroyed`；

  不能有断言，QEMU 一直运行到超时而不 panic。
- 依赖：
  - 15、17：只是同改 `fault.cpp`；
  - 19、20、23、24、25：逻辑依赖。

  已去掉对第 3 条的假依赖。

**27. `test(fault_probe): programs that fault are killed and the kernel carries on`**
- 文件：`examples/fault_probe/{BUILD.gn,fault_probe.cpp}`、`examples/BUILD.gn`、`kernel/products/image/dev.dbg_1/BUILD.gn`（打进包里）、`examples/shell/shell.cpp`（驱动）、`docs/changes/<YYMMDD>/examples-shell.md`
- 改动：见第 3 节。dev 镜像不打 `fault_probe`，init 不改（决策 3）。
- 验证：dbg_1 在 Nehalem、`CPU=max`、`SMP=1` 下各跑一次，按第 3 节核对；dev 仍然满足 B。
- 依赖：16、26。

**28. `fix(syscall): the vDSO check accepts a trap only when the whole syscall lies inside it`**
- 文件：`kernel/object/include/ours/object/process_dispatcher.hpp`、`table.cpp`、`kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp`、`docs/repairs/260923/x86-kernel.md`（删除 #2）、`docs/changes/<YYMMDD>/syscall.md`
- 改动：
  - `vdso_contains(pc)`（hpp:97）改为按区间判断：`vdso_contains(addr, len)`，条件是 `len <= size && addr - base <= size - len`。
  - arch 头加 `SYSCALL_INSN_SIZE = 2`；`trapped_from_vdso` 传 `(frame->ip - SYSCALL_INSN_SIZE, SYSCALL_INSN_SIZE)`。
  - 这等价于 `base + 2 <= ip <= base + span`。草案的 `base < ip` 会放过 `ip = base + 1`；"`ip - 2` 落在 `[base, base + span)`"的写法会放过 `ip = base + span + 1`。
  - 同时改 `table.cpp:43-46` 的注释。精确的白名单仍然等 `VDSO_SYSRET_OFFSET`。
- 验证：B。
- 依赖：无。

**29. `feat(syscall): a trap from outside the vDSO kills the process`**
- 文件：`table.cpp`、`thread_dispatcher.{hpp,cpp}`、`docs/changes/<YYMMDD>/{syscall,object}.md`
- 改动（`sys_not_from_vdso`，:76-83）：
  - 保留 warn 日志；
  - `ThreadDispatcher::current()` 为空时 panic，那是内核线程执行了 syscall；
  - 否则调 `thread->on_policy_exception(PolicyCode::BadSyscall, frame->ax)`。这是对象层的入口，由它构造 `ExceptionContext{PolicyError, synth_code, synth_data, frame = nullptr}`，再转 `on_user_exception`。这样 lib/syscall 仍然只 include kernel/object 的头；
  - 置 `ax = BadSyscall` 后返回，由第 21 条的出口检查 exit。

  对照 Zircon `sys_invalid_syscall` → `SignalPolicyException` → `arch_dispatch_user_policy_exception`（exceptions_c.cc:658-663）。
- 验证：B。临时在 dbg_1 的 shell 里放一条裸 `syscall`（不提交），日志要有 `refused a trap`、`type=PolicyError` 的报告，以及两行 destroyed，内核不 panic。
- 依赖：21、25、26、28。

**30. `test(fault_probe): a trap from outside the vDSO is killed`**（决策 2）
- 文件：`examples/fault_probe/trap.S`（新建，全树唯一一处裸 `syscall`，注释写明它是 D8 规则的反例）、`examples/fault_probe/{BUILD.gn,fault_probe.cpp}`、`examples/shell/shell.cpp`
- 改动：新增探针 `trap_outside_vdso`。
- 验证：见第 3 节。
- 依赖：27、29。

### 条件项（决策 6、7 如果选"留在 P1-A"，就插在第 13 条之后）
- X1：RDRAND/RDSEED 只检测一次，失败时重试。`arch::rand` 已经存在（`kernel/lib/arch/include/arch/random.hpp`，physboot 在用），只需补缓存和重试。
- X2：ChaCha20 和 BLAKE2s，建在 `system/lib/crypto`，用 copier 搭骨架。
- X3：`kernel/lib/kcrypto` 里的开机播种 CPRNG，InitLevel::Platform。
- X4：每张句柄表各取随机数（`handle_table.cpp:9`）。
- X5：arena 保留 2^18 个槽，按页提交。

### 并行与汇合
按依赖关系，阶段 B、C、D 和第 19–25 条可以各开一个会话。

同文件的冲突点：
- `table.cpp`：第 6、7、8、13、28、29 条；
- `syscall.S`：第 7、21 条；
- `fault.cpp`：第 15、17、26 条；
- `shell.cpp`：第 16、27、30 条；
- `task/thread.cpp`：第 1、2、20 条。

汇合点：
- 第 26 条等第 15、17、19–25 条；
- 第 29 条等第 21、25、26、28 条。

P1-A 从 P1-B 提前拿走的是第 22、23、25 条，要告知 P1-B 的会话。留给 P1-B 的有：进程转 Dead、`finish_dead_transition`、`VmAspace::destroy`、TERMINATED、`process_exit`、`task_kill`、get_info、job 子表、init-critical、tid 改用 koid。

## 3. 测试程序与期望输出

### 3.1 `shell: check_bad_pointers`（第 16 条，两个镜像都跑）

**调用方式**：直接调 vDSO 的 `ours_*` 函数，不经过 `ou` 包装，这样坏指针能原样传进内核。

**用到的地址**：
- U = `0x7fff'ffff'e000`：在 `is_user_range` 以内，但在私有 aspace 之外（私有 aspace 止于 2^46），`find_mapping` 找不到，走 resume；
- K = `0xffff'8000'0000'0000`：内核半区；
- V = `vdso_base`：R+X 映射。

**检查项**：用现有的 `expect()` 输出格式，按顺序打印：
```
shell: debug_write(unmapped) = InvArgs
shell: debug_write(kernel half) = InvArgs
shell: channel_write(from unmapped) = InvArgs
shell: channel_write(1 byte) = Ok
shell: channel_read(into the vDSO) = InvArgs
shell: channel_read(after the lost message) = ShouldWait
shell: wait_one(observed = vDSO, poll) = InvArgs
shell: channel_create(out0 = vDSO) = InvArgs
shell: channel_create(after that) = Ok
```
随后关闭四个句柄，每次打印 `… = Ok`，最后一行是 `shell: bad-pointer checks passed`；任何一项不符，打印 `shell: bad-pointer checks FAILED`。

**说明**：
- `wait_one` 必须传 deadline=0。`wait.cpp:63` 支持轮询；TimeOut 也算结束，之后才会去拷 observed。传无限 deadline 会一直挂住。
- 只有 U 和 V 两类真正检验了 data_fault_resume。K 类在第 15 条之前就回 `InvArgs`，由 `user_copy.cpp` 的 `is_user_range` 挡掉，只算回归项。
- channel_read 那条消息已经出队，拷贝失败后就丢了。这与 Zircon 的取舍相同，见 `channel.cpp:145-148` 的注释。

### 3.2 `fault_probe` 与 dbg_1 shell 驱动（第 27、30 条）

**`fault_probe` 的行为**：
- 等 bootstrap 通道可读，读出探针名，打印 `fault_probe: <probe>`，然后执行对应动作。所有动作都写成 `asm volatile`，防止被优化掉。
- 如果执行越过了故障指令，就往通道写 `survived`，然后用 `pause` 自旋。
- 遇到未知的探针名，打印 `fault_probe: unknown probe <name>`，并写回 `unknown`。

**shell 驱动**：
- 只在 `answer_bootstrap` 得到 `BadType` 时运行，也就是被内核直接启动、手里拿着根 job 的时候。
- 必须放在 `check_handle_close(bootstrap)`（shell.cpp:232）之前，因为那一步会把根 job 关掉。
- 对每个探针依次执行：
  1. `channel_create(here, there)`；
  2. 往 `here` 写探针名；
  3. `process_spawn_boot(job, "fault_probe", there)`；
  4. 在 `here` 上等 `Readable|PeerClosed`（INSTANT_INFINITE）；
  5. 可读，就打印 `shell: probe <p> SURVIVED (<读到的文本>)`；只有 PeerClosed，就打印 `shell: probe <p> killed`；
  6. 关闭 `here`。
- 全部探针跑完后打印 `shell: probes done (<k> killed, <s> survived)`。

每个探针在 `shell: probe <p> killed` 之前，都必须依次出现：`fault_probe: <p>`、带下表字段的 `terminating process 'fault_probe'` 报告、`object: thread 'fault_probe' … destroyed`、`object: process 'fault_probe' … destroyed`。

| 探针 | 动作 | 报告中必须出现的字段 |
|---|---|---|
| `pf_unmapped` | 读 `0x10` | `type=FatalPageFault vector=page fault cr2=0x10` |
| `pf_kernel` | 读 `0xffff800000000000` | `type=FatalPageFault cr2=0xffff800000000000` |
| `pf_write_vdso` | 写 `vdso_base` | `type=FatalPageFault cr2=<vdso_base>`、`error_code=0x7` |
| `ud2` | `ud2` | `type=UndefinedInstruction vector=invalid opcode` |
| `gp_hlt` | `hlt` | `type=General vector=general protection error_code=0x0` |
| `gp_int80` | `int $0x80` | `type=General vector=general protection error_code=0x402` |
| `gp_int3` | `int3`（DPL0 门，所以是 #GP） | `type=General vector=general protection error_code=0x1a` |
| `de` | `xor %ecx,%ecx; div %ecx` | `type=General vector=divide error` |
| `db_tf`（决策 5A） | `pushf; orq $0x100,(%rsp); popf; nop` | `type=HwBreakpoint vector=debug` |
| `trap_outside_vdso`（第 30 条） | `trap.S` 中的裸 `syscall` | 先有 `syscall: refused a trap from …, which is outside the vDSO`，再有 `type=PolicyError synth_code=BadSyscall` |

**结尾**：第 27 条之后是 `shell: probes done (9 killed, 0 survived)`；第 30 条之后是 `(10 killed, 0 survived)`；不采纳 db_tf 时各少 1。之后照常出现 `shell: close again = BadHandle`、`shell: channel checks passed`、`shell: bad-pointer checks passed`、`--- end ---`。

## 4. 待主人拍板的决策

1. **"传坏指针"判据怎么理解**。推荐：系统调用拿到坏指针回 `InvArgs`；程序自己解引用坏指针、#GP、ud2、伪造陷入，则被杀掉。这与 Zircon 和 Linux 的 `-EFAULT` 一致；在缺页处理里杀，会跳过 handler 栈上 Arc、Box、已出队消息的析构。同时建议按这个意思改写设计的完成判据。
2. **`fault_probe/trap.S` 能否含一条裸 `syscall`**。推荐允许：只放在这一个文件里，只打进 dbg_1 镜像，注释写明是反例。不推荐在运行时往 RWX 映像里写 `0f 05` 来绕过规则，那只是把同一件事藏起来。
3. **探针由谁驱动**。
   - 推荐：只在 dbg_1 里由 shell 持根 job 驱动，`fault_probe` 只打进 dev.dbg_1。
   - 原因：dev 的内核用 dbg.0 编译，断言全部编掉；第 2、19、20、26 条新加的 IRQ、抢占、锁断言只有在 dbg_1 里才会执行。只维护一个驱动，也免得在 init 里写一份 P1-C 反正要重写的测试代码。
   - 另两个选项：只由 init 在 dev 里驱动（草案原案）；两个镜像都驱动。
4. **第 22、23、25 条（P1-B 最小集）提前进 P1-A**。推荐提前。不提前的话，第一次杀进程，ThreadDispatcher 还挂在进程的侵入式线程表上就被析构，DPC worker 会在 SafeLink 检查上 trap。
5. **用户态 #DB 是否杀进程**。推荐杀，Zircon 对无人处理的调试异常也是 `EXCEPTION_KILL`；不杀的话，用户 `popf` 置上 TF 就能刷满串口。今天用户态只够得到 #DB，#BP、#OF 够不到（原因见第 26 条）。照 Zircon `idt.cc:99-101` 把 3 号门设成 DPL3，推荐不放在 P1-A，因为现在没有调试器需要它。
6. **arena 惰性提交**。推荐推迟到 P1-C 的句柄记账，并以修好 `docs/repairs/260908/heap.md` #3 的 OOM 死循环为前提。惰性 grow 遇到 OOM 时，会在 arena 锁下死循环；固定 2^12 个槽耗尽时，是一个干净的 `NoResource`。
7. **CPRNG 与每表随机数**。推荐 P1-A 保持 random=0，设计本身允许"暂时仍取 0"；CPRNG 和 `cprng_draw` 一起在 P1-E 只造一次。不要用 TSC 过渡：CR4.TSD 从未置位，用户能读 `rdtsc`，这样的随机数可猜。
8. **user_copy 两处偏离设计 4.6**。两处都推荐采纳，并同步修订设计 4.6 的文字：
   - (a) resume 除了看槽非 0，还要求 ip 落在拷贝例程的出错区间内。否则在没有 SMAP 的 Nehalem 上，拷贝窗口里的嵌套中断如果碰到用户地址，会被改道成一次任意跳转；
   - (b) 用两个 `.macro` 变体取代设计写的代码补丁，因为 `ours_kernel_has_code_patches = false`（`BUILDCONFIG.gn:85`）。
9. **三处桩的处置**（按仓库规则，桩要先问）：
   - (a) 第 8 条删 `sys_not_implemented`，并让号 8..255 改回 `BadSyscall`：推荐做；
   - (b) 第 20 条删 `if (signals & None) {}`：它的条件恒为假，推荐删；
   - (c) `detach_thread`：推荐保留原名直接实现，不另起 `remove_thread`。

以下由规划方直接定：
- `OURS_SYS_*` 不保留；
- 调用文档写进描述文件；
- 寄存器序放在 arch 头；
- 生成物只在默认工具链里产出一次；
- `vdso.hpp` 保留手写外壳；
- `ExceptionType` 放内核私有头；
- 7 参数的桩推迟到 P1-C 的 `vmar_map`；
- 生成器产物 ⑦（`VDSO_SYSRET_OFFSET`、build-id）不在 P1-A，设计 :424 本来就写着"P1 后期"。

明确不在 P1-A 的还有：`user_executable` 改为链接 ifs 桩、capture-faults 变体、拒绝 RSVD/PK/SS 错误码位、IST 配置（repairs x86-kernel.md #1）、`panic_exception` 改调 `panic()`、prof 的 tid 改宽。

## 5. 风险

- **缺页在 IF=0、抢占关闭的状态下拿 aspace 的 Mutex**（`kernel/mm/vm_aspace.cpp:165`）。Zircon 的 `x86_pfe_handler` 会先开中断。data_fault_resume 让更多内核态缺页走这条路径；P1-A 的进程都是单线程，锁没有争用，所以没暴露。一旦出现死锁或断言，记进 repairs。
- **`pf_write_vdso` 依赖写只读映射时 `VmAspace::page_fault` 返回非 Ok**。如果它返回 Ok，探针会在同一条指令上无限缺页，表现为挂住而不是被杀。第 27 条之前先单独验证。
- **析构链上任何一处引用泄漏**（Thread → ThreadDispatcher → ProcessDispatcher → HandleTable），PeerClosed 都不会到，驱动就永远挂在没有 deadline 的等待上（deadline 要到 P1-E），而不会打印 FAILED。第 24 条的 destroyed 日志用来定位断在哪一环。
- **已知代价**：
  - 每杀掉一个进程，都会泄漏 aspace、页表、用户页和内核栈，数量以探针个数为上限；
  - retcode 只在内核日志里能看到；
  - init 崩溃时会被安静地杀掉；
  - `suspend_or_kill` 对另一个 CPU 上 Running 的线程不发 IPI（`thread.cpp:264-266`），多线程进程要等下一个时钟中断才会死。P1-A 里只有进程杀自己，不受影响。
- **IST 仍未配置**（repairs x86-kernel.md #1）。NMI 和 #MC 依旧会因为 RSP=0 而致命；第 19、26 条只保证它们不进入 kill 和信号路径。
- **`SMP=1` 在基线上是否能启动没有验证过**。先跑一次，如果基线就起不来，改用 `SMP=2`，并记进 repairs。
- **生成头的顺序竞态只会在干净构建里出现**。第 4–10 条每条都要做干净构建。
- **第 20 条在 `exit` 上加的新断言，可能在已有的内核线程退出路径上触发**。这是在暴露真实问题，要修那条路径，不要放宽断言。
- **dbg_1 的 shell 同时承担了测试驱动**。等 P1-B 有了 `process_exit` 和 get_info，应当把驱动挪进一个专门的测试程序。