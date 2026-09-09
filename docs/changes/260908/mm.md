
## 为 vmalloc 划出内核地址空间窗口（2026-09-08）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp` — 新增 `ARCH_VMALLOC_VIRT_BASE` / `ARCH_VMALLOC_SIZE`
- `kernel/mm/init_vmm.cpp` — `PHYS_VMOS[]` 增加 `k:vmalloc` 一条

**症状**
无，这是新增。`init_vmm.cpp:175` 那个空的 `init_heap()` 注释里写着它要装的是
"the VMM's own allocator: virtually contiguous, physically scattered"，本次先把它要用的
地址空间划出来。

**原因**
vmalloc 的返回地址必须落在一段**固定**范围里，这不是审美选择，是 `operator delete` 能
成立的前提：`ktl::kfree()` 靠 `virt_to_folio()` 经 physmap 回查所属 cache
（`ktl/kmalloc.cpp:80`），对窗口外的地址无意义；而 `KERNEL_DEBUG_LEVEL=0` 的镜像里
`PhysMap::virt_to_phys()` 对非 physmap 地址返回 0，`phys_to_frame(0)` 拿到 0 号帧，
`try_as<SlabBody>` 的断言又被编译掉——于是**在 release 镜像里把一个非 physmap 指针交给
kfree 是未定义行为，而不是一次断言**。所以 delete 必须先做范围判断，而范围判断要快到
可以放在每一次 delete 上，那就只能是一次减法，也就只能是固定窗口。Linux 的
`VMALLOC_START`/`VMALLOC_END` 与 `is_vmalloc_addr()` 是同一个道理。

**位置为什么在镜像之上**：这一条是约束而非偏好。cpulocal 窗口结束于
`KERNEL_ASPACE_BASE + 32 GiB + 64 MiB`，而镜像在 `KERNEL_LOAD_BASE`（base + 33 GiB）
再加最多 `GB(1)` 的 KASLR 偏移，中间的空隙不足 1 GiB。窗口若放在那里，要么小得没用，
要么盖住镜像。放 `base + GB(64)` 则与偏移上限无关，是个不依赖镜像实际落点的常数；
`KERNEL_ASPACE_SIZE = GB(512)`，到 base+68GB 远未触顶。

那段空隙保持原样也是有意的：`VmPlacement::anywhere()` 是**最低地址 first-fit**
（`vm_area_or_mapping.cpp:180`），local APIC、HPET、IDT、GDT 和每一个内核栈现在都落在
那里，不该被一个新窗口挤走。

**改法**

```c
#define ARCH_VMALLOC_VIRT_BASE (KERNEL_ASPACE_BASE + GB(64))
#define ARCH_VMALLOC_SIZE GB(4)
```

```cpp
{.name = "k:vmalloc",
 .base = VirtAddr(ARCH_VMALLOC_VIRT_BASE),
 .size = usize(ARCH_VMALLOC_SIZE),
 .vmaf = VmaFlags::Read | VmaFlags::Write,
 .backing = PhysVma::Backing::AddressSpace},
```

`Backing::AddressSpace` 而不是 `Mapped`：`entrol_preset_vma()` 对前者**只
`create_subvma()` 就返回，不调 `reserve()`**（`init_vmm.cpp:143`）。这是必须的——窗口
在任一时刻绝大部分都没有映射，`reserve()` 要去改写并不存在的页表项。和上面的
`k:cpulocal` 是同一个约定：VMM 只保证这段地址不会被发给别人，页表归子系统自己管。

一次性把 4 GiB 全部声明，而不是按需增长：这条 VMA 的作用就是拦住
`VmPlacement::anywhere()`。窗口里若混进了别人的映射，它会被
`is_vmalloc_addr()` 认成 vmalloc 的地址，然后在 `delete` 时被当作 vmalloc 分配释放掉。

**参照**
- `Linux VMALLOC_START` / `VMALLOC_END`（x86 在 `pgtable_64_types.h`）：同样是固定窗口，
  同样服务于 `is_vmalloc_addr()`。注意 Linux 的 `VMALLOC_END` 是**闭区间**上界而
  `is_vmalloc_addr` 用 `<` 比较；本树统一用半开区间 `[base, base + size)`。
- 本树 `k:cpulocal`（`init_vmm.cpp:122`）：同样的 `Backing::AddressSpace` 语义。

**验证**
`ninja` 通过；QEMU 实测 `kernel.dev.dbg_1.obi`，启动日志新增一行且范围与预期一致：

```
Reserve area k:cpulocal: [FFFFFF8800000000, FFFFFF8804000000)
Reserve area k:vmalloc:  [FFFFFF9000000000, FFFFFF9100000000)
```

与镜像（`FFFFFF8840000000` 起）不重叠。userboot 照常进入 ring 3 并打出
`hello from ring 3`，30 秒内 910 次用户态时钟中断，无回归。

**故意没做**
- **`init_heap()` 仍然是空的**。本次只划地址，分配器本身是下一步。
- **没有 guard page 机制**。整棵树都没有（`vm_area_or_mapping.hpp:284` 的相邻区间间隙为
  零），vmalloc 的 guard 由它自己在窗口内留页实现，不动 VMM。
- **riscv / arm 没有对应宏**。等那两个架构真的有启动路径时再加。
