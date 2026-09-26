# 系统调用改动记录（2026-09-25）

## `process_exit`：进程自己结束，shell 跑完就退出，init 看得见（2026-09-25）

**文件**
- `system/public/ours/syscalls.gni` — 新增 `process_exit(i64 retcode)`，`noreturn`，`ret = "void"`
- `kernel/lib/syscall/src/process.cpp` — `sys_process_exit()`：只是调对象层
- `kernel/object/process_dispatcher.cpp`、`include/ours/object/process_dispatcher.hpp` — 新增 `ProcessDispatcher::exit()`
- `kernel/object/thread_dispatcher.cpp`、`include/ours/object/thread_dispatcher.hpp` — 新增 `ThreadDispatcher::exit_current()`
- `system/lib/ou/include/ou/process.hpp`、`src/process.cpp` — `ou::process_exit()`
- `examples/shell/shell.cpp` — 末尾的 `pause` 死循环换成 `ou::process_exit(0)`
- `system/service/init/init.cpp` — 读到 "pong" 之后再等 PEER_CLOSED，打 `init: shell closed its channel`；
  `halt()` 的注释接过原先在 shell 里的"`pause` 为什么不能省"

**症状**
用户程序没有办法结束自己。shell、init 做完事只能原地自旋（从 `_start` 返回会返回到内核留在栈上的任何东西），
进程永远不死，它持有的句柄永远不关；启动它的一方也就永远不知道它做完了。

**原因**
缺的是这个调用。上一步（进程在最后一个线程离开时 Dead 并关句柄）已经把"死"做对了，这里补上"自己去死"的入口。

**改法**
改前：shell 的结尾

```cpp
    for (;;) {
        __asm__ volatile("pause");
    }
```

改后：

```cpp
    ou::process_exit(0);
```

```cpp
auto sys_process_exit(i64 const retcode) -> void {
    object::ProcessDispatcher::current()->exit(retcode);
}
```

```cpp
auto ProcessDispatcher::exit(i64 const retcode) -> void {
    ...
    {
        Guard guard{get_lock()};
        if (m_state != State::Dying) {
            m_retcode = retcode;
            m_state = State::Dying;
            for (auto &thread: m_thread_list) {
                if (&thread != current) {
                    thread.kill();
                }
            }
        }
    }

    ThreadDispatcher::exit_current();
}
```

```cpp
auto ThreadDispatcher::exit_current() -> void {
    DEBUG_ASSERT(current() != nullptr, ...);
    task::Thread::Current::exit(0);
}
```

- **全在对象层**：系统调用层只做入口，不碰 task（设计 4.7 的规矩：lib/syscall 只 include 对象层的头）。
- **retcode 只在进入 Dying 时记一次**：已经在死的进程（先被 kill，或另一个线程先 exit）保留它原来的码；两个线程
  同时 exit 时，答案不能取决于谁第二个拿到锁。
- **不杀当前线程**：它自己马上就退出，杀它只会给它挂一个再也回不到 ring 3 去处理的信号。
- **线程的退出码给 0**：进程的码记在 ProcessDispatcher 上，内核线程的 retcode 对谁都没有意义，Zircon 同样传 0。
- **Dead 由最后离开的线程完成**：`exit_current()` → `Thread::Current::exit()` → `on_exit()` →
  `detach_thread()`，上一步的逻辑，一行不用改。
- **生成器**已支持 `noreturn`：vDSO 桩在 `syscall` 后接 `ud2`，内核 shim 不写 `frame->ax`，声明带 `[[noreturn]]`。

**参照**
- Zircon `sys_process_exit()` → `ProcessDispatcher::ExitCurrent(retcode)`（`zircon/kernel/object/include/object/process_dispatcher.h:155`）
  → `ProcessDispatcher::Exit()`（`zircon/kernel/object/process_dispatcher.cc:303`）：持锁时仅在未处于 DYING 时写 retcode_，
  `SetStateLocked(DYING)` 杀掉全部线程，然后 `ThreadDispatcher::ExitCurrent()`。
- ours 与之的差别：不杀当前线程（理由见上），没有 critical-to-root-job 的处理（没有这个概念）。

**验证**
- 构建通过（vDSO 的 ABI 核对随 `libours.ifs` 自动更新并通过）；clang-format 干净；clang-tidy 在改动行上无新告警。
- dev：`init: shell says "pong"` 之后出现 `init: shell closed its channel`，随后 `object: process 'shell' (koid 35) destroyed`。
- dbg_1：`probes done (10 killed, 0 survived)` 之后 `object: process 'shell' (koid 23) dead, 0 handles closed`
  （root job 的句柄它早先在 `check_handle_close()` 里关了），随后 destroyed。

**故意没做**
- `thread_exit`、`task_kill`：计划里不需要它们的步骤（设计 §7.6 的"基本范围"）。
- init 仍然 `halt()` 而不退出：它是其余程序的起点，今天没有任何东西会注意到它不在了。
- fault_probe 失败时仍然自旋回话，改成退出在 K14（探针名改走 argv 时一起改）。

## `fault_probe` 加 `pf_write_vdso`：程序自己写只读页，被杀（2026-09-25）

**文件**
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — 新探针 `pf_write_vdso`；删掉"缺这一项"的注释；`Action` 的注释改写
- `examples/shell/shell.cpp` — 探针表加一项，共 11 个
- `docs/design/260924/userspace-filesystem.md` — 外部前置 X1 标为已修；到 U6 的探针数由 11 改 12

**症状**
fault_probe 缺一类故障：页**存在**但权限不够（写只读）。已有的 `pf_unmapped`、`pf_kernel` 都是"页不在"或"内核地址"，
走不到地址空间"拒绝一个保护违例"的那条路。以前加不了：每个叶子页表项都可写，写 vDSO 会成功，探针会报 survived。

**原因**
X1（`docs/changes/260925/x86-kernel.md`，叶子只带请求的权限）修掉之后，vDSO 第一次真的只读了。

**改法**
改前：`PROBES[]` 上方一段注释写明"缺：写 vDSO；等页表项的修复"。

改后：

```cpp
auto pf_write_vdso(ours::VirtAddr const vdso_base) -> void {
    __asm__ volatile("movb $0, (%0)" : : "r"(vdso_base) : "memory");
}
```

`vdso_base` 就是程序启动时 `%rsi` 里的那个值，`Action` 一直为此带着这个参数。

**参照**
Fuchsia 的 vDSO 测试守的是同一条边界的另一面：`src/zircon/tests/vdso/vdso.cc:124-127` 断言对 vDSO 代码页做
`zx_vmar_protect` 得到 `ZX_ERR_ACCESS_DENIED`——vDSO 的保护改不了，这里验证它的保护确实生效。没有找到"直接写 vDSO 被杀"
的现成对应物；本探针是 ours 自己的。

**验证**
dbg_1：

```
fault_probe: pf_write_vdso
terminating process 'fault_probe' (koid 55) thread koid 59: type=FatalPageFault vector=page fault error_code=0x7 rip=0x201980 sp=0x11fff78 cr2=0xa00000 ...
shell: probe pf_write_vdso killed
shell: probes done (11 killed, 0 survived)
```

error_code 0x7 = 页存在、写、用户态；cr2 是 vDSO 的基址。dev 不受影响。

**故意没做**
写程序**自己**只读段的探针（设计里的 `pf_write_ro`）：程序映像今天仍整体 RWX 映射，要等 launcher 按段映射（U6）。

## 句柄经 channel 转移：写者的表里摘下，读者的表里装上，写的句柄无论成败都被消耗（2026-09-25）

**文件**
- `kernel/lib/syscall/src/channel.cpp` — `sys_channel_write()` 先拷句柄值、摘下全部句柄，再按次序回答；`sys_channel_read()` 先拷字节、再拷句柄值、最后装入
- `kernel/object/handle_table.cpp`、`include/ours/object/handle_table.hpp` — 新增 `remove_for_transfer()`；摘除的公共部分抽成 `unlink_locked()`，`remove()` 也用它；改正"Zircon 在同一处取消等待"的注释
- `build/abi/gen_syscalls.py` — `release` 允许用在 `dir = "in"` 的 HandleValue 数组上
- `system/public/ours/syscalls.gni` — `channel_write` 的 `handles` 标 `release = true`；文档改写
- `examples/shell/shell.cpp` — 新增 `check_handle_transfer()`

**症状**
`channel_write` 的 `num_handles` 非零一律回 NotSupported：程序之间没有办法递交句柄。

**原因**
缺的是系统调用层这一半。上一节已让 `MessagePacket` 能持有句柄。

**改法**
改前：

```cpp
    if (num_handles != 0) {
        return Status::NotSupported;
    }
```

改后（写端摘要）：

```cpp
    HandleValue values[CHANNEL_MAX_MSG_HANDLES];
    ... handles.copy_array_from_user(values, num_handles) ...

    ustl::Arc<ChannelDispatcher> dispatcher;
    auto const resolved = table.get_dispatcher(channel, Rights::Write, &dispatcher);

    ustl::Box<Handle> taken[CHANNEL_MAX_MSG_HANDLES];
    auto const removed =
        table.remove_for_transfer(values, num_handles, dispatcher.as_ptr(), taken);

    if (resolved != Status::Ok) { return resolved; }
    if (options != ChannelWriteOptions::None) { return Status::InvArgs; }
    if (removed != Status::Ok) { return removed; }
    ...
    MessagePacket::create(bytes, num_bytes, taken, num_handles, &message)
```

```cpp
auto HandleTable::remove_for_transfer(HandleValue const *const values, u32 const count,
                                      Dispatcher const *const channel, ustl::Box<Handle> *const out)
    -> Status {
    ...
    Guard guard{m_lock};
    for (u32 i = 0; i < count; ++i) {
        auto *const handle = lookup_locked(values[i]);
        if (handle == nullptr) {
            latch(Status::BadHandle);
            continue;
        }
        out[i] = unlink_locked(handle);
        if (!out[i]->has_rights(Rights::Transfer)) {
            latch(Status::AccessDenied);
        } else if (out[i]->dispatcher().as_ptr() == channel) {
            latch(Status::NotSupported);
        }
    }
    return status;
}
```

- **消耗**：句柄值拷进来之后，被点名的每个句柄都在一次表锁内摘下，进 `taken`；写失败时它们随 `taken` 在所有锁之外关闭，
  写成功时进消息。只有两种出口什么也不消耗：句柄数超限、句柄值数组拷不进来——那时根本不知道要消耗哪些。
- **先摘后查**，只记第一个错误：摘不到回 BadHandle（同一个值第二次出现时，第一次已经把它摘走了）；缺 Transfer 回 AccessDenied；
  句柄正是被写的 channel 回 NotSupported——一个排在自己对端队列里的 channel 端，会通过自己的队列让两端互相保活，外面再也关不掉。
- **回答的次序**照设计 4.5：channel 句柄本身的错误最先，然后是选项，然后是句柄数组，最后是字节数。channel 句柄要在摘除之前解析：
  它可能就在数组里，这里持有的引用让它在被拒绝期间活着。
- **读端**先拷字节，再把每个句柄的值（值只取决于句柄与本表，装入前就算得出）一次拷给用户，两次都成功才逐个装入本表；
  拷失败时句柄随 `taken` 关闭，没有任何东西被看见过——与 `channel_create()` 发布出参的次序相同。
- **取消等待**的注释改正：本树在句柄离开表时就取消经它的等待；Zircon 的 `RemoveHandleLocked()` 并不取消，关闭的句柄在销毁时取消，
  转移的句柄在接收端 `msg_get_handles()` 装入之前取消。效果相同，本树更早，而且只在一处。

**参照**
- Zircon `msg_put_handles()`（`zircon/kernel/lib/syscalls/channel.cc:314`）：拷入句柄值，在一次写锁内逐个
  `get_handle_for_message_locked()`，只锁存第一个错误，失败的槽留空；`get_handle_for_message_locked()` / `common_handle_checks_locked()`
  （`zircon/kernel/object/user_handles.cc`）先 `RemoveHandleLocked()` 再查 Transfer 与"是不是 channel 自己"。
- Zircon `channel_write()` 用 `fit::defer` 的 `RemoveUserHandles()` 保证早退时也消耗句柄；本树先摘再回答，效果相同。
- Zircon `msg_get_handles()`（同文件 :146 起）：先把值拷给用户，成功后取消等待、装入句柄表。

**验证**
- 构建通过；clang-format 干净（shell 里剩下的差异是原有的两处）；clang-tidy 在改动行上无新告警。
- 生成器的新规则（临时脚本）：按值的 HandleValue 与输入数组接受 `release`，输出数组与非 HandleValue 拒绝。
- 两个镜像的 shell 都打出：

```
shell: write(a, handles = [d]) = Ok
shell: close(d) after sending it = BadHandle
shell: read(b) = Ok
shell: write(c, ping) = Ok
shell: read(the received end) = Ok
shell: write(a, handles = [a]) = NotSupported
shell: close(a) after that = BadHandle
shell: write(c, handles = [e, bad]) = BadHandle
shell: close(e) after that = BadHandle
shell: a 1000-deep chain of ends, closed
shell: handle transfer checks passed
```

  一千层的那条链——每个 channel 端排在下一个 channel 的队列里，只保留最外层一个再关掉——由 K8 的 `rc_recycle()` 摊成循环释放；
  内核照常跑完：dev `init: shell closed its channel`，dbg_1 `probes done (11 killed, 0 survived)`，到 `--- end ---`。

**故意没做**
- 读端的句柄类型与 rights 校验（`channel_read_etc`）：设计里推迟到 GetBackingMemory 之前。
- 在途句柄的按进程记账（`HANDLE_MAX_PER_PROCESS`）：下一步。

## `vmo_read`：按页经一页内核中转缓冲拷给用户，全部检查先于第一次读（2026-09-25）

**文件**
- `system/public/ours/syscalls.gni` — 末尾追加 `vmo_read`（追加在最后，已有桩的编号和字节都不变）
- `kernel/lib/syscall/src/vmo.cpp` — `sys_vmo_read()`：解析句柄（要 Read），然后交给对象；改掉"VMO 类型还不存在"的过时注释
- `kernel/object/vmo_dispatcher.cpp`、`include/ours/object/vmo_dispatcher.hpp` — 新增 `VmoDispatcher::read_user()`，范围检查、零长度、用户半区检查、逐页中转都在这里；`read()` 的注释改为指向它
- `kernel/lib/syscall/BUILD.gn` — 改掉说 vmo.cpp 是空壳的过时注释
- `system/lib/ou/include/ou/vmo.hpp`、`src/vmo.cpp`、`BUILD.gn` — `ou::vmo_read()`
- `examples/shell/shell.cpp` — `check_vmo_read()`
- `docs/design/260924/userspace-filesystem.md` — K14 的验证列补上这一步测不到的路径

**症状**
无，新功能。文件系统计划 K13：init 要用 `vmo_read` 读 BootFS 的头部、条目表和名字表（I1），BootServer 按请求拷文件内容。

**原因**
这一节的学习价值在"为什么要中转、为什么中转写在对象层"。

- **VMO 锁下不能拷给用户。**拷到用户内存可能缺页；缺页路径将来要拿 VMO 的锁（`docs/repairs/260925/mm.md` 第 1 条的修法，
  照 Zircon），到那时如果本线程还拿着同一把锁、用户缓冲区又恰好映射自同一个 VMO，就自己等自己。今天缺页路径只拿地址空间的自旋锁，
  所以不会死锁，但这是"碰巧"——修 mm 那一条时不应该还要回头翻系统调用。
- **Zircon 不中转**：`VmObjectPaged::ReadUser()` 在 VMO 锁下直接从物理页拷给用户，用的是 `copy_array_to_user_capture_faults`
  ——出错时不跑缺页处理，而是把出错地址报回来；它随即放锁、手动把那一页缺进来、再从断点继续。本内核的用户拷贝会直接走缺页处理，
  没有"只报告不处理"的变体，所以锁必须在拷贝前就放掉，字节得先落在一块内核内存里。代价是每页多一次 memcpy。
- **中转缓冲为什么是堆上的一页**：内核栈只有 4 页，而拷贝途中的 #PF 不走 IST，会叠在同一个栈上；今天系统调用里最大的栈上缓冲
  不到 1 KiB。`new (GAF_KERNEL)` 一页正好落在 kmalloc 最大的 4096 档。
- **为什么写在对象层**：第一版把中转循环写在 `sys_vmo_read()` 里，用到了 `new (mm::GAF_KERNEL)` 和 mm 的 `PAGE_SIZE`——
  系统调用层直接碰了 kernel/object 以下的东西，违反了 `kernel/lib/syscall/BUILD.gn` 里"只有 spawn_boot 这一个例外"的规则
  和接口设计 4.7 的依赖白名单。编译得过，只是因为 `//kernel/object:headers` 公开地把 mm 和 heap 的头文件一路传了上来，
  这条规则今天没有机械检查。审查指出后挪进了 `VmoDispatcher::read_user()`，与 Zircon `VmObjectDispatcher::Read(user_out_ptr)`
  和本仓库 `MessagePacket::copy_data_to()`（对象层自己把消息拷给用户）同形。

**改法**
改前：`kernel/lib/syscall/src/vmo.cpp` 只有两个占位声明，没有任何 VMO 系统调用。

```cpp
namespace ours::syscall {
// Declarations only -- the VMO object type does not exist yet. ...
auto vmo_create(object::Handle *handle) -> object::Handle *;
auto vmo_create_child(object::Handle *handle) -> object::Handle *;
}
```

改后，系统调用只剩两步，和 Zircon `sys_vmo_read()`（`zircon/kernel/lib/syscalls/vmo.rs:81-83`）一样：

```cpp
auto sys_vmo_read(HandleValue const vmo, user_copy::UserOutPtr<u8> const buffer, u64 const offset,
                  usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Read, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->read_user(buffer, offset, length);
}
```

对象层做其余的事：

```cpp
    auto const size = m_vmo->size();
    if (length > size || offset > size - length) {
        return Status::OutOfRange;
    }
    if (length == 0) {
        return m_vmo->read(nullptr, VirtAddr(offset), 0);
    }
    if (!user_copy::is_user_range(out.address(), length)) {
        return Status::InvArgs;
    }
    auto bounce = ustl::Box<Bounce>::from_raw(new (mm::GAF_KERNEL) Bounce);
    ...
    while (done < length) {
        auto const at = offset + done;
        auto const step = std::min<usize>(length - done, PAGE_SIZE - at % PAGE_SIZE);
        // m_vmo->read() takes and drops the VMO's lock; the copy out holds none.
        ... m_vmo->read(bounce->bytes, VirtAddr(at), step) ...
        ... out.element_offset(done).copy_array_to_user(bounce->bytes, step) ...
    }
```

- **检查的顺序就是用户看到的错误优先级**：句柄（BadHandle > BadType > AccessDenied，零长度也一样）→ 范围（OutOfRange）
  → 零长度直接回 Ok → 用户缓冲区（InvArgs）→ 拷贝。
- **范围一次查完**：`VmObjectPaged::read()` 自己只查它拿到的那一段；按页循环时，一个越过末尾的请求会先把前几页交出去，
  再在最后一段回 OutOfRange。先对整个范围查一次，才是"要么全给、要么一个字节都不给"。先查"长度不超过大小"再做减法，
  所以 `offset + length` 回绕到 0 附近的请求也被挡住。
- **零长度也问一次 VMO**：`m_vmo->read(nullptr, offset, 0)`。paged VMO 对零字节读在做任何事之前就回 Ok，不看指针；
  physical VMO 回 NotSupported。这样零长度也和别的长度一样，告诉调用者这个 VMO 能不能读。
- **缓冲区整段先查**：本内核没有共享零页，读一个没人写过的页会真的提交一页清零的内存。一个注定失败的调用不该让 VMO 平白长出内存，
  所以在读第一页之前就把 `[buffer, buffer + length)` 整段对照用户半区查一遍。
- **每一步不越过 VMO 的页边界**：第一步可能从页中间开始，之后每一步都从页边界开始。一次持锁只覆盖一页，无论用户要多长。
- **部分失败不报字数**：拷到一半缺页失败，就回 InvArgs，已经拷过去的字节留在用户缓冲区里。Zircon 内部算了字数
  （`VmObjectDispatcher::Read()` 返回 status 和字数），但 `sys_vmo_read()` 只取了 `.first`，把字数丢了；这里干脆不算。
- `blocking` 属性不加：它在本仓库的意思是"可能等待事件"，Zircon 给 `vmo_read` 标 `@blocking` 是因为 pager 支持的 VMO 可能要等页，
  这里没有 pager；等 VMO 互斥锁那一下不算。

**参照**
- Zircon `sys_vmo_read()`（`zircon/kernel/lib/syscalls/vmo.rs:66-83`）、`VmObjectDispatcher::Read()`
  （`zircon/kernel/object/vm_object_dispatcher.cc:168-172`）、`VmObjectPaged::ReadUser()` 与 `ReadWriteInternal()`
  （`zircon/kernel/vm/vm_object_paged.cc`）。
- 与 Zircon 的两处差别，都写在 `read_user()` 的注释里：
  - 用户缓冲区不能写时，这里一律回 InvArgs（本内核 user_copy 的统一规则，接口设计 4.6）；Zircon 在缓冲区不在用户半区或未映射时
    回 NotFound，只读时回 AccessDenied。
  - physical VMO：Zircon 的 `VmObject::ReadUser()` 在看范围和缓冲区之前就回 NotSupported；这里要等范围检查、用户半区检查和
    中转缓冲分配都过了，`mm::VmObject::read()` 才回 NotSupported，所以可能先回 OutOfRange、InvArgs 或 NoMemory。潜伏：
    `create_physical()` 没有调用者，用户态拿不到 physical VMO。

**验证**
- 生成器：`sys_vmo_read` 编号 9，4 参数桩，`ours_vmo_read` 进了 libours.ifs；vDSO 的 ABI 检查照常通过。
- 两个镜像构建、启动。dev 和 dbg_1 的 shell 都打出下面 6 行和 `shell: vmo_read checks passed`；dev `init: shell closed its channel`，
  dbg_1 `shell: probes done (11 killed, 0 survived)`：

  ```
  shell: vmo_read(HANDLE_INVALID) = BadHandle
  shell: vmo_read(a channel) = BadType
  shell: vmo_read(a channel, 0 bytes) = BadType
  shell: vmo_read(a channel, into the kernel half) = BadType
  shell: vmo_read(a channel, far offset) = BadType
  shell: vmo_read(closed) = BadHandle
  ```

  后三行把"句柄错"和"参数错"配在一起（零长度、内核半区的缓冲区、够不着的偏移），证明句柄先于一切参数检查。
- **未实测**：成功的读、跨页、OutOfRange、零长度读、坏缓冲区的 InvArgs——用户态现在还拿不到任何 VMO 句柄（BootFS VMO 在 K14
  随 processargs 交出去）。这些已写进设计 K14 的验证列，由 dbg_1 的 shell 在 BootFS VMO 上补测。
- 审查：四个方向（语义、内存与锁、注释准确性、构建与测试）并行审查，每条发现另派一人反驳。成立三条，都已在本提交处理：
  中转循环越层（挪进对象层）、physical VMO 的注释与实际顺序不符（改注释）、测试覆盖缺口（写进 K14）。

**故意没做**
- `ou::vmo_write` 与 `vmo_write`：设计把它放在 K17，与 `vmo_create` 一起，那时才有可写的 VMO。
- `VmoDispatcher::size()` 的公开接口：第一版为了在系统调用里先查范围加过，范围检查挪进对象层之后没有调用者，删掉；`vmo_get_size` 落地时再加。
- 共享零页：读没写过的页会提交一页清零内存（`mm::VmObjectPaged::read()` 的注释已写明）。今天所有 VMO 都是 Pinned、创建时整体提交的，
  碰不到；Lazy VMO 在 K17 出现后才有意义，属于 mm。
- `vmo_dispatcher.hpp` 里 `create_paged` 注释说"size 由 VMO 自己按页向上取整"，不对：`VmCowPages` 原样保存大小。这是旧注释，
  和本提交无关，不在这里顺手改。
  已记为 `docs/repairs/260925/object.md` 第 1 条。
