# kernel/mm — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. VMO 与用户缓冲区之间直接拷贝，删掉中转页（2026-09-27）

**文件**
- `kernel/mm/include/ours/mm/vm_object.hpp` — 基类加 `read_user()` / `write_user()`，默认 Unsupported
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`、`kernel/mm/vm_object_paged.cpp` — 实现二者；`read_write()` 补上 capture、放锁、重试
- `kernel/mm/BUILD.gn` — 依赖 `//kernel/lib/user_copy`；`:headers` 带上它的 include 路径 config
- `kernel/object/vmo_dispatcher.cpp` — `read_user()` 删掉中转页，改为转发
- `kernel/commands/user_copy_test.cpp` — `usercopy.test` 加 5 项 VMO 检查
- `docs/development/kernel/sync/atomic-context.md` — "还没有 capture 接口"那句改成现状

**症状**

`VmoDispatcher::read_user()` 每页经一个内核中转页：先在 VMO 锁里读进中转页，放锁，再拷给用户。
每页多一次 memcpy，每次调用多一页 kmalloc。中转页存在的唯一理由写在它自己的注释里：

```cpp
/// This kernel has no capture-faults copy -- its copy takes the fault, and
/// resolving it goes through the page-fault path -- so the lock has to be gone
/// before the copy starts, and a kernel page is what carries the bytes across
/// the gap. The cost is one extra memcpy per page.
```

capture 已经有了（`x86-kernel.md` 第 1 条、`kernel-lib-user-copy.md` 第 1 条），这个理由不再成立。

**原因**

在 VMO 锁里直接拷用户内存，难点只有一个：用户那边缺页时不能就地解析。就地解析要拿地址空间的锁，
放进 VMO 锁里面就和缺页路径"地址空间 → VMO"的锁序反了；用户缓冲区要是正好映射着这个 VMO，
就是自己等自己。capture 让拷贝只报告缺页、不解析，于是锁可以一直持着，直到真的需要解析时再放。

`VmObjectPaged::read_write()` 原本就照 Zircon 的 `ReadWriteInternal()` 写，注释里写明了少掉的正是这一段：

```cpp
/// minus everything that exists for a user buffer: dropping the lock around a
/// copy that may fault, the partial-length return, the soft fault retry.
```

**改法**

`read_write()` 的拷贝回调从"无返回值"改为返回 `UserCopyCaptureResult`；内核缓冲区的回调恒返回 Ok，
用户缓冲区的回调用 capture 版拷贝。外面多一层循环：

改前（单次持锁、cursor 建一次走到底）：

```cpp
Guard guard{m_mutex};
...
auto cursor = m_cow_pages->make_cursor(first_page, end_page - first_page);
...
while (done < size) {
    ...
    auto page = cursor->require_page(will_write, 1, &request);
    ...
    copy(base + in_page, done, chunk);
    done += chunk;
}
```

改后（缺页就跳出锁、解析、从 `done` 处重建 cursor 再来）：

```cpp
while (done < size) {
    std::optional<UserCopyFault> fault;
    {
        Guard guard{m_mutex};
        // 每次重新拿锁都重新检查范围、按 offset + done 重建 cursor
        auto cursor = m_cow_pages->make_cursor(align_down(offset + done, PAGE_SIZE), ...);
        while (done < size) {
            ...
            auto const result = copy(base + in_page, done, chunk);
            if (result.fault.has_value()) {
                fault = result.fault;
                break;
            }
            ...
            done += chunk;
        }
    }
    if (!fault.has_value()) {
        break;
    }
    if (retried_at == fault->va && retried_done == done) {   // 解析说好了、拷贝却原地再缺页
        log::warn(...);
        return Status::InvArgs;
    }
    retried_at = fault->va;
    retried_done = done;
    if (task::Thread::Current::soft_fault(fault->va, fault->cause) != Status::Ok) {
        return Status::InvArgs;
    }
}
```

几个要点：

- **cursor 每次重建，不复用。** 放锁期间 VMO 谁都能动：这个偏移上的页可能被换掉，或被 decommit
  之后内存另作他用。能活过放锁的只有 `done`——它是本函数自己的进度，和 VMO 状态无关。
- **活锁检查。** `soft_fault()` 说解析成功、拷贝却在同一地址、零进度地再次缺页，继续转就是永远空转且
  没有任何提示，所以改为返回 InvArgs 并记一行 warn。竞争也可能走到这里（页刚补上又被 unmap），
  那时 InvArgs 本来就对：程序自己把缓冲区拿走了。Zircon 没有这道检查。
- **`write_user()` 的重试会把被打断的那一块整块重写一遍**，写进 VMO 页的是同样的字节，无害。

`VmoDispatcher::read_user()` 保留调用层面的检查（越界、零长度、用户地址范围），然后直接
`m_vmo->read_user(...)`；`Bounce` 结构、`ustl::Box`、`new (GAF_KERNEL)` 分配全部删除。

构建：mm 的源码依赖 `//kernel/lib/user_copy`。`vm_object.hpp` 现在 include `<user_copy/user_copy.hpp>`，
所以 `//kernel/mm:headers` 要把 user_copy 的 include 路径传出去——但只能传 config
（`//kernel/lib/user_copy:headers.config`），不能依赖它的 `:headers` 目标：后者反过来依赖
`//kernel/mm:headers`（它的头文件用到 `mm::VmfCause`），双向依赖是 GN 拒绝的环。heap 拿 mm 的 config
也是同样的做法、同样的理由。

**参照**

`Zircon VmObjectPaged::ReadUser()` / `WriteUser()` / `ReadWriteInternal()`（`vm/vm_object_paged.cc`）：
锁内 `copy_array_to_user_capture_faults()`，缺页就跳出，锁外 `Thread::Current::SoftFaultInRange()`，
带着进度重试。`VmObject::ReadUser()` 对不支持的对象返回 `ZX_ERR_NOT_SUPPORTED`，这里物理 VMO 同样落在基类的默认实现上。

**验证**

构建通过；`clang-format` 跑过全部改动的 C++ 文件，`clang-tidy -p out/bringup.x86` 跑过
`vm_object_paged.cpp`、`vmo_dispatcher.cpp`、`user_copy_test.cpp`，改动行无新告警。

启动本身就走了新路径：`kernel.dev.obi` 里 `init` 经 `vmo_read` 读 BootFS 头，日志
`init: bootfs has 5 entries`（与 userabi 的 `5 files` 一致）；`kernel.dev.dbg_1.obi` 里是
`shell: bootfs has 4 entries`。不过这两次的用户缓冲区都在已在内存的栈页上，不会走到 capture。

`usercopy.test` 新增的 5 项专门逼出 capture（`kernel.dev.obi`）：

```
read_user(VMO[0..16) -> absent Q0): 0, Q0 holds 0x1111222233334444 0x5555666677778888
read_user(VMO seam -> Q0 end + absent Q1): 0, got 0x9999aaaabbbbcccc 0xddddeeeeffff0000
write_user(Q0 -> VMO[8..16)): 0, VMO holds 0x123456789abcdef
read_user(VMO -> hole): 10
read_user(VMO[0..16) -> its own mapping at 0x207000): 0, VMO[PAGE..] holds 0x1111222233334444 0x123456789abcdef
usercopy.test: ok -- 18 checks
```

- 第 1 项的目标页一开始不在内存，而同一测试的前一组已经证明 capture 拷贝**不会**顺手补页；所以这里读成功，
  就只能是走了"捕获 → 放锁 → `soft_fault()` → 重试"这一整圈。
- 第 2 项前 8 字节落在已在内存的 Q0 末尾，后 8 字节在不在内存的 Q1：成功且两个值都对，说明重试是从第 9 个字节、
  从 VMO 第二页重建的 cursor 接着来的，没有重复也没有漏。
- 第 5 项：用户缓冲区是**同一个 VMO 自己的映射**，页表里还没有这一页。拷贝在持有这个 VMO 的锁时缺页，
  而解析它要查的正是这个 VMO。结果成功，VMO 第二页里是第一页的 `A E`。缺页路径目前还不拿 VMO 的锁
  （`docs/repairs/260925/mm.md` 第 1 条的第 3 步），所以这一项今天证明的是"不丢字节"；等第 3 步落地，
  它就同时是死锁的回归测试——就地解析的拷贝会让这条命令永远不返回。

`kernel.dev.dbg_1.obi`（`DEBUG_ASSERT` 开着）同一次启动连跑两遍，36/36，无断言；日志里没有出现活锁检查的 warn。

**故意没做**

- `write_user()` 在对象层还没有调用方（没有 `sys_vmo_write`），目前只由 `usercopy.test` 调到。
- 没有做 Zircon 的"部分长度返回"：失败就是失败，与 user_copy 其余接口的约定一致。
- 缺页路径在建 cursor 前拿 VMO 的锁（第 1 条的第 3 步）还没做；那之前页树仍只由地址空间的锁间接保护，第 1 条不删。

## VmObjectPaged::read_write 的 copy 参数由模板改为 ustl::function::Function（2026-09-27）

**文件**
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`：新增 `CopyPage` 别名，`read_write()` 不再是模板
- `kernel/mm/vm_object_paged.cpp`：定义随之改签名，四个调用方不动
- `kernel/mm/include/ours/mm/vm_mapping.hpp`、`pm_node.hpp`：删掉没人用的 `<ustl/function/fn.hpp>`

**症状**
没有功能问题，是代码体积问题。`read_write()` 是 `template <typename Copy>`，
`read()`、`write()`、`read_user()`、`write_user()` 各传一个类型不同的 lambda，
所以整段页遍历被实例化了四份：范围检查、cursor、重试循环、活锁检查、日志。

**原因**
模板换来的是每页省一次间接调用。可每次调用要拷贝的是最多一整页（4 KB），
一次间接调用在它面前可以忽略。代价却是四份几乎一样的代码，这正是项目拒绝
virtual 的理由（指令缓存）反过来落在了自己身上。这个模板是 `92da681e` 引入的，
`8b84de4c` 加 user 拷贝时原样沿用，没有重新权衡。

**改法**

之前：

```cpp
template <typename Copy>
auto read_write(VirtAddr offset, usize size, bool will_write, Copy const &copy) -> Status;
```

之后：

```cpp
using CopyPage =
    ustl::function::Function<auto(u8 *page, usize done, usize chunk)->UserCopyCaptureResult>;

auto read_write(VirtAddr offset, usize size, bool will_write, CopyPage const &copy) -> Status;
```

读和写的页指针原本一个是 `u8 const *`，一个是 `u8 *`。统一成 `u8 *` 之后，
读方向的 lambda 照样写 `u8 const *` 参数，`u8 *` 能隐式转换过去，所以一个类型
就能覆盖两个方向。四个 lambda 都只捕获一个指针（`dst`、`src`、`UserOutPtr`、
`UserInPtr`），都在 24 字节以内。

被否掉的方案是"函数指针 + `void *context`"，即线程入口和 Dpc 用的写法。
能用，但每个调用方都得自己把缓冲区装进 context 再在回调里转回来。有了
`Function`，这一步可以省掉，而且同样保证不分配。

**参照**
Zircon `VmObjectPaged::ReadWriteInternal()`，它的 copy 参数也是模板
（`T copyfunc`）。这里没有照搬，理由见上。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：两个镜像的 `usercopy.test` 都是 18/18，包括缺页重试和"读进同一个
  VMO 的映射"这两项；无 panic。
- 没有量化代码体积变化（未实测）。

**故意没做**
没把 `read_write()` 提到 `VmObject` 里共用。`VmObjectPhysical` 还不支持 read/write，
现在只有一个使用者。

## VmObjectPaged::read_write 改名为 copy_page_by_page（2026-09-27）

**文件**
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`、`kernel/mm/vm_object_paged.cpp`：改名，注释补上函数做什么
- `kernel/mm/include/ours/mm/vm_object.hpp`、`kernel/object/vmo_dispatcher.cpp`：注释里的引用跟着改

**症状**
无运行时症状，是可读性问题。`read_write` 看起来像是"读或写"，但这个函数本身既不读也不写：
它把 `[offset, offset + size)` 一页一页取出来交给 `copy`，
真正搬字节的是 `copy`，搬的方向也由 `copy` 决定。

**原因**
这个名字来自 Zircon 的 `ReadWriteInternal()`，说的是谁调用它（read 和 write 两条路径），
而不是它做了什么。读者得先看完函数体，才知道它是一个按页遍历的循环。

**改法**

之前：

```cpp
auto read_write(VirtAddr offset, usize size, bool will_write, CopyPage const &copy) -> Status;
```

之后：

```cpp
auto copy_page_by_page(VirtAddr offset, usize size, bool will_write, CopyPage const &copy)
    -> Status;
```

**参照**
Zircon `VmObjectPaged::ReadWriteInternal()`，注释里仍然写着它，方便对照。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：两个镜像的 `usercopy.test` 都是 18/18，无 panic。

**故意没做**
没改 `will_write` 和 `CopyPage`。前者说的是"这一页要不要可写"，名字准确；
后者就是"复制一页"，也名副其实。

## VmMapping::fault 改名为 page_fault（2026-09-27）

**文件**
- `kernel/mm/include/ours/mm/vm_mapping.hpp`、`kernel/mm/vm_mapping.cpp`：改名
- `kernel/mm/vm_aspace.cpp`：唯一的调用方
- `kernel/mm/include/ours/mm/vm_fault.hpp`、`kernel/arch/x86/mm/fault.cpp`：注释里的引用

**症状**
无运行时症状，是可读性问题。

**原因**
单写 `fault`，读起来像是"制造一次错误"，而不是"处理一次缺页"。调用链上其它几层的
名字都带着 `page_fault`（`VmAspace::page_fault_internal()`、`Thread::Current::page_fault()`），
唯独到了 VmMapping 这一层断掉了。Zircon 在这一层叫 `VmMapping::PageFault()`。

**改法**
之前：`auto fault(VmFault *vmf) -> ustl::Pair<Status, usize>;`

之后：`auto page_fault(VmFault *vmf) -> ustl::Pair<Status, usize>;`

**参照**
Zircon `VmMapping::PageFault()`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：两个镜像的 `usercopy.test` 都是 18/18，无 panic。

**故意没做**
`Thread::Current::fault(VirtAddr, F &&)` 没改名。它是 `page_fault()` 和 `soft_fault()` 共用的私有辅助函数，只负责挑出当前线程的地址空间再转交，要改另外处理。

## 缺页和 map 在碰 VMO 的页树之前先拿 VMO 的锁；map() 拿地址空间锁（2026-09-27）

修掉 `docs/repairs/260925/mm.md` 的第 1 条。前两步见 `260926/x86-mm.md`（#PF 在允许睡眠时开中断）
和 `260926/mm.md`（`VmAspace::m_lock` 改为 Mutex）。

**文件**
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`：新增 `lock()`；`make_cursor()` 加上 `TA_REQUIRES(m_mutex)`
- `kernel/mm/vm_mapping.cpp`：
  - `page_fault()` 和 `map_paged()` 建 cursor 之前取 `Guard{vmo->lock()}`，一直持到页表写完；
  - `map()` 开头取 `Guard{get_lock()}`，删掉第 289 行的 FIXME
- `docs/repairs/260925/mm.md`：删掉第 1 条，原第 2 条改为第 1 条

**症状**
潜伏，当前触发不到：还没有两个地址空间同时映射同一个 VMO，也没有一边缺页一边
`vmo_write` 的场景。一旦出现，最早会是 K16 的共享 vDSO VMO，其次是 K17/K21 里
launcher 先 `vmo_write` 再映射。到那时 `VmCowPages::m_pages` 这棵 B 树会被并发插入，
表现为页丢失、同一个偏移上出现两个页，或者 B 树结构被破坏。

**原因**
`m_pages` 只有一把锁，就是 `VmObjectPaged::m_mutex`。`commit_range_internal()` 和
`copy_page_by_page()` 都持着它，唯独缺页路径没有：`VmMapping::page_fault()` 只持地址空间锁，
就直接 `make_cursor()`、`require_page()`，边查边插。`map_paged()` 也一样。

地址空间锁保护的是"这个地址空间里有哪些映射"，管不到"这个 VMO 里有哪些页"。
两个地址空间各持各的锁，同时对同一个 VMO 缺页，互相完全看不见；一边缺页、一边
`write()` 持 VMO 锁，同样看不见对方。说"两把锁"其实不准确，这棵树在缺页路径上根本没被锁住。

以前改不了，是因为地址空间锁是自旋锁，而且持锁时关着中断，在它里面不能拿会睡眠的
Mutex。前两步把它换成了 Mutex，并让 #PF 只在允许睡眠的上下文里进入 VM，现在才能嵌套。

`map()` 那个 FIXME 是同一类问题的另一面：它不拿地址空间锁，所以会和 `protect()`、
`unmap()` 并发修改页表和 `m_protection_ranges`。如果 `is_active()` 检查通过之后恰好
有人 `destroy()`，页还会被装进一个已经不属于任何映射的区间。

**改法**

`page_fault()` 之前：

```cpp
auto cursor = paged->make_cursor(m_vmo_ofs + (va - m_base), PAGE_SIZE);
...
arch_aspace.map(va, frame_to_phys(*page), 1, mmuf, ...);
```

之后：

```cpp
Guard guard{paged->lock()};
auto cursor = paged->make_cursor(m_vmo_ofs + (va - m_base), PAGE_SIZE);
...
arch_aspace.map(va, frame_to_phys(*page), 1, mmuf, ...);   // 仍在 VMO 锁内
```

`map_paged()` 同理。`map()` 在最前面拿地址空间锁，`is_active()` 的检查也被包进了锁里。

VMO 锁要一直持到页表写完，不能拿到页就放。否则在"查到页"和"写进页表"之间，
这个页可能被 decommit 掉并还给分配器，页表里就会装上一个已经归别人的物理页。
Zircon 的 `PageFaultLockedObject()` 也是持着对象锁完成映射的。

锁序固定为地址空间在外、VMO 在内。反方向的嵌套只可能出现在
`copy_page_by_page()` 里：它持 VMO 锁拷贝用户内存，拷贝过程中缺页。
但那里用的是捕获式拷贝，会先放掉 VMO 锁再调 `soft_fault()`，所以不会形成反向嵌套。

`make_cursor()` 加上 `TA_REQUIRES(m_mutex)`，把"建 cursor 必须持 VMO 锁"
从约定变成编译期检查。实测：临时删掉 `page_fault()` 里的 Guard，编译报错
`calling function 'make_cursor' requires holding Mutex 'paged->m_mutex' exclusively`。

**参照**
Zircon：
- `VmMapping::PageFault()`（`zircon/kernel/vm/vm_mapping.cc`）：对分页 VMO 先取
  `Guard<CriticalMutex>{paged->lock()}`，再进 `PageFaultLockedObject()`；
- `VmMapping::MapRange()`：同样在对象锁下建 lookup cursor 并写页表。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过，`-Wthread-safety -Werror` 下无告警。
- 故意漏拿锁的探针如上，确实编译失败，确认注解起作用。
- QEMU 实测，无 panic：
  - dev 连跑两次 `usercopy.test`，两次都是 18/18；
  - dbg_1 跑到 69.8 s，`usercopy.test` 18/18。
- "`read_user` 读进同一个 VMO 的映射"这一项，现在同时是这条锁序的死锁回归测试：
  `copy_page_by_page()` 持 VMO 锁，拷贝时对同一个 VMO 缺页。如果捕获机制失效，
  缺页路径会在 `page_fault()` 里再拿同一把 Mutex，当场死锁。
- 没有专门写"两个地址空间并发缺页同一个 VMO"的测试，这个场景未实测。

**故意没做**
- 没给 `VmCowPages::make_cursor()` 本身加注解。VmCowPages 没有自己的锁，它的锁在
  VmObjectPaged 里；它的调用方 `commit_range_locked()` 和 `copy_page_by_page()`
  都已经持着 `m_mutex`。
- `map_paged()` 忽略 `commit` 参数的 FIXME 没动，那是另一个问题。
- 创建映射这条路径没动：`create_mapping()` 挑空位、挂进父 VMA 的 `m_subvmas`、
  `add_mapping()` 挂进 `VmObject::m_mappings`，全程不持任何锁。这和本条同属一类问题，
  但范围更大，已记入 `docs/repairs/260927/mm.md` 第 1 条。

## 对象锁上移到 VmObject，并由它保护 VMO 的映射链表（2026-09-27）

修 `docs/repairs/260927/mm.md` 第 1 条的前一半。后一半（创建映射时持地址空间锁）见下一节。

**文件**
- `kernel/mm/include/ours/mm/vm_object.hpp`：新增 `m_lock` 和 `lock()`；`m_mappings` 加 `TA_GUARDED(m_lock)`；
  `add_mapping()` 和 `remove_mapping()` 加 `TA_REQUIRES(m_lock)`
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`、`vm_object_paged.cpp`：删掉自己的 `m_mutex` 和 `lock()`，
  改用基类的 `m_lock`
- `kernel/mm/vm_mapping.cpp`：`activate()` 和 `destroy()` 在 `Guard{m_vmo->lock()}` 下增删映射
- `kernel/mm/include/ours/mm/vm_aspace.hpp`：注释里的引用跟着改

**症状**
潜伏，当前触发不到：还没有两个地址空间同时映射同一个 VMO。一旦出现（首先是 K16 的共享 vDSO），
两边的 `activate()` 或 `destroy()` 会并发修改同一个侵入式链表 `VmObject::m_mappings`。
轻则丢一个节点，重则链表成环，或者 `~VmMapping` 析构时发现 hook 仍在链上而 trap。

**原因**
`m_mappings` 属于 VMO，而改它的人属于各自的地址空间。每个改动方最多持着自己那个地址空间的锁，
彼此不相干。所以这个链表需要一把属于 VMO 的锁。

VMO 本来有一把锁，但它在 `VmObjectPaged` 里（`m_mutex`），只管页树；
`VmObjectPhysical` 根本没有锁，而物理 VMO 同样会被映射。

**改法**
把锁从 `VmObjectPaged` 上移到 `VmObject`，叫 `m_lock`，同时保护页树和映射链表：

之前：

```cpp
class VmObject { ... VmMapping::List m_mappings; };
class VmObjectPaged: public VmObject { ... sync::Mutex m_mutex; };

// VmMapping::activate()
m_vmo->add_mapping(*this);
```

之后：

```cpp
class VmObject {
    ...
    sync::Mutex m_lock;
    VmMapping::List m_mappings TA_GUARDED(m_lock);
};

// VmMapping::activate()
Guard guard{m_vmo->lock()};
m_vmo->add_mapping(*this);
```

只用一把锁而不是两把，理由和 Zircon 相同：以后改页的操作（比如 decommit）需要在"页和映射都不会变"
的状态下，遍历这个 VMO 的每一个映射去撤掉页表项。如果是两把锁，就得同时拿两把，还要再定一个锁序。

锁序不变：地址空间在外，VMO 在内。`activate()` 和 `destroy()` 都在地址空间锁下被调用
（`destroy()` 已经如此；`activate()` 要等下一节补上）。

**参照**
Zircon：
- `VmObject::AddMappingLocked()`，声明为 `TA_REQ(lock())`（`include/vm/vm_object.h`）；
- `VmMapping::Activate()` 取 `Guard<CriticalMutex> guard{object_->lock()}` 再进 `ActivateLocked()`
  （`vm_mapping.cc`）。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过，`-Wthread-safety -Werror` 下无告警。
  这说明除了 `activate()` 和 `destroy()`，没有别的地方在不持锁时碰 `m_mappings`。
- QEMU 实测，无 panic：dev 的 `usercopy.test` 18/18；dbg_1 跑到 64.9 s，`usercopy.test` 18/18。

**故意没做**
`VmObjectPhysical` 的 `lookup_range()` 没加锁。物理 VMO 的范围在创建后就不会变，没有需要保护的可变状态。

## object_cache 归还 slab 页框时不再提前标 Role::Pmm（2026-09-27）

**文件**
- `kernel/mm/object_cache.cpp`：`slab::destroy()` 和 `slab::create()` 的失败回滚里，删掉
  `free_frames()` 之前的 `become<page::PmmBody>()`

**症状**
偶发。dbg_1 跑 `usercopy.test`，6 次里有 1 次：18 项全部通过，但在收尾释放页框时内核停在
`#UD`，没有打出汇总行：

```
[20009617000 cpu1 trace] Free frame(B5F000, 0)
[20009988000 cpu1 error] #UD Invalid opcode: vector=6, error_code=0x0
[20010978000 cpu1 error]   rip=0xffffff884003e379 (+0x3e379) ...
[20013553000 cpu1 error]   r8=0x1  r9=0xb5e  r10=...  r11=0x0
```

`+0x3e379` 是 `FrameBuddy::release_inner()` 末尾的 `ud2`。跳到这里的是
`circular_list_algorithms` 的断言（`detail::require`）：合并伙伴时，要从空闲链表摘下
伙伴 `B5E`，但它的节点 `next` 为空（`r11=0`），根本不在任何链表上。就在此前，
cpu0 刚打出 `Free frame(B5E000, 0)`。`B5E` 和 `B5F` 互为伙伴。

**原因**
伙伴系统判断"伙伴是否空闲"只看 `FrameBuddy::is_mergeable()`：`role == Pmm`、order 相同、
同 zone 同节点。也就是说，`Role::Pmm` 的含义就是"我在伙伴系统的空闲链表上"。这个标记只该由
`insert_frame()` 在 zone 锁内、入链的同时打上；分配时 `remove_frame()` 在锁内把它改成 `None`。

`object_cache` 却在锁外先打了这个标记：

```cpp
slab->become<page::PmmBody>();   // 锁外：role = Pmm，但还没入链
mm::free_frames(slab, order);    // 这里面才去拿 zone 锁、入链
```

在这两行之间，这个页框自称空闲却不在链表上。此时另一个 CPU 释放它的伙伴，会在 zone 锁内
看到 `role == Pmm`，把它当成空闲伙伴去合并，`erase` 一个从未入链的节点，于是 trap。
dbg_1 下日志量大，QEMU 的 vCPU 容易在两行之间被挂起，窗口因此被拉长。

其它所有调用 `free_frames()` 的地方（`VmCowPages` 的页、`CpuLocal`、`early_mem`）都是带着
使用时的角色直接归还，只有这里提前改了角色。

`memmap.hpp` 里 `frame_to_phys()` 旁边的注释记录过同一类错：当年 `as_ptr<PmFrame>()`
顺手把角色设成 `Pmm`，已分配的页框因此被当成空闲块合并，被分配了两次。

**改法**

之前：

```cpp
stamp_compound_offsets(slab, order, false);
slab->become<page::PmmBody>();
mm::free_frames(slab, order);
```

之后：

```cpp
stamp_compound_offsets(slab, order, false);
mm::free_frames(slab, order);   // 带着 Slab 角色归还，由 insert_frame() 在锁内改成 Pmm
```

`slab::create()` 失败回滚的那一处同样处理。

**参照**
Linux 用 `PageBuddy` 标记"在伙伴空闲链表上"，只在 `__free_one_page()` 里、持
`zone->lock` 时由 `set_buddy_order()` 设置；`page_is_buddy()` 检查的正是这个标记。
slab 归还页面走 `__free_pages()`，自己从不设 `PageBuddy`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：dbg_1 连跑 6 次（3 个一组并行，加重主机负载），dev 跑 2 次，全部 18/18，
  没有任何 `#UD`、`#PF`、`#GP`。修复前是 6 次出现 1 次，所以"6 次没出现"只能算有利证据，
  不能算证明。结论主要靠上面的推理：寄存器、伙伴关系、释放顺序都和这个竞态对得上。
- 同一工作区里还有未提交的 `vm_area.cpp` 加锁改动（见下一节），上面的启动都包含它。
  这个修复本身只删了两行，不依赖那个改动。

**故意没做**
- 没有给伙伴系统加一个独立于 `role` 的"在空闲链表上"标记。`Role::Pmm` 本来就承担这个含义，
  问题在于有人在锁外写它，而不是缺一个标记。
- `page::PmmBody` 仍然保留，`pm_frame.hpp` 里还引用它（hook 说明和 static_assert）。
