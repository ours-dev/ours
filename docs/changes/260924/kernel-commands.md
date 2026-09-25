# kernel/commands 改动记录

## `log` 命令认识目标：`log targets`、`log level <target> <level>`、`log dump <target>`（2026-09-24）

**文件**
- `kernel/commands/log.cpp` — 新增 `targets` / `dump [<target>]` / `level <target> <level>|default` 三种用法；命令的回复改为直接写控制台 sink

**症状**
logz4 有了具名目标和按名字查找（`docs/changes/260924/logz4.md`），但控制台上够不着：
`log` 只会 dump 全部、只会设全局下限。另外一个旧问题：命令自己的回复走的是 `log::warn`，
先敲 `log level error` 再敲错一个级别名，报错被刚设的下限吞掉，屏幕上什么都没有。

**原因**
回复和日志不是一回事，却走了同一条路：

```cpp
// log.cpp（改前）
if (!logz4::parse_level(argv[2], level)) {
    log::warn("log: unknown level '{}'", argv[2]);
    return 1;
}
```

这条 warn 要过全局下限，还会进环形记录。前者让"调下限的命令"的输出受它自己调的下限
控制——`log level error` 之后 `log targets` 会什么都不打印，恰好在最需要看清当前状态的
时候看不见；后者把控制台的答复混进"内核说过的话"里，下次 panic dump 时还会再放一遍。

**改法**
回复单独一个 `reply()`：在栈上格式化成一整行，一次 `write()` 直接交给当前 logger，
不过滤、不进 ring——和 `dump_recent()` 回放记录走的是同一条路：

```cpp
// log.cpp（改后）
template <typename... Args>
auto reply(std::format_string<Args const &...> fmt, Args const &...args) -> void {
    char line[128];
    auto const result = ustl::fmt::format_to_n(line, sizeof(line) - 1, fmt, args...);
    auto length = static_cast<usize>(result.out - line);
    line[length++] = '\n';
    logz4::get_global_logger().write(line, length);
}
```

格式串是 `std::format_string`，写错占位符在编译期就报。

语法：

```
log                               dump 全部（保留，panic 排查和旧文档都这么敲）
log dump [<target>]               dump 全部，或只 dump 一个目标的行
log targets                       列出全局下限和每个目标的下限
log level <level>                 设全局下限（原有）
log level <target> <level>        给一个目标单独的下限
log level <target> default        去掉它，回到跟随全局
```

`level` 后面是级别还是目标，按参数个数区分而不是按"能不能解析成级别"：
这样一个恰好叫 `warn` 的目标也能被点名。`default` 用一个词而不是某个级别名，
因为它不是级别——目标从此没有自己的下限。`log targets` 里跟随全局的目标
显示它此刻实际生效的级别，后面标 `(default)`。

**参照**
- `env_logger` 的 `RUST_LOG=warn,kshell=debug`：全局一条、按目标覆盖；这里是它的交互版。
- Linux dynamic_debug 的 control 文件：`echo 'module kshell +p' > /sys/kernel/debug/dynamic_debug/control`，
  `cat control` 列出每个点的当前状态（`ddebug_proc_show()`）——对应 `log level` 和 `log targets`。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format / clang-tidy 无告警。
- QEMU `kernel.dev.dbg_1.obi`，在 `kshell> ` 依次敲：

  ```
  log level warn           （main.cpp 测试线程的 debug 洪流停下）
  log targets              → log: global floor warn
  log level bogus          → log: unknown level 'bogus'
  log level nosuch debug   → log: no target 'nosuch' (see `log targets`)
  log dump nosuch          → log: no target 'nosuch' (see `log targets`)
  log frob                 → usage: log [dump [<target>] | targets | level [<target>] <level>|default]
  log level error
  log targets              → log: global floor error     （下限在 error，回复照样显示）
  ```

  这一步内核里还没有目标，所以列表只有全局一行；按目标设级别的实测在 kshell 迁移那一条里。

**故意没做**
- **没有通配（`log level k* debug`）和一次多个目标。** Rust 的目标是按前缀匹配的
  （`kshell` 同时管 `kshell::parser`），这里的目标名是平的，目前没有层级可匹配。
- **`log` 以外的命令（`prof.*`、`mutex.stress`）的回复仍走 `log::info`。** 同样会被下限吞，
  但那是各自命令的事；需要时把 `reply()` 挪到 kshell 提供给所有命令用。
