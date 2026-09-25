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
