# kernel/mm — 改动记录

格式见 `docs/changes/TEMPLATE.md`。待修项在 `docs/repairs/260903/mm.md`。
2026-09-02 及更早的 mm 改动在只读存档 `docs/repairs-20260830.md` 里（第 31–40 节）。

---

## 1. `VmArea::protect()` 从声明到实现，`reserve()` 不再绕过映射层（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_mapping.hpp` — 声明 `protect_locked()`
- `kernel/mm/vm_mapping.cpp` — 把 `protect()` 拆成校验 + 加锁的外壳与 `protect_locked()` 内核
- `kernel/mm/include/ours/mm/vm_area.hpp` — 声明 `protect_locked()` 与 `can_protect_locked()`
- `kernel/mm/vm_area.cpp` — 实现区域级 `protect()`；`reserve()` 改走映射层

**症状**

潜伏，当前启动触发不到。`VmArea::protect()` 直接 `return Status::Unimplemented`，所以
映射一旦建立就再也改不了权限——这是 VMA 接口里唯一没有实现的入口。

同时 `VmArea::reserve()` 越过自己的子节点，直接调 `m_aspace->arch_aspace().protect()` 写页表。
这一条现在能"正常工作"，但它让页表和映射自己记的 `ProtectionRanges` 成为两个独立的真相来源，
一旦有人再调一次映射级的 `protect()`，以哪个为准是没有定义的。

**原因**

权限这件事的真正拥有者是 `VmMapping`：它有 `ProtectionRanges`，把自己的地址窗口铺成一串不相交、
相邻且同权限即合并的区间，`protect()` 先更新这串区间，再逐区间下发到页表，并且把"没有任何权限"
的区间翻译成解除映射而不是设权限。`VmArea` 手里没有页表，只有子节点——所以区域级的 protect
不可能自己动手，只能下发。原来它没有下发，于是要么什么都不做（`Unimplemented`），要么像
`reserve()` 那样伸手越过子节点去写页表。

拆 `protect_locked()` 是被锁逼出来的：`get_lock()` 返回的是整个地址空间那一把锁，而且不可重入。
区域在持锁下降子节点时不能再调公开的 `protect()`，因为后者要自己取锁。`unmap`/`unmap_locked`
早就是这个分层，这里只是补齐对称。

**改法**

`VmArea::protect()` 校验后取锁，然后**先问后写**：`can_protect_locked()` 递归确认每个覆盖到的
子节点都接受这组权限，全部通过才由 `protect_locked()` 统一下发。

两趟而不是一趟，是因为 protect 半途失败无法恢复：前几个子节点已经改写，旧权限没有留底，返回值
也说不出边界落在哪里，调用方既不知道改到哪也没法回滚。`unmap` 不需要这个待遇——半解除的范围
再解除一次就行，操作是幂等的。

`reserve()` 末尾的直接页表写改成 `mapping->protect(0, size, mmuf)`。语义等价（`VmMapping::create()`
已经用同一个 `mmuf` 初始化了整个窗口的 `ProtectionRanges`，这次 update 是空操作），但它顺带
接上了零权限即解除映射那条分支，并且让页表只有一个写入者。

**参照**

`Zircon VmAddressRegion::Protect()`（`zircon/kernel/vm/vm_address_region.cc`）。它同样是先遍历
子节点做一次可行性检查再应用，理由和这里一样。下发到叶子的对应物是
`Zircon VmMapping::ProtectLocked()`。Linux 那边是 `mprotect_fixup()`，但 Linux 的 VMA 是平的，
没有嵌套，所以没有"递归下降"这一层。

**验证**

构建通过，`clang-format` 与 `clang-tidy -p out/bringup.x86` 对四个文件跑过，新增行无新告警。

QEMU 实测（`kernel.dev.dbg_1.obi`）：`init_vmm_preheap()` 的九个预置 VMA 全部经由新的
`reserve()` → `mapping->protect()` → `protect_locked()` 路径建立成功，日志里九条
`Reserve area` 齐全，启动继续推进到线程创建。这是 `protect_locked()` 拆分的正向证据。

`VmArea::protect()` 的递归下降**未实测**——当前没有任何调用方，启动路径走不到。它与已经验证
可用的 `unmap_locked()` 同构（同样的子节点遍历、同样的区间裁剪、同样的映射／区域分派），但这只是
类比，不是证据。

**故意没做**

- 没有让 `protect()` 要求范围被完全覆盖。Zircon 要求范围内不能有空洞，我们的 `unmap()` 不要求，
  两者保持一致比单独向 Zircon 看齐更重要。等到有真实调用方时再定。
- 没有动 `map_at()` 那个既当输入提示又当输出结果的 `VirtAddr *virt_base`。它是之前把 LAPIC 的
  MMIO 放到 physmap 起点那个 bug 的温床，该改，但那是接口签名的改动，会牵动所有调用方，单独一条。
- 没有统一"偏移 vs 绝对地址"。`unmap`/`protect`/`reserve`/`create_subvma` 收相对 `m_base` 的偏移，
  而 `map_at`/`map` 返回绝对地址、`contains`/`find_mapping_locked` 收绝对地址，调用方要反复换算
  （`init_vmm.cpp` 里就写着 `phys_vma.base - root_vma->base()`）。同样是签名级改动。
- 没有补遍历接口。目前只有 `find_mapping_locked()` 能找叶子，没有找子区域的，也没有对外枚举，
  地址空间转储、范围操作、拆除记账都无从下手。列在 `docs/repairs/260903/mm.md`。
