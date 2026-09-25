# kernel/include/ours/boot — 改动记录

## vmours 有了一个统一的启动参数读取器 boot::BootOptions（2026-09-24）

**文件**
- `kernel/include/ours/boot/options.hpp` —— 新增 `ours::boot::BootOptions`、`setup_options()`、`options()`
- `kernel/top/boot_options.cpp` —— 新增，解析 OBI 命令行
- `kernel/top/main.cpp` —— `start_kernel()` 在 `setup_handoff()` 之后调 `setup_options()`
- `kernel/top/BUILD.gn` —— 加入 `boot_options.cpp`
- `kernel/lib/userabi/src/userboot.cpp` —— 删掉自己的 `select_userboot()`，改读 `boot::options().userboot()`

**症状**
无，行为不变。这是为下一步 `kernel.shell` 做的准备：在它之前，vmours 里读启动参数的
只有 userboot 一处，而且是现场自己走一遍命令行；kshell 要读 `kernel.shell`，照抄就是
第二份。

**原因**
读一个启动参数不止是 `.value(key)` 一行，它连带着三个决定：**默认值是什么、长度上限
是多少、值不合法时怎么办**。userboot 里这三件事是和查找写在一起的：

```cpp
// kernel/lib/userabi/src/userboot.cpp（改前）
auto select_userboot() -> ProgramName {
    ProgramName name{};
    auto selected = USERBOOT_FILE;                       // 默认值
    auto const package = boot::boot_package();
    if (package.is_ok()) {
        auto const cmdline = package.unwrap().command_line();
        if (cmdline.is_some()) {
            auto const value = cmdline.value().value(USERBOOT_OPTION);
            if (value.is_some()) {
                if (value.value().size() < sizeof(name.text)) {   // 长度上限
                    selected = value.value();
                } else {
                    log::error(...);                      // 不合法时的反应
                }
            }
        }
    }
    ...
}
```

第二个消费者如果也这么写，这三件事就有了两份，而且各自会漂：一个参数的默认值是
「谁读它谁说了算」，就不再是这个参数的属性了。Zircon 正是为此把所有参数收进一张表
（`lib/boot-options/options.inc`），名字、类型、默认值、说明各写一次，内核里所有人读
同一个 `gBootOptions`。

**改法**
照 Zircon 的形状，但只收 vmours 自己用的参数：

```cpp
// kernel/include/ours/boot/options.hpp（改后）
class BootOptions final {
  public:
    static constexpr std::string_view USERBOOT_OPTION{"kernel.select.userboot"};
    auto apply(obi::CommandLineView const &cmdline) -> void;
    auto userboot() const -> std::string_view;
  private:
    char m_userboot[64]{"userboot"};
};
auto setup_options() -> void;
auto options() -> BootOptions const &;
```

```cpp
// kernel/top/main.cpp（改后）
boot::setup_handoff(handoff);
boot::setup_options();
```

```cpp
// kernel/lib/userabi/src/userboot.cpp（改后）
auto const program = boot::options().userboot();
```

几个取舍：

1. **解析一次，放在 `setup_handoff()` 之后**。命令行是 boot package 里的一个 item，
   而 package 是 handoff 定位的，所以这是最早能解析的时刻。之后谁读都是读同一个对象。
   `setup_options()` 和 `options()` 是照 `setup_handoff()` / `handoff()` 那一对写的。

2. **`options()` 在解析之前被调用就 panic**，和 `handoff()` 一样。不这么做，一个过早的
   读者会拿到默认值，看起来和「镜像没设这个参数」一模一样——这正是最难查的那种错。

3. **和 phys::BootOptions 分开，不合并**。分界是「谁消费」：phys 的参数（`aslr.disabled`、
   `kernel.phys.next`、`kernel.name`）在内核加载前就用完了。phys handoff 故意不携带
   命令行（vmours 能从 boot package 重新得到它，两份就是同一个问题的两个答案），所以
   这里重新解析 OBI 的 Config item，而不是像 Zircon 那样从 physboot 接一份解析好的
   `gBootOptions`。它也永远看不到 GRUB 的命令行：那个只驱动 phys。

4. **值不合法时记日志并保留默认值，不返回错误**。下游没有人能对这个错误做任何事，
   而默认值永远是一个能启动的内核。行为和改前完全一样，只是日志前缀从 `userboot:`
   变成 `boot:`，并且提前到了启动最早期（`setup_handoff` 之后立刻打出）。

5. `ProgramName` 留在 userboot.cpp：`launch()` 还要给 `process_spawn_boot` 传进来的
   名字用，那是一个来自用户态缓冲区的 view，不是启动参数。

**参照**
- Zircon `lib/boot-options`：`BootOptions`、`options.inc` 的 `DEFINE_OPTION`、
  `BootOptions::SetMany()`；内核里读 `gBootOptions`。
- Zircon physboot `HandoffPrep::UsePackageFiles()`：Zircon 在 phys 阶段就把
  `kernel.select.userboot` 对着 kernel package 解析掉，交给内核一个已加载的 ELF。
  这里 handoff 不带 userboot 镜像，所以 `userabi::userboot_init()` 自己查。

**验证**
- 因为另一个会话正在改 logz4（未提交，`panic.cpp` 暂时编不过），本条是在一个只含
  HEAD + 本改动的临时 worktree 里构建和启动的。
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format / clang-tidy 对改动行无新告警。
- QEMU：
  - dev：`userabi: handing init the root job`，`init: shell says "pong"`、
    `shell: channel checks passed`、`shell: bad-pointer checks passed`，`kshell> ` 下
    `log` 正常。
  - dbg_1：`handing shell the root job`，两条 checks passed。
  - 临时把 dbg_1 的 cmdline 改成 70 个字符的名字：打出
    `boot: kernel.select.userboot=xxx…x is longer than 63 characters, keeping 'userboot'`，
    随后 `handing userboot the root job`、`found 'userboot'`——回退路径和改前一致。

**故意没做**
- **没有做成 Zircon 那样的 X-macro 参数表**。现在只有一两个参数，一张 `.inc` 表加一套
  宏展开的收益撑不起它的成本，而且这个仓库不用宏去生成代码。参数多起来再说。
- **没有和 phys 的布尔解析共享代码**：本条还没有布尔参数，等 `kernel.shell` 进来时再处理。
