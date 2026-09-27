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
