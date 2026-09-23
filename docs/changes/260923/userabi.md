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
