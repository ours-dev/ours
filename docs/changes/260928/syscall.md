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

## 新增 vmo_replace_as_executable（K19，2026-09-28）

**文件**
- `kernel/object/include/ours/object/resource_dispatcher.hpp`、`resource_dispatcher.cpp`：`object::validate_ranged_resource()`
- `kernel/lib/syscall/src/vmo.cpp`：`sys_vmo_replace_as_executable()`
- `system/public/ours/syscalls.gni`：`vmo_replace_as_executable` 条目
- `system/lib/ou/include/ou/vmo.hpp`、`system/lib/ou/src/vmo.cpp`：`ou::vmo_replace_as_executable()`
- `examples/shell/shell.cpp`：`check_vmo_replace_as_executable()`
- `docs/design/260923/userspace-interface.md`：§2 W^X 一句、§4.2 的 Vmo 行、§9 的 Execute 行
- `docs/design/260924/userspace-filesystem.md`：K19 标为已落地

**症状**
VMO 的默认权限不带 Execute（D3），而用户态没有任何办法拿到一个带 Execute 的 VMO 句柄。
K18 已经把 vmex 交到第一个进程手里，但它还换不出东西；launcher 按 X 映射 BootFS 里的代码（K21、§4.6）要靠这一步。

**原因**
缺功能。

**改法**

```cpp
// object：Zircon validate_ranged_resource() 的三种回答
auto validate_ranged_resource(HandleValue handle, RsrcKind kind, u64 base, u64 size) -> Status {
    get_dispatcher<ResourceDispatcher>(handle, Rights::None, &r);   // BadHandle / BadType
    if (r->kind() != kind) { return Status::BadType; }
    if (base < rbase || size > rsize || base - rbase > rsize - size) { return Status::OutOfRange; }
}

// syscall：步骤照 Zircon
auto sys_vmo_replace_as_executable(HandleValue vmo, HandleValue vmex, UserOutPtr<HandleValue> out) -> Status {
    vmex_status = vmex == HANDLE_INVALID ? AccessDenied : validate_ranged_resource(vmex, System, VMEX, 1);
    original = table.remove(vmo);                     // 从这里起原句柄已经没了
    if (!original || 不是 VMO) { return BadHandle; }
    if (vmex_status != Ok) { return vmex_status; }
    Handle::make(original->dispatcher(), (rights & ~Write) | Execute);
    out.copy_to_user(value); table.add(handle);        // 先写值，再入表（设计 4.1）
}
```

几处取舍：
- **先算 vmex 的结果，再摘原句柄，最后才报 vmex 的错**。这个顺序直接照搬 Zircon：vmex 不对，原句柄也照样消耗。
  调用方因此只需要记一条规则：“传进去就没了”，不必分辨是哪一种失败。想留一份，就先 `handle_duplicate`（§4.6 的“先复制，再换”）。
- **原句柄不是 VMO 时回 BadHandle，不是 BadType**。Zircon 在这里也回 `ZX_ERR_BAD_HANDLE`：
  此时句柄已经从表里摘掉，从调用方看，它传进来的值已经不再指向任何东西。这和设计 4.5 的优先级
  （BadHandle > BadType）并不冲突，因为消耗先于类型检查，是这个调用自身语义的一部分。
- **vmex 句柄不要求任何 right**。持有它本身就是能力；Zircon 同样只查种类和范围。
  设计评审当时否决了“用去掉 right 的 vmex 副本做反向检查”，原因就在这里。
- **新句柄去掉 Write（D3）**。Zircon 保留 Write，是为了让 JIT 能改正在执行的代码；
  ours 没有 JIT，于是让“能执行”和“能改写”不能落在同一个句柄上。这条偏离已记入接口设计 §9。
- **vmex = HANDLE_INVALID 回 AccessDenied**。Zircon 这时去查 job 的 `ZX_POL_AMBIENT_MARK_VMO_EXEC` 策略；
  ours 没有 job 策略，相当于这条策略永远是 deny。
- **范围检查写成不会溢出的形式**：不计算 `base + size`，而是先比 base，再比“从资源起点量过去的剩余长度”。
  调用方今天只传 `(VMEX, 1)`，但 `validate_ranged_resource` 以后还会被 `vmo_create_physical` 用来检查任意的 MMIO 范围。

**参照**
Zircon `sys_vmo_replace_as_executable()`（`zircon/kernel/lib/syscalls/vmo.rs`）：
先 `validate_ranged_resource(vmex, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_VMEX_BASE, 1)`，
再 `remove_handle()`，然后判断类型，最后 `make_and_add_handle(..., rights | ZX_RIGHT_EXECUTE)`。
`validate_ranged_resource()` 在 `zircon/kernel/object/resource.rs`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过；`boot_check.py` 两个镜像都 PASS
  （dbg 11 passed；rel 10 passed、3 skipped），两个镜像都打出 `vmo_replace_as_executable checks passed`：
  - vmex 为 HANDLE_INVALID 回 AccessDenied，vmex 为 channel 回 BadType，vmex 为已关闭的句柄值回 BadHandle；
    这三次之后，用原 VMO 句柄 `vmo_read` 都回 BadHandle，说明原句柄已被消耗；
  - vmo 位置传 channel 回 BadHandle，之后再 close 这个 channel 也回 BadHandle，说明它同样被消耗了。
- 只在 dbg_1 跑的部分（shell 由 userboot 启动，手里有 vmex 和 BootFS）：
  - vmex 位置传 BootFS VMO 回 BadType；
  - 用真正的 vmex 换出新句柄：新句柄能读出换之前写入的字节，`vmo_write` 回 AccessDenied（Write 已去掉），
    原句柄回 BadHandle。

**故意没做**
- 没测“换出的 VMO 能按 X 映射”：这要等 `vmar_map` 支持 vmo_offset 和权限上限（K21），验证放在那里。
- 没测 OutOfRange：全系统只能铸出 `[VMEX, VMEX+1)` 这一个 System 资源，构造不出不覆盖该槽位的 System 资源。
- 没测新句柄超出配额时回 NoResource：这条路径和 `handle_duplicate` 一样，由 `Handle::make()` 统一负责，已有覆盖。

## 系统调用处理函数改名：`ours::sys_<name>` → `ours::syscall::<name>`（2026-09-28）

**文件**
- `build/abi/gen_syscalls.py`：生成的声明改为 `auto <name>(...)`；shim 改名为 `<name>_shim`，调用 `syscall::<name>`
- `kernel/lib/syscall/include/syscall/handlers.hpp`：声明所在的命名空间从 `ours` 改为 `ours::syscall`
- `kernel/lib/syscall/src/{channel,command,debug,handle,object,process,task,vmo}.cpp`：定义挪进 `ours::syscall`，去掉 `sys_` 前缀；
  `task.cpp` 里 4 个 thread 占位函数一并改
- `kernel/lib/syscall/src/table.cpp`：内部的 `sys_not_from_vdso`、`sys_bad_syscall_number` 同样去掉前缀
- 注释与文档里指 ours 自己处理函数的地方：`loader.cpp`、`vma_dispatcher.hpp`、`kernel/top/BUILD.gn`、
  `kernel/lib/syscall/BUILD.gn`、`system/public/ours/syscall.hpp`、`syscalls.gni`、`system/lib/ou/README.md`、
  `docs/design/260923/userspace-interface.md`
- `.agents/skills/ours-kernel-design/references/layering.md`：记下命名规则

**症状**
处理函数写成 `ours::sys_vmo_create`：前缀和命名空间说的是同一件事，却一个都省不掉。
而且同一个名字在不同地方写法不同：用户态是 `ours_vmo_create`，ou 里是 `ou::vmo_create`，
内核里偏偏多一个 `sys_`。

**原因**
照搬了 Zircon 的写法。Zircon 的 `sys_*` 放在全局命名空间里，和内核其余所有函数挤在一起，只能靠前缀区分。
ours 的处理函数本来就有自己的库（`kernel/lib/syscall`），给它一个命名空间，前缀就多余了。

**改法**

```cpp
// 之前
namespace ours { auto sys_vmo_create(u64 size, VmoOptions options, UserOutPtr<HandleValue> out) -> Status; }
auto sys_vmo_create_shim(arch::SyscallFrame *frame) -> void { ... sys_vmo_create(...); }

// 之后
namespace ours::syscall { auto vmo_create(u64 size, VmoOptions options, UserOutPtr<HandleValue> out) -> Status; }
auto vmo_create_shim(arch::SyscallFrame *frame) -> void { ... syscall::vmo_create(...); }
```

- shim 仍留在 `table.cpp` 的 `ours::{匿名}` 里：它是分发层，不是处理函数，所以只写 `<name>_shim`，
  调用时写全 `syscall::<name>`，一眼能看出跨过了哪一层。
- 注释里提到 Zircon 函数的地方（`Zircon sys_vmo_create()` 这类）保留原名，那是 Zircon 的名字，照原样写才能对照查找。
- `docs/changes/` 下的旧记录只增不改，里面的 `sys_*` 是当时的名字，不动。

**参照**
Zircon 的 `sys_*`（`zircon/kernel/lib/syscalls/*.rs`）和它们的生成 wrapper，即这里去掉前缀之前照搬的对象。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。生成的声明和手写定义只要有一处名字对不上，shim 就会链接失败，
  所以构建通过本身就说明每个处理函数都改对了。
- `boot_check.py` 两个镜像都 PASS（dbg 12 passed；rel 11 passed、4 skipped）。

**故意没做**
- `.agents/tasks/` 下的旧计划文档没改，它们记录的是当时的状态。

## 新增 vmar_map；VmarRoot 暂由内核补发（K21b，2026-09-28）

**文件**
- `kernel/lib/syscall/src/vmar.cpp`（新）：`syscall::vmar_map()`；`kernel/lib/syscall/BUILD.gn` 加上它，删掉过时的“vmo.cpp 有占位”说明
- `system/public/ours/syscalls.gni`：`vmar_map` 条目，第一个 7 参数的调用（G2 的桩）
- `system/lib/ou/include/ou/vmar.hpp`、`system/lib/ou/src/vmar.cpp`（新）、`system/lib/ou/BUILD.gn`：`ou::vmar_map()`
- `kernel/object/include/ours/object/vmar_dispatcher.hpp`：`VmarDispatcher::unmap()`
- `kernel/object/include/ours/object/channel_dispatcher.hpp`、`channel_dispatcher.cpp`：临时的 `deliver_first()`
- `kernel/lib/userabi/src/userboot.cpp`：`deliver_root_vmar()`，`launch()` 调用它
- `system/lib/runtime/src/start.cpp`：启动时读两条消息
- `examples/shell/shell.cpp`：取 VmarRoot，`check_vmar_map()`
- `docs/design/260923/userspace-interface.md` §3.2：launcher 按 D3 修订；`docs/design/260924/userspace-filesystem.md`：K21 标为已落地，K23 行记下要删的临时通道

**症状**
用户态没法把 VMO 映射进自己的地址空间。launcher（K23）、GetBackingMemory 的使用者（U12）都要靠 `vmar_map`。
另外，就算有了系统调用，进程也拿不到 VMAR 句柄：在 K23 之前，进程由内核的 `launch()` 创建，
而写 processargs 的是父进程（init、userboot），它写消息时子进程和子进程的根 VMAR 都还不存在。

**原因**
缺功能。VmarRoot 的问题出在时序上：Zircon 由用户态 launcher 先 `process_create` 拿到根 VMAR，再写消息；
我们的 launcher 要到 K23 才有。

**改法**

```cpp
// 系统调用：照 Zircon sys_vmar_map() 的顺序
auto vmar_map(vmar, options, vmar_offset, vmo, vmo_offset, length, mapped_addr) -> Status {
    两个句柄连同 rights 取出;                          // BadHandle / BadType
    if (!(vmo_rights & Map)) { return AccessDenied; }
    未知位或 CanMap* → InvArgs;  W/X 不带 R → InvArgs;  W 与 X 同时 → InvArgs（W^X，D3）;
    每个 Perm 位要两边句柄都有对应 right            // AccessDenied
    两边都有的 right → CanMap*（映射的上限）
    addr = region->map(options | CanMap*, ...);        // 偏移、大小、位置
    if (写回地址失败) { region->unmap(addr, length); return status; }
}

// 临时通道（K23 删）：launch() 往子进程的 bootstrap channel 队首塞一条只带 VmarRoot 的 processargs
channel->deliver_first(packet);
// runtime：先读内核那条，再读父进程那条，句柄合进一张表
```

几处取舍：
- **错误顺序照 Zircon**：Map 权限（AccessDenied）→ 选项合法性（InvArgs）→ 各 Perm 位对两边 rights（AccessDenied）。
  设计 4.5 的“AccessDenied 先于参数错误”在这里不完全成立：Zircon 先查保护组合是否合法，再查 rights，照它。
- **W^X 回 InvArgs**，“写或执行必须带读”也回 InvArgs：它们说的是“这不是本系统接受的保护组合”，
  和 Zircon `is_valid_mapping_protection()` 不通过时的回答一致。Zircon 本身允许 W+X，也允许只执行，这两点是我们的偏离。
- **地址写回失败就拆掉映射**。句柄可以先算值、写给调用方、再入表（设计 4.1），映射地址却只有建好之后才知道。
  所以反过来：写回失败，就把刚建的映射拆掉，程序不会留下一块“被告知失败、其实已映射”的内存。
  shell 用固定位置测了这一点：第一次写回失败，第二次在同一位置映射成功。如果映射还在，第二次会冲突。
- **VmarRoot 走一条单独的消息，而不是寄存器**。这样 VmarRoot 仍通过 processargs 交付，和 K23 之后一致，
  K23 只需把两条消息并回一条，runtime 改回读一次。内核把自己的消息插在队首（`deliver_first()`），
  不管父进程是在 launch 之前还是之后写的消息，子进程读到的第一条总是内核的，runtime 靠这个顺序区分两条。
  句柄在父进程的系统调用里创建，按 K11 记在父进程名下，和设计里 init 的配额算法一致。
- **用户程序不能传 CanMap***：那是上限，程序自己选的上限等于没有上限。Zircon 同样拒绝。

**参照**
- Zircon `sys_vmar_map()`（`zircon/kernel/lib/syscalls/vmar.rs`）：`GetDispatcherAndRights` 两次，查 `ZX_RIGHT_MAP`，
  `is_valid_mapping_protection()`，拒绝用户给的 `CAN_MAP_*`，按两边 rights 补上 `CAN_MAP_*`，再调 `VmAddressRegionDispatcher::Map()`。
- Zircon 的 launcher（`process_builder`）：先 `zx_process_create` 得到根 VMAR，再把它作为 `PA_VMAR_ROOT` 写进 processargs。
  这是 K23 要回到的形状。

**验证**
- 两个镜像均构建通过，vDSO ABI 检查通过；反汇编 `ours_vmar_map` 是 G2 的 7 参数桩
  （`push %r12; mov %rcx,%r10; mov 0x10(%rsp),%r12`）。
- `boot_check.py` 两个镜像都 PASS（dbg 13 passed；rel 12 passed、5 skipped），两个镜像的 shell 都打出 `vmar_map checks passed`：
  - 按 vmo_offset = 第 2 页只读映射，读出第 2 页的字节；读写映射后写入一字节，`vmo_read` 读出同一个值；
  - Slice 能按 R 映射并读出父对象的字节；映射长度超过 Slice 回 OutOfRange；
  - 只 W、只 X、RWX、带 CanMapWrite 都回 InvArgs；
  - vmo_create 的 VMO 按 RX 映射回 AccessDenied；不带 Map 的句柄回 AccessDenied；
  - 长度 0、vmo_offset 不对齐、不带 Specific 却给 vmar_offset 都回 InvArgs；越过 VMO 回 OutOfRange；Compact 回 NotSupported；
  - 地址写回内核半区回 InvArgs，随后在同一固定位置映射成功；vmar 位置传 VMO 回 BadType。
- 只在 dbg_1 跑的部分：BootFS 按 RW 映射回 AccessDenied；复制 BootFS 并经 vmex 换出后，按 RX 映射成功，读出 BootFS 头部的 magic。
- 所有进程启动时都要经过“读两条消息”，两个镜像里的 userboot、init、shell 和各探针都正常启动，说明新的启动路径可用。

**故意没做**
- 没有 `vmar_unmap`、`vmar_protect`：shell 映射的内存在进程退出前不会拆掉；映射上限的检查要等 `vmar_protect`。
- 没测执行映射进来的代码：换出的 BootFS 里没有能直接跳进去运行的函数，K23 的 launcher 起进程时会真正执行它。
- VmarRoot 的临时通道已记在 K23 行，由 K23 删除。
