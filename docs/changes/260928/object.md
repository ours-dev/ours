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
