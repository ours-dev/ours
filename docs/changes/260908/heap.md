# heap 改动记录

## kmalloc 与 vmalloc 合并到 kernel/lib/heap（2026-09-08）

**文件**
- `kernel/lib/heap/` — 新库：`kmalloc.{hpp,cpp}`、`new.{hpp,cpp}`、`vmalloc_arena.{hpp,cpp}`、`tests/`
- `kernel/lib/ktl/` — 交出 kmalloc / new，只剩 `memory_resource.cpp`
- `kernel/mm/` — 交出 `vmalloc_arena.*`
- 全树 23 个文件把 `<ktl/new.hpp>` 改成 `<heap/new.hpp>`

**症状**
无，是结构调整。两个分配器原先分居两处：kmalloc 在 `kernel/lib/ktl`，
`VmallocArena` 在 `kernel/mm`。

**原因**
它们是**一件事的两半**，而先前的位置把这件事切开了。

kmalloc 服务内核里绝大多数的小对象，从 2 的幂 size class 出，背后是 physmap 里物理连续
的 slab；vmalloc 服务少数大块，从专用地址窗口出，逐页从 buddy 拿——虚拟连续、物理离散。
Linux 也是这条线（`mm/slub.c` 对 `mm/vmalloc.c`）。

真正要求它们同处一库的是**路由**：`operator new` 要按大小在两者之间选，`operator delete`
要按地址路由回去。把这个选择放在两个实现旁边，是让它在一处就能被审阅的唯一办法——
Linux 的 `kvmalloc()`/`kvfree()` 出于同样理由紧挨着 slab 分配器。

放在 `ktl` 里也不对：ktl 是内核的 C++ 模板层（容器、`Name`、`Result`），分配器不是模板；
放在 `kernel/mm` 里同样不对：vmalloc 是 mm 的**客户**（它调 `alloc_frames`、`map_bulk`），
不是 mm 的一部分。

**改法**
`git mv` 保留历史。命名空间随位置走：

| 改前 | 改后 |
|---|---|
| `ktl::kmalloc` / `ktl::kfree` / `ktl::init_kmalloc` | `heap::` 同名 |
| `ours::mm::VmallocArena` | `heap::VmallocArena` |
| `<ktl/kmalloc.hpp>` / `<ktl/new.hpp>` / `<ours/mm/vmalloc_arena.hpp>` | `<heap/…>` |

`operator new(usize, Gaf)` 的签名从 `ktl::Gaf` 改写成 `ours::mm::Gaf`。**这不是行为改动**：
`ktl::Gaf` 本来就是 `ours::mm::Gaf` 的别名，mangled name 一字不差，调用点一处不用动。
改它是为了让 heap 不依赖 ktl——ktl 依赖 `//kernel/mm:headers`，而 heap 也要依赖 mm，
取那个别名等于把 ktl 无谓地塞进这条路径上。

**依赖方向上唯一有意思的一处**：`//kernel/mm:headers` 必须依赖 `//kernel/lib/heap:headers`
（每个说 `new (gaf)` 的 mm 源文件都要 `<heap/new.hpp>`），而 heap 又需要 mm 的 `Gaf` 和
`PAGE_SIZE`。两个方向都取 target 依赖就是 GN 拒绝的环。解法是 heap 只取 mm 的**包含路径
config** 而不是 target：

```gn
cpp_interface("headers") {
  public_configs = [ "//kernel/mm:headers.config" ]
  public_deps = [ "//system/lib/ustl:headers", "//system/public" ]
}
```

heap 的 source_set 仍然取完整的 `//kernel/mm:headers`——那是另一个 target，不成环。
`//kernel/mm/tests` 早就用同一个手法（它的注释解释过：`//kernel/phys:handoff` 在宿主
工具链下求值不了）。

**参照**
- `Linux mm/slub.c` 与 `mm/vmalloc.c`：同样的两类分配器；`kvmalloc()`/`kvfree()` 的路由
  与 slab 同处一文件。
- 本树 `kernel/lib/lz4`、`kernel/lib/prof`：一个 lib 一件事的既有粒度。

**验证**
`ninja` 通过；宿主单测 `//kernel/lib/heap/tests:vmalloc-arena-smoke` 8 组全过；QEMU 实测
两个镜像，`k:vmalloc` 窗口照常保留，userboot 照常 `hello from ring 3`（dbg_1 897 次
用户态时钟中断），均以 signal 15 结束。全树 23 个包含点的改动经 `git diff` 确认**只有
include 行**。

**故意没做**
- **`operator new` 的路由还没接上**。这一步只搬家，`new` 仍然只走 kmalloc、>4 KiB 仍返回
  nullptr。路由是 vmalloc 本体到位之后的事。
- **`ktl::init_memory_resource()` 留在 ktl**。它是 ustl 容器的默认资源，属于 C++ 胶水，
  不是分配器；它调 `heap::kmalloc` 是正常的客户关系。
