# userabi 改动记录（2026-09-24）

## 启动程序前先确认它是个程序：拒绝 vDSO、拒绝入口不在可执行段的镜像（2026-09-24）

**文件**
- `kernel/lib/elfload/include/elfload/program.hpp`、`kernel/lib/elfload/src/program.cpp` — 新增 `elfload::check_program()`
- `kernel/lib/elfload/BUILD.gn` — 挂上新文件
- `kernel/lib/userabi/src/userboot.cpp` — `launch()` 在创建进程之前按名字拒绝 vDSO，再调 `check_program()`

**症状**
潜伏，但任何用户程序一个 syscall 就能触发：`ours_process_spawn_boot(job, "vdso", ...)`
会把 vDSO 当程序装载并启动。vDSO 的 `e_entry` 是 0，装载器把它换算成映像基址，
于是新线程从 ELF 头的第一个字节开始执行。在 P1-A 的"用户故障杀进程"做完之前，
ring 3 的 #UD/#GP 没有被收住的路径，结果是内核出事；即便收住了，也是"调用成功、
进程第一条指令就死"，调用者拿到的是一个误导性的 Ok。

包里其它不是程序的文件（将来的数据文件、损坏的 ELF）同理：`Loader::map()` 只检查
"够不够一个 ELF 头那么大"，不看魔数、类型、机器、入口。

**原因**
这里有两个独立的缺口。

第一，**检查的时机**。`launch()` 原先的顺序是找文件 → `ProcessDispatcher::create()`
→ `Loader::map()`。装载器属于一个地址空间（它由根 VMA 构造），所以它能说
"这不是个程序"的时候，进程已经建好，要再拆掉。把"是不是程序"放进装载器，时机
永远晚一步。Linux 的 `load_elf_binary()` 正是这么划的线：所有 ELF 头检查都在
`begin_new_exec()` 之前——那之后旧映像已经没了，失败就只剩杀进程一条路。

第二，**"入口在某个 PT_LOAD 里"不等于"入口可执行"**。lld 产出的第一个 PT_LOAD
装的是 ELF 头和只读数据，权限 R。一个 `e_entry` 为 0 的 PIE（共享对象都是这样，
vDSO 也是）恰好落在它里面。eftl 的 `KernelElfLoadPolicy::make_plan()` 只查
"入口被映射了"，`BasicImage::from()` 查了"入口在可执行段"，但只对 ET_EXEC 查——
它把 ET_DYN 当成可装载模块，模块的入口写在自己的描述符里，不看 `e_entry`。而本项目
的用户程序就是 ET_DYN（PIE），于是两道检查都放行了。今天整个映像按 RWX 一段映射，
所以"不可执行"还不会在硬件层面体现出来；等按段映射（`docs/repairs/260919/elfload.md`
第 1 条）落地后，这种入口就是一次取指 #PF。

**改法**
改前（`launch()`）：

```cpp
    auto const image = package.file(program.view());
    if (image.is_err()) {
        ...
        return Status::NotFound;
    }

    log::info("userboot: found '{}', {} bytes of ELF", ...);

    object::KernelHandle<object::ProcessDispatcher> process;
    ...
    auto status = object::ProcessDispatcher::create(job, program.text, process, ...);
```

改后：

```cpp
    if (program.view() == VDSO_FILE) {
        log::error("userabi: '{}' is the vDSO, not a program", program.view());
        return Status::InvArgs;
    }

    auto const image = package.file(program.view());
    ...

    if (auto const status = elfload::check_program(image.unwrap()); Status::Ok != status) {
        log::error("userabi: '{}' is not a program this kernel can start", program.view());
        return status;
    }
```

`check_program()` 不手写 ELF 解析，全部复用 eftl：`Parser::parse_as` 查魔数与位宽，
`BasicImage::from` 查类型与段重叠，`KernelElfLoadPolicy::make_plan` 查机器、段合法性、
入口被映射；自己只补一步——在 `plan->regions()` 里找到包含入口的那个区域，要求它带
`SegPerms::Executable`，对 ET_EXEC 和 ET_DYN 一视同仁。

vDSO 按名字单独拒绝，尽管 `check_program()` 本身也会拒它（入口 0 在只读段）：
名字那条让错误日志说出真正的原因，而且不依赖"vDSO 的入口恰好不可执行"这个巧合。

被否掉的方案：
- 把检查塞进 `Loader::map()`。时机不对（见上），而且 vDSO 也走 `map()`，它的入口本来就无意义，
  `map()` 得学会区分"程序"和"vDSO"。
- 改 eftl 让 `BasicImage::from()` 对 ET_DYN 也查可执行入口。phys 装载模块依赖现在的行为，
  那是另一个调用方的语义，不该为了这里改。

`make_plan` 的映射粒度用默认的 1 字节而不是 `PAGE_SIZE`：当前整映像一段映射，两个段共页无害；
按段映射落地时这里应改成 `PAGE_SIZE`，注释里写了。

**参照**
- `Linux load_elf_binary()`（`fs/binfmt_elf.c`）：`elf_check_arch()`、`e_type` 检查都在 `begin_new_exec()` 之前。
- Zircon 的 userboot 在内核外用 `elfldltl` 装载程序，内核只负责装 userboot 自己；本项目在内核里装，
  所以把同样的前置检查放在内核里，等用户态 loader（P1-C）落地后随临时 syscall 一起退场。

**验证**
- 构建通过；两个镜像启动，init 仍启动 shell 并收到 "pong"（`check_program` 放行真正的程序）。
- 临时探针（已还原）：init 在启动 shell 之前 `process_spawn_boot(job, "vdso", HANDLE_INVALID)`，
  并临时关掉按名字拒绝那一条，让 `check_program` 自己面对 vDSO。日志：

  ```
  elfload: the entry point 0 is not in an executable segment
  userabi: 'vdso' is not a program this kernel can start
  init: probe spawn(vdso) = InvArgs
  userabi: 'shell' starts with handle 0x10000b
  init: shell says "pong"
  ```

  没有建出进程，init 照常往下走。按名字拒绝那一条本身未单独实测。

**故意没做**
- 没有在检查里限制程序的加载范围（比如要求段落在用户地址空间内）：装载器把整个映像放在
  `USER_ASPACE_BASE`，段的原始 `p_vaddr` 只决定相对布局，不决定落点。
- 没有给错误单独开一个 Status（类似 Linux 的 ENOEXEC）：沿用 `Loader::map()` 对畸形 ELF
  已经在用的 `InvArgs`，这个 syscall 是临时的，不值得为它扩 ABI。
