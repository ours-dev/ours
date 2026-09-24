## `ou::debug_line` 一行只发一次系统调用（2026-09-23）

**文件**
- `system/lib/ou/include/ou/debug.hpp`、`src/debug.cpp` — 新增 `debug_line(std::initializer_list<Str>)`：各段拼进一个缓冲区，连同换行一次写出；`debug_line(Str)` 转发给它

**症状**
init 和 shell 在两个 CPU 上同时打印时，一行被从中间劈开：

```
shell: close(bootstrap) = init: shell says "pongOk"
```

**原因**
`debug_line` 先写正文、再写换行，是两次系统调用；调用者拼一行也是每段一次调用。内核的日志器只保证**一次**写入
在锁内完整输出，两次调用之间别的进程可以插进来。

**改法**
改前：

```cpp
auto debug_line(Str const text) -> Status {
    auto const status = debug_write(text);
    if (Status::Ok != status) {
        return status;
    }
    return debug_write("\n");
}
```

改后：各段加换行不超过 `MAX_DEBUG_WRITE` 时，先拷进栈上的 256 字节缓冲，再一次 `ours_debug_write()`；超长才退回
分段写（那时仍可能交错，注释里写明了）。调用方用 `ou::debug_line({"init: shell says \"", reply, "\""})` 拼一行。

**参照**
无，本项目自有；同样的原因 Zircon 的 `zx_debug_write` 调用方也是按整行写。

**验证**
dev 镜像（init 与 shell 并行打印）连续启动 5 次，每行都完整；此前 5 次里有 2 次被劈开。

**故意没做**
没有在内核里按行缓冲：那等于让内核替每个进程维护半行文字的状态。
