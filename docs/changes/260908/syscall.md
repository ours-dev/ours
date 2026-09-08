# syscall 改动记录

## 系统调用入口帧有了类型，帧指针从 `%rbp` 改成 `%rsp`（2026-09-08）

**文件**
- `kernel/lib/arch/include/arch/syscall.hpp` — 新文件，按架构分派
- `kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp` — 新文件，`arch::SyscallFrame`
- `kernel/arch/x86/lib/arch/include/arch/x86/macro/syscallframe.hpp` — 新文件，汇编与 C++ 共享的偏移
- `kernel/arch/x86/kernel/syscall.S` — 帧指针改用 `%rsp`，并按上面的偏移注释
- `kernel/lib/syscall/table.cpp` — 处理函数签名由 `void *` 改为 `arch::SyscallFrame *`
- `kernel/arch/x86/BUILD.gn`、`kernel/lib/arch/BUILD.gn` — 登记新头文件

**症状**
两件事，第二件是可被用户态利用的。

其一：树里**没有任何类型描述 `syscall.S` 压出来的寄存器帧**。`table.cpp` 自己的注释
承认了这一点，并且诚实地选了 `void *` 而不是猜一个：

```cpp
    /// The frame is deliberately opaque here: no type describes that push
    /// order anywhere in the tree yet, and guessing at one would be worse than
    /// admitting it.
    using SyscallHandler = auto (*)(void *frame) -> void;
```

其二：那个 `void *` 指向的地方本身就是错的。

```
    mov %rbp, %rdi  // Pass pointer pointed to SyscallFrame.
```

**原因**
AT&T 语法是 `mov 源, 目的`，所以这一行把 `%rbp` 送进 `%rdi`。但帧是**压在栈上**的，
十七次 push 之后帧顶就是 `%rsp`；`%rbp` 是这个 stub 自己刚刚压进帧里的那个
**用户态可控的值**。

于是每一个系统调用处理函数拿到的"帧指针"，都是用户自己选的一个数。而处理函数是靠
**写 `frame->ax`** 来返回结果的（stub 事后把整个帧弹回寄存器，C++ 返回值会被覆盖），
所以这不是读错几个寄存器的问题——它是一个内核任意地址写原语：用户在 `%rbp` 里放一个
内核地址，发一次系统调用，内核就替它往那里写。

`exception.S` 里的 `interrupt_common` 一直传的是 `%rsp`，正是同样的道理。

之所以一直没炸，是因为在此之前没有任何东西真的执行过 `syscall`。

**改法**
1. 汇编与 C++ 共享一组偏移宏（和 `arch/x86/macro/irqframe.hpp` 同样的安排）：

   ```c
   #define X86_SYSCALLFRAME_OFFSET_AX (0 * X86_SYSCALLFRAME_OFFSET_SCALE)
   #define X86_SYSCALLFRAME_OFFSET_BX (1 * X86_SYSCALLFRAME_OFFSET_SCALE)
   ...
   #define X86_SYSCALLFRAME_SIZE (17 * X86_SYSCALLFRAME_OFFSET_SCALE)
   ```

   顺序是 push 顺序的**逆序**——栈向下长，最后压进去的 `%rax` 在偏移 0。

2. `arch::SyscallFrame` 按这组宏 `static_assert` 自己：

   ```cpp
   static_assert(offsetof(SyscallFrame, ax) == X86_SYSCALLFRAME_OFFSET_AX);
   ...
   static_assert(sizeof(SyscallFrame) == X86_SYSCALLFRAME_SIZE);
   ```

   于是在 `syscall.S` 里增删或调换一次 push 而不改这个头，会变成**编译失败**，而不是
   运行期读错寄存器。

   这个帧**不是**少了几个字段的 `IrqFrame`，区别来自 SYSCALL 指令而不是设计选择：
   SYSCALL 什么都不压栈，它把返回地址放进 `%rcx`、RFLAGS 放进 `%r11`（顺手毁掉调用者
   原本放在那里的东西），并且让 `%rsp` 仍指向**用户栈**。所以帧里没有 CS/SS
   （SYSRET 从 IA32_STAR 重装两者）、没有错误码、没有向量号；`ip`/`flags` 来自
   `%rcx`/`%r11` 而不是栈。那两个寄存器原本的用户值在 stub 第一条指令执行时就已经
   没了，所以对应的槽位压的是**字面量 0**——存进被毁的值只会诱使处理函数去相信它。

   用户栈指针同样不在帧里：stub 必须先换栈才有地方可压，所以它把用户 `%rsp` 停在本
   CPU 的 percpu 块里（`X86_PERCPU_SAVED_USER_SP_OFFSET`）。

3. 帧指针改对：

   改前
   ```
       mov %rbp, %rdi  // Pass pointer pointed to SyscallFrame.
   ```
   改后
   ```
       mov %rsp, %rdi
   ```

4. `table.cpp` 的签名跟着变成 `auto (*)(arch::SyscallFrame *) -> void`，
   `sys_not_implemented()` 也从"什么都不做"改成写 `frame->ax = Status::Unimplemented`
   ——原先它连一个错误码都不给，用户态收到的是自己传进去的 `%rax`。

**参照**
- `Zircon x86_syscall_general_regs_t`（`zircon/kernel/arch/x86/include/arch/x86/general_regs.h`）：
  同样由 `syscall.S` 的 prologue 填，同样不含 CS/SS。
- 本树 `arch/x86/macro/irqframe.hpp` + `arch::IrqFrame`：同样的"宏定偏移、结构体
  static_assert"安排，这次只是把它复制到系统调用这一侧。
- 本树 `exception.S` 的 `interrupt_common`：帧指针一直是 `%rsp`。

**验证**
`ninja` 通过（`static_assert` 本身就是对 push 顺序的验证）；QEMU 实测
`kernel.dev.dbg_1.obi` 无回归，userboot 照旧进入 ring 3，30 秒内 901 次
`Exception[interrupt] from user space`。

**行为**上这一步还验证不到：userboot 目前不发系统调用，`x86_syscall` 一次也没执行过。
下一步补上边界检查、再下一步接上真正的调用之后，才谈得上实测——那时这两条修正正是
"能不能活着回来"的前提。

**故意没做**
- **`%rax` 还没有做边界检查**。`call *(%r11, %rax, 8)` 里的 `%rax` 完全由用户控制，
  表只有 256 项，越界即越表间接调用。这是独立的一条，下一个提交单独修。
- **仍然没有系统调用编号，也没有真实处理函数**。全部 256 个槽位一律
  `Unimplemented`。
- **arm / riscv 的 `arch/<arch>/syscall.hpp` 没有建**。`arch/syscall.hpp` 里给它们留了
  分派分支，真有那两个架构的启动路径时再补；现在建只是两份没人编译的死代码。

## 用户可控的 `%rax` 没有边界检查，越表间接调用（2026-09-08）

**文件**
- `system/public/ours/syscall.hpp` — 新文件，系统调用 ABI（本次只有编号总数）
- `system/public/public_headers.gni` — 登记
- `kernel/arch/x86/kernel/syscall.S` — 加入范围钳制
- `kernel/lib/syscall/table.cpp` — 表尾多一个哨兵槽位

**症状**
潜伏，且是可从用户态直接利用的那一类。

```
    lea SYSCALL_TABLE(%rip), %r11
    call *(%r11, %rax, 8)
```

`%rax` 完全由用户态提供，表只有 256 项，而这条 `call` 自己没有任何上限。也就是说
`syscall` 带一个足够大的 `%rax`，就是一次通过 `SYSCALL_TABLE` 之后**任意 `.data`
内容**的间接调用，偏移由调用者挑。

**原因**
表的注释里已经写明"每个槽位都必须是有效目标——空指针项会变成从用户态经零地址的间接
调用"，也就是说"`%rax` 不可信"这件事是想到过的，但防的是**表内**的空洞，没有防
**表外**。索引的上界和槽位的有效性是两个独立条件，只补了后者。

**改法**
钳制，不是跳过。表被做成比 `OURS_SYSCALL_COUNT` 多一项，多出来的那一项回答
`Status::OutOfRange`；超范围的编号被改写到这个下标上，然后和别的编号一样被分发。

```
    mov $OURS_SYSCALL_COUNT, %r11
    cmp %r11, %rax
    cmovae %r11, %rax
```

编译出来的是：

```
    8131: 49 c7 c3 00 01 00 00   movq $0x100, %r11
    8138: 4c 39 d8               cmpq %r11, %rax
    813b: 49 0f 43 c3            cmovaeq %r11, %rax
```

为什么不用条件跳转绕开 `call`：那样返回值就得**在汇编里**写，也就是要在 `.S` 文件里
手抄一个 `Status` 的数值，而没有任何东西会去和枚举核对——本文件上一节刚记过一次
"两边约定却无人校验"的教训。哨兵槽位是个普通的 C++ 函数，`Status::OutOfRange` 由编译
器填。顺带的好处是这条路径上没有分支可供用户态训练。

钳制会破坏 `%rax`，这是安全的：它的用户值早已在帧里（它是最后一次 push），出口从帧
里恢复。

编号总数放在 `system/public/ours/syscall.hpp`，因为这是 ABI——两边都得同意。这个头
写成汇编器读得懂的样子（数值是宏，C++ 那份是镜像），`syscall.S` 直接 include 它，
省去了在汇编里再写一遍 256 的机会。

**参照**
`Zircon x86_syscall` 的 `cmp $ZX_SYS_COUNT, %rax; jae ...`：同样的检查，Zircon 用
条件跳转到一个返回 `ZX_ERR_BAD_SYSCALL` 的桩；这里用哨兵槽位加 `cmov`，理由见上。

**验证**
`ninja` 通过；`llvm-objdump` 确认三条指令按预期发出（见上）；QEMU 实测
`kernel.dev.dbg_1.obi` 无回归，910 次 `Exception[interrupt] from user space`。

超范围这条路径**本次仍未被真正执行过**——userboot 还不发系统调用。下一步接上真实调用
之后可以顺手发一次越界编号来实测。

**故意没做**
没有给"编号存在但未实现"和"编号不存在"之外的情况再分级。Zircon 还会区分
`ZX_ERR_BAD_SYSCALL` 与权限不足等，那要等真的有编号和权限检查之后才有意义。
