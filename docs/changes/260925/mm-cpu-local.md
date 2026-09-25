# mm/CpuLocal 改动记录（2026-09-25）

## `CpuLocal::ScopedInstall`：在 %gs 不可信的入口临时装上本 CPU 的基址（2026-09-25）

**文件**
- `kernel/mm/include/ours/mm/cpu_local.hpp` — 新增嵌套类 `CpuLocal::ScopedInstall`
- `kernel/arch/x86/kernel/include/ours/arch/cpu_local.hpp` — 新增 `ArchCpuLocal::installed()`，直接从 MSR 读出基址寄存器当前的值

**症状**
本条只加接口，行为上的症状和验证见 `x86-kernel.md` 里同日的"NMI、#DF、#MC 不再按 CS 决定 %gs"。

**原因**
x86 的 NMI/#MC 能落在 swapgs 成对指令的中间，那时 %gs.base 可能还是用户的。处理这种入口要做的是：
先把基址寄存器里现有的值存下来，换成本 CPU 的值，处理完再写回。"本 CPU 的值"是 `CPU_OFFSETS[cpu]`。

最直接的写法是让汇编 stub 自己去读 `CPU_OFFSETS`。那样 CpuLocal 的内部表就被 arch 的汇编直接访问，
CpuLocal 的封装就破了——"基址寄存器里装什么"应当只有 CpuLocal 自己知道。所以这件事做成 CpuLocal 的接口，
调用方只提供"我是哪颗 CPU"（x86 从 TR 得到，见 x86 那条）。

现有的 `offset_of()` 不能直接用，原因有两个：它是私有的；而且它断言"这颗 CPU 已经有 unit"。
`CpuLocal::init_early()` 之前 BSP 本来就合法地以偏移 0 运行在模板上，那时来一个 NMI 就会触发这个断言。

改前：没有这样的接口，`CPU_OFFSETS` 只经由 `offset_of()`（私有、带断言）读：

```cpp
    static auto offset_of(CpuNum cpunum) -> isize {
        DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU Number");
        DEBUG_ASSERT(Self::CPU_OFFSETS[cpunum] != 0,
                     "CPU[{}] has no per-CPU unit; resolving would land in the template", cpunum);
        return Self::CPU_OFFSETS[cpunum];
    }
```

**改法**
改后：

```cpp
class CpuLocal::ScopedInstall {
  public:
    explicit ScopedInstall(CpuNum cpu) : m_saved(ArchCpuLocal::installed()) {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU number: {}", cpu);
        ArchCpuLocal::install(CpuLocal::CPU_OFFSETS[cpu]);
    }
    ~ScopedInstall() {
        ArchCpuLocal::install(m_saved);
    }
  private:
    isize m_saved;
};

// ArchCpuLocal (x86)
static auto installed() -> isize {
    return ::arch::MsrIo::read<isize>(::arch::MsrRegAddr::IA32GsBase);
}
```

- **还原旧值，而不是判断"应该是什么"再写回。** 这样一条路径就能覆盖所有窗口：被打断的代码不管是在内核里用着内核的 %gs，
  还是停在一对 swapgs 中间用着用户的 %gs，回去时拿到的都是它原来的值，它接下来那条 swapgs 照样配对。
  swapgs 的另一半（`KERNEL_GS_BASE` 里换出去的值）从头到尾不碰。
- **构造和析构都不碰 per-CPU 存储。** `CPU_OFFSETS` 按普通地址读，其余只动寄存器本身，所以 %gs 还错着的时候就能用——这正是它存在的意义。
  `installed()` 同理：读的是 MSR，而不是 `CURRENT_CPU_OFFSET`（后者要 %gs 已经正确才读得到）。
- 被否掉的方案：
  - 汇编直接读 `CPU_OFFSETS`：破坏封装，见上。
  - 把 GS 值缓存在 IST 栈顶，由 `x86_setup_tss_percpu()` 通过 CpuLocal 的接口写入：也不破坏封装，但那是 `CPU_OFFSETS` 的第二份副本，
    将来 CPU 热插拔等改动某颗 CPU 的偏移时会过期；让 CpuLocal 保持唯一来源更稳。
  - Linux 无 FSGSBASE 时的判法"GS 为负就是内核值"：ours 的 %gs 装的是偏移（`base(cpu) - origin()`），不是地址，没有固定符号，这个约定不成立；
    而且有 FSGSBASE 时用户可以写入任意值，Linux 自己在那种情况下也放弃了这个判法。

**参照**
- `Linux paranoid_entry` 里的 `SAVE_AND_SET_GSBASE` 与 `paranoid_exit` 里的 `wrgsbase` 还原（`arch/x86/entry/entry_64.S`），CPU 号来自 `GET_PERCPU_BASE`（`arch/x86/entry/calling.h`）。
- `Zircon interrupt_common` 的 `.Lnmi` 分支（`kernel/arch/x86/exceptions.S`）：`rdmsr64 MSR_IA32_GS_BASE` 存进 `%rbx`，写入本 CPU 的值，调用 `x86_nmi_handler()`，再 `wrmsr64` 还原。

**验证**
见 `x86-kernel.md` 同日条目：同一次构建、启动与实验同时覆盖本接口。

**故意没做**
- 没有用 `rdgsbase`/`wrgsbase` 代替 MSR：只有 NMI/#DF/#MC 走这里，不是热路径；FSGSBASE 也不是每颗 CPU 都有（默认的 Nehalem 就没有）。
- 没有给 `MsrIo::write` 加 `"memory"` clobber：之后的 per-CPU 访问全是 `asm volatile`，与 `wrmsr` 的先后由 volatile 保证。
