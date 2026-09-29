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

## process_create，以及从未启动的进程关掉最后一个句柄就销毁（K23a-1 第 1 步，2026-09-28）

**文件**
- `kernel/object/include/ours/object/process_dispatcher.hpp`、`process_dispatcher.cpp`：`ProcessDispatcher::on_zero_handles()`
- `kernel/lib/syscall/src/process.cpp`：`syscall::process_create()`
- `system/public/ours/syscall.hpp`：`ProcessCreateOptions`；`system/public/ours/syscalls.gni`：`process_create` 条目
- `system/lib/ou/include/ou/process.hpp`、`src/process.cpp`：`ou::process_create()`
- `examples/shell/shell.cpp`：`check_process_create()`
- `docs/design/260923/userspace-interface.md` §2.2、`docs/design/260924/userspace-filesystem.md`：K23 行

**症状**
用户态没法建进程；就算有了 `process_create`，launcher 在“建进程”和“启动进程”之间任何一步失败，
都会留下一个映射了 vDSO、状态永远是 Initial 的进程：没有线程，没人会把它转成 Dead，它的地址空间永远不会拆。

**原因**
进程的状态机里 Initial 只有一条出路：`attach_thread`（首线程启动）转 Running。
“句柄全关了”这件事没有任何反应：`Dispatcher::on_zero_handles()` 对进程是空的。

**改法**

```cpp
auto ProcessDispatcher::on_zero_handles() -> void {
    { Guard guard{get_lock()};
      if (m_state != State::Initial) { return; }   // Running 靠自己的线程活着，不靠句柄
      m_state = State::Dead; }
    finish_dead_transition();                        // 锁外：拆地址空间、清句柄表、置 TERMINATED
}
```

- 状态检查和迁移在同一段进程锁内，而 `attach_thread()` 检查 Initial 用的是同一把锁。
  “启动”和“最后一个句柄关掉”并发时只有两种结果：启动先拿到锁，进程 Running，`on_zero_handles()` 看到 Running 什么也不做；
  或者它先拿到锁，进程 Dead，启动看到 Dead 回 BadState。没有第三种。
- **引用不是句柄**：启动的一方手里有一个进程的引用，所以对象的内存不会因为句柄关光而释放；
  但句柄数归零决定的是进程的**状态**，引用计数对此毫无说明，所以不能假定“持有进程的人一定还持有它的句柄”，必须在锁内重查状态。
- 拆除放在锁外、只走一次：`kill()`、`detach_thread()` 和这里，三处都是在锁内把状态设成 Dead 的那一条边，每条边只有一个调用者会赢，
  `finish_dead_transition()` 因此只跑一次。
- `process_create` 的发布顺序照 `channel_create`（设计 4.1）：两个句柄（进程、根 VMA）的值先都写给调用者，再都入表；
  写失败时两个句柄在栈上销毁，进程失去最后一个持有者，`on_zero_handles()` 把它拆掉，不需要手工清理。
- vDSO 不在这里映射（用户决定，选 A）：launcher 拿到 vDSO 的 VMO 句柄后用 `vma_map` 映射，内核在映射时登记范围（K23a-2）。

**参照**
Zircon `ProcessDispatcher::on_zero_handles()`：Initial 状态下最后一个句柄关掉，转 DEAD，`FinishDeadTransition()`；
Zircon `sys_process_create()`：ManageProcess 检查、名字截断到 `ZX_MAX_NAME_LEN`、一次返回进程句柄和根 VMAR 句柄。

**验证**
- 两个镜像均构建通过；`boot_check.py` 两个镜像都 PASS（dbg 15 passed；rel 13 passed、6 skipped）。
- dbg_1 的 shell（有根 job）打出 `process_create checks passed`：
  - job 传无效值回 BadHandle，传 channel 回 BadType，句柄不带 ManageProcess 回 AccessDenied，带选项位回 InvArgs；
  - 成功时得到两个不同的句柄；复制出第二个进程句柄后关掉第一个，`wait_one(TERMINATED, poll)` 回 TimeOut
    （还剩一个句柄，进程没死）；关掉最后一个，才销毁；
  - 把表填到只剩正好两个空位，再用内核半区的输出地址连续调用 3 次，每次都是 InvArgs（若某次留下了句柄，
    下一次会回 NoResource），随后一次正常调用成功。
- 内核日志里 5 个名为 `created` 的进程创建、5 个 `aspace 'created' destroyed` 和 5 条 `process 'created' … destroyed`：一一对应，没有泄漏。
- 最初的泄漏测试是“失败 quota+1 次”，每次都真的建一个地址空间，dbg 镜像慢到 60 秒超时；
  改成“先填满表、只留两个空位”，同样能测出泄漏，只需要 4 次建进程。

**故意没做**
- 没测“启动与最后一个句柄关闭并发”的那两种结果：要等 `process_start` 出来才能构造（下一步）。
- job 没有状态，所以“job 已 Dying/Dead 回 BadState”还没法做；进程也还没登记到 job 的子进程表里。

**怎么发现的**
无。

**用户的预测**
决策卡：进程从 Initial 启动，与“最后一个句柄关闭”并发，靠什么保证只有一个成功。
- 用户先想的是 CAS：`CAS(Initial → Running)` 成功则继续启动，失败则检查当前状态；对方 `CAS(Initial → Dying)`。
  这正确回答了“检查并修改必须是一个原子步骤”，但漏了 CAS 成功之后、线程还没挂上之前的那段窗口：
  别的调用看到 Running 却是空的线程表，去杀进程，进程进入 Dying 却永远等不到最后一个线程退出。
- 讨论后用户改选“锁内状态机”，理由是启动和杀死都要同时改状态和线程表，CAS 管不住多个变量的一致性。
  结论：不变量只涉及一个字，无锁可以；涉及多个字，必须一把锁把它们圈在一起。

**你应该能回答的三个问题**
- 引用计数和句柄数各管什么？为什么句柄数归零时要重查进程的状态？
- `on_zero_handles()` 和 `attach_thread()` 并发时，为什么只有两种结果？把它们的检查放到锁外会多出哪一种？
- 为什么 `finish_dead_transition()` 要在锁外做，又为什么它只会被执行一次？
