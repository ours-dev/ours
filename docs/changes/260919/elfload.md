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
