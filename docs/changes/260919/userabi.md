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

## 用户程序改成加载时按名字绑定 vDSO，不再自己查表（2026-09-19）

**文件**
- `system/user_executable.gni` —— 新建。用户程序的构建模板：PIE、链接 vDSO、
  `-fno-plt -z now --no-dynamic-linker`，打包进内核包。
- `system/BUILD.gn` —— 新增 `config("user.program")`。
- `system/public/ours/vdso.hpp` —— 新增 `ours_debug_write` / `ours_command_list`
  的 `extern "C"` 声明。
- `kernel/lib/userabi/src/userboot.cpp` —— 新增 `bind_imports()`（按名字把程序的
  未定义符号绑到 vDSO 的 `.dynsym`，并应用程序自己的 `R_X86_64_RELATIVE`），
  `MappedImage` 增加内核侧视图与 `image_base`。
- `kernel/lib/userabi/BUILD.gn` —— 依赖 `//kernel/lib/eftl`。
- `kernel/lib/userabi/vdso/BUILD.gn` —— 加 `-soname,libours.so`，visibility 放开。
- `examples/shell/BUILD.gn`、`examples/shell/shell.cpp` —— 改用新模板，直接调用。

**症状**
潜伏。加一个系统调用要动五个地方：vdso.S 加 stub、`vdso.hpp` 加偏移宏、加结构体
字段、版本号加一，然后**每个用户程序**在自己的 `Vdso` 类里加一个访问器。程序在能
调用系统调用之前，得先自己跑一遍「动态链接」。

**原因**
这是把链接器和加载器的活派给了应用程序。原来的机制是：vDSO 在 e_entry 放一张表，
内核把表的运行时地址塞进 `%rdi`，程序检查 magic/version，再把每个 `u32` 偏移加回
表基址、强转成函数指针。等于每个程序各自手写一份只支持一张表的动态链接器。

ELF 早就描述了这件事：共享对象的 `.dynsym` 是导出表，导入方的 `.rela.dyn` 是
「这些名字请在加载时填进这些槽位」。physload 那边已经用上了（`ModuleLoader::load()`
的 `SymbolBinder` 把模块的未定义符号绑到 physload 的导出表），用户态却在另起炉灶。

**改法**
三件事凑成一个机制：

1. 程序侧（`user_executable()` + `config("user.program")`）：PIE，链接 vDSO，
   `-fno-plt -z now`。于是 `ours_debug_write` 变成一个 `R_X86_64_GLOB_DAT` 重定位，
   而不是一次手工偏移计算；拼错名字是链接错误，不是启动时才发现的空指针。
2. 头文件侧：`<ours/vdso.hpp>` 直接声明这两个 `extern "C"` 函数。程序 include 它、
   调用它，别的什么都不做。
3. 加载器侧：`bind_imports()` 用 eftl 读程序的 `PT_DYNAMIC`，逐条处理
   `.rela.dyn`/`.rela.plt`，`RELATIVE` 用程序自己的 bias，其余按符号名去 vDSO 的
   `.dynsym` 里查，查不到就报名字并拒绝启动。

shell 改前：
```cpp
class Vdso {
  public:
    using DebugWriteFn = auto (*)(char const *buffer, usize length) -> usize;
    static constexpr auto bind(ours::VdsoEntryTable const *table) -> Vdso {
        if (table == nullptr || table->magic != OURS_VDSO_MAGIC ||
            table->version != OURS_VDSO_VERSION || table->nr_entries < OURS_VDSO_NR_ENTRIES) {
            return Vdso{};
        }
        return Vdso{table};
    }
    auto debug_write() const -> DebugWriteFn { return entry<DebugWriteFn>(m_table->debug_write); }
    // ... entry<Fn>()：reinterpret_cast<usize>(m_table) + offset
};

extern "C" [[noreturn]] auto _start(ours::VdsoEntryTable const *const table) -> void {
    auto const vdso = Vdso::bind(table);
    if (!vdso.valid()) { for (;;) { __asm__ volatile("pause"); } }
    Console const console{vdso};
    console.line(BANNER);
```

改后：
```cpp
extern "C" [[noreturn]] auto _start() -> void {
    Console::line(BANNER);
```
`Console::write()` 里那一行也从 `m_write(at, chunk)` 变成 `ours_debug_write(at, chunk)`
——一个普通函数调用。整个 `Vdso` 类（约 50 行）连同 `_start` 的参数一起删掉。

加载器新增的核心：
```cpp
if (type == R_X86_64_RELATIVE) {
    value = program_bias + static_cast<u64>(rela.r_addend);
} else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT) {
    ...
    auto const definition = find_export(exports.value(), spelling);
    if (definition.is_none()) {
        log::error("userboot: the program needs '{}', which the vDSO does not export", spelling);
        return Status::NotFound;
    }
    value = vdso_bias + definition.value() + static_cast<u64>(rela.r_addend);
}
memory.write<u64>(static_cast<u64>(rela.r_offset), value);
```

两个容易写错的点，都在注释里写清楚了：一是**写必须走内核侧视图**，不能写用户虚拟
地址——那是另一个地址空间，从内核写过去打到的是当前地址空间的同名地址；`MappedImage`
因此多了 `view`（同一批物理页的内核指针）和 `image_base`。二是 `r_offset` 是链接期
地址，交给 `eftl::elf::MappedMemory` 按 `LoadRegion` 翻译，**不能**再自己加一次 bias，
否则双重计数、写到镜像外面去。

被否掉的方案：把 `ours::phys::ElfImage::relocate_and_bind()` 直接搬来用。它的内存
模型不对——phys 那边镜像就躺在一段物理内存里，写它就是写它；内核这边镜像是映射进
另一个地址空间的，写要走自己的物理映射视图。共用的是 eftl 和思路，不是那段代码。

**参照**
`Zircon`：程序链接 `libzircon.so` 并直接调用 `zx_*`，userboot 用 elfldltl
做重定位（`zircon/kernel/lib/userabi/userboot/`）。本项目内部的对照物更贴近：
`ours::phys::ElfImage::relocate_and_bind()` 与 `kernel/phys/elf_image.cpp` 里的
`SymbolicRelocationPass`——支持的重定位类型（1/6/7 + 8）、`st_shndx == 0` 判导入、
找不到定义就报名字失败，全部照抄它的判断，只是导出方从 `resolve_export()` 换成了
另一个 ELF 的 `.dynsym`。

**验证**
- 构建通过；clang-format/clang-tidy 干净（`_start` 的命名告警是 ELF 入口点约定，
  不能改）。
- `llvm-readelf` 看 shell 的产物形状：
  ```
  Type: DYN (Shared object file)
  NEEDED  Shared library: [libours.so]
  FLAGS   SYMBOLIC BIND_NOW
  Relocation section '.rela.dyn' contains 2 entries:
    ... R_X86_64_GLOB_DAT  ours_debug_write + 0
    ... R_X86_64_GLOB_DAT  ours_command_list + 0
  ```
- QEMU 实测两个镜像。`kernel.dev.dbg_1.obi` 打出了绑定过程本身：
  ```
  [debug] userboot: bound 'ours_debug_write' to A01300
  [debug] userboot: bound 'ours_command_list' to A01310
  ours shell: the kernel's command registry, seen from ring 3
  ```
  两个地址都落在 vDSO 的映射区间 `[A00000, A03000)` 内。文件里那两个 GOT 槽位是
  零，所以「程序跑出了完整输出」本身就是绑定成功的证据——没绑上就是调用地址 0。

**故意没做**
- 没转 `//kernel/lib/userabi/userboot`。它还是静态 PIE、还在用入口表，所以
  `VdsoEntryTable` 也还留着。下一条单独做，做完那张表就能删。
- 没做符号版本、`DT_NEEDED` 搜索、作用域规则、惰性绑定。只有一个导入方和一个导出
  方，多出来的都是还没有的问题。
- 查导出仍是线性扫描。vDSO 现在导出两个符号；`DT_GNU_HASH` 就在镜像里，等表长到
  值得用再用。
