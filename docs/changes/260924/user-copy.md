# user-copy 改动记录（2026-09-24）

## 拷贝改为一段汇编，分 SMAP 与普通两个变体（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/user_copy.S`（新建）— 一个 `.macro` 展开出 `x86_user_copy` 与 `x86_user_copy_smap`
- `kernel/arch/x86/kernel/user_copy.cpp` — 删掉 `UserAccessWindow` 和 `memcpy`，按 `feature_has_smap` 选变体
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — 注释指向新实现；改正 Zircon 文件路径
- `kernel/arch/x86/kernel/BUILD.gn` — 加 `user_copy.S`

**症状**
无运行时症状。这是故障恢复（下一条）的前置重构。

**原因**
原来的拷贝是 `stac; memcpy; clac`，由 C++ 对象的构造和析构包住。它是对的——`"memory"`
clobber 让拷贝自己的访存留在窗口内——但窗口里放的是对内核通用 `memcpy` 的一次调用，
和其他所有调用者跑的是同一个函数。

故障恢复（设计 4.6）要做两件事，`memcpy` 都给不了：

1. 认出"这是拷贝在访问用户内存时出的错"——凭出错指令的地址。`memcpy` 被整个内核共用，
   它的地址说明不了任何事；
2. 把出错的拷贝送到一个已知地址、以已知的寄存器状态继续——编译器生成的代码不保证
   任何一点的寄存器和栈是什么样。

**改法**
改前：

```cpp
    class UserAccessWindow final {
      public:
        UserAccessWindow() {
            if (feature_has_smap) {
                asm volatile("stac" ::: "memory");
            }
        }
        ~UserAccessWindow() { ... clac ... }
    };

auto arch_copy_from_user(void *const dst, VirtAddr const src, usize const size) -> void {
    UserAccessWindow const window;
    std::memcpy(dst, reinterpret_cast<void const *>(src), size);
}
```

改后（`user_copy.S`）：

```asm
.macro user_copy name, smap
.function \name, global, nosection=nosection
    movq %rdx, %rcx
    shrq $3, %rcx
.if \smap
    stac
.endif
    rep movsq
    movl %edx, %ecx
    andl $7, %ecx
    rep movsb
.if \smap
    clac
.endif
    ret
.end_function
.endm

.section .text.x86_user_copy, "ax", @progbits
user_copy x86_user_copy, 0
user_copy x86_user_copy_smap, 1
```

两个变体放在同一节里前后相接，下一条用它们合起来的地址区间认出拷贝的故障。普通变体的
任何路径上都没有 `stac`/`clac`：在没有 SMAP 的 CPU 上它们是 #UD（Nehalem，这里 QEMU 的
默认型号，就是这样的 CPU）。C++ 一侧只剩按 `feature_has_smap` 二选一。原来 `UserAccessWindow`
上的说明（中断如何不继承 AC、拷贝自己的缺页如何恢复）搬到了 `.S` 的头注释。

**参照**
- Zircon `kernel/arch/x86/user-copy/user-copy.S`：同一个文件按 `-DSMAP` 汇编两次；
  `user-copy.cc` 用代码补丁在启动时选定变体。本项目不带 `.code-patches` 构建，所以是缓存的标志位。
- Linux `arch/x86/lib/copy_user_64.S`。

**验证**
- 构建通过；反汇编：`x86_user_copy` 只有 `rep movsq`/`rep movsb`，`stac`/`clac` 只出现在
  `x86_user_copy_smap` 里。
- 两个镜像在默认的 Nehalem 与 `CPU=max`（有 SMAP）下都启动正常，init↔shell 对话与通道检查照常。
- `CPU=max` 下在 `sys_debug_write` 里临时加一次对用户缓冲区的裸解引用（未提交），仍然报
  `potential SMAP violation: ring 0 touched user address 0x11ffe10 outside user_copy`。

**故意没做**
故障恢复本身在下一条。

## 用户拷贝的故障恢复：`data_fault_resume`（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/thread.hpp`、`kernel/arch/x86/kernel/task/thread.cpp` — `X86Thread` 加 `m_data_fault_resume` 与 `current_data_fault_resume()`
- `kernel/arch/x86/kernel/user_copy.S` — 第 4 个参数是槽的地址；拷贝前写槽、两个出口都清槽；故障出口；`x86_user_copy_begin`/`_end` 两个标签
- `kernel/arch/x86/kernel/user_copy.cpp` — 传槽、把 1 翻成 `InvArgs`；进出各有断言
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — `arch_copy_*` 返回 `Status`
- `kernel/arch/x86/mm/fault.cpp` — 调页失败后，满足四个条件就把 ip 改到槽里的地址
- `kernel/lib/user_copy/src/user_copy.cpp`、`include/user_copy/user_copy.hpp` — 把 arch 的结果传出去；契约改为"不可用也回 InvArgs"
- `docs/design/260923/userspace-interface.md` — 4.6 两处修订（见下）

**症状**
用户程序把一个落在用户半区、但没有映射的指针交给系统调用，内核在拷贝时缺页，调页失败，
`x86_handle_page_fault` 返回错误，内核 panic。任何程序一个 syscall 就能让内核停机。

**原因**
范围检查只能回答"这个地址是否属于用户可以指名的区间"，回答不了"这里现在有没有页"——后者
只有真去访问才知道，而且可能在检查之后被程序的另一个线程改变。所以拷贝必须允许失败，而失败
发生在一条内存访问指令的中间，唯一能接住它的是缺页处理。这就是 Zircon 的 `page_fault_resume`、
Linux 的异常表（`_ASM_EXTABLE_UA` + `fixup_exception()`）存在的原因。

**改法**
约定：拷贝例程在碰用户内存之前，把自己故障出口的地址写进当前线程的槽；两个出口都清槽。
缺页处理在调页失败之后检查：

```cpp
    auto resume_user_copy(arch::IrqFrame *const frame, VirtAddr const addr, bool const supervisor)
        -> bool {
        if (!supervisor || mm::is_kernel_address(addr)) {
            return false;
        }
        auto const resume = *task::X86Thread::current_data_fault_resume();
        auto const ip = frame->ip;
        auto const in_copy = ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
                             ip < reinterpret_cast<usize>(x86_user_copy_end);
        if (resume == 0 || !in_copy) {
            return false;
        }
        frame->ip = resume;
        return true;
    }
```

改前：

```cpp
    auto const status = task::Thread::Current::page_fault(addr, cause);
    if (Status::Ok != status) {
        return status;
    }
    return Status::Ok;
```

改后：

```cpp
    auto const status = task::Thread::Current::page_fault(addr, cause);
    if (Status::Ok == status) {
        return Status::Ok;
    }
    if (resume_user_copy(frame, addr, supervisor)) {
        return Status::Ok;
    }
    return status;
```

汇编一侧（每个变体各一份）：

```asm
    movq %rcx, %r10              // 槽的地址；%rcx 要给 rep movs
    leaq .Lfault_\name(%rip), %rax
    movq %rax, (%r10)            // 先写槽……
.if \smap
    stac                         // ……再开窗
.endif
    ... rep movsq / rep movsb ...
    xorl %eax, %eax
.Ldone_\name:
.if \smap
    clac
.endif
    movq $0, (%r10)
    ret
.Lfault_\name:
    movl $1, %eax
    jmp .Ldone_\name
```

四个条件缺一不可：内核态出错（用户态的故障是程序自己的）；地址在用户侧（拷贝的内核侧出错是
内核 bug，应当停机）；槽非零（确实在拷贝里）；出错指令在拷贝例程内。

**与设计 4.6 的两处偏离，已同步修订设计文字：**

1. **ip 必须落在拷贝例程区间内**。设计只写了"设置了 resume 就改 ip"。只看槽的话，在没有 SMAP
   的 CPU 上（那里内核随时能碰用户页），拷贝途中来一个中断，处理函数若碰了一个用户地址并出错，
   也会被送去拷贝的出口——而那时的栈是中断的栈，出口的 `ret` 就成了一次跳到任意地址。
   区间检查让"是谁的故障"由出错的指令本身来回答。
2. **两个宏变体而不是代码补丁**（上一条已做）。

两条不变式写在 `.S` 的头注释里，都只关乎这一个文件：设槽到清槽之间不 call、不动栈（所以出口
的 `ret` 回到调用者）；出口只用 `%r10` 和 `%rax`，而 `%r10` 在异常入口被存进帧、出口恢复。

槽按线程存而不是按 CPU：拷贝在一个惰性提交的页上缺页时可能阻塞、换 CPU 醒来，槽得跟着线程走。

失败不打日志：程序传坏指针是寻常事，系统调用返回的 `InvArgs` 已经说明了一切。

`user_copy.cpp` 进出各加断言：进入时槽必须为零（否则是拷贝里嵌套拷贝，内层清槽会让外层失去
恢复）；返回时槽为零、SMAP 窗口已关——故障出口平时走不到，这两条就是它的看守。

**参照**
- Zircon `x86_pfe_handler()`（`kernel/arch/x86/exceptions_c.cc`）末尾的 `page_fault_resume` 路径，
  `_x86_copy_to_or_from_user(dst, src, len, &thr->arch().page_fault_resume, ...)`
  （`kernel/arch/x86/user-copy/user-copy.cc`），以及 `user-copy.S` 的 `.Lfault_copy`。
- Linux 的异常表：`copy_user_64.S` 里的 `_ASM_EXTABLE_UA`，`fixup_exception()` 按出错 ip 查表——
  "按 ip 认领故障"这一点与本实现的区间检查同源。

**验证**
shell 里临时加了一组坏指针检查（下一条提交），在两个镜像、Nehalem 与 `CPU=max`、`SMP=4` 与
`SMP=1` 下跑：

```
shell: debug_write(unmapped) = InvArgs
shell: debug_write(kernel half) = InvArgs
shell: channel_write(from unmapped) = InvArgs
```

`0x7fff'ffff'e000` 在用户半区内但未映射，走的正是恢复路径；之后的系统调用和 `--- end ---`
照常出现，dbg_1 下新加的断言（槽与 AC）都没有触发。

**没能验证的：写进只读用户页的恢复。** 计划里的另一半检查是往 vDSO（只读）写。实测这些写
都**成功**了：用 QEMU monitor 在 shell 运行时看 `info tlb`，vDSO 的页表项是 `-G-DA--UW`——可写、
且是全局页。原因在 `X86Pte::make()`：它把 `X86_KERNEL_PAGE_FLAGS`（含 Writable 与 Global）OR
进每一个叶子项，不管调用者要什么权限。所以整个系统没有只读保护（内核代码段也可写），用户页也
都是全局页。这不是 user_copy 的问题，已另开会话交接（`x86_pagings.hpp`），并记入
`docs/repairs/260924/x86-kernel.md`。修好之前，"写只读页"这条恢复路径没有被实测。

（排查中先怀疑过 AP 蹦床没设 CR0.WP——`bootstrap16.S` 确实没设——但 monitor 显示每个 CPU 的
CR0 都是 WP=1、CD=0，说明之后有别处设好了；这个怀疑是错的，交接也已撤回。）

**故意没做**
- capture-faults 变体（在不能睡眠的上下文里读用户内存，给 futex 用）：P1 之后。
- 拒绝错误码里的 RSVD/PK/SS 位：计划列为不在 P1-A。

## 范围检查与拷贝之间加 `lfence`（Spectre V1 / V1.1）（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/user_copy.cpp` — `copy()` 在调用汇编之前 `lfence`

**症状**
潜伏。`user_copy::is_user_range()` 的范围检查是一个分支，拷贝在它被预测的那一侧先跑起来。

**原因**
- 从用户拷入（`copy_from_user`）：误预测时，源地址可以是内核地址，拷贝会推测地读内核内存，
  留下可测的缓存痕迹——Spectre V1。
- 拷出到用户（`copy_to_user`）：误预测时，目的地址可以是内核地址，拷贝会推测地写内核数据
  （返回地址、函数指针），后续的推测代码可能用上这些值——Spectre V1.1（推测写越界）。

`rep movs` 的地址来自调用者，没法像句柄查找那样用 `cmov` 夹住一个下标；`lfence` 让它之后的指令
在它之前的全部指令（包括那个检查）完成之前都不开始执行。

**改法**
改前：

```cpp
        DEBUG_ASSERT(*resume == 0, "a user copy inside a user copy");

        auto const faulted = feature_has_smap ? x86_user_copy_smap(dst, src, size, resume)
                                              : x86_user_copy(dst, src, size, resume);
```

改后：

```cpp
        DEBUG_ASSERT(*resume == 0, "a user copy inside a user copy");

        asm volatile("lfence" ::: "memory");

        auto const faulted = feature_has_smap ? x86_user_copy_smap(dst, src, size, resume)
                                              : x86_user_copy(dst, src, size, resume);
```

放在 arch 层而不是通用层的检查旁边：`lfence` 是 x86 的指令，其他架构有各自的屏障（arm64 的 CSDB）。

**参照**
Zircon `_arch_copy_to_or_from_user()`（`kernel/arch/x86/user-copy/user-copy.cc`）：`can_access()` 之后、
`_x86_copy_to_or_from_user()` 之前 `__asm__ __volatile__("lfence" ::: "memory")`，注释同样分 V1 与 V1.1。

**验证**
- 构建通过；反汇编 `arch_copy_from_user`：`lfence` 在取槽之后、调用 `x86_user_copy_smap`/`x86_user_copy` 之前。
- 两个镜像在 Nehalem 与 `CPU=max` 下都跑到 `shell: bad-pointer checks passed`。

**故意没做**
无。

## 拷贝里的故障只在"被放行的那个用户操作数"上才恢复（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/thread.hpp`、`kernel/arch/x86/kernel/task/thread.cpp` — 槽换成 `UserCopyState{resume, user_base, user_size}`，访问器改为 `current_user_copy()`
- `kernel/arch/x86/kernel/user_copy.cpp` — `copy()` 多一个 `user` 参数，拷贝前后填、清被放行的用户区间；改正一处 Zircon 函数名
- `kernel/arch/x86/mm/fault.cpp` — `inside_user_copy()` 改为 `user_copy_may_resume(frame, addr)`，多查 CR2 是否落在该区间

**症状**
审查发现（P1-A 第 1–16 条的对抗审查）：内核自己的一侧出错也会被恢复。比如
`copy_from_user(dst = nullptr, 合法的用户源, n)`：`rep movs` 写 0 地址缺页，CR2 在 [0, 2 MiB) 里，
按 `!is_kernel_address` 算是"用户地址"，出错指令又在拷贝例程里——于是被送去故障出口，系统调用
安静地返回 `InvArgs`。一个内核 bug 被伪装成了程序传了坏指针。第 15 条的记录说"拷贝的内核侧出错是
内核 bug，应当停机"，代码没有兑现。

**原因**
拷贝有两个操作数，只有一个是用户的。"地址不在内核半区"不等于"地址是这次拷贝的用户操作数"。
能区分二者的只有拷贝自己：它知道范围检查放行的是哪一段。

**改法**
改前：

```cpp
    auto inside_user_copy(arch::IrqFrame const *const frame) -> bool {
        auto const resume = *task::X86Thread::current_data_fault_resume();
        auto const ip = frame->ip;
        return resume != 0 && ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
               ip < reinterpret_cast<usize>(x86_user_copy_end);
    }
```

改后：

```cpp
    auto user_copy_may_resume(arch::IrqFrame const *const frame, VirtAddr const addr) -> bool {
        auto const &state = *task::X86Thread::current_user_copy();
        auto const ip = frame->ip;
        auto const in_routine = ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
                                ip < reinterpret_cast<usize>(x86_user_copy_end);
        auto const in_operand = addr - state.user_base < state.user_size;
        return state.resume != 0 && in_routine && in_operand;
    }
```

`copy()` 在调用汇编前填 `user_base/user_size`（`arch_copy_from_user` 传 `src`，`arch_copy_to_user` 传 `dst`），
返回后清零。拷贝里、但不在被放行区间的内核态故障，在调页之前就被拒绝，并打出专门的消息
"user copy faulted on {addr}, outside the user range it was admitted for"。

仍然管不到的一种：内核侧操作数恰好是一个**已映射**的用户地址——那根本不缺页。这只能靠 SMAP
（有 SMAP 时窗口内也允许，所以连 SMAP 也挡不住）。这是内核自己的 bug，不在这一条的范围内。

**参照**
Zircon 的 `page_fault_resume` 只有恢复地址，没有区间；区间检查是本内核的补充（设计 4.6 已修订的"指令须在拷贝例程内"之外的又一条）。
Linux 的异常表同样只按出错 ip 认领，不看地址。

**验证**
- 构建通过；两个镜像在 Nehalem 与 `CPU=max` 下都跑到 `shell: bad-pointer checks passed`（用户侧的故障仍然恢复）。
- 在 `sys_debug_write` 里临时 `copy_array_from_user(nullptr, 1)`（未提交）：
  `user copy faulted on 0x0, outside the user range it was admitted for`，随后内核停机——改前是静默返回 `InvArgs`。

**故意没做**
无。

## 勘误：P1-A 第 1–16 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「拷贝改为一段汇编」一节写 Zircon 的 `user-copy.S` "同一个文件按 `-DSMAP` 汇编两次；`user-copy.cc` 用代码补丁在启动时选定"。
  都不准：它按 MOVSB × SMAP 汇编成**四个**变体（movsq、movsq_smap、movsb、movsb_smap，见 `user-copy/BUILD.gn`），
  `SelectX86UserCopyAlternative()` 在 ERMS/FSRM/AMD Zen 上选 `rep movsb`、有 SMAP 时选 \_smap，补丁由
  `kernel/arch/x86/code-patches/code-patches.cc` 打上，不在 `user-copy.cc`。本项目只有两个 SMAP 变体、都是四字拷贝加字节尾，
  没有按 ERMS/FSRM 选 `rep movsb` 的那一半。`user_copy.S` 的注释已改正。
- 「范围检查与拷贝之间加 `lfence`」一节引用的 `_arch_copy_to_or_from_user()` 不存在；Zircon 的函数叫
  `_arch_copy_to_from_user()`（模板，`user-copy.cc`），`lfence` 在它的 `can_access()` 之后。代码注释已改正。
- 「用户拷贝的故障恢复」一节说"拷贝的内核侧出错是内核 bug，应当停机"，但当时的代码按 `!is_kernel_address` 判断，
  0 地址等内核侧故障会被当作用户地址恢复成 `InvArgs`。已由「拷贝里的故障只在被放行的那个用户操作数上才恢复」一节改正。
