# vmalloc

实现一套 vmalloc，并让**内核侧的 `new` 成为 vmalloc 的接口**。

这份文档先"扩散"再"收敛"：每一节先把可选项摆开，再说选哪个、为什么。事实全部带
`file:line`，是读过代码确认的，不是推测。实现按 §7 的顺序，一步一提交，每步 build +
两镜像 boot。

---

## 1. 目标与范围

owner 的原话是"先实现一套 vmalloc 吧，让内核侧的 new 成为 vmalloc 的接口"。
"new 成为 vmalloc 的接口"有三种读法：

| 读法 | 含义 | 采纳？ |
|---|---|---|
| (a) `new` 一律走 vmalloc | 每个对象一页起步，`ThreadCache`/`VmaCache` 这些几十字节的对象全部变成一页 | **否**。内核里绝大多数 `new` 是小对象，kmalloc 的 size class 正是为它们存在的 |
| (b) 加一个独立的放置标签 `new (mm::VMALLOC, gaf) T` | 调用方自己选分配器 | **否**。调用方不该知道 4 KiB 这条线在哪；而且这条线是实现细节，将来 kmalloc 长出更大的 size class 就变了 |
| (c) `new` 是**统一入口**，按大小在 kmalloc / vmalloc 之间路由 | 小的走 kmalloc，大的走 vmalloc，`delete` 自动认出来 | **是** |

(c) 就是 Linux 的 `kvmalloc()` / `kvfree()`。取它的理由：**现在 `operator new(size, Gaf)`
在 size > 4 KiB 时返回 nullptr**（`ktl/kmalloc.cpp:61`：`if (size > MAX_MALLOC_SIZE) return
nullptr`），于是每一个需要大块内存的地方都手写 `get_frames_n` + 手工 `free_frames`
（userboot 两处、prof 一处），而 `get_frames_n` 会**悄悄向上取整到 2 的幂**
（`pmm.hpp:129`）并且**超过 1024 页直接失败**（`pm_node.cpp:566`）。让 `new` 覆盖全尺寸，
这些手写就都能收回来。

范围内：kernel 侧的 vmalloc/vfree、`new`/`delete` 路由、把现有手写调用点迁过来。
范围外：用户态、`vmap`（映射调用方给的页表）、大页、per-cpu 缓存、延迟 TLB 刷新。

---

## 2. 现状事实（约束设计的那些）

### 2.1 致命的一条：VMO 的页永远不归还

```cpp
~VmCowPages() = default;                       // kernel/mm/vm_cow_pages.cpp:12
~VmObjectPaged() = default;                    // vm_object_paged.hpp:48
auto VmObjectPaged::decommit(...) -> Status { return Status::Unimplemented; }  // :82
```

`VmPageMap` 只是一棵 `BTreeMap<PgOff, Slot>`，没有释放页的析构
（`vm_page_map.hpp:33`）。整条 VMO 路径上唯一的 `free_frames` 是插入失败时的那一次
（`vm_cow_pages.cpp:95`）。`docs/repairs/260903/mm.md:18` 已记录这一条。

**推论**：vmalloc **不能**建在 `VmObjectPaged` 之上。建上去的话 `vfree` 只能拆 PTE，
页全漏——一个"能分配不能释放"的分配器比没有分配器更糟。

顺带印证：内核栈就是这么漏的。`Thread::destroy` 只跑 `~Stack`，而 `~Stack` 只丢一个
`Arc`，父 `VmaSet` 还持有一份（`vm_area_or_mapping.cpp:82` insert 会 retain），所以
**每个死线程漏掉 VA、VmMapping、VMO 和 4 个页**（`thread.cpp:365`）。

### 2.2 已有的正确配方：预留窗口 + 自持页 + arch 批量映射

`CpuLocal::Mapper::build_dense`（`cpu_local.cpp:334`）就是这条路：

```cpp
auto frame = alloc_frames(nid, gaf, order);
m_aspace.map(va + done * PAGE_SIZE, frame_to_phys(frame), n, MAP_FLAGS,
             MapControl::ErrorIfExisting, &mapped);
```

窗口由 `init_vmm.cpp` 的 `PHYS_VMOS[]` 表以 `Backing::AddressSpace` 声明
（`init_vmm.cpp:122` 的 `k:cpulocal`）——VMM 只预留范围，**不映射也不保护**，由子系统
自己往里填。这正是 vmalloc 需要的语义。

`ArchVmAspace` 提供的原语（`arch_vm_aspace.hpp:26-32`）：

```cpp
auto map(VirtAddr, PhysAddr, usize n, MmuFlags, MapControl, usize *mapped) -> Status;
auto map_bulk(VirtAddr, PhysAddr *, usize n, MmuFlags, MapControl, usize *mapped) -> Status;
auto unmap(VirtAddr, usize n, UnmapControl, usize *unmapped) -> Status;
```

`unmap` 只清 PTE + `invlpg`，**不释放帧**（`page_table.cpp:86`）——页的归还是调用方的事，
这对 vmalloc 恰好正确。

### 2.3 页分配面

- `alloc_frames(gaf, order)`：一个 2^order 的**物理连续**块，`MAX_FRAME_ORDER = 10`
  即 4 MiB 封顶（`cfg.hpp:97`，`pm_node.cpp:562`）。
- `alloc_frames(nid, gaf, PmFrame::List*, n)`：n 个 order-0 帧串成链表，失败时释放已分配
  的部分（`pm_node.cpp:609`）。**这是"给我 n 个散页"的原语**。
- `free_frames(PmFrame::List*)`：按每帧自记的 order 归还（`pm_node.cpp:625`）。
- `PmFrame` 有 32 字节的 role payload，`Role::Heap` 是**空闲的 tag**（`pm_frame.hpp:26`），
  `become<Body>()` 是挂私有元数据的正规做法（`pm_frame.hpp:306`）。
- 注意 `PmNode::alloc_frames` 会 `gaf &= ALLOWED_GAF`，而 `ALLOWED_GAF = GAF_BOOT`
  （`pm_node.cpp:202`），**`Gaf::Zero` 在 GAF_BOOT 里而 GAF_KERNEL 没有** —— 要零页得自己
  memset，不能指望 gaf。

### 2.4 kfree 靠物理页回查 cache

```cpp
auto *const host = virt_to_folio(object);
DEBUG_ASSERT(host, "kfree() on an address that is outside the physmap");
DEBUG_ASSERT(host->try_as<page::SlabBody>(), "kfree() on an address that no slab backs");
auto *const cache = static_cast<MemCache *>(host->slab_owner());
```
（`ktl/kmalloc.cpp:80`）

kmalloc 返回的**永远是 physmap 地址**。vmalloc 返回的地址在**另一个窗口**里，
`virt_to_folio` 对它无意义。所以：**"这是不是 vmalloc 地址"就是一次窗口范围比较**，
和 Linux 的 `is_vmalloc_addr()` 一模一样，O(1)，不需要查表。这是整个 delete 路由能成立
的基础。

### 2.5 地址空间还有多少余地

实测 boot 日志：

```
k:physmap  [FFFFFF8000000000, FFFFFF8800000000)   32 GiB
k:cpulocal [FFFFFF8800000000, FFFFFF8804000000)   64 MiB
k:head..k:init [FFFFFF8840000000, FFFFFF88400A5000)  内核镜像
```

`KERNEL_ASPACE_SIZE = GB(512)`，镜像在 `KERNEL_ASPACE_BASE + 32 GiB + 1 GiB` 处，
镜像之后到 512 GiB 边界是空的。有的是地方。

### 2.6 C++ 侧

- 真正的分配器是 `ktl/new.cpp` 的 `operator new(usize, ktl::Gaf)`；
  `libc/cpp_dependencies.cpp:16` 和 `ccrt/compiler/itanium.cpp:82` 里的是 **panic 桩**，
  给 libc++ 冷路径兜底用的。
- `operator delete(void*) noexcept` 目前直接 `ktl::kfree(ptr)`（`ktl/new.cpp:21`）——
  **这就是要改的那一个函数**。
- `ustl::RefCounted::release()` → `destroy()`（`~T()`）→ `reclaim()`，`reclaim` 走
  `T::Disposer` 或 `operator delete`（`rc.hpp:76,121,245`）。`ustl::Box` 同理走
  `detail::default_deallocate<T>`（`box.hpp:127`）。**两者最终都落到
  `operator delete(void*)`**，所以改那一个点就覆盖了全部。
- **sized delete 是存在的，我先前写反了。** 本树没有传 `-fno-sized-deallocation`，
  clang 23 在 `-std=c++2c` 下预定义 `__cpp_sized_deallocation`，链接后的镜像里有 46 个
  目标文件引用 `_ZdlPvm`（deleting destructor 与 libc++ `<format>` 内部）。它们绑到的是
  `libc/cpp_dependencies.cpp:29` 的**弱符号空实现**——也就是说今天没有任何一条活路径
  走它（`RefCounted`/`Box` 都走无尺寸的那个），但**只要将来有人写 `delete p`，就是静默
  泄漏**。

  结论没变（按地址路由），但理由变了：不是"size 拿不到"，而是**尺寸信息不可靠且不覆盖
  全部重载**。而且这意味着路由要同时覆盖 sized 与数组重载，否则管线只做了一半。

- **`operator new(usize, Gaf)` 没有 `noexcept`**（`new.hpp:8`）。于是
  `new (gaf) T(...)` 在分配失败时会**先在空指针上跑构造函数**，caller 的 `if (!self)`
  永远等不到。这已是 `docs/repairs/260903/ktl.md:19` 的待修项；接上 vmalloc 之后 `new`
  会在**新的场合**失败（窗口耗尽、调用过早、map 失败），所以这条要在同一串提交里一起修。
- `-fno-exceptions`：`new` 失败返回 nullptr 是本树的约定。

---

## 3. 设计

### 3.1 分层

```
operator new(size, Gaf) ─┬─ size <= KMALLOC_MAX  → ktl::kmalloc  →  MemCache（physmap 地址）
                         └─ size >  KMALLOC_MAX  → mm::vmalloc   →  VMALLOC 窗口地址
operator delete(void*)  ─┬─ mm::is_vmalloc_addr(p) → mm::vfree
                         └─ 否则                    → ktl::kfree
```

vmalloc 自己不碰 `VmObjectPaged`（§2.1），它**自持页**：

```
mm::vmalloc(size, gaf)
  1. 页数 n = align_up(size, PAGE_SIZE) / PAGE_SIZE
  2. 在 VMALLOC 窗口里分配 n+1 页的 VA（多一页是 guard），拿到 base
  3. alloc_frames(gaf, &list, n)        ← 散页，链表
  4. map_bulk(base, phys[], n, R|W)      ← 装 PTE
  5. 把 (base, n, list) 记进一条 VmallocArea
  6. 返回 base
```

`vfree(p)`
```
  1. 由 p 找到 VmallocArea（窗口内下标 → 直接命中，见 §3.4）
  2. arch unmap n 页（清 PTE + invlpg）
  3. free_frames(&list)                  ← 页真的还回去了
  4. 释放 VA
```

### 3.1b 树里已经给它留了位置

`kernel/mm/init_vmm.cpp:175` 有一个空函数，注释写得很清楚：

```cpp
/// Placeholder for vmalloc.
///
/// Empty on purpose, and not the kernel object heap -- that is
/// mm::init_cache(), which runs several init levels earlier, before the VMM
/// exists at all. This one is the VMM's own allocator: virtually
/// contiguous, physically scattered, and therefore impossible to build
/// before the address space it carves from.
INIT_CODE
auto init_heap() -> void {}
```

"virtually contiguous, physically scattered" —— 就是本设计。所以初始化挂点不是新造的，
是填这个函数；`init_vmm()` 已经在 `init_vmm_preheap()` 之后调它（`init_vmm.cpp:193`），
顺序天然正确：窗口的 sub-VMA 在 preheap 里建好，vmalloc 在其后初始化自己的空闲链表。

### 3.2 窗口与放置

新增一条 preset VMA。**位置必须在内核镜像之上**：

```c
// kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp
#define ARCH_VMALLOC_VIRT_BASE (KERNEL_ASPACE_BASE + GB(64))
#define ARCH_VMALLOC_SIZE GB(4)
```

先前草稿写的是 `ARCH_CPU_LOCAL_VIRT_BASE + ARCH_CPU_LOCAL_SIZE`（= base+32GB+64MB），
**那是错的**：从那里起 4 GiB 会盖住 `KERNEL_LOAD_BASE`（base+33GB，再加最多 1 GiB 的
KASLR 偏移）上的镜像。cpulocal 之后到镜像之前只有约 960 MiB 的空隙，塞不下 4 GiB。

选 base+64GB 的理由：它在 `KERNEL_LOAD_BASE + 1GB`（KASLR 偏移上限）之上，是个不依赖
镜像实际落点的常数；`KERNEL_ASPACE_SIZE = GB(512)`，到 base+68GB 远未触顶。
那 960 MiB 的空隙保持不动——`anywhere()` 是最低地址 first-fit，LAPIC / HPET / IDT /
GDT / 内核栈现在都落在那里，不该被挪动。

```cpp
// kernel/mm/init_vmm.cpp，PHYS_VMOS[] 末尾
{.name = "k:vmalloc",
 .base = VirtAddr(ARCH_VMALLOC_VIRT_BASE),
 .size = usize(ARCH_VMALLOC_SIZE),
 .vmaf = VmaFlags::Read | VmaFlags::Write,
 .backing = PhysVma::Backing::AddressSpace},
```

`Backing::AddressSpace` 是关键：`entrol_preset_vma` 对它**只 `create_subvma` 就返回，
不 `reserve()`**（`init_vmm.cpp:143`），窗口内的页表完全归 vmalloc 自己管——和
`k:cpulocal` 同样的约定，也是同样的理由。

为什么是独立窗口而不是在 root VMA 上 `anywhere()`：
- delete 路由要 O(1) 的"是不是 vmalloc 地址"（§2.4）。散落在 root VMA 里就只能查树。
- `alloc_spot` 走的是 `m_subvmas.find_spot`（`vm_area.cpp:68`），每次分配都要拿
  **aspace 的 SpinLock**（`vm_aspace.hpp:107`）并动一棵全局树；vmalloc 应该有自己的锁。

**窗口内的 VA 分配器**：一个简单的**空闲区间链表 + first-fit**，按地址有序，释放时与
左右邻居合并。不上红黑树：4 GiB / 4 KiB = 1M 页，但真实并发在用的 vmalloc 区块是几十个
量级；Linux 的 augmented rb-tree 是给百万区块准备的，这里是过度设计。链表节点从
kmalloc 出（节点是几十字节，走 size class，不会递归回 vmalloc）。

**guard page**：每个区块尾部留一页不映射。Linux 默认就这么做（`VM_NO_GUARD` 是例外），
理由是溢出立刻 #PF 而不是踩到邻居。代价是每个区块多一页 VA（不占物理页）。

### 3.3 数据结构

```cpp
// kernel/mm/include/ours/mm/vmalloc.hpp
namespace ours::mm {

/// 一段已分配的 vmalloc 区间。
///
/// Linux 对应物：struct vm_struct（include/linux/vmalloc.h:52），字段一一对得上——
/// addr/size/nr_pages/pages。这里把 Linux 的 `struct page **pages` 换成 PmFrame::List，
/// 因为本树的帧描述符自带侵入式链表钩子（pm_frame.hpp:427），不需要另开一个指针数组。
struct VmallocArea {
    VirtAddr base;        ///< 首地址，返回给调用方的就是它
    usize nr_pages;       ///< 映射的页数，不含 guard page
    PmFrame::List frames; ///< 这个区间自持的物理页，vfree 时按此归还
    ustl::collections::intrusive::ListMemberHook<> hook;
};
}
```

区块记录存哪里：三选一——
- (a) 一棵按 base 排序的树/链表，`vfree` 时查找；
- (b) 把 `VmallocArea*` 写进首页的 `PmFrame` role payload（`Role::Heap` + 新 `HeapBody`）；
- (c) 把头信息放在区间**头部页内**（返回 base + header_size）。

**选 (a) + first-fit 链表**，和 §3.2 的空闲链表是同一棵结构的两半（已分配 / 空闲）。
(b) 更快但要新增一个 role body 并与 `PageQueues` 争用页元数据（`page_queues.cpp:192`
会写 `VmmBody::vmo`，见风险）；(c) 破坏对齐、且让 `base` 不再页对齐。规模上不来之前
(a) 足够，且最容易 DEBUG_ASSERT 出错误。

### 3.4 `is_vmalloc_addr`

```cpp
FORCE_INLINE
constexpr auto is_vmalloc_addr(void const *p) -> bool {
    auto const va = VirtAddr(p);
    return va - ARCH_VMALLOC_VIRT_BASE < ARCH_VMALLOC_SIZE;   // 减法形式，不会回绕
}
```
和 `is_kernel_address`（`vmm.cpp:14`）同样的写法、同样的理由。Linux 对应物
`is_vmalloc_addr()`（`include/linux/mm.h:1859`）。

### 3.5 锁与调用上下文

- vmalloc 自己一把 `sync::Mutex`（不是 SpinLock）：它内部要 `alloc_frames`，而
  `PmNode::Current::alloc_frames` 会取当前线程的 resource lock（`pm_node.cpp:699`），
  已经不是"能在 irq 里做"的操作了。
- **契约：`vmalloc`/`vfree` 不可在中断上下文、不可持自旋锁调用。** 用
  `DEBUG_ASSERT` 把这条钉死（检查 `arch::interrupts_enabled()` 与抢占计数）。
  Linux 同样：`vmalloc` may sleep；`vfree` 在不可睡眠时走 `vfree_deferred`——
  **本设计不做 deferred**，而是断言，因为一个连自己调用点都数得清的内核不该先上延迟队列。
- 于是 `operator new(size, Gaf)` 在 size > KMALLOC_MAX 时**继承了这条契约**。这是本设计
  最大的一处语义变化，必须写进 `new.hpp` 的注释。

### 3.6 new / delete 路由

```cpp
// kernel/lib/ktl/new.cpp
auto operator new(usize size, ktl::Gaf gaf) -> void * {
    // Linux kvmalloc(): 小的走 slab，大的走 vmalloc。这条线是实现细节，
    // 调用方不该知道它在哪 —— 这正是路由长在 new 里而不是长成第二个
    // placement tag 的理由。
    if (size > ktl::KMALLOC_MAX_SIZE) {
        return ours::mm::vmalloc(size, gaf);
    }
    return ktl::kmalloc(size, gaf);
}

auto operator delete(void *ptr) noexcept -> void {
    // 地址决定归属，不是大小 —— 本树没有 -fsized-deallocation
    // （kernel/products/BUILD.gn:89 只有 -faligned-new=8），所以
    // delete(void*, size_t) 不会被发出，size 这条路走不通。
    // 窗口比较是 O(1)，而 kfree 那条路要 virt_to_folio()，对一个
    // 非 physmap 地址会直接断言（ktl/kmalloc.cpp:82）。
    if (ours::mm::is_vmalloc_addr(ptr)) {
        return ours::mm::vfree(ptr);
    }
    return ktl::kfree(ptr);
}
```

`KMALLOC_MAX_SIZE` 要从 `kmalloc.cpp` 的匿名 `MAX_MALLOC_SIZE`（`kmalloc.cpp:25`）
提到 `ktl/kmalloc.hpp` 里公开，这样阈值只有一处定义。

**要覆盖的重载不止一个**：`operator delete[]`（`ktl::Allocator::deallocate` 就走它，
`allocator.hpp:31`）要和 `operator new[]` 同样路由；`libc/cpp_dependencies.cpp` 里那些
sized / aligned 的弱空实现，要么一并转发到无尺寸版本，要么明确留着并在注释里说清为什么
（见 §2.6 的更正）。只改 `operator delete(void*)` 是半套。

**启动顺序是硬约束**：`SparseMemMap` 在 `pmm_enabled()` 一成立就
`new (gaf, nid) T[n]`（`sparse_memmap.cpp:30`），那是 `InitLevel::Pmm/Heap`，而 vmalloc
要到 `init_vmm() -> init_heap()` 才存在（`main.cpp:150`）。所以：
- 小尺寸（kmalloc）那条路**必须和 VMM 无关**，不能让路由函数去碰 vmalloc 的状态；
- 超阈值的 `new` 若发生在 `InitLevel::Vmm` 之前，要**明确失败并断言**，不能去解引用还没
  初始化的 vmalloc 结构。用一个 `VMALLOC_READY` 标志把这条守住。

### 3.7 失败路径

每一步都能失败，回滚必须完整：

| 失败点 | 回滚 |
|---|---|
| VA 分配不到 | 直接返回 nullptr，无副作用 |
| `alloc_frames` 中途 OOM | `alloc_frames_bulk` 自己释放已分配部分（`pm_node.cpp:609`），再释放 VA |
| `map_bulk` 部分成功 | 按 `*mapped` unmap 已装的 PTE，`free_frames(&list)`，释放 VA |
| `VmallocArea` 分配不到 | 同上全部回滚 |

`map_bulk` 会报告 `*mapped`（`page_table.cpp:86` 逐页映射并计数），所以部分失败可以精确
回滚——这一点比 `map_with_vmo` 的处境好，后者只能整段 destroy。

### 3.8 TLB

单 CPU 现状下 `arch unmap` 里的 `invlpg` 就够，而且**顺序必须是"先 unmap 再
free_frames"**——Linux 反过来（`vmalloc.c:3488` 先还页，TLB 刷新批量推迟到
`__purge_vmap_area_lazy`），它敢这么做是因为有 lazy purge 机制兜底。本设计不做 lazy
purge，所以刷新必须同步，且必须在页归还之前完成，否则被复用的帧还能通过陈旧的内核 TLB
表项摸到。

**多核时需要 TLB shootdown IPI**，本树还没有——这条记进 repairs，不假装不存在。
Linux 的对应物是 `flush_tlb_kernel_range()`；Zircon 是 `ArchVmAspace::Unmap` 里的
`TlbInvalidation`。

---

## 4. 与 Linux / Zircon 的对照

| 本设计 | Linux | Zircon | 说明 |
|---|---|---|---|
| `mm::vmalloc(size, gaf)` | `__vmalloc_node_range()` | — | Zircon 没有 vmalloc，它一切走 VMO |
| `mm::vfree(p)` | `vfree()` | — | Linux 有 deferred，本设计断言代之 |
| `mm::is_vmalloc_addr(p)` | `is_vmalloc_addr()` (`mm.h:1859`) | — | 同样的窗口比较 |
| `VmallocArea` | `struct vm_struct` (`vmalloc.h:52`) | — | `pages**` → `PmFrame::List` |
| `operator new` 的路由 | `kvmalloc()` (`mm/util.c`) | — | 本树把它藏在 `new` 里而不是另起一个名字 |
| `operator delete` 的路由 | `kvfree()` | — | 同样靠 `is_vmalloc_addr` |
| 空闲 VA 链表 | `struct vmap_area` + augmented rb-tree | — | 规模不到，用有序链表 |
| guard page | 默认行为，`VM_NO_GUARD` 是例外 | — | 同 |

---

## 5. 迁移的调用点

**迁**（只要虚拟连续）：
- `kernel/lib/userabi/userboot.cpp:90` —— 解压内核包的缓冲区，MB 级，
  现在是 `get_frames_n(GAF_KERNEL, n)`，会向上取整到 2 的幂。
- `kernel/commands/prof.cpp:89` —— FdeTable，同上。
- `HandleTable` 的 arena（`.agents/tasks/syscall-object-layer/DESIGN.md` §3.2）——
  这是**下一个任务的直接消费者**，4096 × sizeof(Handle)。

**不迁**（真需要物理连续）：
- `kernel/arch/x86/mm/page_table.cpp:41` —— 页表页，硬件按物理地址走。
- `kernel/lib/userabi/userboot.cpp:228` —— userboot 镜像的 backing，之后要
  `PhysMap::virt_to_phys()` 交给 `VmObjectPhysical`（`userboot.cpp:270`）。
- `CpuLocal` 的 chunk —— 它自己就是一个窗口分配器，且要按 NUMA 节点分别 backing。

**注意**：迁过去的调用点，释放要从 `free_frames(virt_addr, order)` 改成 `vfree`。
`free_frames(VirtAddr)` 走 `virt_to_frame`，只认 physmap 地址（`pmm.hpp:194`）。

---

## 6. 测试

**启动期内核自检**（`KERNEL_DEBUG_LEVEL=1` 下跑一次，串口可见）：

| 用例 | 验证 |
|---|---|
| vmalloc(1 页) / 写满 / vfree | 基本往返 |
| vmalloc(1 MiB) 非 2 的幂，如 3 页 | 不做 2 的幂取整（对比 `get_frames_n`） |
| 连续 alloc/free 100 次，地址复用 | 空闲链表合并正确，不碎片化到耗尽 |
| 两个区块之间隔着 guard page | `query()` 那一页应无翻译 |
| `is_vmalloc_addr(kmalloc(64))` == false | 路由判据 |
| `new` 一个 8 KiB 对象再 `delete` | **端到端**：路由到 vmalloc 又正确回来 |
| `new` 一个 64 B 对象再 `delete` | 未回归 kmalloc 路径 |
| vmalloc 到窗口耗尽 → nullptr，不 panic | 失败是普通错误 |
| 自检前后 `alloc_frames` 计数平衡 | **没漏页**——这条最硬 |

最后一条要 PMM 有个可读的已分配页计数；没有就加一个 `GKTL_COUNTER`。

**宿主单测**：空闲区间链表（插入/摘除/合并/first-fit）是纯逻辑，抽成
`vmalloc_arena.hpp` 不依赖 mm，在 host 工具链下测。这是唯一值得写宿主单测的部分。

**负向**：`vfree(nullptr)` 无害；`vfree` 一个 kmalloc 指针 → DEBUG_ASSERT；
`vmalloc(0)` → nullptr。

---

## 7. 提交顺序

| # | 改动 | 验证 | 可观察 |
|---|---|---|---|
| 1 | `ARCH_VMALLOC_VIRT_BASE/SIZE` + `k:vmalloc` preset VMA | build + boot | 日志多一行 `Reserve area k:vmalloc: [...]` |
| 2 | `vmalloc_arena.hpp`（纯 VA 空闲链表）+ 宿主单测 | 单测 | 内核无行为变化 |
| 3 | `mm::vmalloc/vfree/is_vmalloc_addr` 实现 + 填 `init_heap()`（还没人调用） | build + boot | 无行为变化 |
| 4 | 启动期自检 + 页计数平衡断言 | boot | 串口出现 `vmalloc: N cases passed` |
| 5 | `KMALLOC_MAX_SIZE` 提到公开头；`new`/`delete` 路由 | build + boot | 大对象 new 不再返回 nullptr |
| 6 | 迁 `prof.cpp` | boot + `prof.start` | 行为不变 |
| 7 | 迁 `userboot.cpp:90`（包缓冲区） | boot | userboot 照常打出 hello |

每一步都能独立回退。第 5 步是语义变化最大的一步，所以放在自检之后——先证明 vmalloc
自己是对的，再让 `new` 依赖它。

---

## 8. 交给别的 lane

| lane | 事项 | 依据 |
|---|---|---|
| mm | `~VmCowPages` 不释放页——**每个 VMO 死时漏光所有页** | `vm_cow_pages.cpp:12`，已在 `docs/repairs/260903/mm.md:18` |
| mm | 内核栈从不回收：`Thread::destroy` 只跑 `~Stack`，父 `VmaSet` 仍持引用 | `thread.cpp:365`, `vm_area_or_mapping.cpp:82` |
| mm | `Stack::init` 在 `map()` 失败时不 `destroy()` 映射，留下挂在两个链表上的对象 | `stack.cpp:43` |
| mm | `VmoFlags::Pinned` / `CommitOptions::Pin` 是空操作；`pin_frame/unpin_frame` 只有声明 | `vm_object_paged.cpp:48`, `pmm.hpp:199` |
| mm | `ALLOWED_GAF = GAF_BOOT` 把 `Gaf::Dma/Write/NoFail` 全部掩掉；zone-type 过滤是死代码 | `pm_node.cpp:202,433` |
| mm | `get_frame_n<T>(nid,...)` 调用不存在的 `get_frame<T>` | `pmm.hpp:159` |
| arch | **多核 TLB shootdown 缺失**——vfree 之后其它 CPU 可能仍持旧翻译 | `page_table.cpp` 只有本地 `invlpg` |
| mm | `create_subvma` / `create_mapping` / `map_with_vmo` / `VmMapping::map` **完全不加锁**，而 `unmap`/`protect`/`page_fault` 加 aspace 的 SpinLock；两个并发 `anywhere()` 可拿到同一段 VA，且 `VmaSet::insert` 只检测 base 完全相同，部分重叠会静默塞进去 | `vm_area.cpp:71,116`，`vm_area_or_mapping.cpp:82` |
| mm | aspace 的 `Guard` 不存 IRQ/抢占状态，持锁者可被抢占/中断 | `spin_lock.hpp:55` |
| mm | `alloc_frames_slowpath` 对带 `DirectReclaim`（即 `GAF_KERNEL`）且无 `NoRetry`（`NoRetry` 恰好被 `ALLOWED_GAF` 掩掉）的请求，在内存耗尽时**死循环**而不返回 nullptr，reclaim/compact 都是返回 0 的桩 | `pm_node.cpp:486,467` |
| ktl | `operator new(usize, Gaf)` 缺 `noexcept`，失败时先在空指针上跑构造函数 | `new.hpp:8`，`docs/repairs/260903/ktl.md:19` |
| ktl | sized / aligned 的 `operator delete` 是弱空实现，将来任何 `delete p` 都是静默泄漏 | `libc/cpp_dependencies.cpp:29` |

---

## 9. 判官/读者指出但未采纳的点

- **把区块记录挂进 `PmFrame` 的 role payload**（`Role::Heap` 空着）：更快，但
  `PageQueues` 会写 `VmmBody::vmo`（`page_queues.cpp:192`），页一旦被排队就不安全；
  要安全就得新增一个 role body，那是 mm 的 lane。规模不到，不换。
- **用 `VmObjectPaged` 做页容器**：会继承 §2.1 的漏页，直接否掉。等
  `~VmCowPages` 修好后可以重新评估——那时 vmalloc 能瘦一大圈。
- **`vfree_deferred`**（Linux 在不可睡眠时的退路）：本设计改用 DEBUG_ASSERT。
  调用点还数得清，先让违约者响，而不是先建队列。
- **augmented rb-tree 管空闲 VA**：Linux 的规模才需要。有序链表 + first-fit，
  合并逻辑二十行，出错时容易断言。
- **`vmap()`（映射调用方给的页）**：本任务不需要，且 `VmObject::supply_pages`
  仍是 `Unimplemented`（`vm_object_paged.cpp:82`）。
