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

## Target 注册表：链接段收集，按名字查找（2026-09-24）

**文件**
- `system/lib/logz4/include/logz4/target.hpp` — `Target::named` 放进 `LOGZ4_TARGET_SECTION`；新增 `Target::all()` / `Target::find(name)`
- `system/lib/logz4/src/target.cpp` — 新增：按段边界符号给出 `std::span<Target>`，线性查名字
- `system/lib/logz4/BUILD.gn` — 加 `src/target.cpp`
- `kernel/kernel.ld`、`kernel/phys/phys.ld`、`kernel/phys/physload.ld` — `.data` 开头 `KEEP(*(logz4_targets))` 并划出 `__start_logz4_targets` / `__stop_logz4_targets`
- `system/lib/logz4/test/logz4_test.cpp` — 注册表用例

**症状**
上一条让每个 Target 带上了自己的级别，但只有手里拿着这个对象的代码能改它。控制台敲
`log level kshell debug` 时，命令只有一个字符串 "kshell"，没有办法从名字找到对象。

**原因**
Target 分散在各个翻译单元里，彼此不知道对方存在。要按名字找，得有一张表；而这张表
不能靠"第一次打日志时登记"来填：那样的话，一个还没打过日志的目标查不到，
而"先把它的 debug 打开、再看它的第一行 debug"恰恰是最常见的用法——等它先说话再去开，
第一行已经被过滤掉了。

**改法**
和 `GKTL_COMMAND` 进 `.gktl.commands`、Linux dynamic_debug 描述符进 `__dyndbg` 同一个办法：
让链接器把所有 Target 排成一个数组。定义处只多一个段属性：

```cpp
// target.hpp（改前）
template <TargetName Name>
constinit Target Target::named{Name.chars};

// target.hpp（改后）
template <TargetName Name>
LOGZ4_TARGET_SECTION
constinit Target Target::named{Name.chars};
```

每个实例化是段里的一个 COMDAT 成员，同名只留一份，所以 `all()` 里每个名字出现一次。
段属性写在库里、只写一次，使用处 `constexpr auto &KSHELL = Target::named<"kshell">;`
不需要任何属性或宏。

几个取舍：

1. **段名因目标文件格式而异，所以用一个宏 `LOGZ4_TARGET_SECTION`。** ELF 取裸名
   `logz4_targets`，Mach-O 要 `"__DATA,__logz4_targets"`（主机单测在 macOS 上是 Mach-O）。
   宏在这里承载的是构建配置（目标文件格式）到厂商属性的映射，符合"宏只做这两件事"的规矩。

2. **ELF 段名是 C 标识符，边界符号用 `__start_`/`__stop_` 这对名字。** 默认的 GNU ld / LLD
   布局会为 C 标识符命名的段自动合成这两个符号，所以托管的 ELF 测试不需要链接脚本；
   内核和 phys 的脚本则显式 `PROVIDE_HIDDEN` 同名符号。`target.cpp` 在所有镜像里读的
   是同一对符号。Mach-O 由链接器合成 `section$start$__DATA$__logz4_targets`。

3. **放在 `.data`，不在 RELRO。** 命令表和计数器描述符在 `.data.rel.ro` 里，因为只读；
   Target 带着运行时要写的级别，必须可写。phys 的两个脚本里同样显式放进 `:data`：
   记录里有名字指针，physload 是 PIC，指针是 `R_X86_64_RELATIVE` 重定位，目标必须在可写段
   （和 `.phys.exports` 同一个约束）；放成孤儿段的话，在写明了 PHDRS 的脚本里它会继承
   前一个段的 PHDR，落到哪儿说不准。

4. **KEEP。** 一个目标如果所有打日志的代码都被 `--gc-sections` 收掉了，它仍然留在表里、
   仍然可以按名字设级别。

```
# kernel.ld（改后，.data 开头）
PROVIDE_HIDDEN(__start_logz4_targets = .);
KEEP(*(logz4_targets))
PROVIDE_HIDDEN(__stop_logz4_targets = .);
*(SORT_BY_ALIGNMENT(.data*))
```

**参照**
- Linux dynamic_debug：`DEFINE_DYNAMIC_DEBUG_METADATA` 把 `struct _ddebug` 放进 `__dyndbg`，
  `vmlinux.lds.h` 的 `BOUNDED_SECTION_BY(__dyndbg, ___dyndbg)` 划边界，
  `dynamic_debug_init()` 遍历；模块各带一个 `__dyndbg`，`ddebug_add_module()` 在加载时登记。
- 本仓库 `GKTL_COMMAND` / `.gktl.commands` / `gktl::run_command()`。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 全量通过（kernel、physload 两遍链接、bootshim）。
- 主机 `logz4-test`（Mach-O）通过：`find("alpha") == &ALPHA`、`find("alph") == nullptr`、
  `all()` 恰好 2 个、经 `find()` 设下限后 `ALPHA.debug` 在全局 error 下放行。
- `llvm-nm vmours`：`__start_logz4_targets == __stop_logz4_targets == __data_start`
  （这一步内核里还没有目标，空表，符号存在）。
- clang-tidy 无新告警（边界数组是可写对象，按变量规则命名 `section_start`/`section_end`）。
- QEMU：`kernel.dev.obi` 仍有 `init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed`；`kernel.dev.dbg_1.obi` 两条 shell 检查行都在。

**故意没做**
- **边界符号不是 weak。** 默认 ELF 布局只在段存在时才合成 `__start_`/`__stop_`，所以一个托管的
  ELF 程序链接了 logz4 却一个 Target 都没声明，会链接失败。用 weak 可以让它链过去，
  但弱未定义符号解析成 0，得到一个悄悄说"没有这个目标"的空表——不如一个说明原因的链接错误。
  目前这样的程序不存在（主机上链接 logz4 的只有 logz4-test，它声明了目标）。
- **phys 模块的目标不并进 physload 的表。** 每个镜像一张表，和 Linux 每个模块一个 `__dyndbg`
  一样；phys 没有控制台，暂时没人需要跨镜像查。
- **没有哈希/排序。** 一个镜像几十个目标，查找只在有人敲命令时发生，线性扫描足够。

## 改名：`Target` → `Domain`（2026-09-24）

**文件**
- `system/lib/logz4/include/logz4/target.hpp` → `domain.hpp`，`src/target.cpp` → `src/domain.cpp`
- `system/lib/logz4/include/logz4/{log,ring}.hpp`、`src/{log,ring}.cpp`、`test/logz4_test.cpp`、`BUILD.gn` — 标识符与注释
- `kernel/kernel.ld`、`kernel/phys/phys.ld`、`kernel/phys/physload.ld` — 段名 `logz4_targets` → `logz4_domains`
- `kernel/commands/log.cpp` — `log targets` → `log domains`，提示文字随之改
- `kernel/lib/kshell/kshell/shell.cpp` — `Target::named` → `Domain::named`

**症状**
无运行时症状，是命名问题。

**原因**
这个功能在需求里叫"日志空间"，用户给出的目标写法是 `LogDomain`：一个作用域，
作用域里的日志行都属于它。"target" 是 Rust `log` crate 的叫法，它在那里是每条记录上
的一个字符串属性，跟作用域无关；"domain" 是 GLib 的叫法（`G_LOG_DOMAIN`），
而下一步要做的正是按作用域进入/退出的域。名字和接下来的模型不一致，就先改名字。

**改法**
纯改名，行为不变：

```cpp
// 改前
constexpr auto &KSHELL = logz4::Target::named<"kshell">;
// 改后
constexpr auto &KSHELL = logz4::Domain::named<"kshell">;
```

`TargetName` → `DomainName`，`LOGZ4_TARGET_SECTION` → `LOGZ4_DOMAIN_SECTION`，
边界符号 `__start_/__stop_logz4_targets` → `__start_/__stop_logz4_domains`，
控制台 `log targets` → `log domains`。类注释里对照物的说法同步改正：GLib 叫 domain，
Rust 叫 target。

**参照**
GLib `G_LOG_DOMAIN`、`g_log_set_handler(log_domain, ...)`。

**验证**
- 全量构建通过；`logz4-test` 通过；clang-tidy 无新告警。
- `llvm-nm vmours`：`__start_logz4_domains`..`__stop_logz4_domains` 相距 0x10，正好一个 `Domain`（kshell）。
- QEMU `kernel.dev.dbg_1.obi`：`log level warn`、`log level kshell debug`、`log domains` →
  `[... debug] (kshell) run 'log' with 1 argument(s)` / `log: global floor warn` / `kshell  debug`。

**故意没做**
改名和改模型分开提交：这一条只动名字，作用域守卫 `LogDomain` 是下一条。

## `LOGZ4_DOMAIN_SECTION` 登记进 .clang-format，属性宏单独占一行（2026-09-24）

**文件**
- `.clang-format` — `AttributeMacros` 和 `Macros` 各加一项 `LOGZ4_DOMAIN_SECTION`
- `system/lib/logz4/include/logz4/domain.hpp` — 重新格式化后宏回到自己那一行

**症状**
`Domain::named` 的定义被 clang-format 排成了一行：

```cpp
// 改前（格式化结果）
template <DomainName Name>
LOGZ4_DOMAIN_SECTION constinit Domain Domain::named{Name.chars};

// 改后
template <DomainName Name>
LOGZ4_DOMAIN_SECTION
constinit Domain Domain::named{Name.chars};
```

本仓库的规矩是属性宏单独占一行（`PACKED` 除外）。

**原因**
clang-format 只在它认得出是属性的地方断行。`BreakAfterAttributes` 只作用于真正的 `[[...]]`，
一个它不认识的宏名在它眼里就是普通标识符。根 `.clang-format` 里 `INIT_CODE` 等宏靠
`AttributeMacros`（别把名字读错）加 `Macros`（给出一个"长得像属性"的展开）两处登记才会断行，
新宏没有登记，于是被并到声明上。

**改法**
照根配置里已有条目的写法登记。`Macros` 里的展开只给格式化器看，写 ELF 的拼法就够了——
真正的展开按目标文件格式二选一（见 `domain.hpp`），格式化器不关心。

**参照**
无，本项目格式化配置。

**验证**
`clang-format -i domain.hpp` 后宏独占一行；其它文件的格式不受影响（这个宏只在 `domain.hpp` 出现）。

**故意没做**
没有在 `system/lib/logz4/.clang-format` 里登记：`InheritParentConfig` 下列表类键是整体覆盖而不是合并，
在子配置里写 `AttributeMacros` 就得把父配置的整张表抄一遍。
