# system/lib/ou 改动记录（2026-09-26）

## `ou::channel_read`/`channel_write` 带上句柄参数（2026-09-26）

**文件**
- `system/lib/ou/include/ou/channel.hpp`、`src/channel.cpp` — 两个入口加上句柄参数，默认"没有句柄"；文档写明句柄的去向
- `examples/shell/shell.cpp` — `check_handle_transfer()` 改走 ou，不再直接调 vDSO

**症状**
用户程序经 ou 收发不了句柄：`ou::channel_read`/`channel_write` 把句柄参数写死成 `nullptr, 0`，头文件还写着"消息里的句柄还不支持"。
内核早在 K10（`5d863ffd`）就支持句柄随消息转移，shell 测它时只能绕过 ou 直接调 `ours_channel_write`/`ours_channel_read`。
下一步（U2）的 runtime 要从 bootstrap channel 收一条带句柄的 processargs 消息，按 ou 的约定（"程序到 ou 找怎么调一个系统调用，不直接调
`ours_*`"，`system/lib/ou/README.md`）它得经 ou 读。

**原因**
ou 的入口是在内核还不能转移句柄的时候写的，那时"不带句柄"就是全部功能；K10 补上内核之后，ou 这层没有跟上。

**改法**
改前：

```cpp
auto channel_write(ours::HandleValue channel, void const *bytes, u32 num_bytes) -> ours::Status;
auto channel_read(ours::HandleValue channel, void *bytes, u32 num_bytes, u32 *actual_bytes)
    -> ours::Status;
```

改后：

```cpp
auto channel_write(ours::HandleValue channel, void const *bytes, u32 num_bytes,
                   ours::HandleValue const *handles = nullptr, u32 num_handles = 0)
    -> ours::Status;
auto channel_read(ours::HandleValue channel, void *bytes, u32 num_bytes, u32 *actual_bytes,
                  ours::HandleValue *handles = nullptr, u32 num_handles = 0,
                  u32 *actual_handles = nullptr) -> ours::Status;
```

- **默认参数而不是第二个入口**：ou 的约定是"一个系统调用一个入口"。只带字节的消息是最常见的，句柄参数放在后面、默认为空，
  已有的调用方一行不用改，也不会出现 `channel_write` 和 `channel_write_handles` 两个名字对应同一个调用。
- **文档写清句柄的去向**，因为这是最容易用错的地方：写出去的每个句柄**无论成败**都离开本进程的句柄表（成功就随消息走，失败就被关掉），
  之后不能再用这些值；读进来的句柄是新值，归本进程所有，要关掉或转交。只准备了字节缓冲的读者遇到带句柄的消息，得到的是 BufferTooSmall，
  消息留在队列里，句柄不会被悄悄丢掉。这些都是 K10 在内核里定下的语义，ou 这里只是写到调用方看得见的地方。

**参照**
Zircon 的 `zx_channel_write`/`zx_channel_read` 就是一个入口同时收字节和句柄；ou 按本仓库"一个调用一个入口"的约定，用默认参数表达"没有句柄"。

**验证**
两个镜像构建、启动。dev 与 dbg_1 的 shell 改走 ou 之后，句柄转移这组检查的结果与以前逐行相同：`write(a, handles = [d]) = Ok`、
`read(b) = Ok`、`write(a, handles = [a]) = NotSupported`、`a 1000-deep chain of ends, closed`、`shell: handle transfer checks passed`；
dev `init: shell closed its channel`，dbg_1 `shell: probes done (11 killed, 0 survived)`。

**故意没做**
- `check_bad_pointers()` 仍直接调 vDSO：它要让坏指针原样到达内核，经过 ou 就多了一层可能碰到指针的代码。
- `system/lib/ou/README.md` 里"目前内核只有两个系统调用"已经过时，与本改动无关，不在这里顺手改。
