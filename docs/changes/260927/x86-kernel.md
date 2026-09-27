# x86-kernel — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. user copy 支持 fault capture（2026-09-27）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/thread.hpp` — `UserCopyState::RUN_FAULT_HANDLER`
- `kernel/arch/x86/kernel/user_copy.S` — 第五个参数 `resume_mask`；出错出口带回缺页地址和原因
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — `arch_copy_{from,to}_user_capture_faults()` 与结果类型
- `kernel/arch/x86/kernel/user_copy.cpp` — 按 mask 分成两种拷贝
- `kernel/arch/x86/mm/fault.cpp` — `x86_handle_page_fault()` 的 capture 分支

**症状**

缺一种能力，不是 bug。要在持锁时直接拷用户内存的调用方——`VmoDispatcher::read_user()` /
`write_user()` 去掉中转页之后就是——没有办法在拷贝缺页时安全地退出。

**原因**

持着对象的锁拷用户内存，用户那边缺页时不能就地解析：

- 解析要拿地址空间的锁，而缺页路径的锁序是"地址空间 → VMO"，这里变成了"VMO → 地址空间"，锁序反了；
- 用户缓冲区可能正好映射着被锁住的那个 VMO，那就是自己等自己；
- 调用方持的若是自旋锁，解析可能阻塞，根本不许发生。

中转页绕开了这个问题（先在锁里拷到内核页、放锁、再拷给用户），但每页多一次 memcpy，已被否决。
正确的做法是 Zircon 的 fault capture：拷贝**不解析**缺页，只把缺页地址和原因带回来；调用方放锁、
用 `soft_fault()` 补页、重新拿锁、按索引重新查页，从已有进度继续。

**改法**

标志位藏在恢复地址的第 63 位里。内核地址空间从 `KERNEL_ASPACE_BASE` 一直延伸到顶，
每个内核地址的第 63 位都是 1，所以清掉它就能当"别跑缺页处理程序"的标志，处理程序把它补回来
就还原出规范地址：

```cpp
static constexpr VirtAddr RUN_FAULT_HANDLER = VirtAddr(1) << 63;
```

汇编改前：

```asm
    movq %rcx, %r10
    leaq .Lfault_\name(%rip), %rax
    movq %rax, (%r10)
    ...
.Lfault_\name:
    movl $1, %eax
    jmp .Ldone_\name
```

改后：

```asm
    movq %rcx, %r10
    leaq .Lfault_\name(%rip), %rax
    andq %r8, %rax                  // resume_mask：capture 时清掉第 63 位
    movq %rax, (%r10)
    ...
.Lfault_\name:
    shlq $32, %rcx                  // 处理程序放进 rcx 的 cause，挪到 rax 高半
    movl $1, %eax                   // faulted = 1
    orq %rcx, %rax
    jmp .Ldone_\name                // rdx 里已经是缺页地址
```

返回值从 `u32` 变成 16 字节的结构，SysV 调用约定用 `%rax:%rdx` 返回：

```cpp
struct X86UserCopyRet {
    u32 faulted;
    u32 cause;
    ours::VirtAddr va;
};
static_assert(sizeof(X86UserCopyRet) == 16 && offsetof(X86UserCopyRet, cause) == 4 &&
              offsetof(X86UserCopyRet, va) == 8, "...");
```

处理程序故意把地址写进 rdx、把原因写进 rcx：rdx 正好是返回结构的第二个 8 字节，出错出口只需把 rcx 挪进
rax 的高半。

缺页处理程序新增的分支，放在"不可睡就停机"那道检查**之前**：

```cpp
if (supervisor && user_address) {
    DEBUG_ASSERT(user_copy_may_resume(frame, addr));
    using UserCopyState = task::X86Thread::UserCopyState;
    auto const resume = task::X86Thread::current_user_copy()->resume;
    if ((resume & UserCopyState::RUN_FAULT_HANDLER) == 0) {
        frame->ip = resume | UserCopyState::RUN_FAULT_HANDLER;
        frame->dx = addr;
        frame->cx = static_cast<usize>(cause);
        return Status::Ok;
    }
}
```

必须在那道检查之前：capture 拷贝恰恰是唯一一种可以在原子上下文里合法缺页的内核代码——它事先声明了
"别替我解析"。这条路不开中断、不拿锁、不阻塞。能走到这里的内核态用户地址缺页，已经由函数开头的检查
保证是在拷贝例程内、且落在准许的用户范围内，所以原有的"准许范围"设计保持不变。

C++ 侧：`copy()` 多一个 `resume_mask` 参数，返回原始结构；原有的 `arch_copy_from_user()` /
`arch_copy_to_user()` 用 `RESOLVE_FAULTS`（全 1）调用，行为不变；新增的 capture 版用
`CAPTURE_FAULTS`（清第 63 位）调用，返回 `UserCopyCaptureResult{status, std::optional<UserCopyFault>}`。
另有一条编译期断言保证借位无损：

```cpp
static_assert((KERNEL_ASPACE_BASE & UserCopyState::RUN_FAULT_HANDLER) != 0,
              "RUN_FAULT_HANDLER must be set in every kernel address");
```

capture 返回时不报告已拷字节数，所以调用方重试的是**整次**拷贝请求（按页切块的话就是这一块），
与 Zircon 一致。

**参照**

`Zircon arch/x86/user-copy/user-copy.S`（`andq %r8, %rax`、`.Lfault_copy` 里 `shlq $32, %rcx`）、
`_arch_copy_to_from_user()`（`user-copy.cc`）、`x86_pfe_handler()` 开头对
`X86_PFR_RUN_FAULT_HANDLER_BIT` 的判断（`exceptions_c.cc`）、`X86_PFR_RUN_FAULT_HANDLER_BIT` 的定义与
静态断言（`arch/x86/include/arch/arch_thread.h`）。Linux 的对应物是 `pagefault_disable()` 加
`__copy_from_user_inatomic()`，但它只报失败、不带回缺页地址。

**验证**

构建通过；`clang-format` 跑过四个 C++ 文件，`clang-tidy -p out/bringup.x86` 跑过 `user_copy.cpp` 与
`fault.cpp`，无新告警。两个镜像各跑 50 秒，零错误零断言；原有拷贝全部走 `RESOLVE_FAULTS`，行为不变。

capture 目前没有调用方，用临时探针验证：每当一次普通拷贝失败，就**持着一把自旋锁、关着中断**，用 capture
模式把同一次拷贝重做一遍（`kernel.dev.dbg_1.obi`）：

```
PROBE capture#1: user=0x7fffffffe000 size=8 -> faulted=1 va=0x7fffffffe000 cause=0x2
PROBE capture#2: user=0x7fffffffe000 size=8 -> faulted=1 va=0x7fffffffe000 cause=0x2
PROBE capture#3: user=0x7fffffffe000 size=1 -> faulted=1 va=0x7fffffffe000 cause=0x3
```

`0x2` 是 `Absent`（8 字节的 `copy_from_user`，读），`0x3` 是 `Write | Absent`（1 字节的
`copy_to_user`，写）。三次都带回了准确的缺页地址和原因；在原子上下文里没有触发"不可睡就停机"，
说明 capture 分支在那道检查之前返回；也没有 `#GP`，说明第 63 位去掉又补回之后，出错出口的地址是规范的。
探针已撤掉。

**故意没做**

- `lib/user_copy` 的通用接口（`UserOutPtr::copy_to_user_capture_faults()` 之类）还没加，下一步。
- `VmoDispatcher::read_user()` / `write_user()` 还在用中转页，第三步去掉。
- 探针顺带看到：`0x7fffffffe000` 这个地址的读写一直解析不了（`Absent`），每次启动都有系统调用在用户栈顶
  之上拷贝。这更像是用户栈布局或某个调用方传错了地址，不属于这一条，没有追。

## 捕获式用户拷贝的 fault 字段由 std::optional 改为 ustl::Option（2026-09-27）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp`：`UserCopyCaptureResult::fault` 改为 `ustl::Option<UserCopyFault>`
- `kernel/arch/x86/kernel/user_copy.cpp`：`std::nullopt` 改为 `ustl::NONE`
- `kernel/mm/vm_object_paged.cpp`：`read_write()` 里的 `fault` 和 `retried_at` 改为 `ustl::Option`
- `kernel/commands/user_copy_test.cpp`：`has_value()` 改为 `is_some()` / `is_none()`

**症状**
潜伏，当前触发不到。现有的每次解引用之前都先检查过 `has_value()`。

**原因**
本构建把 libc++ 的 hardening 设成了 NONE（`build/config/clang/include/__config_site`）。
对空的 `std::optional` 做 `*` 或 `->`，`_LIBCPP_ASSERT_VALID_ELEMENT_ACCESS`
不做任何检查，结果是未定义行为：直接读未初始化的存储，然后继续往下跑。
`std::optional` 只有 `.value()` 有检查，会经 `__libcpp_verbose_abort` 走到 `ours::panic`。

`ustl::Option` 的 `value()`、`*`、`->` 都会先调 `require_some()`，为空就
`__builtin_trap()`。漏掉一次检查，它会当场停下，而不是把垃圾值（这里是一个
错误的缺页地址）交给 `soft_fault()`。

之前选 `std::optional`，依据的是"std 和 ustl 都有时用 std"的约定。那条约定的
前提是 ustl 的版本没有增加价值，而在关掉 hardening 的构建里，这个前提对
optional 不成立。

**改法**

之前：

```cpp
std::optional<UserCopyFault> fault;
...
if (retried_at == fault->va && retried_done == done) {
retried_at = fault->va;
```

之后：

```cpp
ustl::Option<UserCopyFault> fault;
...
if (retried_at.is_some() && *retried_at == fault->va && retried_done == done) {
retried_at = ustl::some(fault->va);
```

`ustl::Option` 没有和值比较的 `operator==`，所以要先显式判断 `is_some()`，
读起来也更直白。

**参照**
Zircon 的 `UserCopyCaptureFaultsResult` 用的是 `ktl::optional`（即 `std::optional`）。
Zircon 开启了 libc++ hardening，空访问会被断言拦下；本项目没开，所以没有照搬。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：两个镜像的 `usercopy.test` 都是 18/18，无 panic。

**故意没做**
内核其它地方的 `std::optional` 没有顺手一起换，每处单独处理。

## 更正：`0x7fffffffe000` 的缺页是 shell 自测故意制造的，不是 bug（2026-09-27）

**文件**
无代码改动，只更正本文件"让用户拷贝能捕获缺页"一节末尾的猜测，以及
`260926/mm.md`、`260926/x86-mm.md` 里探针日志的来历。

**症状**
那几节的探针日志显示，每次启动都有内核态的用户拷贝在 `0x7fffffffe000` 上缺页，而且解析不了
（`NotFound`，原因为 `Absent`）。当时猜测是"用户栈布局或某个调用方传错了地址"，但没有追下去。

**原因**
地址是 shell 故意传的。`examples/shell/shell.cpp:573`：

```cpp
constexpr auto UNMAPPED = ours::VirtAddr{0x7fff'ffff'e000};
```

它在用户半区里，所以能通过 `is_user_range()` 的范围检查，但没有映射，拷贝一碰就缺页、解析失败，
最后走数据缺页恢复返回 `InvArgs`。shell 用它测试每个接受用户指针的系统调用，是否都能把
"在范围内但没映射"的指针正确拒绝。与之配对的 `KERNEL` 地址测的是另一条路径：被范围检查
直接拒绝，根本不会发起拷贝。

**改法**
无。确认方法：在 `x86_pfe_handler` 的"内核态访问用户地址、解析失败、转去拷贝的恢复点"分支里，
临时加一行日志，记录地址、原因、状态和 ip，然后启动 dev。每一行都紧跟着一条 shell 自测输出，
一一对应：

```
DIAG unresolved user-copy fault: addr=0x7fffffffe000 cause=0x2 status=4 ...
shell: debug_write(unmapped) = InvArgs
DIAG unresolved user-copy fault: addr=0x7fffffffe000 cause=0x2 status=4 ...
shell: channel_write(from unmapped) = InvArgs
DIAG unresolved user-copy fault: addr=0x7fffffffe000 cause=0x3 status=4 ...
shell: channel_read(into unmapped) = InvArgs
...
DIAG unresolved user-copy fault: addr=0xa00000 cause=0x1 status=20 ...
```

最后一条的 `0xa00000` 是 vDSO，对应"读进 vDSO"那项测试（`AccessDenied`）。探针用完已撤掉，
`fault.cpp` 与提交版本一致。

前两次探针没复现，是因为启动时没往 stdin 送命令，shell 的坏指针自测没跑到那一步。

**参照**
无。

**验证**
如上，dev 启动一次，DIAG 行与 shell 自测一一对应。

**故意没做**
shell 自测本身不改。
