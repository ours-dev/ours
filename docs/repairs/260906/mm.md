# mm 待修项（2026-09-06）

## 1. `VmArea::create_mapping()` 收下 `VmMapOption` 却从不理会 `Commit`

```cpp
auto VmArea::create_mapping(VmPlacement where, usize requested_size, usize vmo_ofs, MmuFlags mmuf,
                            ustl::Arc<VmObject> vmo, char const *name, VmMapOption option,
                            ustl::Arc<VmMapping> *out) -> Status;
```

`option` 只被传给 `Builder`，`Commit` 那一半（`vmo->commit_range()` +
`mapping->map()`）只存在于 `map_with_vmo()` 里。于是直接用 `create_mapping()` 的调用
者会拿到一个"成功"的映射，页表却是空的，第一次访问就缺页。

实测（`//kernel/object` 的 `VmaDispatcher::map()` 第一版）：

```
[error] Unhandled page fault: vector=14, error_code=0x14
[error]   rip=0x201270 (+0x77c0201270)  cs=0x33  rsp=0x1200000
```

两条路都行，但要选一条：
- 让 `create_mapping()` 兑现 `Commit`（那样 `map_with_vmo()` 就只剩建 VMO 的差别）；
- 或者把 `VmMapOption` 从 `create_mapping()` 的签名里去掉，让它诚实地只做"建对象"。

现状是最坏的一种：参数在，语义不在。

## 2. `VmArea::map_with_vmo()` 把 VMO 偏移写死为 0

```cpp
auto status = create_mapping(where, size, 0, mmuf, vmo, name, option, &mapping);
//                                       ^ vmo_ofs
```

`create_mapping()` 本身是支持偏移的，`map_with_vmo()` 不透传。后果是对象层的
`VmaDispatcher::map()` 无法提供 VMO 偏移参数——而 `zx_vmar_map` 是有的，"把一个大
VMO 的某一段映射到某处"是常规操作（把 ELF 的三个 segment 按各自权限分别映射就需要
它，那正是 userboot 下一步要做的事）。
