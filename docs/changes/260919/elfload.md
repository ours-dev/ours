# elfload 改动记录（2026-09-19）

## 绑定从 userboot 里搬出来，成为 `//kernel/lib/elfload` 的 Binder（2026-09-19）

**文件**
- `kernel/lib/elfload/` —— 新建库（copier 生成骨架）。`include/elfload/image.hpp`
  是两半共享的镜像描述；`include/elfload/binder.hpp` + `src/binder.cpp` 是
  `elfload::Binder`。
- `kernel/lib/userabi/src/userboot.cpp` —— 删掉 `bind_imports()`、`find_export()`
  和那四个 `R_X86_64_*` 常量，`MappedImage` 换成 `elfload::Image`，改为
  `Binder::from(vdso...).bind(program...)`。
- `kernel/lib/userabi/BUILD.gn` —— 依赖从 `//kernel/lib/eftl` 换成
  `//kernel/lib/elfload`。

**症状**
潜伏。`userboot.cpp` 同时是两样东西：一条 bring-up 路径（挑程序、建进程、映射、
起线程）和一个 ELF 动态加载器（解析、重定位、按名字绑定）。后者跟 userboot 没有
必然关系——将来 `sys_process_create` 要的是同一套代码，而它不会想把 userboot 也
一起拖进来。

**原因**
这是层次错位，不是文件太长。判据很简单：**这段代码需要知道什么**。绑定需要的是
一段可写内存和两个 ELF 文件；它不需要知道进程、地址空间、VMO、内核包，也不需要
知道「第一个用户程序」这个概念存在。一段依赖比它所在的模块少得多的代码，说明它
不属于那个模块。

phys 那边早就是分开的，可以直接对照：`ours::phys::ElfImage` 负责解析/放置/重定位/
绑定，`ModuleLoader` 只是挑模块、驱动它、然后调用它的入口。用户态这边缺的正是
`ElfImage` 那一层。

顺带一个具体收益：绑定这半边完全不碰对象层，可以在宿主上单测。重定位算术
（bias 加几次、`r_offset` 是链接期地址还是运行期地址）是最容易错、又最难靠启动
日志定位的东西——现在它可以用一段 buffer 加两个 ELF 文件直接测。

**改法**
按「加载器 + 绑定器」两个组件拆，本条先做绑定器。

改前（`userboot.cpp` 里的自由函数，约 130 行）：
```cpp
auto bind_imports(ustl::views::Span<u8 const> program_file, MappedImage const &program,
                  ustl::views::Span<u8 const> vdso_file, MappedImage const &vdso) -> Status {
    // 解析两个 ELF、取符号表、建 MappedMemory、逐条重定位……
}
...
if (Status::Ok != bind_imports(image.unwrap(), loaded.image, vdso_bytes.unwrap(), vdso)) {
    return;
}
```

改后（`userboot.cpp` 只剩调用）：
```cpp
auto const binder = elfload::Binder::from(vdso_bytes.unwrap(), vdso);
if (binder.is_err()) {
    log::error("userboot: the vDSO cannot be bound against: {}", to_string(binder.unwrap_err()));
    return;
}
if (Status::Ok != binder.unwrap().bind(image.unwrap(), loaded.image)) {
    return;
}
```

导出方在构造时固定，因为这才是它的语义：一个「针对 vDSO 的绑定器」。同一个进程里
每个程序都绑向同一个 vDSO，「这个镜像是不是动态对象、有没有符号表」这个问题应该
只回答一次，而不是每来一个导入方就重问一遍。

`elfload::Image` 是两半唯一共享的类型：base / span / entry / **view**（加载者自己
那份可写视图）/ image_base。`view` 这个字段是整件事的关键，注释里写清了为什么：
内核在内核上下文里修正一个用户程序，按用户虚拟地址去写会打到当前地址空间的同名
地址上。

被否掉的方案：只做成一个 `elfload::ElfImage`（像 phys 那样一个类管到底）。否掉是
因为那样绑定就又跟对象层绑在一起了——`map()` 需要 VmaDispatcher，于是整个类都不能
在宿主上测。拆成两个类，Binder 那一半的依赖是零。

**参照**
`ours::phys::ElfImage::relocate_and_bind()` 与 `kernel/phys/elf_image.cpp` 的
`SymbolicRelocationPass`：支持的重定位类型、`st_shndx == 0` 判导入、解析不到就报
名字失败，判断逻辑一致。差别在导出方：phys 是 `resolve_export()` 查生成的导出表，
这里是查另一个 ELF 的 `.dynsym`。
`Zircon` 对应的是 elfldltl 加 userboot 自己的加载代码——那边的加载器跑在 ring 3，
分层动机相同（加载逻辑不属于「第一个程序」这个概念）。

**验证**
- 构建通过；clang-format / clang-tidy 干净。
- QEMU 实测两个镜像，行为与搬动前一致，日志前缀从 `userboot:` 变成 `elfload:`：
  ```
  [debug] elfload: bound 'ours_debug_write' to A012F0
  [debug] elfload: bound 'ours_command_list' to A01300
  ours shell: the kernel's command registry, seen from ring 3
  --- end ---
  ```

**故意没做**
- 加载器还没搬。`map_elf_image()` 和那两个手写的 `Elf64Header`/`Elf64ProgramHeader`
  仍在 `userboot.cpp` 里，下一条做。
- 宿主单测还没写。骨架里的 `test/` 目录先删掉了，等测试跟着代码一起进来，而不是
  留一个指向不存在文件的 BUILD.gn。

## 放置也搬过来，成为 `elfload::Loader`（2026-09-19）

**文件**
- `kernel/lib/elfload/include/elfload/loader.hpp` + `src/loader.cpp` —— 新建。
  `Elf64Header`/`Elf64ProgramHeader`/`PT_LOAD` 和原来的 `map_elf_image()` 整体搬来，
  成为 `elfload::Loader::map()`。
- `kernel/lib/userabi/src/userboot.cpp` —— 少了 190 行；`load_program()` 和
  `load_vdso()` 改成调 `loader.map(...)`。
- `kernel/lib/elfload/BUILD.gn` —— 加 `//kernel/mm`、`//kernel/object` 依赖。
- `docs/repairs/260919/elfload.md` —— 「三个段当成一段映射」那条从 userabi 挪过来，
  因为代码挪过来了。

**症状**
无。承接上一条：绑定已经是组件了，放置还留在 bring-up 路径里，两半各在一处。

**原因**
同一个判据：这段代码需要知道什么。放置需要知道地址空间、VMO、VMA、帧分配器——它需要
对象层；但它**不**需要知道「第一个用户程序」「内核包」「启动选项」。userboot.cpp 里
剩下的才是真正属于 bring-up 的东西：从包里挑哪个程序、建进程、栈放哪、起线程。

`userboot.cpp` 从 659 行降到 469 行，降的正好是「怎么把一个 ELF 放进内存」。

**改法**
`Loader` 由根 VMA 构造，因为它的语义就是「某个地址空间的加载器」：

```cpp
// userboot_init()
elfload::Loader const loader{*root_vma.dispatcher()};
load_program(image.unwrap(), program.text, loader, *root_vma.dispatcher(), loaded);
load_vdso(vdso_bytes.unwrap(), loader, vdso);
```

`load_program()` 仍然额外拿着 `root_vma`，不是遗漏：**栈**是它自己映射的
（`create_paged` + `root_vma.map`）。栈放在哪、多大、要不要可执行，是 bring-up 的
决定，不是加载 ELF 的一部分——Loader 只管把文件里的段放进去。

和 Binder 的构造方式是对称的：一个「针对这个地址空间的加载器」，一个「针对这个导出
方的绑定器」，都把只需回答一次的问题在构造时回答掉。

**参照**
`ours::phys::ElfImage::load_at()`（`kernel/phys/elf_image.cpp`）：同样是「先算出
镜像占多大、再把每段拷进去」，区别是 phys 直接写物理内存，这里要经对象层建 VMO 再
映射。`ModuleLoader::load()` 与这里的 `userboot_init()` 位置相同——挑镜像、驱动
加载器、然后运行它。

**验证**
- 构建通过；clang-format / clang-tidy 干净。
- QEMU 实测两个镜像，行为不变：
  ```
  [info]  userboot: found 'shell', 1608 bytes of ELF
  [info]  userboot: vdso mapped [A00000, A03000)
  [debug] elfload: bound 'ours_debug_write' to A012F0
  --- end ---
  ```
- 行数：`userboot.cpp` 469，`elfload/src/loader.cpp` 175，`binder.cpp` 198。

**故意没做**
- 放置仍然用手写的 `Elf64Header`，没换成 eftl 的 `LoadPolicy::make_plan()`。这一步
  只搬位置，不改实现——换 eftl 是和「按段权限映射」同一件事，一起做才有意义，记在
  `docs/repairs/260919/elfload.md`。
- 宿主单测仍然没写。
## 绑定器拒绝它不会应用的重定位格式

### 症状

没有症状——这正是问题。`Binder::bind()` 只处理 `DT_RELA` 和 `DT_JMPREL`，
对 `DT_REL` 和 `DT_RELR` 一声不吭地跳过，然后返回 `Status::Ok`。

### 为什么这是个错

这是 `a51f4cd` 那个 bug 换了个标签重演。那次是 eftl 读不出一张 PLT 重定位表
（缺 `DT_RELAENT`），绑定器把"读不出来"当成了"不存在"，GOT 保留链接期的填充值，
第一次经 GOT 的调用就在 ring 3 跳到了没人写过的地址。整段跳过一种格式的结局完全
一样：镜像能加载，绑定"成功"，然后带着链接期指针运行。

触发它不需要什么特殊构造，`-Wl,-z,pack-relative-relocs` 就够了——它把每一条
`R_X86_64_RELATIVE` 从 `.rela.dyn` 搬进 `.relr.dyn`，`DT_RELA` 随之消失。

glibc 的 `elf_dynamic_do_Rel()` 是按镜像声明了哪一种就走哪一种的；在本绑定器
做到这一点之前，它至少要拒绝自己应用不了的东西。

### 改动

`kernel/lib/elfload/src/binder.cpp`，接在已有的两处"声明了但读不出来"检查之后：

改前——两种格式静默跳过：

```cpp
    if (plt_relas.is_none() && declares(dynamic.value(), eftl::elf::DynTag::JmpRel)) {
        log::error("elfload: the importer declares DT_JMPREL but it cannot be read");
        return Status::InvArgs;
    }

    if (relas.is_none() && plt_relas.is_none()) {
        return Status::Ok;
    }
```

改后：

```cpp
    if (declares(dynamic.value(), eftl::elf::DynTag::Rel)) {
        log::error("elfload: the importer declares DT_REL, which this binder does not apply");
        return Status::Unsupported;
    }
    if (declares(dynamic.value(), eftl::elf::DynTag::Relr)) {
        log::error("elfload: the importer declares DT_RELR, which this binder does not apply");
        return Status::Unsupported;
    }

    if (relas.is_none() && plt_relas.is_none()) {
        return Status::Ok;
    }
```

### 验证

先试了给 shell 加 `-Wl,-z,pack-relative-relocs`：标志确实进了链接命令，但 shell
一条 `R_X86_64_RELATIVE` 都没有，没东西可打包，`DT_RELR` 也就没出现——这条路
证不了任何东西。

改为直接改产物：把 `shell.image.elf` 动态数组里那条绑定器从不读的 `DT_DEBUG`
（偏移 `0x5f0`）改写成 `DT_RELR`，重新打包启动：

```
[3542545000 cpu0 error] elfload: the importer declares DT_RELR, which this binder does not apply
```

并且这一行之后再没有任何 `bound '...' to ...`，说明是在绑定开始前就退出的。
删掉产物重新链接后，两个镜像都正常绑定 `ours_debug_write` / `ours_command_list`。
