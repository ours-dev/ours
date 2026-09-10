# syscall —— 改动记录（2026-09-10）

## `sys_debug_write` 把用户字节送进了格式化器，写长一点就 panic 内核（2026-09-10）

**文件**
- `kernel/lib/syscall/src/debug.cpp` —— 改走 sink 的 `write()`，不再经 `log::info`

**症状**
ring 3 里写一段够长的字符串，内核**当场 panic**：

```
[3900358000 cpu1 error] operator new
```

之后那个用户线程再无输出。userboot 只写三十字节，从没碰到；`//examples/shell`
第一次要打印 250 字节的命令列表就把内核带下去了。

**原因**
调试器直接指出来了。在 `operator new` 上下断点，`bt`：

```
frame #4: operator new((null)=514) at cpp_dependencies.cpp:17:17
frame #9: std::__ours::__allocate_at_least<allocator<char>>(__n=514)
frame #14: std::__ours::__format::__output_buffer<char>::__copy(...)
frame #18: __formatter_string<char>::format(__str="log  log [level <name>]: ...
           prof.start ... prof.stop ... prof.dump ...")
```

`log::info("{}", view)` 用的是 `std::format`。它把整行拼进一个 `__output_buffer`，
行一旦超出内部数组就**堆分配**——而这个内核没有全局 `operator new`，只有一个 panic
桩（`kernel/lib/libc/cpp_dependencies.cpp:16` 的 `ours::panic(__func__)`）。

把用户字节当**格式参数**而不是格式串，已经挡住了"用户数据操纵格式化器"这一类问题；
但格式化器本身就是不该出现在这条路上的机器：**这些字节已经是字节了**，没有任何需要格式化
的东西，却因此获得了一条通往堆分配的路径，而堆分配在这个内核里等于 panic。

**改法**

```cpp
// 改前
log::info("{}", ustl::views::StringView{buffer, length});

// 改后
log::get_global_logger().write(buffer, length);
```

`Logger::write()` 是 sink 自己的入口（`system/lib/logz4/include/logz4/logger.hpp:18`），
logz4 本来就是"把拼好的一整行交给它"，它不做任何格式化。

顺带的行为变化，而且是**改好了**：输出不再被套上 `[时间戳 cpuN info]` 前缀。一个程序
分几次写完一行时，旧行为会把每个片段各包一层前缀，那不是调用者要的东西。

**参照**
Linux 的 `sys_write` 到 console 也不经过 `printk` 的格式化路径；Zircon 的
`zx_debug_write` 同样直接落到 debuglog。

**验证**
改前：shell 打印 250 字节命令列表 → `[cpu1 error] operator new`，线程死。
改后：同一次启动完整打印，`--- end ---` 也到了：

```
log  log [level <name>]: dump the record ring, or set the runtime floor
prof.start  prof.start [pmu|tick] [period]: start the sampling profiler
prof.stop  prof.stop: stop the sampling profiler
dump-frame
```

**故意没做**
- **没有降低 `MAX_DEBUG_WRITE`**。那只是把阈值挪一挪，不改变"够长就 panic"这件事。
- **没有给内核补一个真正的 `operator new`**。堆是有的（`kernel/lib/heap`），但它要求
  显式的 `Gaf`；无参数的全局 `operator new` 仍然应当是 panic 桩，因为它意味着有人
  在没说清分配策略的情况下分配。
