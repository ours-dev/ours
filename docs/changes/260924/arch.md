# arch 改动记录（2026-09-24）

## 抗推测执行的两个原语：`confine_array_index` 与 `conditional_select_nospec_eq`（2026-09-24）

**文件**
- `kernel/lib/arch/include/arch/nospec.hpp`（新建）— 语义说明与按架构分派；x86 以外 `#error`
- `kernel/arch/x86/lib/arch/include/arch/x86/nospec.hpp`（新建）— x86 实现，`cmp` + `cmov`
- `kernel/lib/arch/BUILD.gn` — 头文件登记

**症状**
暂无使用者；是第 12、13 条（句柄查找、系统调用分发表下标）的前置。

**原因**
普通的边界检查是一次条件跳转，而跳转会被预测：比较还没算完，CPU 已经沿预测的一侧拿着
未经检查的值往下执行；那条路径上的访存在缓存里留下痕迹，误预测回滚之后痕迹还在。下标由用户
决定时，这就是一次任意内核字节的读取，一次一条缓存行（Spectre V1，bounds check bypass）。

解决办法是把"检查"从控制依赖换成数据依赖：结果由条件传送（`cmov`）产生，而 `cmov` 要等它
依赖的比较结果出来才能执行，所以任何推测路径都拿不到未经检查的值。它们不取代原来的检查——
架构路径上报错的仍是那个检查——只是在推测路径上兜底。

必须用内联汇编：`x == y ? a : b` 在 clang -O2 下可以、并且确实会被编成跳转，编译器对"推测"
一无所知。

**改法**
新增，没有改前。x86 实现：

```cpp
inline auto confine_array_index(usize const index, usize const size) -> usize {
    usize safe_index = 0;
    asm("cmp %1, %2\n\t"
        "cmova %1, %0"
        : "+r"(safe_index)
        : "r"(index), "r"(size)
        : "cc");
    return safe_index;
}

inline auto conditional_select_nospec_eq(usize const x, usize const y, usize const a,
                                         usize const b) -> usize {
    usize select = a;
    asm("cmp %1, %2\n\t"
        "cmovnz %3, %0"
        : "+r"(select)
        : "r"(x), "r"(y), "r"(b)
        : "cc");
    return select;
}
```

`confine_array_index`：`cmp` 算 size − index，`cmova` 只在"above"（无符号大于，即 size > index）
时把 index 搬进结果；比较未决时结果仍是初值 0。

ARM 与 RISC-V 分支写 `#error` 而不是猜一个实现：arm64 需要条件选择加 CSDB 屏障，而
`out/bringup.x86` 编不到它们，写了也无法按每条提交验证。

**参照**
Zircon `fbl::confine_array_index()`（`system/ulib/fbl/include/fbl/confine_array_index.h`）与
`fbl::conditional_select_nospec_eq()`（`fbl/conditional_select_nospec.h`），逐条指令照搬。
Intel "Analyzing potential bounds check bypass vulnerabilities" 5.2 节；AMD "Software techniques
for managing speculation" V1-2。

**验证**
树里还没有 include 它的地方，所以用内核自己的编译命令（取自 `compile_commands.json` 里
`table.cpp` 那一条）编了一个探针 TU，反汇编：

```
<probe_confine>:  cmpq %rdi, %rsi ; cmovaq  %rdi, %rax
<probe_select>:   cmpq %rdi, %rsi ; cmovneq %rcx, %rax
```

没有条件跳转。整体构建通过。

**故意没做**
没有照 Zircon 再加 `conditional_select_nospec_lt`：本项目还没有需要它的地方。
