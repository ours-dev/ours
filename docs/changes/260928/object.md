## VmaDispatcher 改名为 VmarDispatcher（2026-09-28）

**文件**
- `kernel/object/include/ours/object/vma_dispatcher.hpp` → `vmar_dispatcher.hpp`，`kernel/object/vma_dispatcher.cpp` → `vmar_dispatcher.cpp`（`git mv`）
- 类 `VmaDispatcher` → `VmarDispatcher`，`VmaDispatcherBase` → `VmarDispatcherBase`，
  `RIGHTS_DEFAULT_VMA` → `RIGHTS_DEFAULT_VMAR`，计数器 `dispatcher.vma.*` → `dispatcher.vmar.*`
- 引用它的地方：`kernel/object/BUILD.gn`、`types.hpp`、`process_dispatcher.{hpp,cpp}`、`vmo_dispatcher.hpp`、
  `kernel/lib/elfload`（`Loader` 的成员 `m_root_vma` → `m_root_vmar`）、`kernel/lib/userabi/src/userboot.cpp`、
  `kernel/mm/include/ours/mm/vm_area.hpp` 与 `system/public/ours/types.hpp` 的注释
- `docs/design/260923/userspace-interface.md`、`docs/design/260924/userspace-filesystem.md`：路径跟着改

**症状**
对象层的地址区域叫 `VmaDispatcher`，而它要实现的 ABI 叫 `ours_vmar_*`，句柄类型是 `ObjectType::Vmar`，
Zircon 叫 `VmAddressRegionDispatcher`。同一个东西在对象层和 ABI 层用了两个名字。

**原因**
名字是照 mm 的 `VmArea` 起的。可 dispatcher 服务的是用户态，它的名字应该跟 ABI 走。
接口设计 §2.3 早就定了：“对象层和 ABI 统一叫 Vmar，`mm::VmArea` 不改名。改名放在第一个 vmar 系统调用之前做。”
下一步 K21 就要加 `vmar_map`。

**改法**
纯改名，行为不变。
- 只改对象层及以上：dispatcher 的类名、文件名、权限常量、计数器，以及持有 dispatcher 的变量
  （`root_vmar`、`m_root_vmar`、`root_vmar_rights`）。
- mm 层不动：`mm::VmArea`、`VmAspace::root_vma()`、`process_dispatcher.cpp` 里取 mm 根区域的局部变量 `root_vma`，
  这些指的是 mm 的对象，名字本来就对。
- dispatcher 内部的成员 `m_vma` 保留：它持有的正是一个 `mm::VmArea`。

**参照**
Zircon `VmAddressRegionDispatcher`（`zircon/kernel/object/vm_address_region_dispatcher.cc`），
对应的 mm 对象是 `VmAddressRegion`：两层各用各的名字，对象层的名字与 `zx_vmar_*` 一致。

**验证**
- `gn gen` 后两个镜像均构建通过（文件改名要重新生成 ninja 文件）。
- `boot_check.py` 两个镜像都 PASS（dbg 12 passed；rel 11 passed、4 skipped）。

**故意没做**
- 设计文档其余位置里“Vmar”“VMA”的一般说法没有逐一改，只改了指向文件路径和类名的地方。
- `docs/changes/` 下的旧记录只增不改。

## VmarDispatcher::map 改用 VmOptions，映射带 vmo_offset 和自己的权限上限（K21a，2026-09-28）

**文件**
- `system/public/ours/syscall.hpp`：新增 `VmOptions`（位值照 `ZX_VM_*`）
- `kernel/object/include/ours/object/vmar_dispatcher.hpp`、`vmar_dispatcher.cpp`：`map()` 换成 Zircon 的形状；`create()` 按区域的权限给句柄加 Read/Write/Execute
- `kernel/mm/include/ours/mm/vm_area.hpp`、`vm_area.cpp`：`create_mapping()`、`map_with_vmo()` 新增 `ceiling`，`map_with_vmo()` 新增 `vmo_ofs`，Commit 从 `vmo_ofs` 开始提交
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp`：`permissions_of(MmuFlags)`、`VmAreaOrMapping::permissions()`
- `kernel/mm/vm_mapping.cpp`：`map_physical()` 加上 `m_vmo_ofs`
- 调用方跟着改：`kernel/mm/stack.cpp`、`kernel/lib/heap/src/vmalloc.cpp`、`kernel/commands/user_copy_test.cpp`（传 `0` 和 `VmaFlags::PermMask`，行为不变），
  `kernel/lib/elfload`（`Loader::map` 收地址和 `VmOptions`）、`kernel/lib/userabi/src/userboot.cpp`
- `docs/design/260923/userspace-interface.md` §2.3、`docs/design/260924/userspace-filesystem.md` 的 K21 行

**症状**
K21 要加 `vmar_map`，可对象层接不住：
- `VmarDispatcher::map()` 收的是 mm 的 `VmPlacement`、`MmuFlags`、`VmMapOption`，对象层的接口等于 mm 的接口；
- 没有 vmo_offset。mm 的 `map_with_vmo()` 把它写死为 0，Commit 也从 0 提交；
- 映射的权限上限一律是所在区域的 `m_vmaf`。一个只读映射以后可以被 protect 成区域允许的任何权限，
  接口设计 §2.3 举的例子就是只读的 BootFS 被 protect 成可写；
- 根 VMAR 的句柄没有 Read/Write/Execute，`vmar_map` 按句柄 rights 检查时什么都映射不了（原来的 TODO）；
- `map_physical()` 算物理地址时漏了 `m_vmo_ofs`：偏移不为 0 的物理映射会映射到对象开头。

**原因**
这些地方以前没有用户态调用者，内核调用方都从偏移 0 映射，也从不 protect，所以一直没暴露。

**改法**

```cpp
// object：Zircon 的 VmAddressRegionDispatcher::Map() 形状，不收 rights
auto VmarDispatcher::map(VmOptions options, usize vmar_offset, VmoDispatcher &vmo,
                         u64 vmo_offset, usize length, char const *name) -> ktl::Result<VirtAddr>;
//   Perm* → MmuFlags（总是加 User）；CanMap* → 映射上限；Specific → vmar_offset 相对区域起点；MapRange → Commit

// mm：映射的上限 = 调用方给的 ceiling ∩ 区域允许的
auto const allowed = ceiling & permissions();
if (permissions_of(mmuf) & ~allowed) { return InvArgs; }        // 起始权限不能高于上限
Builder(&m_aspace, (m_vmaf & ~PermMask) | allowed, size)...
```

几处取舍：
- **照 Zircon 收 options，不收 rights**。接口设计 §2.3 原本写的是 `map(..., Rights vmar_rights, Rights vmo_rights)`。
  Zircon 的做法是：系统调用把两边句柄的 rights 换算成 `CAN_MAP_*` 位放进 options，
  dispatcher 只认 options，并把 `CAN_MAP_*` 存成映射的上限。
  好处是内核调用方（userboot）没有句柄，也能用同一套说法直接写出它要什么；
  CanMap 从内核一直到将来的 `vmar_allocate` 都是同一个意思。§2.3 已按此修订。
- **W^X 和“写必须带读”不在这里查**。这两条约束的是程序能要什么，属于 `vmar_map` 系统调用（K21b）。
  userboot 现在仍把整个镜像按 RWX 映射成一段，要等各段分开映射之后才能收紧。
- **映射超出 VMO 回 OutOfRange**。Zircon 允许映射越过 VMO 末尾，访问越界部分时才 fault；
  接口设计 §2.3 选择在入口就拒绝。K20 的 `make_cursor()` 按窗口查范围，是第二道防线。
- **内核调用方的上限**：`Loader` 映射镜像时，上限就等于它要的权限。vDSO 是 R|X，以后谁都没法把它 protect 成可写；
  userboot 的栈是 R|W，永远不可执行。mm 内部的调用方（内核栈、vmalloc、测试）传 `VmaFlags::PermMask`，
  得到的仍是所在区域的上限，行为和以前一样。
- **根 VMAR 句柄的 rights**：照 Zircon `VmAddressRegionDispatcher::Create()`，按区域自身允许的权限加上 Read/Write/Execute。
  需要读区域的权限，所以 mm 加了 `permissions()`；权限位在对象建好后不会再变，所以读它不用持锁。

**参照**
- Zircon `VmAddressRegionDispatcher::Map()`、`VmAddressRegion::CreateVmMapping()`（Perm 位没有对应的 CanMap 位回 `ZX_ERR_ACCESS_DENIED`）。
- Zircon `VmAddressRegionDispatcher::Create()`：句柄 rights 从 VMAR 的 `CAN_MAP_*` 推出。
- `ZX_VM_*` 的位值。

**验证**
- 两个镜像均构建通过；`boot_check.py` 两个镜像都 PASS（dbg 12 passed；rel 11 passed、4 skipped）。
  userboot、init、shell 和所有探针的镜像、vDSO、栈都经过新的 `map()`，vDSO 仍映射在 `[A00000, A03000)`。
- 两个镜像的 `usercopy.test` 都是 18/18、`pmnode.test` 都是 20/20，经过的是改过签名的 `map_with_vmo()`。

**故意没做**
- 映射上限和 `map_physical()` 的偏移修正还没有专门的测试：前者要等 `vmar_protect`，
  后者要等有人按非零偏移映射物理 VMO。K21b 的 shell 检查会覆盖 paged VMO 的偏移和 Perm/CanMap 检查。
- `VmOptions` 里没实现的位（Compact、SpecificOverwrite 等）在 dispatcher 里一律回 NotSupported，
  程序直接传 CanMap* 回 InvArgs 由 K21b 的系统调用负责。

## 地址区域统一叫 Vma：撤回 Vmar 改名，ABI 一并改成 vma（2026-09-28）

**文件**
- 撤回“VmaDispatcher 改名为 VmarDispatcher”那一节的全部改名：`vma_dispatcher.{hpp,cpp}`、`VmaDispatcher`、`RIGHTS_DEFAULT_VMA`、计数器 `dispatcher.vma.*`、`root_vma` 等变量
- ABI：`ours_vmar_map`/`ours_vmar_allocate` → `ours_vma_map`/`ours_vma_allocate`（`syscalls.gni`，参数名 `vmar`/`vmar_offset` → `vma`/`vma_offset`），
  `syscall::vma_*`（`kernel/lib/syscall/src/vmar.cpp` → `vma.cpp`），`ou::vma_*`（`ou/vmar.hpp` → `ou/vma.hpp`）
- 公共类型：`HandleType::VmarRoot`/`VmarLoaded` → `VmaRoot`/`VmaLoaded`，`ObjectType::Vmar` → `ObjectType::Vma`（数值不变）
- 注释、shell 检查函数名、设计文档里指 ours 自己对象的 “VMAR”；
  `docs/design/260923/userspace-interface.md` §2.3 的命名规则改写；`.agents/skills/ours-kernel-design/references/layering.md` 记下规则

**症状**
同一天先把对象层从 VmaDispatcher 改成了 VmarDispatcher（依据是接口设计 §2.3 的旧规则），用户否决：
这个项目里地址区域就叫 Vma，与 mm 的 `VmArea` 同名同义。

**原因**
旧规则照搬 Zircon 的 VMAR 命名，但 ours 的 mm 层早就叫 `VmArea`，对象层和 ABI 再叫 Vmar，
就是同一个概念在两层用两个名字——正是那次改名想消除的问题，只是统一的方向选反了。

**改法**
按标识符批量替换，然后逐行复查剩下的 “vmar/VMAR”：凡指 Zircon 的（`zx_vmar_map`、`sys_vmar_map()`、
`VmAddressRegion`、“Zircon 的 VMAR”）保留原名，方便对照；指 ours 自己对象的全部改成 Vma。
数值（`HandleType` 0x04、`ObjectType` 18、`VmOptions` 各位）仍照 Zircon，只改名字。

**参照**
无，本项目自有命名。对照 Zircon 时：ours 的 Vma = Zircon 的 VMAR（`VmAddressRegionDispatcher`）。

**验证**
- 重新 `gn gen` 后两个镜像均构建通过，vDSO ABI 检查通过（导出名随之变成 `ours_vma_*`）。
- `boot_check.py` 两个镜像都 PASS（dbg 14 passed；rel 13 passed、5 skipped），shell 的 `vma_map checks passed`、`vma_allocate checks passed` 照旧。

**故意没做**
- `docs/changes/` 旧记录里的 vmar 名字不改，只增不改。

**怎么发现的**
无。

**用户的预测**
无。

**你应该能回答的三个问题**
- ours 的 Vma 对应 Zircon 的哪个对象？为什么数值照搬而名字不照搬？
- 批量改名时，怎样区分“ours 的 vmar”和“Zircon 的 vmar”？
- `mm::VmArea` 和 `object::VmaDispatcher` 为什么是两个对象而不是一个？
