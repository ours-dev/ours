# 系统调用改动记录（2026-09-26）

## fault_probe 迁到 runtime：探针名走 argv[1]，回答走另给的 channel（2026-09-26）

**文件**
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — 手写的 `_start` 换成 `main(argc, argv)`：名字取 `argv[1]`，回答写到启动句柄 User0，
  没被杀就从 `main` 返回，退出码区分"没死""不认识""没给名字"
- `kernel/lib/syscall/test/fault_probe/BUILD.gn` — 依赖 `//system/lib/runtime`
- `examples/shell/shell.cpp` — 新增 `run_probe()`：为每个探针写 processargs、另建回答 channel；`run_probes()` 用它，最后多跑一个对照探针 `none`
- `system/lib/runtime/BUILD.gn`、`test/runtime_smoke.cpp` — 注释改为"一对一对地迁移，fault_probe 先"
- `docs/design/260924/userspace-filesystem.md` — K14 拆为四个提交、U3 的搬移提前，写进这两行；总数那一段注明是计划数

**症状**
无，行为不变（探针照旧 11 个全被杀）。这是设计步骤 K14 拆出来的第一个提交：把"启动方与被启动方"这一对（dbg_1 的 shell → fault_probe）
先切到 processargs 与 runtime。

**原因**
这一节的学习价值在"为什么 fault_probe 可以先走、又为什么回答不能再走 bootstrap"。

- **为什么先走这一对**：K14 要把启动协议从"rdi 里一个句柄，双方私下约定是什么"换成 processargs。协议的两端必须同时换：写的一方开始写
  processargs 的那一刻，读的一方就得会读。fault_probe 只有一个启动方——dbg_1 的 shell——而 shell 作为写的一方只需要只有头文件的
  `encode_process_args()`，和 shell 自己是怎么被启动的无关。所以这一对可以独立切换。内核→init、内核→shell、init→shell 三条连成一体，
  留到后面一起切。
- **回答为什么不能再走 bootstrap**：runtime 读完 processargs 就关掉 bootstrap（U2，照 Zircon）。shell 如果还在 bootstrap 上等回答，
  探针还没进 `main`，它就先看到 PEER_CLOSED、把每个探针都记成"被杀"。所以 shell 另建一个 channel，把远端作为 User0 启动句柄放进
  processargs 交过去，在这一端上等。
- **"被杀"已经证明不了探针跑过**：runtime 启动失败时进程直接退出，回答 channel 同样是"对端关闭且没有数据"。所以验证不能只看
  `11 killed`，要逐个数到 `fault_probe: <名字>`，并核对每个探针的故障特征与以前一致。
- **对照探针**：11 个探针全都在回答之前就被杀，"`main` 返回 → runtime 调 `process_exit`"这条路一次都不会走到。shell 最后用一个
  不存在的名字 `none` 再起一次 fault_probe：它应当回答 "unknown"、从 `main` 返回 2，内核日志里出现 `retcode 2`。它不计入 11 个的统计。

**改法**
改前（fault_probe）：

```cpp
extern "C" [[noreturn]]
auto _start(ours::HandleValue const bootstrap, ours::VirtAddr const vdso_base) -> void {
    ... object_wait_one(bootstrap, ...);
    ... channel_read(bootstrap, buffer, sizeof(buffer), &actual) ...   // the probe's name
    ...
            probe.action(vdso_base);
            answer_and_stay(bootstrap, "survived");                     // write, then spin
```

改后：

```cpp
auto main(int const argc, char **const argv) -> int {
    auto const answer = runtime::take_startup_handle(ours::HandleType::User0, 0);
    ...
    auto const name = ou::Str::from_c_str(argv[1], 32);
    ...
            probe.action(runtime::vdso_base());
            reply(answer, "survived");
            return SURVIVED;
```

改后（shell 为每个探针做的事，`run_probe()`）：

```cpp
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0)};
    std::string_view const args[] = {"fault_probe", {probe.data(), probe.size()}};
    auto status = ours::encode_process_args(message, infos, args, {}, {}, &length);
    ...
        status = ou::channel_write(bootstrap, message, length, &answer_there, 1);
    static_cast<void>(ou::handle_close(bootstrap));
    ...
    if (status = ou::process_spawn_boot(job, "fault_probe", bootstrap_there); ...
```

- **先写后启动**：消息在进程启动之前就已经在队列里；runtime 反正会先等，但这样不依赖它等。
- **bootstrap 这一端写完就关**：它是 shell 建的句柄，按每创建者配额（K11）记在 shell 名下；留着既没用，还会让后面的配额检查少算。
- **回答之后再等对端关闭**：第一次启动时 dbg_1 的配额检查从 512 变成了 511。原因是对照探针正常返回、慢慢退出，而它手里那端回答
  channel 是 shell 建的、记在 shell 名下，shell 读到回答就往下走，到配额检查时子进程还没清理完（日志里晚了一秒多）。改为读到回答后
  再等 PEER_CLOSED，确认子进程放下了它那端再返回——这个数字是查句柄泄漏用的，不该随子进程死得快慢浮动。
  但这只是**缩小**了窗口：审查指出内核在 `Handle::~Handle()` 里先发 PEER_CLOSED（`on_zero_handles()`）、后退还配额（`refund()`），
  所以 shell 醒来时那个句柄可能还记在它名下几条指令的时间；被杀的探针也一样。今天配额检查之前还有三组检查，这个窗口碰不到；
  内核里把两步的顺序调过来是另一个改动，记为 `docs/repairs/260926/object.md` 第 1 条。
- **先编码、后建 channel**：审查指出第一版在编码失败时会漏关回答 channel 的远端（还没写出去，就没人消耗它）。编码不依赖任何句柄，
  挪到最前面，编码失败时手里还没有句柄。
- **退出码**：`SURVIVED = 1`、`UNKNOWN = 2`、`NO_NAME = 3`，调试内核打出的 retcode 就能说明一个没被杀的探针是怎么结束的。

**参照**
Zircon 的做法相同：子进程的参数与启动句柄都在 processargs 里（`zx_proc_args`），父进程与子进程之间另有专用 channel；bootstrap 读完即关。

**验证**
两个镜像构建、启动。dbg_1：
- 13 行 `fault_probe: ...`：11 个探针的名字，外加对照探针的 `fault_probe: none` 与 `fault_probe: unknown probe none`；
- 11 行 `shell: probe <p> killed`、11 行 `retcode -1028`，写 vDSO 的探针故障特征仍是 `error_code=0x7 ... cr2=0xa00000`（`runtime::vdso_base()` 给的地址对）；
- `shell: probes done (11 killed, 0 survived)`；`shell: control probe none answered "unknown"`，内核日志里这个进程是 `terminated, retcode 2`；
- 12 行 `dead, 1 handles closed`（每个探针死时只剩 User0 那一个句柄：bootstrap 已被 runtime 关掉）；
- 0 行 `runtime:`，0 个 `retcode -2147483649`（没有启动失败）；
- `shell: 512 channels made before the quota`（与以前相同）。
dev 与以前一致：`init: shell closed its channel`，`512 channels`。

**故意没做**
- init、shell 自己仍是手写的 `_start`：它们的启动方（内核、init）要一起切换，在 K14 的第 ③ 个提交。
- `same()` 仍是手写比较：可以换成 memcmp，与本步无关。

## `handle_duplicate`：同一个对象的第二个句柄，权限只能减不能增（2026-09-26）

**文件**
- `kernel/object/handle_table.cpp`、`include/ours/object/handle_table.hpp` — 新增 `HandleTable::duplicate()`：在表锁下查找、检查、复制
- `kernel/object/handle.cpp`、`include/ours/object/handle.hpp` — `Handle::clone()` 的参数名 `kernel_handle` 改为 `source`（它是源句柄，不是内核句柄），并写明它为什么必须在表锁下调用；这个函数以前没有调用者
- `kernel/lib/syscall/src/handle.cpp` — 新增 `sys_handle_duplicate()`
- `system/public/ours/syscalls.gni` — 末尾追加 `handle_duplicate`（其余调用的编号不变）
- `system/lib/ou/include/ou/handle.hpp`、`src/handle.cpp` — `ou::handle_duplicate()`
- `examples/shell/shell.cpp` — 新增 `check_handle_duplicate()`

**症状**
无，新功能。设计步骤 K15：init 要把 BootFS 转交出去同时自己留一份（U7），把 VMO 换成可执行的 `vmo_replace_as_executable` 会消耗原句柄、得先复制（K19），
要把权限缩小后再交出去（U12 的 Slice）——这些都要"再开一个句柄"。

**原因**
这一节的学习价值在"为什么复制必须在句柄表的锁里一次做完"和"出参为什么先写后装"。

- **在锁里一次做完**：复制出来的句柄要从源句柄那里拿到对象的引用。如果先查到源句柄、放掉锁、再去复制，中间另一个线程可能把源句柄关掉——
  若那是对象的最后一个句柄，对象已经收到"没人持有了"的通知（channel 的对端会置 PEER_CLOSED），复制品随后又让它"复活"，对象状态自相矛盾。
  所以查找、检查权限、`Handle::clone()` 都在表锁下，Zircon 的 `handle_dup_replace()` 同样持表锁跨越这一整段。这会在表锁里再取 arena 的锁
  （表 → arena），arena 的锁里不再取别的锁；dbg_1 的启动日志里没有 lockdep 报告。
- **先写出参、后装进表**（设计 4.1）：新句柄的值是它的 arena 位置与本表随机数的纯函数，装进表之前就算得出。先把值写到程序给的地址，
  成功了才 `add()`；写失败就让这个句柄在所有锁之外销毁，程序什么也看不到——`channel_create` 发布两个端点也是这个顺序。
  **Zircon 反过来**：处理函数先把句柄加进表，由生成的包装在之后写出参；写失败时句柄已经在表里了，包装只好给线程抛一个
  HANDLE_LEAK 策略异常（`kernel-wrappers.inc.tmpl`）。这里先写，就没有可泄漏的东西。

**改法**
改前：没有这个系统调用；`Handle::clone()` 存在但没有调用者，参数名写成 `kernel_handle`。

改后，检查按 Zircon 的顺序（`handle_ops.cc` 的 `handle_dup_replace()`）：

```cpp
auto HandleTable::duplicate(HandleValue const value, Rights rights, ustl::Box<Handle> *const out)
    -> Status {
    Guard guard{m_lock};
    auto *const source = lookup_locked(value);
    if (source == nullptr) {
        return Status::BadHandle;
    }
    if (!source->has_rights(Rights::Duplicate)) {
        return Status::AccessDenied;
    }
    if (rights == Rights::SameRights) {
        rights = source->rights();
    } else if (!source->has_rights(rights)) {
        return Status::InvArgs;
    }
    *out = Handle::clone(source, rights);
    if (!*out) {
        return Status::NoResource;
    }
    return Status::Ok;
}
```

```cpp
auto sys_handle_duplicate(HandleValue const handle, Rights const rights,
                          user_copy::UserOutPtr<HandleValue> const out) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Box<object::Handle> duplicate;
    if (auto const status = table.duplicate(handle, rights, &duplicate); status != Status::Ok) {
        return status;
    }
    if (auto const status = out.copy_to_user(table.value_of(*duplicate)); status != Status::Ok) {
        return status;
    }
    static_cast<void>(table.add(ustl::move(duplicate)));
    return Status::Ok;
}
```

- **`SameRights` 只按整个字匹配**：`SameRights | Read` 不是"一样的权限再加 Read"，而是在要第 31 位这个"权限"，任何句柄都没有它，
  于是按"超出"回 InvArgs。若用位与去判断，就会把它当成 SameRights 而悄悄放行。要求与源句柄完全相同的权限、或者一个权限都不要（None），都允许。
- **先查 Duplicate、再查权限子集**：两者都不满足时回 AccessDenied，与 Zircon 相同。channel 默认就不带 Duplicate，所以复制 channel 一定是 AccessDenied。
- **新句柄记在调用者名下**：`Handle::clone()` 向当前进程的配额记账（K11），超出配额或 arena 满都回 NoResource（Zircon 没有每进程配额，回 NO_MEMORY）。

**参照**
Zircon `zircon/kernel/lib/syscalls/handle_ops.cc` 的 `handle_dup_replace()` 与 `sys_handle_duplicate()`；`zircon/system/utest/core/handle-info/handle-info.cc`
的 `DuplicateRights` 用例（子集复制得到恰好请求的权限；没有 DUPLICATE 回 ACCESS_DENIED；越权回 INVALID_ARGS 且源句柄不变）。

**验证**
两个镜像构建、启动，`shell: handle_duplicate checks passed`：
- 两边：`handle_duplicate(HANDLE_INVALID) = BadHandle`、`handle_duplicate(a channel) = AccessDenied`；
- dbg_1（持有 BootFS）：`SameRights` 复制成功，原件与副本读出的头部逐字节相同；要 `Write`（BootFS 交给用户态时就不带）回 InvArgs；
  `SameRights | Read` 回 InvArgs；只要 `Basic | Map` 的副本复制成功，但对它 `vmo_read` 回 AccessDenied；出参指向内核半区回 InvArgs；
- **没有漏下句柄，而且是测出来的**：出参指向内核半区的调用连续失败比每进程配额多一次（1025 次），随后一次正常复制仍然成功
  （`handle_duplicate(out = kernel half), past the quota = InvArgs`、`handle_duplicate(bootfs) after that = Ok`）。只检查"回 InvArgs"是不够的：
  先装进表、后写出参的实现（Zircon 的顺序）也回 InvArgs，只是句柄留在了表里。变异检查：把内核临时改成先 `add()` 再写出参，dbg_1 打出
  `past the quota = InternalError`、`after that = NoResource`、`handle_duplicate checks FAILED`——漏下的句柄把配额占满了；还原后通过。
- dbg_1 的 shell 退出时仍是 `dead, 1 handles closed`（只剩没交出去的 BootFS）；`512 channels made before the quota` 不变；dbg_1 日志里没有 lockdep 报告。
- 审查：两个方向（正确性与并发、语义与注释）并行审查、逐条反驳核实。成立三条，都已在本提交处理：shell 声称检查了"不留下句柄"却只断言了 InvArgs
  （改成上面那样真正检查）；`Handle::clone()` 的注释没说到根本——源句柄被关掉后会被销毁、槽位回到 arena 并可能被别的对象的新句柄重用，
  这时再读它就是释放后使用，"对象收到没人持有的通知"只在它恰好是最后一个句柄时才发生，原来举的 channel 例子也走不到（channel 不带 Duplicate）；
  ou 的注释说"复制是交出句柄的唯一方式"不对，交出句柄靠的是 `channel_write` 的转移，复制是"自己留着、再造一个"的唯一方式。
  另有一条"表锁里再取 arena 锁，两者同为 sync::Mutex，lockdep 打开后会报重复获取"被驳回：这是 lockdep 按锁类型分类的已知限制
  （`lockdep/lock_class.hpp` 的注释写明），树里早有同类嵌套（`attach_observer`：表锁里取对象锁）。
