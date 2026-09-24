## init：第一个系统服务起 shell，二者经 channel 交换 ping/pong——P0 切片完成（2026-09-23）

**文件**
- `system/service/init/BUILD.gn`、`init.cpp`（新）
- `examples/shell/shell.cpp` — `answer_bootstrap()`；打印改用整行的 `ou::debug_line({...})`
- `kernel/products/image/dev/BUILD.gn` — 包里加 init；`kernel.select.userboot=init`

**症状**
无——这是方案第 5 节「第一条垂直切片」的最后一步，也是它的完成判据。

**原因**
此前每个镜像只跑一个程序，而且它什么都不能启动；一次会真正睡眠的系统调用从来没有发生过。

**改法**
- **init**（内核给它根 job）：建一对 channel → `process_spawn_boot(job, "shell", 另一端)` → 写 "ping" →
  `object_wait_one(READABLE | PEER_CLOSED, INSTANT_INFINITE)` 睡到 shell 回复 → 读出并打印。
- **shell**：先对 bootstrap 句柄做一次 `channel_read`。回 `BadType` 说明它是被内核直接起的、手里是根 job，
  于是照旧只跑各项检查（dbg_1 镜像保持这条路径做回归）；否则等 READABLE、读出 "ping"、打印、回 "pong"，
  再跑各项检查。
- **dev 镜像**首先启动 init；shell 仍在包里，由 init 起。

这次等待是系统里第一个真正睡眠、由另一个 CPU 上的另一个进程唤醒的系统调用，所以它同时验证了：用户 RSP 存在帧里
（`73bb1ca`）、Event 不丢唤醒（`0f9e32b`）、AP 上的内核分配（`5b1ce2f`）、新线程开中断（`eb028b2`）。

**参照**
方案 `docs/design/260923/userspace-interface.md` 3.1（P0 时序）与第 5 节第 9 步；Zircon userboot → bootsvc 的
「第一个程序持根 job、再起下一个」的形状。

**验证**
dev 镜像连续启动 5 次，每次都按序出现：

```
init: started with the root job
init: spawned shell
shell: bootstrap says "ping"
init: shell says "pong"
```

并且 shell 随后的 `shell: channel checks passed`。dbg_1 镜像启动 2 次：`shell: started by the kernel; no one to answer`、
`channel checks passed`、命令表 `--- end ---`。全部无断言。

第一版跑 5 次时有 2 次在检查上失败：两个进程的日志在同一行里交错。由 `9042bbf`（`ou::debug_line` 整行一次写出）修掉。

**故意没做**
- init 不回收、不监督 shell：没有 `process_exit`/TERMINATED（P1-B）。
- 只传字节：消息里带句柄是 P1-C，届时 bootstrap 改为 processargs 消息。
