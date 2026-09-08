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
