# userabi 改动记录（2026-09-19）

## 第一个用户程序改由 `kernel.select.userboot` 选，不再写死在内核里（2026-09-19）

**文件**
- `kernel/lib/userabi/src/userboot.cpp` —— 删掉 `PROGRAM_FILE`（写死的 `"shell"`）
  和「找不到就退回 userboot」的回退链，改成从 OBI 命令行读
  `kernel.select.userboot=<name>`，默认 `"userboot"`。

**症状**
潜伏，当前不崩。但两件事是错的：一是内核源码里出现了 `//examples/shell` 这个
名字——内核知道了一个示例程序的存在；二是「实际跑起来的是哪个程序」取决于包里
碰巧有什么文件，而不取决于镜像声明了什么。把 shell 从包里去掉，启动行为就静默
地变成另一个程序，日志里只有一行 info，没人会当回事。

**原因**
这是层次放错了。「第一个用户进程跑哪个程序」是**被启动的那个镜像**的属性，不是
内核的属性：同一个内核可以被打进 dev 镜像（要跑 shell）、打进最小镜像（只要
userboot 证明能进 ring 3）、打进某个 boot test（要跑那个 test 自己的程序）。
写死在内核里，等于让内核替所有镜像做了这个决定，然后再用「找不到就退回」这种
猜测去补救——而猜测出来的启动行为是不可复现的：包里多一个文件、少一个文件，跑的
就不是同一个东西。

回退链本身还有个更隐蔽的毛病：它把「打包漏了」这种配置错误变成了一次看起来正常的
启动。镜像明明要求跑 A，结果跑了 B，内核只打一行 info 就继续走完整个引导，串口上
看到的是一次成功的启动。

**改法**
照搬 Zircon 的 `kernel.select.userboot`：名字来自启动选项，默认值是 `"userboot"`，
镜像想跑别的就自己写这一行。找不到选项指名的文件是硬错误，不再回退——镜像已经说了
要跑什么，退而求其次地跑另一个程序，是把配置错误伪装成正常启动。

改前：

```cpp
constexpr std::string_view PROGRAM_FILE{"shell"};
...
auto program = PROGRAM_FILE;
auto image = package.unwrap().file(program);
if (image.is_err()) {
    // Not an error yet: a tree built without //examples has no shell, and
    // the proof-of-life program is still there to start.
    log::info("userboot: no '{}' in the kernel package, falling back to '{}'", program,
              USERBOOT_FILE);
    program = USERBOOT_FILE;
    image = package.unwrap().file(program);
}
```

改后：

```cpp
constexpr std::string_view USERBOOT_OPTION{"kernel.select.userboot"};
...
auto select_userboot() -> ProgramName {
    ProgramName name{};
    auto selected = USERBOOT_FILE;

    auto const package = boot::boot_package();
    if (package.is_ok()) {
        auto const cmdline = package.unwrap().command_line();
        if (cmdline.is_some()) {
            auto const value = cmdline.value().value(USERBOOT_OPTION);
            if (value.is_some()) {
                if (value.value().size() < sizeof(name.text)) {
                    selected = value.value();
                } else { /* 太长：报错，用默认值 */ }
            }
        }
    }

    std::memcpy(name.text, selected.data(), selected.size());
    return name;
}
...
auto const program = select_userboot();
auto const image = package.unwrap().file(program.view());
if (image.is_err()) {
    log::error("userboot: {}={} but no such file in the kernel package: {}", USERBOOT_OPTION,
               program.view(), to_string(image.unwrap_err()));
    return;
}
```

`ProgramName` 是个 64 字节的定长缓冲，不是 `string_view`：选项的值是命令行 payload
里的一段，后面跟的是空格或行尾，不是 NUL，而 `ProcessDispatcher::create()` 和
`load_program()` 都要 C 字符串。64 这个数字跟着 `phys::BootOptions` 里
`kernel.phys.next`／`kernel.name` 的定长串走，也正好是 Zircon `SmallString` 的容量。

被否掉的方案：把名字放进 `phys::Handoff`，由 physboot 解析完再交给内核（Zircon 就是
这么干的，见下）。否掉的理由写在 `Handoff` 自己的注释里——它有意不带命令行，因为
vmours 能从 `boot::boot_package()` 重新读出来，带一份就是同一个问题有两个答案。
ours 的 `Handoff` 也有意不带 userboot/vDSO 镜像，所以内核这边本来就要自己查包。

**参照**
`Zircon kernel.select.userboot`（`zircon/kernel/lib/boot-options/include/lib/boot-options/options.inc`，
`SmallString`，默认 `"userboot"`）。解析位置不同：Zircon 在 physboot 里
`HandoffPrep::UsePackageFiles()` 拿这个选项去匹配 kernel package，把装载好的 ELF
经 `handoff_->userboot` 交给内核；ours 的 Handoff 不带这些，所以查找落在内核侧的
`userboot_init()`，但读的是同一个 OBI 容器里的同一个 Config item。

Zircon 那边还有一个现成的用法可以对照：`zircon/kernel/lib/userabi/trivial-test-userboot/BUILD.gn`
里的 `kernel_package("select-userboot-test")` + `zbi_test` 的
`kernel.select.userboot=$long_name`——想跑别的程序，是镜像自己声明，不是内核去猜。

**验证**
- 构建：`prebuilt/ninja/ninja -C out/bringup.x86`，42 个 target 全过。
- clang-format / clang-tidy（`prebuilt/clang/llvm-23`，`-p out/bringup.x86`）：本文件零告警。
- QEMU 实测两个镜像。此时还没有任何镜像写 `kernel.select.userboot=`，所以两边都走
  默认值，跑的是 userboot 而不是 shell —— 正是预期的中间态：

  ```
  [info] userboot: kernel package holds 6 files
  [info] userboot:   shell (1368 bytes)
  [info] userboot:   userboot (872 bytes)
  ...
  [info] userboot: found 'userboot', 872 bytes of ELF
  userboot: hello from ring 3
  ```

  `kernel.dev.obi` 与 `kernel.dev.dbg_1.obi` 都到这一步，日志一致。

**故意没做**
- 没把 `//examples/shell` 从 `common_package_items` 里挪走，也没给 dev 镜像加
  `kernel.select.userboot=shell`——那是打包侧的改动，下一条单独做，做完 dev 镜像才
  会重新跑 shell。
- 没引入内核侧的 `BootOptions` 聚合类型。目前只有这一个选项要在内核里读，为它建一张
  选项表是提前抽象；等到第二、第三个内核侧选项出现，再照 Zircon 的 `options.inc` 建。
- 没让内核去读 OBI 顶层的 BOOTFS item（产品级 `bootfs` 注入的那块）。内核现在只认
  kernel package，示例程序因此还是打在 kernel package 里；真正的 Zircon 分层
  （内核包只放内核自己的东西，其余走产品 BOOTFS）要等内核能读那个 item 之后再说。
