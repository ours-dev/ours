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
