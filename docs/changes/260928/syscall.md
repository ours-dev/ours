## 新增 vmo_write（K17 第一半，2026-09-28）

`docs/design/260924/userspace-filesystem.md` 的 K17 拆成两个提交，这是第一个；`vmo_create` 在下一个。

**文件**
- `system/public/ours/syscalls.gni`：`vmo_write` 条目；vDSO 桩和内核声明由它生成
- `kernel/lib/syscall/src/vmo.cpp`：`sys_vmo_write()`；顺带删掉 `sys_vmo_read()` 注释里已经过时的"中转页"一句
- `kernel/object/include/ours/object/vmo_dispatcher.hpp`、`vmo_dispatcher.cpp`：`VmoDispatcher::write_user()`
- `kernel/mm/include/ours/mm/vm_object.hpp`、`vm_object_paged.hpp`、`vm_object_paged.cpp`：
  `write_user()` 的缓冲区参数改为 `UserInPtr<u8 const>`
- `kernel/commands/user_copy_test.cpp`：跟着改类型
- `system/lib/ou/include/ou/vmo.hpp`、`system/lib/ou/src/vmo.cpp`：`ou::vmo_write()`
- `examples/shell/shell.cpp`：`check_vmo_write()`；另有两处是 clang-format 顺手重排的已有代码

**症状**
用户态可以读 VMO（`vmo_read`），但写不了。launcher 要靠 `vmo_create` 加 `vmo_write` 复制可写段
（设计 §4.6），所以两者都得先有。

**原因**
缺功能。底层的 `VmObjectPaged::write_user()` 已经在 `8b84de4c` 里写好了：在 VMO 锁下直接拷贝，
不经过中转页，用户侧缺页时先放锁再解析。缺的只是 object 层和 syscall 层把它接出来。

**改法**
照 `vmo_read` 逐层镜像一份：

```cpp
// syscall：先按 Write 权限解析句柄，再交给对象
auto sys_vmo_write(HandleValue vmo, user_copy::UserInPtr<u8 const> buffer, u64 offset, usize length) -> Status;

// object：检查顺序与 read_user() 相同
auto VmoDispatcher::write_user(user_copy::UserInPtr<u8 const> in, u64 offset, usize length) -> Status {
    if (length > size || offset > size - length) { return Status::OutOfRange; }
    if (length == 0) { return m_vmo->write_user(in, VirtAddr(offset), 0); }
    if (!user_copy::is_user_range(in.address(), length)) { return Status::InvArgs; }
    return m_vmo->write_user(in, VirtAddr(offset), length);
}
```

检查顺序和读完全一致，这样写和读同一个范围会得到同样的答案：先 OutOfRange，再零长度，最后是缓冲区。
缓冲区要在写任何页之前先整体检查，因为写入会为落到的页提交内存，注定失败的调用不该为 VMO 花掉内存。

mm 的 `write_user()` 缓冲区类型从 `UserInPtr<u8>` 改为 `UserInPtr<u8 const>`：输入缓冲区本来就只读；
生成的系统调用声明对 `void` 输入缓冲区给出的也是 `u8 const`（`channel_write` 同样如此），
改接口比在调用处转换更对。

**参照**
Zircon `sys_vmo_write()`（`zircon/kernel/lib/syscalls/vmo.rs`）：按 `ZX_RIGHT_WRITE` 取出 dispatcher，
再调用 `VmObjectDispatcher::Write()`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过，vDSO 的 ABI 检查通过。
- QEMU 实测，两个镜像的 `usercopy.test` 都是 18/18，shell 的其它检查没有 FAILED：
  - 两个镜像：无效句柄回 BadHandle；channel 句柄回 BadType，从内核半区写也是 BadType；关掉的句柄回 BadHandle。
  - dbg_1（shell 持有 BootFS）：BootFS 句柄不带 Write，普通写、零长度写、"从内核半区写到很远的偏移"
    都回 AccessDenied，也就是权限检查排在所有参数错误之前；写之前和写之后读出的开头 16 字节相同。
- 真正"写入再读回"要等下一个提交有了可写的 VMO 才能测。

**故意没做**
`vmo_create` 在下一个提交。

## 新增 vmo_create（K17 第二半，2026-09-28）

**文件**
- `system/public/ours/syscall.hpp`：新增 `VmoOptions`（`None`、`Resizable`）
- `system/public/ours/syscalls.gni`：`vmo_create` 条目
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`：`VmObjectPaged::MAX_SIZE`
- `kernel/object/include/ours/object/vmo_dispatcher.hpp`、`vmo_dispatcher.cpp`：`VmoDispatcher::create_user()`
- `kernel/lib/syscall/src/vmo.cpp`：`sys_vmo_create()`；删掉 `vmo_create` 的占位声明
- `system/lib/ou/include/ou/vmo.hpp`、`system/lib/ou/src/vmo.cpp`：`ou::vmo_create()`
- `examples/shell/shell.cpp`：`check_vmo_create()`
- `docs/design/260924/userspace-filesystem.md`：K17 行改为"已落地"，并写明与原计划的差别

**症状**
用户态没有办法建 VMO。launcher 复制可写段（设计 §4.6）、以及之后任何需要一块匿名内存的程序，都要靠它。

**原因**
缺功能。

**改法**

```cpp
auto sys_vmo_create(u64 size, VmoOptions options, user_copy::UserOutPtr<HandleValue> out) -> Status {
    KernelHandle<VmoDispatcher> vmo;
    VmoDispatcher::create_user(size, options, vmo, rights);        // 选项、大小、Lazy 都在 object 层
    auto handle = Handle::make(..., rights);                         // 超出句柄配额返回空 → NoResource
    out.copy_to_user(table.value_of(*handle));                       // 先把值写给调用方
    table.add(ustl::move(handle));                                   // 最后才放进句柄表
}

auto VmoDispatcher::create_user(u64 size, VmoOptions options, ...) -> Status {
    if (未知位) { return Status::InvArgs; }
    if (Resizable) { return Status::NotSupported; }
    if (size > mm::VmObjectPaged::MAX_SIZE) { return Status::OutOfRange; }
    mm::VmObjectPaged::create(mm::GAF_USER, align_up(size, PAGE_SIZE), mm::VmoFlags::Lazy, &vmo);
}
```

几处取舍：
- **新增 `create_user()`，而不是改 `create_paged()`**。设计文档原本写的是"把 `create_paged` 的 Pinned 改为 Lazy"。
  可 `create_paged` 的三个内核调用者（userboot 的栈、程序镜像、BootFS）确实需要在 ring 3 跑起来之前就驻留在内存里。
  所以另开一个给 syscall 用的入口，让两种需求各有各的函数。这样 syscall 层也用不着知道 mm 的 `VmoFlags`：
  syscall 只和 kernel/object 打交道，是这棵树的分层规矩。
- **检查顺序照 Zircon 的 `parse_create_syscall_flags()`**：先查选项，再查大小。未知位优先于 Resizable：
  "Resizable 加一个未知位"回的是 InvArgs，不是 NotSupported。
- **大小上限 `MAX_SIZE`**：取 64 位范围内最后一个整页，再减一页，保证向上取整、或再加一页都不会回绕。
  它放在 mm 的 `VmObjectPaged` 上，和 Zircon 把 `MAX_SIZE` 放在 `VmObject` 一致。
  Zircon 的值受页表容量限制，这里的页树以 64 位页偏移为键，容量更大，所以溢出是更紧的那个约束。
- **句柄的发布顺序和 `channel_create` 相同**（设计 4.1）：先把值写给调用方，最后才放进句柄表。
  写出失败时句柄随之销毁，程序从来看不到它。

**参照**
Zircon `sys_vmo_create()`（`zircon/kernel/lib/syscalls/vmo.rs`）：`parse_create_syscall_flags()` 检查选项和大小，
`VmObjectPaged::Create()` 建对象，`make_and_add_handle()` 发布句柄。
`ZX_VMO_RESIZABLE` 的位值（`1 << 1`）也照搬过来。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过，vDSO 的 ABI 检查通过。
- QEMU 实测，两个镜像的 shell 都打出 `vmo_create checks passed`，其它 shell 检查、`usercopy.test`
  （两个镜像都是 18/18）和 `pmnode.test`（20/20）都照旧通过：
  - 选项：Resizable 回 NotSupported；未知位回 InvArgs；两者同时出现也回 InvArgs。
  - 大小：`~0` 回 OutOfRange；0 字节合法，此时读 0 字节回 Ok、写 1 字节回 OutOfRange；
    "3 页加 1 字节"向上取整为 4 页：第 4 页最后 8 字节能读，再往后 1 字节回 OutOfRange。
  - Lazy：没写过的页读出来全是 0。
  - `vmo_write`：跨页写 16 字节后读回一致；写到末尾之后回 OutOfRange；从内核半区读数据回 InvArgs；
    复制一个不带 Write 的句柄去写，回 AccessDenied。
  - 句柄输出地址在内核半区时回 InvArgs。

**故意没做**
- 没有查询 VMO 大小的系统调用（Zircon 的 `vmo_get_size`），大小是否向上取整，只能通过读取边界间接验证。
- 没测句柄配额用尽时回 NoResource：这是 `Handle::make()` 统一负责的路径，`handle_duplicate` 和
  `channel_create` 的配额测试已经覆盖。
