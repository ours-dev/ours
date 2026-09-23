## 第一个进程拿到根 job 的句柄；rdi 放句柄值，rsi 放 vDSO 基址（2026-09-23）

**文件**
- `kernel/lib/userabi/src/userboot.cpp` — 线程启动前，把根 job 做成句柄装进新进程的句柄表，值作为 `arg1`（%rdi），vDSO 基址作为 `arg2`（%rsi）

**症状**
第一个用户程序进入 ring 3 时两手空空：`arg1`/`arg2` 恒为 0，它的句柄表是空的。于是它什么对象都碰不到，
也就启动不了任何别的程序。

**原因**
句柄表在上一步才有；之前根本无处放句柄，注释里也写着「等有了句柄表再把 rights 交给 `Handle::make()`」。

**改法**
改前：

```cpp
    status = thread.dispatcher()->start({
        .entry = loaded.image.entry,
        .stack = loaded.stack_top,
        .arg1 = 0,
        .arg2 = 0,
    });
```

改后：

```cpp
    auto bootstrap = object::Handle::make(
        ustl::Arc<object::Dispatcher>(object::JobDispatcher::root_job()),
        object::JobDispatcher::default_right());
    ...
    auto const bootstrap_value = process.dispatcher()->handle_table().add(ustl::move(bootstrap));
    log::info("userabi: root job -> {} as handle {:#x}", program.view(), bootstrap_value);

    status = thread.dispatcher()->start({
        .entry = loaded.image.entry,
        .stack = loaded.stack_top,
        .arg1 = bootstrap_value,
        .arg2 = vdso.base,
    });
```

只给根 job：job 是创建进程的地方，握有根 job 的程序就是能把系统其余部分启动起来的那个。
还没有引导消息，所以值直接放进入口寄存器；等 processargs 在 P1 落地，rdi 改为 bootstrap channel。
vDSO 基址不是发起系统调用所必需的（导入在运行前就已绑定），但它是程序无法从别处得知的、关于
自己地址空间的唯一事实，运行时回溯栈、P2 的自绑定都要用它。

**参照**
Zircon userboot 在它的 bootstrap 消息里收到根 job（`kernel/lib/userabi/userboot.cc`，
`HandleType::JobDefault`/`kProcSelf` 等）；入口的 `arg2` 是 vDSO 基址同 Zircon `zx_process_start`
的约定。

**验证**
两个镜像的启动日志都出现

```
userabi: root job -> shell as handle 0x100003
```

`0x100003` = `((1 << 18) << 2) | 0b11`：arena 下标 0、第 1 代、两个固定位——编码与设计一致。
shell 今天忽略入口参数，照常打完命令表。句柄能否被解析、关闭，下一步由 shell 自己验证。

**故意没做**
rsp 对齐（入口处 ≡ 8 mod 16）和 64 KiB 的栈归在第 8 步抽出 `userabi::launch` 时一起做。

## 抽出 `userabi::launch`；内核包常驻；64 KiB 的栈与正确的入口 rsp（2026-09-23）

**文件**
- `kernel/lib/userabi/src/userboot.cpp`、`include/userabi/userboot.hpp` — `launch(job, name, bootstrap)`；`resident_package`；`USER_STACK_SIZE = KB(64)`；`initial_stack_pointer()`

**症状**
- 装载一个程序的整套步骤（建进程、映射映像与 vDSO、绑定导入、登记 vDSO 范围、建栈、起线程）写死在
  `userboot_init()` 一个函数里，内核包视图是它的局部变量——之后任何人都没法再从包里起第二个程序。
- 栈只有一页。对一个整个 text 就是空循环的程序够用；对带 2 KiB 缓冲、层层调用 ou 的 shell 已经很紧。
- 入口处 `%rsp` 是 16 字节对齐的栈顶。SysV 约定函数入口 `%rsp + 8` 才是 16 字节对齐的（调用者压了返回
  地址），`_start` 是按这个约定编译的普通 C++ 函数，但没人 call 它——内核直接 iretq 过去。于是它一辈子都
  错位 8 字节；没出事只是因为用户代码 `-mno-sse`，从不溢出对齐的 16 字节值。

**原因**
这些都是「只有一个程序」时代的写法。

**改法**
- `launch(job, name, bootstrap)`：装载序列原样搬进来，`userboot_init()` 只剩「解压包并常驻 → 选程序 →
  给根 job 做句柄 → launch」。bootstrap 是一个 `ustl::Box<Handle>`，可以为空（rdi 为 0）——将来
  `process_spawn_boot` 从调用者的表里摘下的句柄正是这个形状，权限原样保留。
- 内核包解压一次、常驻：再次解压要花几 MB。启动时写一次，此后只读。
- 栈 64 KiB，下方从不映射，那段地址空间就是 guard。
- 入口 rsp：

  改前：

  ```cpp
        .stack = loaded.stack_top,
  ```

  改后：

  ```cpp
    constexpr auto initial_stack_pointer(VirtAddr const top) -> VirtAddr {
        return (top & ~VirtAddr{15}) - 8;
    }
    static_assert(initial_stack_pointer(USER_STACK_TOP) % 16 == 8);
    ...
        .stack = initial_stack_pointer(loaded.stack_top),
  ```

**参照**
Zircon `compute_initial_stack_pointer()`（`kernel/lib/userabi/userboot.cc` 附近的 `elf-load`/`process_builder`
约定）；Linux `start_thread()` 由 ELF 加载器按 ABI 布置初始栈。

**验证**
两个镜像的启动序列与改动前一致，措辞变为：

```
userabi: handing shell the root job
userboot: found 'shell', 3544 bytes of ELF
userboot: mapped [200000, 205000) into its own aspace, entry at 2017C0, stack 1200000
userabi: 'shell' starts with handle 0x100003
shell: close(bootstrap) = Ok
shell: close again = BadHandle
```

入口对齐由 `static_assert` 在编译期保证。

**故意没做**
- `process_spawn_boot`（8b）与 init（9）：它们在系统调用里、跑在 AP 上做内核分配，被
  `docs/repairs/260923/mm.md` #1 挡住，等那条修好再做并验证。
- `userabi::HandleIndex` 空枚举原样保留。
