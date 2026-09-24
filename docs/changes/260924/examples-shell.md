# examples-shell 改动记录（2026-09-24）

## `check_handle_close` 的注释跟上 init 的存在（2026-09-24）

**文件**
- `examples/shell/shell.cpp` — `check_handle_close()` 的文档注释

**症状**
注释说关掉的是"内核放进来的 root job"。自从 init 成为第一个用户程序，dev 镜像里 shell
由 init 启动，这个句柄是 init 那条 channel 的一端；注释描述的只剩 dbg_1 镜像
（shell 直接由内核启动）那一种情况。读者会以为关掉 channel 端也是无关紧要的"放弃 job"，
看不出它会让 init 那一端出现 PeerClosed。

**原因**
代码没错，注释落后于代码：`answer_bootstrap()` 加进来时改了 shell 的启动方式，这段注释
没跟着改。

**改法**
改前：

```cpp
/// The first close has to succeed: the value came from the kernel, which put
/// the root job into this process's table and the value into %rdi. ...
```

改后：说明句柄来自启动者——内核启动时是 root job，init 启动时是 channel 一端；并说明
关掉 channel 端为什么对 init 无害：`answer_bootstrap()` 先跑，"pong" 已在 init 那端排队，
写端关闭后消息仍可读；init 等的是 `Readable | PeerClosed`，不论哪个信号先唤醒它，
那时 Readable 都已置位，而它检查的正是 Readable。

**参照**
`Zircon zx_channel_read()` 的语义：对端关闭后，已排队的消息仍可读完，读空之后才返回 PEER_CLOSED。

**验证**
仅注释。构建通过，两个镜像启动输出不变。

**故意没做**
无。

## shell 加坏指针检查：交给内核的坏指针回 InvArgs，内核继续运行（2026-09-24）

**文件**
- `examples/shell/shell.cpp` — 新增 `check_bad_pointers()`，在 `check_channel_loopback()` 之后调用；直接 include `<ours/vdso.hpp>`

**症状**
上一条（data_fault_resume）的行为没有常驻的检查：它只在程序传坏指针时才会走到，而仓库里没有
任何程序这么做。

**原因**
P1-A 完成判据的前一半是"把坏指针交给系统调用得到 InvArgs，内核继续运行"。这需要一个每次启动
都会跑的程序去真的传坏指针。

**改法**
改前：`_start` 依次跑 `answer_bootstrap`、`check_handle_close`、`check_channel_loopback`。

改后：多一个 `check_bad_pointers()`。直接调 vDSO 的 `ours_*`，不经 `ou` 包装，让指针原样进内核。
两类地址：

- `UNMAPPED = 0x7fff'ffff'e000`：在用户半区内，范围检查放行，但没有映射——拷贝缺页、调页失败、
  走恢复路径。分别当源（`copy_from_user`）和当目的（`copy_to_user`）用，这是两条不同的拷贝。
- `KERNEL = 0xffff'8000'0000'0000`：内核半区，拷贝之前就被范围检查拒绝。恢复出现之前它就回
  `InvArgs`，放在这里是回归项。

输出：

```
shell: debug_write(unmapped) = InvArgs
shell: debug_write(kernel half) = InvArgs
shell: channel_create = Ok
shell: channel_write(from unmapped) = InvArgs
shell: channel_write(1 byte) = Ok
shell: channel_read(into unmapped) = InvArgs
shell: channel_read(after the lost message) = ShouldWait
shell: wait_one(observed = unmapped, poll) = InvArgs
shell: channel_create(out0 = unmapped) = InvArgs
shell: channel_create(after that) = Ok
shell: close(a) = Ok
shell: close(b) = Ok
shell: close(c) = Ok
shell: close(d) = Ok
shell: bad-pointer checks passed
```

几条的含义：
- `channel_write(from unmapped)` 失败后通道不变：消息先拷进内核缓冲，再碰通道。
- `channel_read(into unmapped)` 失败后消息就丢了：先出队、放锁、再拷出，拷出失败时消息已不在
  队里——与 Zircon 的取舍相同，所以下一次读是 `ShouldWait`。
- `wait_one` 用 deadline 0 轮询，等待立即结束，内核接着去写 `observed`。
- `channel_create(out0 = unmapped)` 失败后什么也不留下：两个值都先写出，再装入句柄。

计划里还有第三类：往**已映射但只读**的页（vDSO）写。现在这类写会成功——每个 x86 叶子页表项
都被强制加了 Writable（`docs/repairs/260924/x86-kernel.md` 第 1 条），所以这三项等那条修好再加，
注释里写明了。

**参照**
无对应物；这是本项目自己的启动自检。行为参照 Zircon 的 `zx_channel_read()` 在拷出失败时丢弃消息。

**验证**
两个镜像在默认（Nehalem，SMP=4）、`CPU=max`、`SMP=1` 下都打出 `shell: bad-pointer checks passed`，
之后照常到 `--- end ---`，dbg_1 下没有断言。`SMP=1` 需要把观察窗口放宽到 50 秒（25 秒时 dev 镜像
还没跑到这里），两次都通过。

**故意没做**
往只读页写的三项（见上）。
