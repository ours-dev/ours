# mm 改动记录

## `VmArea::map_with_vmo()` 由 protected 改为 public（2026-09-06）

**文件**
- `kernel/mm/include/ours/mm/vm_area.hpp` — 声明从 protected 段移到 public 段

**症状**
潜伏，直到有人想「映射一个自己已经拿到的 VMO」为止。

公开的映射入口只有三个：`map_phys()`（自己建 `VmObjectPhysical`）、`map_anon()`
（自己建 `VmObjectPaged`）、`create_mapping()`（接受调用者的 VMO）。前两个不接受
外来 VMO；第三个接受，但**不装页表**——它收下 `VmMapOption` 却从不理会 `Commit`。

后果是具体的：`//kernel/object` 的 `VmaDispatcher::map()` 用 `create_mapping()` 写成
之后，映射返回 Ok、日志正常，userboot 在第一条指令上取指缺页：

```
[error] Unhandled page fault: vector=14, error_code=0x14
[error]   rip=0x201270 (+0x77c0201270)  cs=0x33  rsp=0x1200000
```

（bit4 取指 + bit2 用户态 + bit0=0 页不存在。）

**原因**
`map_with_vmo()` 是那两个便利函数的**一般形式**——它们的差别只在于调用它之前建了哪
种 VmObject。它被放在 protected 段，在它只是两者的共享 helper 时是合理的；一旦有东西
想映射自己单独取得的内存，这个可见性就不再合理了。而「映射一个我已经持有的 VMO」正是
每一个 `zx_vmar_map` 的形状。

它和 `create_mapping()` 的真正差别不是"包了一层"，而是它多做了 `Commit` 那半件事：
提交 VMO 的页、把 PTE 装进去、失败时用 `VmMapping::destroy()` 正确回退。那段回退逻辑
的注释自己解释了为什么不能省——直接返回错误会在还挂在 VMO 映射链表和父 VMA 集合上的
`VmMapping` 上跑析构，ustl 的侵入式钩子对此设了陷阱，表现为析构函数里的 #UD。

**改法**
纯可见性移动，签名与实现一字未改。

改前（protected 段，夹在 `alloc_spot` 和 `dump_locked` 之间）
```cpp
    auto map_with_vmo(VmPlacement, usize, MmuFlags, ustl::Arc<VmObject>, VmMapOption, char const *)
        -> ktl::Result<ustl::Arc<VmMapping>>;
```
改后（public 段，紧跟 `map_phys` / `map_anon`，并写明与 `create_mapping()` 的差别）
```cpp
    /// Map |size| bytes of a VMO the caller already has.
    ...
    auto map_with_vmo(VmPlacement where, usize size, MmuFlags, ustl::Arc<VmObject>, VmMapOption,
                      char const *name) -> ktl::Result<ustl::Arc<VmMapping>>;
```

**参照**
`Zircon VmAddressRegion::CreateVmMapping()`：Zircon 那边接受调用者 VMO 的映射入口本来
就是公开的，`VmAddressRegionDispatcher::Map()` 直接调它。

**验证**
`ninja` 通过；QEMU 实测 `kernel.dev.dbg_1.obi`，userboot 通过
`VmaDispatcher::map()` → `map_with_vmo()` 映射镜像与栈，进入 ring 3 后 30 秒内
908 次 `Exception[interrupt] from user space`，无缺页。

**故意没做**
`create_mapping()` 收下 `VmMapOption` 却不理会 `Commit` 这件事本身没有修——那是 mm
自己该决定的事（要么让它兑现，要么把这个参数从签名里去掉），已记入
`docs/repairs/260906/mm.md`。本次只是让调用者有一条能走通的公开路径。
