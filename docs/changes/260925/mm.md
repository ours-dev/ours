# kernel/mm 改动记录（2026-09-25）

## VMO 提交：错误不再丢掉，区间只提交一次（2026-09-25）

**文件**
- `kernel/mm/vm_object_paged.cpp` — `create()` 检查提交结果，先 adopt 再提交，空 VMO 不提交；`commit_range_internal()` 按整个区间做范围检查，只调一次 `commit_range_locked()`
- `kernel/lib/heap/src/vmalloc.cpp` — 自测里引用 O(n²) 循环的注释改成过去时
- `docs/repairs/260903/mm.md` — 删掉第 1 条里"外层循环是 O(n²)"一项

**症状**
三件，都是潜伏的：

- `VmObjectPaged::create()` 丢掉 `commit_range()` 的返回值。非 Lazy 的 VMO 提交失败时，`create()` 照样回 Ok，
  交出一个有洞的 VMO。
- `commit_range_internal()` 每页调一次 `commit_range_locked(offset, size)`，每次提交整个区间，n 页的 VMO 做 n² 次
  页查找。每个线程的内核栈都是 Pinned VMO，建线程时都要付这笔账。
- 范围检查只比较 `size`，不看 `offset`：`offset + size` 越过末尾的区间也被放行。

修这几件的同时，还挖出第四件：`VmArea::reserve()` 为每个保留区建一个 0 长度的 Pinned VMO，它的提交**一直是失败的**
（Pin 0 字节回 InvArgs），只是返回值被丢掉，没人看见。把错误传上来后，dbg_1 在第一次 reserve 时就断言失败：

```
VMO Commit Action: [ofs: 0, size: 0]
Assertion failed on Status::Ok == status
```

dev 镜像不开断言，那次 reserve 在 dev 里是静默失败的。

**原因**
非 Lazy 的意义就是"创建时把会用到的页全部提交，之后访问不会缺页"，返回值是这个承诺是否兑现的唯一信号。
内核栈是 Pinned：栈页缺了一页，某次压栈压到那里，就是一次对内核地址的缺页，而 `page_fault()` 拒绝一切内核地址，
内核就死在那里，现场离成因很远。

O(n²) 那个循环是照 Zircon `VmObjectPaged::CommitRangeInternal()` 抄了一半：Zircon 的循环存在只有一个原因——
`CommitRangeLocked()` 在需要页源供页时可能提前返回 `ZX_ERR_SHOULD_WAIT`，循环放锁等页请求，**把 offset 推进已提交的长度**，
再试一次。ours 抄了循环，没抄推进，于是循环只是重复自己。而 ours 根本没有可等的东西：没有 VMO 有页源，
`commit_range_locked()` 自己断言永不回 ShouldWait。

**改法**
改前：

```cpp
    auto vmo = VmoPagedCache::get(GAF_KERNEL, vmof, ustl::move(cow_pages));
    ...
    if (!(vmof & VmoFlags::Lazy)) {
        ...
        vmo->commit_range(0, size, options);
    }
    *out = ustl::Arc<Self>::adopt(vmo);
```

```cpp
    Guard guard{m_mutex};
    if (m_cow_pages->size_locked() < size) {
        return Status::OutOfRange;
    }

    for (usize i = 0; i < size; i += PAGE_SIZE) {
        usize nr_commited = 0;
        auto status = m_cow_pages->commit_range_locked(offset, size, &nr_commited);

        if (status != Status::Ok && status != Status::ShouldWait) {
            return status;
        }
    }

    return Status::Ok;
```

改后：

```cpp
    auto vmo_ref = ustl::Arc<Self>::adopt(vmo);

    if (!(vmof & VmoFlags::Lazy) && size != 0) {
        ...
        auto const commit_status = vmo_ref->commit_range(0, size, options);
        if (Status::Ok != commit_status) {
            return commit_status;
        }
    }
    *out = ustl::move(vmo_ref);
```

```cpp
    Guard guard{m_mutex};
    auto const total = m_cow_pages->size_locked();
    if (size > total || offset > total - size) {
        return Status::OutOfRange;
    }

    usize nr_committed = 0;
    return m_cow_pages->commit_range_locked(offset, size, &nr_committed);
```

- **先 adopt 再提交**：提交中途失败时，丢掉这个引用就会让 `~VmCowPages` 把已拿到的页还回去；原来在提交之后才 adopt，
  失败路径上没有任何人持有这个对象和它的页。
- **空 VMO 不提交**，而不是交给 `commit_range()`：Pin 0 字节在那里是 InvArgs，Zircon 同样禁止（下面参照）。
  这条规则针对的是显式的 pin 请求；0 长度的 VMO 本来就没有页可提交，`create()` 不该替它发起一次注定被拒的 pin。
- **范围检查**写成不会回绕的形式 `size > total || offset > total - size`，同 Zircon 的 `InRange()`。

被否掉的做法：把循环改成 Zircon 那样按已提交长度推进。今天没有任何情况会走第二轮，写出来就是一段永不执行、
也无法验证的代码；注释里写明了页源出现时要把 Zircon 的循环整个搬回来——等待、推进、重试三样一起。

**参照**
- Zircon `VmObjectPaged::CommitRangeInternal()`（`zircon/kernel/vm/vm_object_paged.cc:882`）：`while (len > 0)` 循环、
  ShouldWait 时等页请求再按 committed_len 推进；开头用 `add_overflow` 和 `InRange()` 做范围检查；`len == 0` 时对 pin
  明确拒绝（同文件 :926 起的注释："we explicitly forbid zero length pins"）。
- Zircon `VmCowPages::~VmCowPages()` 释放全部页，对应本树 `~VmCowPages()`。

**验证**
- 构建通过；clang-format、clang-tidy 在改动行上无新告警。
- dev：`vmalloc: self test passed`，init 收到 shell 的 "pong"，到 `--- end ---`。
- dbg_1：修空 VMO 之前在第一次 reserve 断言失败（见症状）；修后 `shell: probes done (10 killed, 0 survived)`，无断言。

**故意没做**
- `CommitOptions::Pin` 仍然没有实际语义：没有 pin 计数，也没有回收方会动已提交的页，所以今天 Pinned 与"非 Lazy"
  效果相同。等有回收（`docs/repairs/260903/mm.md` 第 8 条）时再做。
- `docs/repairs/260903/mm.md` 第 1 条的首项"`~VmCowPages()` 不释放自己拥有的页"已经被别的改动修掉了（析构函数现在调
  `drain_owned()`），不是这次修的，这里不删，留给改它的那条线核对。

## VmObjectPaged 的内核缓冲读写，与 VmObject::size()（2026-09-25）

**文件**
- `kernel/mm/vm_object_paged.cpp` — 实现 `read()`/`write()`，共用私有模板 `read_write()`
- `kernel/mm/include/ours/mm/vm_object_paged.hpp` — `write()` 改收 `void const *`；加 `size()`；声明 `read_write()`
- `kernel/mm/include/ours/mm/vm_object.hpp` — `write()` 改收 `void const *`；改正两处抄错的注释；加纯虚 `size()`
- `kernel/mm/include/ours/mm/vm_object_physical.hpp` — 实现 `size()`
- `docs/repairs/260925/mm.md` — 新增两条（见"故意没做"）

**症状**
`VmObjectPaged::read()`/`write()` 都回 `Unimplemented`。文件系统计划里，K3（程序映像改用 paged VMO）、
K12（把内核包做成 BootFS VMO）、K13（`vmo_read` 系统调用）都要往 VMO 里写或从中读，没有这两个就无从下手。

**原因**
缺的是功能，不是错。基类 `write()` 的注释还是 `read()` 的复制品（"Read the |size| of bytes..."），参数也是
非 const 的 `void *`，写入源被声明成可写的。

**改法**
改前：

```cpp
auto VmObjectPaged::read(void *out, VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::write(void *in, VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}
```

改后（摘要）：

```cpp
template <typename Copy>
auto VmObjectPaged::read_write(VirtAddr const offset, usize const size, bool const will_write,
                               Copy const &copy) -> Status {
    ...
    Guard guard{m_mutex};
    auto const total = m_cow_pages->size_locked();
    if (size > total || offset > total - size) {
        return Status::OutOfRange;
    }

    auto const first_page = align_down(offset, PAGE_SIZE);
    auto const end_page = align_up(offset + size, PAGE_SIZE);
    auto cursor = m_cow_pages->make_cursor(first_page, end_page - first_page);
    ...
    while (done < size) {
        ...
        auto page = cursor->require_page(will_write, 1, &request);
        ...
        auto *const base = PhysMap::phys_to_virt<u8>(frame_to_phys(*page));
        copy(base + in_page, done, chunk);
        done += chunk;
    }
    return Status::Ok;
}
```

`read()` 与 `write()` 各传一个 `memcpy` 的 lambda，方向相反。

- **只收内核缓冲**：拷贝在 `m_mutex` 下做，不能缺页。用户缓冲要在每次拷贝前后放锁（Zircon 的 `ReadUser`），
  等 `vmo_read`/`vmo_write` 系统调用时再做（计划里经内核中转页，接口设计 §4.6）。`m_mutex` 是会睡眠的锁，
  长拷贝只挡住这个 VMO 的其他使用者，不会关着中断占住 CPU。
- **越界整体拒绝**：越界就回 OutOfRange，一个字节也不拷，不做短读。Zircon 同样回 `ZX_ERR_OUT_OF_RANGE`，
  除非调用方要 `TrimLength`。
- **没写过的页读出为零**：读和写走同一个 cursor，读用 `require_read_page()`。对匿名 VMO 这会提交一个清零的页，
  读到的是 0；Zircon 用零页标记而不提交，占用更小，这里没有标记（`VmCowPages::Cursor` 的注释写了取舍）。
- **`size()`** 不拿锁：分页 VMO 的大小今天没有任何东西能改（`Resizable` 没有实现）。注释写明，哪天能改了就要拿锁，
  Zircon 的 `size_locked()` 同样要求持锁。

**参照**
Zircon `VmObjectPaged::ReadWriteInternal()`（`zircon/kernel/vm/vm_object_paged.cc:1281`）：范围检查，为整段页范围建
lookup cursor，每页 `RequirePage(write, ...)`，经 `paddr_to_physmap()` 拷贝。`Read()`/`Write()`（同文件 :1428、:1451）
各传一个拷贝 lambda，形状相同。

**验证**
- 构建通过；clang-format、clang-tidy 在改动行上无新告警（两条 `modernize-use-override` 落在原有的析构函数上）。
- 临时启动探针（未提交），dev 与 dbg_1 结果相同：3 页 Lazy VMO；跨第 0/1 页写 20 字节再读回，一致；读从没写过的
  第 2 页，全 0；读越过末尾、offset 回绕、在末尾写 1 字节都回 OutOfRange；在末尾读 0 字节回 Ok；`size()` 为 0x3000。
- 去掉探针后两个镜像照常：dev 到 `--- end ---`，dbg_1 `probes done (10 killed, 0 survived)`。

**故意没做**
- 用户缓冲的读写：见上。
- 物理 VMO 的 `read()`/`write()` 仍回 `Unsupported`，与接口设计一致（physical VMO 回 NotSupported）。
- 缺页路径不拿 VMO 的锁：`VmCowPages` 的页仓库由地址空间的 SpinLock 和 VMO 的 Mutex 分头保护，同一个 VMO
  被两处同时改写时会并发插入。登记为 `docs/repairs/260925/mm.md` 第 1 条，必须在 K16（共享 vDSO VMO）之前修。
- 查这件事时看到 `m_last_fault` 用 `Arc::adopt()` 包查找出来的裸指针，可能多出一个所有者，登记为同文件第 2 条，
  K6 之前核对。

## 勘误：`m_last_fault` 的 `Arc::adopt()` 不是缺陷（2026-09-25）

本文件只增不改，更正写在这里。

「VmObjectPaged 的内核缓冲读写」一节的"故意没做"说 `m_last_fault = Arc::adopt(find_mapping_locked(va))` 可能凭空多出一个
所有者，并登记为 `docs/repairs/260925/mm.md` 第 2 条。这是误判：ustl 的 `Rc(T *)` 与 `Rc::adopt(T *)`
（`system/lib/ustl/include/ustl/rc.hpp:283-293`）总是 `retain()`，对象从强计数 0 起步，第一个 Rc 把它变成 1——它不是 fbl 的
`AdoptRef`（那个不加计数）。所以 `m_last_fault` 拿到的是它自己的一个引用，替换或析构时减掉的正是它加过的那一个。

误判的来源是 `VmAspace::kernel_aspace()` 的注释，它说 adopt "takes ownership of a raw pointer without retaining it"，与 ustl 的
实现不符。该注释已改正（`kernel/mm/include/ours/mm/vm_aspace.hpp`），待修项第 2 条已删除。

## 拆掉的映射放下它的 VMO，清掉地址空间的 last-fault 缓存，并且只拆一次（2026-09-25）

**文件**
- `kernel/mm/vm_mapping.cpp` — `VmMapping::destroy()`：已拆则回 BadState；先清 `m_aspace->m_last_fault`（若是自己）；`remove_mapping` 后置空 `m_vmo`；`VmMapping::map()` 对已拆的映射回 BadState

**症状**
潜伏，是文件系统计划 K6（`VmAspace::destroy`）的前置：

- 拆掉的映射仍持有 `m_vmo`。映射对象本身可以比"被拆"活得久——任何持有它 `Arc` 的人都让它活着——而它活着 VMO 就活着，
  VMO 的每一页也就不还。
- `VmAspace::m_last_fault` 持有最近一次缺页命中的映射的一个引用。这个映射被拆后，缓存仍指着它：映射、它的 VMO、以及
  （映射持有 `Arc<VmAspace>`）整个地址空间都被这个缓存留住，下一次落在这段范围的缺页还会拿到一个什么也不覆盖的映射。
- `destroy()` 没有"只做一次"的保护：第二次调用会把已摘下的节点再摘一次、父节点的计数再减一次。

**原因**
`destroy()` 撤销的是 `activate()` 做过的事——挂进 VMO 的映射链表、父节点计数、父节点的集合——但它没有撤销"持有 VMO"这一件，
也不知道地址空间里还有一个缓存指着自己。

**改法**
改前：

```cpp
auto VmMapping::destroy() -> Status {
    m_canary.verify<Self>();

    auto const status = m_aspace->arch_aspace().unmap(m_base, m_size >> PAGE_SHIFT, {}, 0);
    ...
    if (m_vmo) {
        m_vmo->remove_mapping(*this);
    }
    ...
```

改后：

```cpp
auto VmMapping::destroy() -> Status {
    m_canary.verify<Self>();

    if (!is_active()) {
        return Status::BadState;
    }

    if (m_aspace->m_last_fault.as_ptr() == this) {
        m_aspace->m_last_fault = {};
    }

    auto const status = m_aspace->arch_aspace().unmap(m_base, m_size >> PAGE_SHIFT, {}, 0);
    ...
    if (m_vmo) {
        m_vmo->remove_mapping(*this);
        m_vmo = {};
    }
    ...
```

`VmMapping::map()` 开头加 `if (!is_active()) return Status::BadState;`：拆掉的映射没有 VMO 了，也没有东西可映射。`fault()`
本来就有同样的检查。

放下 VMO 可能就是它的最后一个引用，于是 `~VmObjectPaged` → `~VmCowPages` → `free_frames()` 在调用者持有的锁下运行；
这条路上没有会睡眠的锁，在自旋锁下也安全。

**参照**
Zircon `VmMapping::DestroyLockedObject()`（`zircon/kernel/vm/vm_mapping.cc` 约 :1120-1165）：先
`if (aspace_->last_fault_ == this) aspace_->last_fault_ = nullptr;`，最后 `object_.reset()`；对不再存活的映射，
`DestroyLocked()` 回 `ZX_ERR_BAD_STATE`。

**验证**
- 构建通过；clang-format 干净。
- 两个镜像照常：dev `vmalloc: self test passed`（vfree 走的就是拆映射这条路，现在它连 VMO 一起放掉）、`init: shell closed its channel`；
  dbg_1 `probes done (11 killed, 0 survived)`。
- 地址空间整体的回收在下一步（`VmAspace::destroy`），届时核对帧数。

**故意没做**
`VmAreaOrMapping::m_aspace` 不置空：地址空间销毁后，仍可能有人经 VMAR 句柄拿着区域对象，它们加锁要经过这个指针。

## VmAspace::destroy：进程 Dead 时拆掉它的地址空间，析构时从全局链表摘下（2026-09-25）

**文件**
- `kernel/mm/vm_aspace.cpp`、`include/ours/mm/vm_aspace.hpp` — 新增 `destroy()`、`m_destroyed`、`m_name`/`name()`；`~VmAspace()` 从 `ALL_ASPACE_LIST` 摘下；`page_fault_internal()` 对已拆的地址空间回 NotFound
- `kernel/mm/vm_area.cpp` — `VmArea::destroy()` 遇到映射拆除失败不再中途返回
- `kernel/object/process_dispatcher.cpp` — `finish_dead_transition()` 先调 `m_aspace->destroy()`，再清句柄表
- `docs/repairs/260924/object.md` — 第 2 条收窄为"只剩页表页不释放"

**症状**
进程死后，它的地址空间从来不会被释放：映射、程序映像、栈、只被它们引用的 VMO 的每一页，全部留着。每杀一个进程泄漏一份。

**原因**
三个引用环，谁也拆不开：
1. `VmAspace::m_root_vma` 持有根 VMA，根 VMA 的 `m_aspace` 持有地址空间；
2. 每个映射、每个子区域都持有 `Arc<VmAspace>`，它们又被根 VMA 的集合持有；
3. `m_last_fault` 持有一个映射，映射持有地址空间。

`~VmAspace()` 是空的，而且从来没有运行过——也就从来没人发现它还挂在 `ALL_ASPACE_LIST` 上：那个钩子是 SafeLink，
带着链接析构会 `__builtin_trap`。

**改法**
改前：

```cpp
VmAspace::~VmAspace() {}
```

```cpp
auto ProcessDispatcher::finish_dead_transition() -> void {
    auto const closed = m_handle_table->clean();
    ...
```

改后：

```cpp
auto VmAspace::destroy() -> Status {
    ...
    ustl::Arc<VmArea> root;
    ustl::Arc<VmMapping> last_fault;
    Status status;
    {
        Guard guard{m_lock};
        if (m_destroyed) {
            return Status::BadState;
        }
        m_destroyed = true;

        last_fault = ustl::move(m_last_fault);
        status = m_root_vma->destroy();
        root = ustl::move(m_root_vma);
    }
    ...
}

VmAspace::~VmAspace() {
    DEBUG_ASSERT(!m_root_vma && !m_last_fault, ...);
    Guard guard{ALL_ASPACE_LIST_LOCK};
    if (m_managed_hook.is_linked()) {
        static_cast<void>(ALL_ASPACE_LIST.erase(ALL_ASPACE_LIST.iterator_to(*this)));
    }
}
```

```cpp
auto ProcessDispatcher::finish_dead_transition() -> void {
    static_cast<void>(m_aspace->destroy());

    auto const closed = m_handle_table->clean();
    ...
```

- **打断环**：在锁内拆掉根 VMA 下的整棵树，再把 `m_root_vma` 与 `m_last_fault` 移出；移出的引用在放锁之后才释放，因为释放
  可能跑 `~VmMapping` 并释放 VMO 的页。
- **页表不在这里释放**：`destroy()` 通常跑在进程最后一个线程上，CR3 还指着这张表。页表留给对象真正析构的时候——那时最后
  一个引用在 DPC 线程上放掉，这颗 CPU 早已切走（下一步）。Zircon 同样这样拆成两半。
- **`m_aspace` 保留到进程对象析构**：还在跑的最后一个线程经 `aspace()` 拿它。
- **VMA 各自的 `Arc<VmAspace>` 不置空**：VMAR 句柄可能比进程活得久，区域加锁要经过它；这样的句柄看到区域已不活跃，所有操作回 BadState。
- **`VmArea::destroy()` 不再中途返回**：它原来在第一个映射拆除失败时就返回，剩下的树照样映射着、活着，而地址空间以为自己拆完了。
  现在报告第一个错误并继续；`VmMapping::destroy()` 无论 unmap 成败都会把映射从集合里摘下，所以循环总有进展。

**参照**
- Zircon `ProcessDispatcher::FinishDeadTransition()`（`zircon/kernel/object/process_dispatcher.cc:547`）：先拆地址空间，再处理句柄表。
- Zircon `VmAspace::Destroy()`（`zircon/kernel/vm/vm_aspace.cc:303` 起）：`DestroyLocked()` 根 VMAR、`root_vmar_.reset()`，不释放页表；
  `~VmAspace`（同文件 :270-293）先从全局链表摘下，再 `arch_aspace_.Destroy()`，:285-289 的注释写明了为什么页表要留到析构。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- dbg_1（默认配置、`SMP=1`）：11 个探针和 shell 各打出一行 `mm: aspace '…' destroyed`，`probes done (11 killed, 0 survived)`；
  dev：`init: shell closed its channel`。
- 临时在 `~VmAspace()` 里打一行日志（已去掉）：11 个 fault_probe 与两个镜像里的 shell 的析构都跑了，都紧跟在各自
  `process … destroyed` 之后，从 `ALL_ASPACE_LIST` 摘下时没有 trap——这是 `~VmAspace` 在本树里第一次运行。
- 观察到的一个现象：dbg_1 里落在 cpu0 上的探针，从线程退出到 `aspace destroyed` 要 0.6～1.5 秒，其余 CPU 上约 10 毫秒。
  时间花在日志上：dbg_1 以 trace 级别每释放一帧打一行 `Free frame`（每个探针约 22 行），而 cpu0 同时被约一万行
  `I'm Thread` 的调试输出占着，每行要等 10～100 毫秒。拆除本身只有这 22 页的工作量。

**故意没做**
- 页表页的释放：下一步，`~VmAspace()` 里的 `ArchVmAspace::destroy()`。
- 在锁外释放 VMO 的页：今天 `VmMapping::destroy()` 在地址空间的自旋锁下放掉 `m_vmo`，最后一个引用会在锁内释放它的页。
  这条路上没有会睡眠的锁，正确；只是锁持有时间随映射大小增长。等地址空间的锁换成可睡眠的锁（`docs/repairs/260925/mm.md`
  第 1 条）时一起处理。
- 从未 Running 过就被丢掉的进程（`create()` 的失败路径、`launch()` 的提前返回）不会走到 `destroy()`，它们的地址空间照旧因为
  引用环而泄漏。要等进程对象有 `on_zero_handles()`（计划 K23）才有地方统一处理。

## `pmm::free_frame_count()`：空闲帧总数，供"还回来了没有"这类核对（2026-09-25）

**文件**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `FrameSet::free_frames()`（按阶遍历 free 链表，块数乘块大小）；声明 `PmZone::free_frames()`
- `kernel/mm/pm_zone.cpp` — `PmZone::free_frames()`：持 zone 锁调上面那个
- `kernel/mm/include/ours/mm/pmm.hpp`、`kernel/mm/pmm.cpp` — `free_frame_count()`：对在线节点的本地 zone 求和

**症状**
没有办法回答"物理内存还回来了没有"。文件系统计划的 K6（拆地址空间、释放页表）要用"每个死进程前后的空闲帧数之差"
来证明没有泄漏，而 PMM 没有任何地方给出空闲帧数。

**原因**
缺的是接口。伙伴分配器本来就有逐阶的 `FrameBuddy::free_count(order)`，只是没人往上汇总。

**改法**
改前：无。

改后：

```cpp
    auto free_frames() const -> usize {
        usize total = 0;
        for (usize order = 0; order < FrameBuddy::NUM_ORDERS; ++order) {
            total += m_buddy.free_count(order) << order;
        }
        return total;
    }
```

```cpp
auto free_frame_count() -> usize {
    usize total = 0;
    PmNode::for_each_nid(PmNode::State::Online, [&total](NnId const nid) {
        ...
        for (auto *const zone: node->zone_queues()->get_queue(ZoneQueues::LocalContiguous)) {
            if (zone) {
                total += zone->free_frames();
            }
        }
    });
    return total;
}
```

- 只数本地 zone（`LocalContiguous`）：共享队列里有别的节点的 zone（用于回退分配），按节点累加会重复计数。
- 遍历链表而不是维护计数器：只用于诊断，不给分配路径加任何开销；代价是和碎片程度成正比的时间，以及结果一返回就可能过时——
  所以它适合做"之前/之后"的差，不适合当水位线用。

**参照**
Linux 的 `nr_free_pages()`（`global_zone_page_state(NR_FREE_PAGES)`）是维护出来的计数器；这里是按需遍历，更慢但零侵入。
Zircon 的 `pmm_count_free_pages()`（`zircon/kernel/vm/pmm.cc:154`）转调 `Pmm::Node().CountFreePages()`，同样是整机汇总。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- 临时启动探针（未提交）：分配一个 order-3 块前后与释放后分别计数，两个镜像都是"少 8、再恢复"
  （dev：126309 → 126301 → 126309；dbg_1：126240 → 126232 → 126240）。

**故意没做**
每 CPU 的帧缓存（`FramePcpuCache`）还是空壳，今天不持有帧；有了之后要把它们也算进来，或者明确说"不含 pcpu 缓存"。

## zone 锁关着中断拿：不带选项的 `Guard` 什么也不关（2026-09-25）

**文件**
- `kernel/mm/pm_zone.cpp` — 三处 `Guard guard{m_lock}` 换成 `ZoneGuard`（`Guard<sync::SpinLock, IrqSave>`）

**症状**
释放死进程页表的改动（下一节）加上之后，dbg_1 间歇性断言失败，三次启动挂一次：

```
Assertion failed on !is_held()
```

临时让这条断言打出锁地址和调用者（未提交）：锁在 physmap 里，调用者符号化后是 `ours::mm::PmZone::free_frame()`——
同一颗 CPU 重复获取了同一个 zone 的锁。

**原因**
`Guard guard{m_lock}` 经推导指引变成 `Guard<SpinLock>`，不带任何选项。`lockdep::Guard` 为每个选项构造一个状态保存器
（`std::tuple<StateSaver<LockType, Options>...> m_savers`），没有选项就是空 tuple：**既不关中断，也不关抢占**。
`spin_lock.hpp` 的注释自己写着"until the Guard learns to save that state, holding this lock disables nothing at all"。

于是线程可以持着 zone 锁被时钟中断抢占；同一颗 CPU 上的下一个线程再去释放同一个 zone 的帧，就看到锁"被本 CPU 持有"。
debug 构建里断言拦住；release 构建里它会一直自旋，直到自己也被抢占、让持锁者回来——一个潜伏的活锁。以前很少撞上，
死进程开始从 DPC 线程成批地还页表页之后，窗口就被频繁踩中了。

**改法**
改前：

```cpp
        Guard guard{m_lock};
        frame = m_fset.acquire_frame(order);
```

改后：

```cpp
    using ZoneGuard = Guard<sync::SpinLock, IrqSave>;
    ...
        ZoneGuard guard{m_lock};
        frame = m_fset.acquire_frame(order);
```

- **IrqSave 而不是 PreemptDisable**：PMM 在还没有线程的时候就在用，那时没有"抢占"可关；而在中断里释放的帧也不能撞上底下
  持着的同一把锁。关中断同时挡住了这两件事。
- 用的是共享原语本来就提供的选项，不是另写一个替代品。

**参照**
Linux 的 `zone->lock` 一律 `spin_lock_irqsave()`（`rmqueue_bulk()`、`free_one_page()`）。Zircon 的 PMM 锁是 `Mutex`，
与这里的模型不同。

**验证**
- 构建通过；clang-format 干净。
- 带着下一节的页表释放一起启动 dbg_1 连续 4 次，都是 `probes done (11 killed, 0 survived)`、没有断言（改前 3 次里挂 1 次，
  另一轮单次启动也挂过）。

**故意没做**
全树其余不带选项的 `Guard{spinlock}`（约 30 个文件）有同样的问题，属于 sync/lockdep 那条线：已交接给独立会话，由它在框架层面
决定"默认 Guard 关抢占"还是"必须写明选项"，再审一遍调用点。

## 死进程的页表页在 `~VmAspace` 里归还（2026-09-25）

**文件**
- `kernel/lib/arch/include/arch/paging/mod.hpp` — 分页引擎新增 `free_tables()`（与 `TableTeardown`），自底向上释放给定顶层表项下的全部中间表
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`、`kernel/arch/x86/mm/page_table.cpp` — `PageTable::destroy(base, end)`；`init_mmu()` 改经 `alloc_table()` 取根表
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`、`kernel/arch/x86/mm/arch_vm_aspace.cpp` — `ArchVmAspace::destroy()`；`m_range` 改成 `{base, base + size}`
- `kernel/mm/vm_aspace.cpp` — `~VmAspace()` 调 `m_arch.destroy()`，报告残留的叶子，debug 构建打出页表页数与空闲帧总数
- `docs/repairs/260924/object.md` — 第 2 条删除

**症状**
上一节之后，死进程的映射和 VMO 都还回来了，页表还没有：`~ArchVmAspace() = default`、`~PageTable() = default`，而 unmap 从不
释放变空的中间表。每个死进程留下它的 PML4 和用户半区的全部 PDPT/PD/PT。

**原因**
没有人释放页表，也没有能释放它的地方：`VmAspace::destroy()` 跑在进程最后一个线程上，CR3 还指着这张表，不能在那里释放。

**改法**
改前：`~VmAspace()` 只从 `ALL_ASPACE_LIST` 摘下自己。

改后：

```cpp
    template <typename PhysToVirt, typename Deallocator>
    static auto free_tables(PhysAddr table, PhysToVirt &&phys_to_table, Deallocator &&deallocate,
                            usize first, usize last) -> TableTeardown {
        TableTeardown result;
        free_below<Traits::PAGING_LEVEL>(table, phys_to_table, deallocate, first, last, result);
        return result;
    }
```

`free_below<Level>` 逐项：不存在的跳过；叶子（包括上层 PS=1 的大页）只计数、不释放；否则先递归，再清表项、释放子表。

```cpp
auto PageTable::destroy(VirtAddr const base, VirtAddr const end) -> TableTeardown {
    ...
    ASSERT(last < Engine::Traits::virt_to_index(TOP_LEVEL, KERNEL_ASPACE_BASE), ...);

    Guard<sync::SpinLock> guard(m_lock);
    auto result = Engine::free_tables(m_pgd_phys, phys_to_virt, free_table, first, last);
    free_table(m_pgd_phys, Engine::Traits::SIZE_OF_TABLE<TOP_LEVEL>);
    result.tables += 1;
    m_pgd_phys = 0;
    return result;
}
```

```cpp
auto ArchVmAspace::destroy() -> Teardown {
    ...
    DEBUG_ASSERT(arch::Cr3::read().get<arch::Cr3::PageTableAddress>() != m_page_table.pgd_phys(),
                 "freeing the page tables this CPU is running on");

    return m_page_table.destroy(m_range.start, m_range.end);
}
```

- **只在析构里释放**：最后一个引用在 `Thread::destroy()`（一个 DPC）里放掉，那时这颗 CPU 已经最后一次切离这个线程，切换时
  装入了别的根表；PCID 从未分配、用户叶子不再是 Global，所以这次写 CR3 冲掉了这张表喂过的全部 TLB 与分页结构缓存；进程只有
  一个线程，没有别的 CPU 在它上面跑。断言替代 Zircon 的 `active_cpus_ == 0`（本树不维护 `m_active_cpus`），至少在本 CPU 上
  抓住前提失效的那一天。
- **绝不碰内核的顶层表项**：`alias_to()` 把内核根表的顶层表项浅拷进每个用户根表，它们下面的表是内核的、所有地址空间共享。
  范围由 `m_range` 算出，并用 `ASSERT` 核对最后一项在内核那一项之前。
- **叶子不释放**：它们指向的帧归 VMO，`VmAspace::destroy()` 已经拆掉并放掉了；这里剩下叶子就说明拆除漏了什么，打 warn。
- **一对分配器**：根表原来直接 `alloc_frames()`，改经 `alloc_table()`，与其下所有表一样，由 `free_table()` 对称归还。
- **`m_range` 的构造**：`gktl::Range` 是 `{start, end}`，原来写成 `(base, size)`，end 被赋成了长度；之前没有人读过它。

**参照**
- Zircon `VmAspace::~VmAspace()`（`zircon/kernel/vm/vm_aspace.cc:270-293`）：先从全局链表摘下，再 `arch_aspace_.Destroy()`；
  :285-289 的注释说明页表为什么要等到析构。
- Zircon `X86ArchVmAspace::Destroy()`（`zircon/kernel/arch/x86/mmu.cc` 约 :913 起）：断言 `active_cpus_ == 0`，再释放顶层页。
  差别：Zircon 的 unmap（`RemoveMapping`）在表变空时就把它挂进 `ConsistencyManager` 延后释放，所以到 Destroy 时用户半区已空、
  只需释放顶层页；ours 的 unmap 从不释放表，所以在这里一次性扫一遍。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- dbg_1（默认配置 4 次、`SMP=1`、`CPU=max` 各 1 次，都在上一节 zone 锁修复之后）：每个地址空间（11 个 fault_probe 与 shell）
  都是 `gone, 6 page-table pages freed`——PML4、一张 PDPT、一张 PD，程序映像、vDSO、栈各一张 PT——从没有 `still had … pages
  mapped` 的 warn；`probes done (11 killed, 0 survived)`。dev：`init: shell closed its channel`。
- 设计里的另一项判据——"相邻两个进程的空闲帧数之差恰好是一个内核栈"——这里**核对不了**：相邻两次计数之间差 −26 到 +54，
  因为测量时系统并不静止（shell 在进程 Dead 时就起下一个探针，与上一个的析构并行；另有一组测试线程持续分配与释放）。能确认的是
  每个地址空间归还的页表页数恒定、没有残留叶子。要做精确的泄漏核对，得在静止点取数，留给有进程句柄与 TERMINATED 之后（计划
  K7、U6），那时 shell 可以等上一个进程彻底消失再起下一个。

**故意没做**
- unmap 时释放变空的表（Zircon 的做法）：进程活着的时候表只增不减，到死一次还清。长寿进程反复映射/拆除会一直攒着表，
  等真有这样的进程再做。
- 每个进程一个的内核栈仍随线程泄漏（`kernel/mm/stack.cpp`，释放要等 TLB shootdown），与本节无关，另案。

## mm 其余的自旋锁也都写明 IrqSave（2026-09-25）

**文件**
- `kernel/mm/vm_aspace.cpp`、`vm_area.cpp`、`vm_mapping.cpp` — 地址空间锁（`VmAspace::m_lock`，VmArea/VmMapping 的
  `get_lock()` 就是它）六处、`ALL_ASPACE_LIST_LOCK` 两处改为 `Guard<sync::SpinLock, IrqSave>`
- `kernel/mm/pm_node.cpp`、`include/ours/mm/pm_node.hpp` — 节点锁四处、当前线程资源锁三处改为 IrqSave
- `kernel/mm/object_cache.cpp` — `NodeCache::lock` 四处（原来写成 `Guard<sync::SpinLock>`，显式地不带选项）改为 IrqSave
- `kernel/mm/cpu_local.cpp` — `CpuLocal::Dyn` 的锁两处改为 IrqSave
- `include/ours/mm/vm_aspace.hpp`、`pm_node.hpp` — 锁的声明处写明为什么是 IrqSave

**症状**
潜伏，当前启动里没有观察到。和上面「zone 锁关着中断拿」一节是同一个缺陷的其余位置：持锁线程被抢占，同 CPU 下一个线程要
同一把锁，自旋在"本 CPU 持有"的锁上——dbg_1 在 `SpinLock::acquire()` 的 `DEBUG_ASSERT(!is_held())` 停下，release 版自旋
到自己也被抢占为止。最容易撞上的是地址空间锁：进程的一个线程在 `map`/`unmap`/`protect` 里持锁被抢占，同一进程的另一个
线程在同一颗 CPU 上缺页，缺页路径（`VmAspace::page_fault_internal()`）去拿同一把锁。今天用户进程都是单线程，所以碰不到。

**原因**
`Guard guard{get_lock()}` 经推导指引成了 `Guard<SpinLock>`；`Guard<sync::SpinLock> guard{lock}` 则是显式写出的同一个
类型。两者的 saver 元组都是空的，`SpinLock::acquire()` 本身只做 CAS，所以持锁期间中断和抢占都开着。改之前：

```cpp
    Guard guard{get_lock()};
    return unmap_locked(aligned_base, aligned_size);
```

```cpp
auto MemCache::NodeCache::take_partial() -> Slab * {
    Guard<sync::SpinLock> guard{lock};
```

缺页路径是在 #PF 处理函数里、中断还关着的时候拿地址空间锁的，所以缺页那一侧"碰巧"关着中断；线程上下文的持有者没有，
两侧不对称，而自旋锁要求**所有**持有者都不会在持锁时让出 CPU。

**改法**
全部 IrqSave：

```cpp
    Guard<sync::SpinLock, IrqSave> guard{get_lock()};
    return unmap_locked(aligned_base, aligned_size);
```

```cpp
auto MemCache::NodeCache::take_partial() -> Slab * {
    Guard<sync::SpinLock, IrqSave> guard{lock};
```

为什么不是 PreemptDisable：节点锁在 `InitLevel::CpuLocal + 1` 由 `PmNode::Current::init()` 拿，远早于
`InitLevel::Threading` 给启动 CPU 装上线程——那时 `AutoPreemptDisabler` 要经 `Thread::Current` 找 PreemptionState，
还无处可找；地址空间锁的缺页一侧本来就关着中断；`NodeCache` 在它上层的 per-CPU 缓存已经是 `Guard<LocalLock, IrqSave>`。
IrqSave 只碰标志寄存器，在哪个阶段都成立。

地址空间锁在关中断下持有的时间会变长（`map`、`destroy` 里的页表操作都在锁内），这是自旋锁本身的代价，不是这次改动引入的：
`docs/repairs/260925/mm.md` 第 1 条已经建议照 Zircon 把它换成可睡眠的锁，那时这里的 IrqSave 随之消失。

**参照**
- Linux SLUB `kmem_cache_node::list_lock`：`spin_lock_irqsave()`（`mm/slub.c` 的 `get_partial_node()`、`__slab_free()`）。
- Zircon `VmAspace::lock_` 是 `CriticalMutex`（`zircon/kernel/vm/include/vm/vm_aspace.h`），可睡眠，不存在这个问题。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- QEMU：dev 到 `init: shell closed its channel`、`--- end ---`；dbg_1 到 `shell: probes done (11 killed, 0 survived)`、
  `--- end ---`，无断言。每个 fault_probe 的地址空间照旧 `6 page-table pages freed`。

**故意没做**
- 没有把地址空间锁换成可睡眠的锁，那是 mm 的待修项，不是选项问题。
- `page_queues.cpp` 本来就写的 IrqSave，未动。

## `PmNode::Current` 的分配入口不再走穿空的 per-CPU 指针（2026-09-25）

**文件**
- `kernel/mm/include/ours/mm/pm_node.hpp` — 新增私有 `Current::resolve_local_node()`
- `kernel/mm/pm_node.cpp` — `Current::alloc_frames()`、`alloc_frames_fragment()`、
  `alloc_frames_bulk()` 三个入口改用它，拿不到节点就回 `Status::BadState`

**症状**
潜伏，当前没有调用者会在这个窗口里走到——但只要有，就是一次读低端物理内存的野指针。
触发条件：在 `PmNode::Current::init()`（`GKTL_INIT_HOOK(..., InitLevel::CpuLocal + 1)`）
之前调用不带节点参数的 `alloc_frames(gaf, ...)`。把 `alloc_page_table()` 的判据改成
「buddy 已接管」之后立刻就能看到，形态是

```
#GP General protection fault: vector=13, error_code=0x0
  rip=+0x37fd9   r14=0x22a840f00000004
```

`+0x37fd9` 在 `PmNode::alloc_frames` 里 `movq (%r14,%r13), %rdi`，`r14` 是
`ZoneQueues` 的 span 基址，非规范地址。

**原因**
`PmNode::Current` 继承 `CpuLocal::Current<PmNode *>`，那个 per-CPU 指针
（`CURRENT_INSTANCE`）由 `Current::init()` 在 `InitLevel::CpuLocal + 1` 发布。而
`mm::CpuLocal::init()` 本身排在 `InitLevel::CpuLocal`——它要给每个 CPU 建 unit，还要为
那些 unit 建页表，这些分配全都发生在指针被发布**之前**。此时 `Current::get()` 读到的是
模板里那份初值，也就是空。

空指针在这里不会当场挂掉，这是最坏的部分：`vmours` 接手的还是 physload 那张页表，
低端物理内存是恒等映射的，所以 `nullptr->m_zone_queues` 读的是物理页 0 附近的垃圾，
读得通。垃圾被当成 `Span<PmZone*>` 的基址，于是错误在 `get_frame_from_zone_queues()`
里以别的形态爆出来，离真正的原因很远。

这个坑踩过一次了：`Current::publish()` 的注释记着「That is what left CURRENT_INSTANCE
null across the zero-page allocation in `init_vmm_preheap()`: `Current::alloc_frames()`
went on through a null PmNode, and `get_frame_from_zone_queues()` spun on the
ZoneQueues it read out of address 0x48.」当时的修法是让 `publish()` 无条件发布，
把那一次的空补上；但**分配入口自己走穿空指针**这件事没动，所以窗口一换就又来一遍。

`DEBUG_ASSERT(local_node)` 没挡住，因为 `kernel.dev.obi` 是 `KERNEL_DEBUG_LEVEL=0` 的
那份镜像，断言编译掉了；断言完了还照样解引用，等于发布版里什么检查都没有。

**改法**
把「当前 CPU 的 PmNode」这件事收进一个函数：per-CPU 指针有就用，没有就退回
`Current::nid()`，即 `DYNAMIC_CPU_TO_NODE[cpu]`，再 `PmNode::node(nid)`。这张表是
`PmNode::bootstrap()` 用 `set_nid_early()` 填的，比任何 per-CPU 存储都早——
`nid()` 的注释本来就写着「The array is also the only answer that works this early」，
只是 `alloc_frames()` 当时没照着用。

`DEBUG_ASSERT` 换成真的 `return ustl::err(Status::BadState)`。走到这里只有两种可能：
AP 在 `claim_possible_cpus()` 之前分配（调用者的错），或者没有任何节点拥有内存
（`bootstrap()` 已经在那种情况下回 `BadState` 了）。两种都该是可见的失败，
不该是一次能读通的野指针。

没有把 `nid()` 直接替换掉 `get()`：per-CPU 指针是热路径上省一次数组查找和一次
`PmNode::node()` 的手段，发布之后它是对的，只是不能当成「永远非空」。

**参照**
Linux 在同一个窗口用同一张表：`early_per_cpu(x86_cpu_to_node_map, cpu)`
（`arch/x86/mm/numa.c`）在 per-CPU 区建立之前回答 `cpu_to_node()`，
`setup_per_cpu_areas()` 之后才切到 per-CPU 副本。差别在于 Linux 的
`early_per_cpu` 宏把「有没有 per-CPU 区」这个判断藏在一个地方，而这里原来是
每个调用点自己 assert 一下就往下走。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过。
- `out/bringup.x86` 启动无变化：一路到 shell 跑完并正常销毁进程，无 panic。
- `out/numa.x86` 仍然停在原来那个写缺页（`cr2=0xffffff8800016fff`）——本条是它的
  前置条件，不是它的修法，那条是 `docs/repairs/260925/mm.md` 第 1 条。

**故意没做**
- 没有动 `hard_affinity()`，它用的是 `CpuLocal::access(CURRENT_INSTANCE->m_hard_affinity)`
  那个「取成员地址当 handle」的写法，和本条无关。
- 没有让 `resolve_local_node()` 在节点不存在时找别的节点顶上。`fallback` 掩码是
  `PmNode::alloc_frames()` 里的事，这里只负责回答「我在哪个节点」。

## `InitStage` 拆开：buddy 接管和堆可用不再是同一个值（2026-09-25）

**文件**
- `kernel/mm/include/ours/mm/init_stage.hpp` — `InitStage` 在 `Pmm` 和 `Vmm` 之间插入 `Heap`
- `kernel/mm/include/ours/mm/pmm.hpp` — 新增 `heap_enabled()`；改正 `alloc_page_table()` 那段写反了的注释
- `kernel/mm/init_pmm.cpp` — `set_init_stage(InitStage::Pmm)` 移到 `EarlyMem::do_handoff()` 之后紧邻的一行；`init_cache()` 改成置 `InitStage::Heap`
- `kernel/mm/sparse_memmap.cpp` — `SparseMemMap::Impl::allocate()` 改判 `heap_enabled()`
- `docs/repairs/260925/mm.md` — 删掉第 1 条

**症状**
`out/numa.x86`（`mm_numa = true`，QEMU `NUMA=1`、8 CPU、1 GiB、四节点）启动到
`Released 16 regions, ... to the buddy allocator` 之后立刻死在一次写缺页：

```
[error] Unhandled page fault: vector=14, error_code=0x2
[error]   cr2=0xffffff8800016fff
[error]   rip=0xffffff8840024525 (+0x24525)  cs=0x10  rsp=0xffffff884008c3b8
[error]   rax=0xffffff8800000000  rcx=0x17000  rdx=0x17000  rsi=0xffffff803fe60000
```

`error_code=0x2` 是「不存在 + 写 + ring 0」。`+0x24525` 在 `memmove` 里
（`movb %dil, -0x1(%rax,%rcx)`），`rax` 是 `ARCH_CPU_LOCAL_VIRT_BASE`、`rcx` 是
`CpuLocal::Mapper::unit_size()`、`rsi` 是 physmap 里的 replica，所以挂在 `CpuLocal::init()`
里那句 `std::copy_n(replica, Mapper::unit_size(), base)` 的**第一条**存储上——`memmove`
见 `dst >= src` 走倒序拷，先写最后一个字节。`out/bringup.x86`（单节点）不复现。

在 fault 现场用 QMP 顺着 `CR3` 走页表，上面三级都在，叶级整页是 0：

```
PML4[511] @0x3febbff8 = 0x000000003fea0027   -> PDPT 0x3fea0000
PDPT[0x20] @0x3fea0100 = 0x000000003fe9c027  -> PD   0x3fe9c000
PD[0]      @0x3fe9c000 = 0x000000003fe9b027  -> PT   0x3fe9b000
PT[0..31]  @0x3fe9b000 = 全 0
```

`build_dense()` 把 23 个页分 16+4+2+1 四次映射，四次都返回 Ok；叶级表是事后被整页清零的。

**原因**
一个 `InitStage` 同时回答两个互不相干的问题，而这两件事在时间上不重合。

`pmm_enabled()` 的名字和注释都说的是「the real page allocator has taken over from
EarlyMem」——这件事在 `EarlyMem::do_handoff()` 那一刻就成立了。但
`set_init_stage(InitStage::Pmm)` 原来写在 `init_cache()` 里，也就是**两步之后**，
因为 `SparseMemMap::Impl::allocate()` 也拿同一个判据分支，而它的非 early 路径是
`new`，要等 kmalloc。于是同一个值被迫同时表示「buddy 接管了」和「堆能用了」，
只能取两者中较晚的那个。

`do_handoff()` 和 `init_cache()` 之间夹着 `mm::CpuLocal::init()`（`kernel/top/main.cpp:151`）。
它经 `ArchVmAspace::map` → `PageTable::map_pages` → `alloc_table` → `alloc_page_table()`
要页表页，而后者按 `pmm_enabled()` 分支——此时还是 false，于是走 EarlyMem：

```cpp
auto alloc_page_table(usize size, usize alignment, NnId node) -> PhysAddr {
    if (pmm_enabled()) { /* buddy */ }
    auto *table = EarlyMem::allocate<u8>(size, alignment, node);
    ...
}
```

关键在于 `do_handoff()` **不是一次完整的交接**：它只是把 bootmem 里还标着空闲的区间
逐个 `free_frames()` 给 buddy，bootmem 自己的位图原封不动。所以从那一刻起，
同一批物理页有两个主人。`alloc_page_table()` 走 EarlyMem 拿到的 `0x3fe9c000`、`0x3fe9b000`
（TopDown，紧贴 physload 自己那几张表下面）在 bootmem 里被标成已用，但 buddy 手上那份
副本不会因此消失。接着 `build_dense()` 为 unit 1..15 调
`alloc_frames(nid, GAF_BOOT, order)`（`GAF_BOOT` 含 `Gaf::Zero`），NUMA 下节点 3 的内存
就在顶端，于是某个 unit 拿到了 `0x3fe9b000` 并清零——擦掉了还在用的叶级页表。

`alloc_page_table()` 的注释把这条不变量写反了：

> Before the buddy this comes from EarlyMem, which never gives it back:
> do_handoff() releases what bootmem still calls free, and an allocation is not free.

「分配过的就不是空闲的」只在 `do_handoff()` **之前**分配才成立。这条注释是对的推理
用在了错的时刻上，而正是 `pmm_enabled()` 迟到两步把那个时刻拖了过来。

单节点之所以躲过去：只有一个节点，buddy 从低端发页，碰不到 EarlyMem TopDown 占的顶端。
这也是为什么同样的代码在 `out/bringup.x86` 上一直是好的——它不是好的，只是没被踩到。

**改法**
把两个问题拆成两个值：

- `InitStage::Pmm` = buddy 拥有物理内存，紧跟 `do_handoff()` 置位；`pmm_enabled()` 照旧读它，
  现在名副其实。
- `InitStage::Heap` = 对象缓存、kmalloc、`ktl` 的 memory resource 都在，`new` 可用；
  `init_cache()` 置位。新增 `heap_enabled()` 读它，`SparseMemMap::Impl::allocate()` 改用它。

两个枚举项都写了注释说明各自断言的是什么事实，以及为什么不能合并——合并就是这条 bug。

被否掉的两个方案：

- **只把 `set_init_stage(InitStage::Pmm)` 提前，不加 `Heap`**：实测缺页没了，换成
  `#GP  rip=+0x37fd9`，因为 `SparseMemMap::Impl::allocate()` 会在堆还不存在时走 `new`。
  `init_cache()` 原来那句「It waits for the heap rather than for the page allocator」
  说的就是这件事，它是对的，只是把约束加在了错的地方——该拆的是判据，不是推迟事实。
- **让 `do_handoff()` 把交出去的区间在 bootmem 里标成已用**（或者之后一律拒绝分配）：
  这只是把静默踩内存变成一次可见的失败，`CpuLocal::init()` 依然拿不到页表页，还得配合
  本条才能起来。可以以后作为一道额外的护栏加上，但不是修法。

还有一件不是选择而是前置条件：`pmm_enabled()` 提前之后，`CpuLocal::init()` 的页表改从
buddy 拿，而 `alloc_page_table()` 走的是不带节点参数的 `alloc_frames(gaf, ...)`，那条路经
`PmNode::Current::get()`——那个 per-CPU 指针要到 `InitLevel::CpuLocal + 1` 才发布。
所以先有上一条「不再走穿空的 per-CPU PmNode」，本条才能起来；两条一起才是完整的修法。

**参照**
Linux 把这两件事分给两个各自独立的判据，从来没有合过：`alloc_low_pages()`
（`arch/x86/mm/init.c`）看 `after_bootmem`，而 `after_bootmem` 就在
`mm_core_init()` 里 `memblock_free_all()` 之后置位——和这里把 `InitStage::Pmm` 紧贴
`do_handoff()` 是同一个位置、同一个理由。「堆能不能用」则由
`slab_is_available()`（`mm/slab_common.c`，`slab_state >= UP`）单独回答，
`kmem_cache_init()` 里置位。本条做的就是把 ours 这边合成一个的东西拆回这两个。

另外 Linux 的 `memblock_free_all()` 会把交出去的区间从 memblock 的可用集合里摘掉
（`memblock_discard()` / `memblock.memory` 之后不再被 `memblock_alloc` 使用），
所以它没有「交接之后 memblock 还在发同一批页」这个口子。ours 的 `do_handoff()`
目前没有这一步，本条靠的是不再有人在那之后问 EarlyMem 要页；把那一步补上是另一件事。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置全量构建通过。
- `out/numa.x86`（`NUMA=1`，8 CPU，四节点）**首次完整启动**：8 个 CPU 全部 online
  （`CPU[1]..CPU[7] is online`），shell 跑完 `channel checks passed`、
  `handle transfer checks passed`、`handle quota checks passed`、
  `bad-pointer checks passed`、`vmo_read checks passed`、`libc checks passed`，
  进程正常销毁；整段日志里没有 page fault / #GP / panic / 断言失败。
- `out/bringup.x86` 无回归：同样跑完全部 checks 并正常销毁，无 panic。
- 对照：把这两条改动 checkout 回 HEAD~2 重新构建再启动，缺页的 vector、cr2、rip 与
  改动前完全一致，确认修的就是这一条。

**故意没做**
- 没有让 `do_handoff()` 在 bootmem 里回填「已交出」。见「改法」里被否掉的第二条——
  它是一道护栏而不是修法，而且要单独验证「EarlyMem 之后一律失败」不会打到别的调用者。
  作为待修项留在 `docs/repairs/260925/mm.md`。
- 没有修 `alloc_page_table()` 忽略 `node` 参数这件事。它在 buddy 分支上从不使用传进来的
  `node`，调用者指定的节点被丢掉；这是另一个独立的缺陷，同样留在待修项里。
- 没有动 `free_page_table()` 的 `!pmm_enabled()` 早退。判据提前之后它反而更对了：
  窗口里的页表现在本来就来自 buddy，还给 buddy 是对的。
- 没有碰 `InitStage::Vmm` / `All` 的含义，也没有给它们加判据函数——当前没有调用者按它们分支。

## `do_handoff()` 交接之后把 bootmem 整个封掉，让它再也发不出内存（2026-09-25）

**文件**
- `kernel/mm/early_mem.cpp` — 新增 `retire_bootmem()`，`EarlyMem::do_handoff()` 在
  `free_unused_memory()` 之后调用它
- `docs/repairs/260925/mm.md` — 删掉第 2 条的 (a)

**症状**
潜伏，当前触发不到。上一条把 `InitStage::Pmm` 提到 `do_handoff()` 后面之后，已经没有
调用者会在交接之后问 EarlyMem 要内存了。但那是**调用者的性质**，不是 bootmem 的性质：
下一个在那之后调 `EarlyMem::allocate()` 的人会拿到 buddy 也认为自己拥有的页，
而且和上次一样安静——不报错、不返回 null，就是两个主人写同一页。

**原因**
`free_unused_memory()` 把 bootmem 里还标着空闲的区间逐个 `free_frames()` 交给 buddy，
然后**不动 bootmem 自己的位图**。于是从那一行起，同一批物理页有两个主人，谁先发出去谁赢，
另一个不知情。这是一次不完整的交接：给出去了，但没有从账上划掉。

这不是假设。`alloc_page_table()` 原来就在这个窗口里从 EarlyMem 拿页表页，
接下来一个用 `Gaf::Zero` 分配的 per-CPU unit 落在了还在用的叶级页表上把它清零，
启动在几千条指令之后死在一个 PTE 已经蒸发的虚拟地址上
（见本文件「`InitStage` 拆开」那一条）。

**改法**
交接完成后，用一条 `EarlyMem::reserve(min_address(), max_address() - min_address())`
把 bootmem 描述的整个范围标成已保留。`find_free()` 从此找不到任何空闲区间，
所以交接之后的 `allocate()` 返回 null，调用者现有的 null 检查
（`alloc_page_table()` 回 0 → `Status::NoMemory`；`init_bootstrap16_buffer()` 的
`ASSERT(buffer, ...)`）就把它变成一次可见的失败，而不是一次静默的踩内存。

**一条整段，而不是逐个区间**，有两个原因：

- 不能在 `visit_free()` 的遍历里边走边 `reserve()`——保留会改变 free 视图，
  遍历器看到的剩余区间当场就变了。
- 既然释放的是**全部**空闲区间，「保留刚释放的」和「保留全部」是同一个集合。
  顺带保留了本来就不可用的空洞也没有代价：保留只从 usable 列表里减，
  空洞那里本来就没有 usable 区间可减。

`ASSERT` 而不是记一行日志：这一步失败意味着 bootmem 仍然愿意发出 buddy 拥有的内存，
正是这个函数存在的目的所在，带着这个状态往下走没有意义。

`max_address()` 是 `end()`，即开区间上界，所以尺寸是 `upper - lower`；
`usable_count_ == 0` 时两者都是 0，提前返回，因为 `bootmem::Range::from()` 拒绝零长度。

被否掉的方案：**给 EarlyMem 加一个 `handed_off_` 标志，之后一律拒绝分配并断言**。
效果类似，但它是在 bootmem 旁边再放一个「bootmem 还有没有内存」的说法，
而 bootmem 的保留列表本来就是回答这个问题的地方——两个东西说同一件事，就会有一天说不一致
（`pmm_enabled()` 的注释里已经写过这条理由）。

**验证过交接之后确实没人再要**：`EarlyMem::allocate()` 的全部调用点——
`init_bootstrap16_buffer()`（`PlatformInitLevel::PrePmm`）、`PmNode::bootstrap()`
与 `PmNode::init()`（`init_pmm()` 内，`do_handoff()` 之前）、
`CpuLocal::Mapper::init_early()`（`start_kernel` 里远在 PMM 之前）、
`SparseMemMap::Impl::allocate()`（早期路径，memmap 在 `do_handoff()` 之前建完，
之后走 `heap_enabled()` 那支）、`alloc_page_table()`（上一条已经改成走 buddy）——
全部在交接之前。`free_bootstrap16_buffer()` 把那三页还给 buddy 而不是还给 EarlyMem，
不受影响。

**参照**
Linux 从另一头到达同一个状态：`memblock_free_all()` 结束时调 `memblock_discard()`，
把 memblock 的数组整个扔掉，此后 `memblock_alloc()` 是一次 NULL 解引用，
而不是一次静默的双重分配。ours 这边 bootmem 的存储是静态数组，扔不掉，
所以用「全部保留」表达同一件事。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过。
- `out/numa.x86`（`NUMA=1`，8 CPU，四节点）启动无变化：8 个 CPU 全部 online，
  shell 六组检查全过，进程正常销毁，`Released 16 regions, 255668 frames (998 MiB)`
  与上一条相同，无 fault / panic / 断言失败。
- `out/bringup.x86` 无回归：同样跑完全部 checks 并正常销毁。

**故意没做**
- 没有让 `EarlyMem::allocate()` 在交接之后额外断言。返回 null 已经足够，
  而调用者本来就该检查分配失败；再加一层断言等于替调用者决定 null 一定是 bug。
- 没有动 `EarlyMem::deallocate()` / `release()`。交接之后没人调它，
  而它本来就是 `allocate()` 的反操作，跟着一起失效是自然的。
- 没有回收 bootmem 的静态存储（`USABLE_STORAGE` / `RESERVATION_STORAGE`）。
  它们在 `.init` 数据里，`reclaim_init_area()` 会连着整段一起还给 buddy；
  单独处理它们没有意义。
