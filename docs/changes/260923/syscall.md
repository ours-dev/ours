## 第一个带句柄的系统调用：`ours_handle_close`（2026-09-23）

**文件**
- `system/public/ours/syscall.hpp` — `OURS_SYS_HANDLE_CLOSE 2`
- `system/public/ours/vdso.hpp` — `ours_handle_close(HandleValue) -> Status`
- `kernel/lib/userabi/vdso/vdso.S` — 桩与 `OURS_VDSO_SYSRET_HANDLE_CLOSE`
- `kernel/lib/syscall/src/handle.cpp`（新）、`handlers.hpp`、`table.cpp` — `sys_handle_close` 与分发 shim
- `system/lib/ou/include/ou/handle.hpp`、`src/handle.cpp`（新）— `ou::handle_close`
- `examples/shell/shell.cpp` — `_start(bootstrap, vdso_base)`；关两次入口句柄并打印结果

**症状**
无——这是新功能。它是第一个走完整条「句柄值 → 句柄表 → 对象」路径的系统调用。

**原因**
句柄表、句柄 arena、第一个进程拿到的根 job 句柄都已就位，但用户态还没有任何办法碰到句柄。
`close` 是最小的一个：不需要 rights，不需要新对象，却完整地检验了编码、五道关、摘表和销毁。

**改法**
每一层各加一处，严格按「系统调用只经 vDSO」的路径：

```
shell        ou::handle_close(bootstrap)
ou           ours_handle_close(handle)            <ours/vdso.hpp>
vDSO         mov $OURS_SYS_HANDLE_CLOSE, %eax; syscall
内核         sys_handle_close_shim → sys_handle_close(HandleValue)
对象层       ProcessDispatcher::current()->handle_table().remove(value)
```

两处值得说明：

- **`HANDLE_INVALID` 回 Ok**，照 Zircon `zx_handle_close()`：清理路径可以把每个槽都关一遍，
  不用先问哪些填过。
- **shim 显式截断**：`static_cast<HandleValue>(frame->di)`。SysV 对 32 位参数所在寄存器的高 32 位
  不作保证，编译器在那里留下垃圾也是合法调用；不截断，一个合法句柄会被当成不存在。
- `remove()` 返回句柄而不在锁内销毁，句柄在 `sys_handle_close` 的语句末尾离开作用域——表锁那时
  已经释放。关掉某对象最后一个句柄会运行它的 `on_zero_handles()`，那可能要拿别的锁。

**参照**
Zircon `sys_handle_close()`（`kernel/lib/syscalls/handle_ops.cc`）；`zx_handle_close` 对
`ZX_HANDLE_INVALID` 的约定（`vdso/handle.fidl`）。

**验证**
两个镜像都绑定了第三个导入，shell 打出：

```
elfload: bound 'ours_handle_close' to A01340
shell: close(bootstrap) = Ok
shell: close again = BadHandle
```

第一次关闭证明内核装进表里的值能被解析并摘除；第二次证明同一个数字在关闭后不再指向任何东西——
这正是句柄表存在的意义，而且是从依赖它的那一侧观察到的。

**故意没做**
经这个句柄挂着的等待应以 Cancelled 结束（Zircon 的 `Dispatcher::Cancel(handle)`）——要等下一步有了
等待机制才有东西可取消。
