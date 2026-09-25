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
