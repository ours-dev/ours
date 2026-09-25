# logz4 改动记录

## 具名日志目标 `logz4::Target`：行上带出处，按目标设级别（2026-09-24）

**文件**
- `system/lib/logz4/include/logz4/target.hpp` — 新增：`TargetName`（名字做模板实参）和 `Target`（名字 + 自己的级别下限 + trace/debug/info/warn/error）
- `system/lib/logz4/include/logz4/log.hpp` — `vlog_dispatch()`/`vlog()` 多一个 `Target const *`；参数处理收进 `detail::emit()`，自由函数和 `Target` 共用；新增 `dump_recent(Target const &)`
- `system/lib/logz4/src/log.cpp` — 前缀后面渲染 `(name) `；级别检查按目标自己的下限
- `system/lib/logz4/include/logz4/ring.hpp`、`src/ring.cpp` — 记录槽多存一个目标指针；新增按目标过滤的 `dump_to(sink, only)`
- `system/lib/logz4/test/logz4_test.cpp` — 目标的渲染、同名同对象、各自下限、按目标 dump
- `kernel/top/panic.cpp`、`kernel/phys/panic.cpp` — 直接调 `vlog()` 的两处传匿名目标 `nullptr`

**症状**
不是故障，是缺一个能力。日志行看不出是谁打的：调用点只能手写前缀，
`log::warn("kshell: failed to spawn console thread")`，写不写、怎么写全凭自觉。
级别下限只有一个全局的 `set_min_level()`：想看某个子系统的 debug，只能把全局放到
debug，所有子系统的 debug 一起涌出来，要看的那几行被淹掉。

**原因**
logz4 的整条路径上没有"出处"这个概念：

```cpp
// log.hpp（改前）
auto vlog_dispatch(Logger &logger, Level level, ustl::views::StringView fmt,
                   ustl::fmt::FormatArgs const &args) -> void;

// ring.hpp（改前）
auto append(char const *line, usize len, Time time, u32 cpu, Level level) -> void;
```

调用点到 sink 只传级别，环形记录也只存 (line, len, time, cpu, level)。所以
"只看 kshell 的行"既不能在打印时过滤（没有东西可比），也不能在 dump 时过滤
（记录里没有，只能反过来解析自己渲染出的文本）。按目标设级别更无从谈起——
级别下限挂在哪儿，得先有个"目标"对象。

**改法**

一个具名对象，不是宏：

```cpp
constexpr auto &KSHELL = logz4::Target::named<"kshell">;
KSHELL.warn("failed to spawn console thread");   // [warn] (kshell) failed to ...
```

几个取舍：

1. **名字是模板实参，不是构造参数。** `Target::named<"kshell">` 是一个静态成员
   变量模板，同一个名字在整个镜像里只实例化出一个对象（COMDAT 合并）。于是
   "kshell" 这个名字本身就是身份：两个 .cpp 各写一次，拿到的是同一个 Target，
   `log level kshell debug` 一次管住所有行。被否掉的方案是每个文件
   `constinit Target KSHELL{"kshell"}` 各定义一个——同名多个对象，设级别就得
   按名字把所有同名的都找出来改，而且哪个文件忘了改就漏哪个。
   构造函数是私有的，`named` 是树里唯一的构造点，不会有游离在外的 Target。
   名字在编译期校验：非空、只含可打印非空白字符（控制台按空白切词，含空格的名字
   打得出来却敲不回去）；不合格时编译器的 note 里会点出
   `target_name_must_be_printable_without_blanks`。

2. **级别放在对象里，对象是 `constinit` 不是 `constexpr`。** constexpr 对象不能有
   可变状态，而每目标级别必须可变。`constinit` 保证常量初始化——镜像打第一行日志时
   它已经就绪，不依赖任何动态初始化器——同时对象本身落在可写数据里。
   调用点拿的是 `constexpr auto &` 引用，名字照常量规则写成 `KSHELL`。
   级别用一个 `std::atomic<Level>` 存，"跟随全局"用枚举范围外的 0xFF 表示：
   读"自己的下限或没有"是一次 relaxed load；拆成"标志 + 级别"两个原子要读两次，
   还可能读到改了一半的状态。

3. **优先级和 `RUST_LOG=warn,kshell=debug` 一样。** 目标没设过下限就跟随全局；
   设了就用自己的，可以比全局松（只放开这一个子系统的 debug），也可以比全局严。
   `clear_min_level()` 交还给全局。

4. **渲染在方括号外面**：`[42 cpu3 warn] (kshell) x`。时间、CPU、级别是"这一刻"的
   元数据，目标是"谁在说"，位置正是 Linux `pr_fmt` 前缀、也就是这棵树里手写
   `"kshell: "` 的地方。匿名目标什么都不打，没迁移的行一个字节都不变。

5. **环形记录存目标指针**，`dump_to(sink, only)` / `dump_recent(only)` 按它过滤。
   槽从 544 字节涨到 552（多一个指针），256 槽的 .bss 从 ~136 KiB 到 ~138 KiB：

   ```cpp
   // ring.cpp（改后）
   struct Slot final {
       std::atomic<u32> state;
       u32 cpu;
       std::atomic<u64> ticket;
       Time time;
       Target const *target;   // 新增
       Level level;
       u16 len;
       char bytes[LINE_BYTES];
   };
   static_assert(sizeof(Slot) == 552, "Slot layout is part of the .bss budget");
   ```

   过滤条件读在第一次 ticket 检查之后、复查之前，和 `len` 一样由复查背书：
   正在被覆盖的槽，目标连同字节一起丢弃。

6. **自由函数保留**，就是匿名目标（`nullptr`）：只受全局下限约束，不打名字。
   参数退化那段逻辑挪进 `detail::emit()`，自由函数和 `Target` 的成员都走它：

   ```cpp
   // log.hpp（改后）
   template <typename... Args>
   inline auto log(Level level, ustl::views::StringView fmt, Args &&...args) -> void {
       detail::emit(nullptr, level, fmt, ustl::forward<Args>(args)...);
   }
   ```

7. **级别检查仍然只在 `vlog()` 里一处**，按目标选下限：

   ```cpp
   // log.cpp（改前）
   if (!is_enabled(level)) {
       return;
   }

   // log.cpp（改后）
   if (target != nullptr) {
       if (!target->is_enabled(level)) {
           return;
       }
   } else if (!is_enabled(level)) {
       return;
   }
   ```

   被拒绝的一行的代价和改前相同：一次 `vlog_dispatch()` 调用 + 一两次 relaxed load +
   比较。

**参照**
- Rust `log` crate：`Record::target()`，`env_logger` 的 `RUST_LOG=warn,kshell=debug`
  指令解析（`env_logger::filter::Builder::parse`）——目标即字符串、没有指令就用默认级别，
  这里的"跟随全局"就是照它。
- GLib：`G_LOG_DOMAIN` 宏 + `g_log_set_handler(domain, ...)`，`G_MESSAGES_DEBUG=domain`
  按域放开 debug。
- Linux：`pr_fmt()`（每文件前缀）+ dynamic_debug（`struct _ddebug` 放在 `__dyndbg` 段里，
  `ddebug_exec_query()` 按 module/file 开关 `pr_debug`）。Linux 把"打名字"和"按来源开关"
  拆成两套机制，这里合在一个对象上。
- Android `__android_log_print(prio, tag, ...)` / Fuchsia syslog 的 tag。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 全量通过；`logz4-test`（主机）通过，新增用例：
  目标渲染 `[info] (alpha) hello 7`、带时间和 CPU 的 `[42 cpu3 warn] (alpha) x`、
  同名同对象、跟随全局、比全局松（全局 error 下只放出 alpha 的 debug）、比全局严、
  清除后回到全局、按目标 dump（4 行里只回放 alpha 的 2 行）。
- clang-format / clang-tidy（`-p out/bringup.x86 --header-filter=system/lib/logz4/`）无新告警。
- QEMU：`kernel.dev.obi` 仍打出 `init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed`；`kernel.dev.dbg_1.obi` 启动到 `kshell> `。
  这一步还没有调用点用 Target，日志内容应与改前一致，实测一致。

**故意没做**
- **没把级别检查内联进 `Target` 的成员里。** 内联能省掉被拒绝行的那次调用，但 phys 模块
  （physboot）以 hidden 可见性各自持有一份内联的全局下限（`runtime_min_level()` 的函数内
  static），而 `log.level=` 启动参数写的是 physload 那一份。内联检查会让模块里的行按编译期
  默认值过滤。检查留在 `vlog()`（physload 导出）里，全局下限就只有一份。
- **注册表（按名字找目标）不在这一步。** 要把所有 Target 收进一个链接段、再给各个链接脚本
  划出边界，是单独的一步。
- **没给 panic 行一个目标。** 两处 `vlog()` 直接调用传 `nullptr`，panic 输出保持原样。
