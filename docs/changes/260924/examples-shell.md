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
