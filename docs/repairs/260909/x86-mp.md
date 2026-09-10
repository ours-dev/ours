# x86-mp —— 待修项（2026-09-09）

AP 已经能起来并接线程了（见 `docs/changes/260909/x86-mp.md`），下面是没修的。

## 1. INIT-SIPI-SIPI 只发了一次 SIPI

**严重程度**：中，真机上才会咬人。

`kernel/arch/x86/kernel/mp.cpp` 的 `x86_bring_aps()`：先给所有目标发 INIT，
睡 10 ms，然后每颗 CPU 发**一次** Startup IPI。Intel SDM（§9.4.4 的 MP 初始化协议）
要求的是 INIT → 10 ms → SIPI → 200 µs → **第二次 SIPI**，第二次是给第一次没被接住的
处理器兜底的。QEMU 一次就起来，真机不保证。

同时该看的：INIT 之后没有等 ICR 的 Delivery Status 清零就往下走
（`XApic::wait_for_ipi()` 只在 `send_ipi()` 里做，看着是够的，但值得对一遍）。

**参照**：Linux `wakeup_secondary_cpu_via_init()` / `wakeup_cpu_via_init_nmi()`。

## 2. `arch_activate_cpu()` / `arch_deactivate_cpu()` 返回 `Unimplemented`

**严重程度**：低（没有调用者）。CPU 热插拔还不存在，留着是脚手架。

## 3. 跳板里 `mov %ds, %ebx` 依赖 16 位下写 32 位寄存器会零扩展

`kernel/arch/x86/kernel/bootstrap16.S`：

```asm
    mov %ds, %ebx
    shl $4, %ebx
```

`MOV r32, Sreg` 的高 16 位在 Intel 手册里是 undefined（部分处理器补零，部分保留原值），
而进入长模式时 `%rbx` 的高 32 位也不是架构保证为零的。QEMU 上没问题。
稳妥写法是 `movzwl %ds, %ebx`，并在长模式段开头 `mov %ebx, %ebx` 把高位显式清掉。

## 4. AP 仍然一颗一颗串行唤醒

**严重程度**：低，冷路径。

`x86_bring_aps()` 每颗 AP 要付一次 1 ms 的 `sleep_for` 加一段无上界的
`while (!Cpu::is_online(cpu));`，然后才发下一颗的 SIPI。
`mp_max_cpus = 16` 时是 16 ms 左右的一次性引导开销。

现在唯一逼着串行的是跳板数据页里那一组 `cpunum` / `stack_top` / `thread`
（2026-09-10 从 `percpu[]` 表改回单槽时是有意选的：一次只起一颗本来就是当时的协议）。
真要并行，AP 得自己从 APIC ID 反推出 cpunum——`apic_id_to_cpunum()`
（`kernel/arch/x86/apic/local_apic.cpp`）已经有了——然后填满所有槽、一次发完所有 SIPI、
对整个掩码等一次。

## 5. AP 与 BSP 的 arch／generic 边界是反的

**严重程度**：低，结构问题。

- BSP：generic 驱动 arch —— `start_kernel()` 调 `init_arch_early()`，
  而 `Cpu::State::Online` 是在 **arch** 代码（`kernel/arch/x86/kernel/init.cpp`）里发布的。
- AP：arch 驱动 generic —— `x86_start_nonboot_cpu()` 做完 per-CPU 的事再尾调
  `start_nonboot_cpu()`，`Online` 在 **generic** 代码（`kernel/top/main.cpp`）里发布。

于是"这颗 CPU 起来了"这件事写在两个文件、两个层次上，只能靠注释解释这个分法。
签名也在强化它：`start_nonboot_cpu()` 不收参数、自己去问 `Cpu::id()`，
而它的调用者两帧之前刚被显式告知过；`init_arch_early()` 则把 `x86_init_percpu(0)` 写死。

**怎么修**：让跳板的远端入口直接是 generic 的 `start_nonboot_cpu(CpuNum, Thread *)`，
arch 那部分收成一个钩子 `init_arch_early_percpu(cpunum)`
（装今天 `CpuLocal::init_percpu` + `init_local_apic_percpu` + `x86_init_percpu` 那三步），
对称于 `start_kernel` → `init_arch_early`。这样 `init_arch_early()` 变成
`init_arch_early_percpu(0)` 加 BSP 专属的 IDT/MMU 步骤，`Cpu::set_state(Online)`
两条路径共用一处，`x86_start_nonboot_cpu` 缩回它本该是的 ABI 垫片。
