## vDSO 只装载一次，所有进程映射同一个 VMO（K16，2026-09-28）

`docs/design/260924/userspace-filesystem.md` 的 K16。

**文件**
- `kernel/lib/elfload/include/elfload/loader.hpp`、`kernel/lib/elfload/src/loader.cpp`：
  - 新增 `ImageVmo`；
  - `Loader::map(bytes, ...)` 拆成两步：`Loader::load()`（静态函数，只装进 VMO）和 `Loader::map(ImageVmo &, ...)`（只做映射）；
  - 原来的 `map(bytes, ...)` 保留，改为依次调用这两步。
- `kernel/lib/userabi/src/userboot.cpp`：
  - 新增 `SharedVdso` 和 `lay_out_vdso()`，在 `userboot_init()` 里调用一次；
  - `load_vdso()` 改为 `map_vdso()`，映射共享的 VMO；
  - `launch()` 不再从包里读取 vDSO 文件并重新装载。
- `docs/design/260924/userspace-filesystem.md`：修订 K16 行和 §4.6 的"vDSO 一致性"
- `.agents/skills/ours-kernel-design/references/layering.md`：记下"vDSO 由启动方映射"的决定

**症状**
每启动一个进程，`launch()` 都调用 `Loader::map(vdso_bytes, ...)`。它新建一个 VMO，
把 vDSO 的 ELF 重新拷贝进去，于是每个进程都有一份私有的 vDSO 页。dbg_1 一次启动会起 14 个进程，
就是 14 份拷贝，每份 3 页。

**原因**
`Loader::map()` 把两件事捆在了一起："把 ELF 装进 VMO"和"把 VMO 映射进某个地址空间"。
对于程序镜像，这两件事本来就是一对一的；但 vDSO 是所有进程共用的同一个镜像，只该装载一次。
VMO 和映射之所以是两个对象，本来就是为了让同一块内存能被映射多次，`loader.cpp` 里自己的注释也这么说，
可调用方式却没有用上这一点。

之前没法这么做，还有一个原因：多个地址空间同时对同一个 VMO 缺页时，VMO 的页树没有锁保护
（`260925/mm.md` 原第 1 条）。这一条在 `3a2cb2e4`、`87326a46`、`3a8b5960` 里已经修掉。

**改法**

```cpp
// elfload
struct ImageVmo {
    ours::object::KernelHandle<ours::object::VmoDispatcher> vmo;
    usize span;
    ours::VirtAddr image_base;
    ours::VirtAddr entry_offset;
};
static auto Loader::load(Bytes file, char const *name, ImageVmo &out) -> Status;   // 不需要地址空间
auto Loader::map(ImageVmo &image, VmPlacement where, MmuFlags flags, char const *name, Image &out) const -> Status;

// userabi
SharedVdso shared_vdso;                      // 在 userboot_init() 里用 lay_out_vdso() 装载一次
status = map_vdso(loader, vdso);             // launch()：映射同一个 VMO
auto const binder = elfload::Binder::from(shared_vdso.file, vdso);
```

**映射由谁来做**：按主人的要求，照 Zircon 的形状。内核只负责装载一次；映射交给启动进程的那一方：
今天是内核里的 `userabi::launch()`，K23 之后是用户态 launcher。`ProcessDispatcher::create` 不碰 vDSO。
设计文档里 K16 原本写的是"由 `ProcessDispatcher::create` 映射"，已同步修订。

`ImageVmo` 的 `map()` 参数不是 const：映射要给 VMO 加一个引用，而 `ustl::Arc` 会把 const 一路传到它指向的对象上。
`SharedVdso` 没有放进 `ustl::Option`：Option 要求元素的移动操作是 `noexcept`，而 `KernelHandle` 的移动不是。
改为用 VMO 句柄是否为空来判断是否已经装载。

**参照**
Zircon：
- `VDso::Create()`（`zircon/kernel/lib/userabi/vdso.cc`）只执行一次，结果保存在 `instance_`；
- `userboot.cc` 的 `MapHandoffElf()` 把 vDSO 映射进 userboot，并把 vDSO VMO 的句柄交给它；
- 之后的进程由用户态（process_builder）映射同一个 VMO。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测，无异常：
  - dev：`usercopy.test` 18/18、`pmnode.test` 20/20，`init: bootfs has 5 entries`；
  - dbg_1：`usercopy.test` 18/18，14 个进程都打出了 `userboot: vdso mapped`。
- 新增一行日志：`userabi: vdso laid out once, 3 page(s), shared by every process`。
- 临时探针：在每次 `launch()` 映射时打印 vDSO VMO 的地址。userboot、shell 和 12 个 fault_probe 打出的都是同一个
  `0xffffff800010c100`。探针已撤掉。
- 共享之后，任何一个进程都不能改写 vDSO：dbg_1 的 `pf_write_vdso` 往 `0xa00000` 写，
  `error_code=0x7`，进程以 FatalPageFault 终止，shell 打出 `probe pf_write_vdso killed`。

**故意没做**
- 没有像 Zircon 那样把 vDSO 拆成"只读段"和"代码段"两个映射。今天仍然是一个 R+X 映射，和改动前一样。
- 没有把 vDSO VMO 的句柄交给第一个进程。这是 K23 用户态 launcher 自己映射 vDSO 时才需要的，届时再做。

## vmex：内核铸造 System 资源槽位，交给第一个进程，userboot 再转交（K18，2026-09-28）

`docs/design/260924/userspace-filesystem.md` 的 K18。

**文件**
- `system/public/ours/syscall/resource.hpp`：`RSRC_SYSTEM_VMEX_BASE = 1`、`RSRC_SYSTEM_COUNT = 2`
- `kernel/object/include/ours/object/resource_dispatcher.hpp`、`resource_dispatcher.cpp`：重写 `create()`；新增 `kind()`、`base()`、`size()`
- `kernel/lib/userabi/src/resource.hpp`（新）、`resource.cpp`：`make_system_resource_handle()`，取代原来的空函数 `make_resource_handle()`
- `kernel/lib/userabi/src/userboot.cpp`：`make_bootstrap()` 多放一个 `SystemResource` 句柄
- `kernel/lib/userabi/userboot/userboot.cpp`：userboot 把 vmex 原样转交给下一个程序
- `system/service/init/init.cpp`、`examples/shell/shell.cpp`：按槽位取出 vmex 并报告
- `docs/design/260924/userspace-filesystem.md`：K18 标为已落地

**症状**
- `ResourceDispatcher::create()` 没有任何一种调用能通过：它先拒绝 size 为 0，接着又要求 Root 种类的 size 必须为 0；
  而且无论如何都不构造对象，直接返回 Ok，让调用方以为 `out` 里已经有了句柄。
- `userabi::make_resource_handle()` 是空函数。
- processargs 里预留的 `SystemResource = 0x54` 从来没有发过。

没有 vmex，K19 的 `vmo_replace_as_executable` 就无从校验。按设计 D3，"可执行"是一项能力，
只有持有 vmex 的进程才能把 VMO 换成可执行的。

**原因**
资源对象只写了一个框架，内容从来没补上。

**改法**

```cpp
auto ResourceDispatcher::create(...) -> Status {
    if (kind != Kind::System) { return Status::NotSupported; }
    if (size != 1 || base >= RSRC_SYSTEM_COUNT) { return Status::InvArgs; }
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(kind, base, size, flags, name));
    rights = default_right();
    *out = KernelHandle<Self>(ustl::move(self));
}

// make_bootstrap()：根 job、BootFS 之后
handles[count] = make_system_resource_handle(RSRC_SYSTEM_VMEX_BASE, "vmex");
infos[count++] = handle_info(HandleType::SystemResource, u16(RSRC_SYSTEM_VMEX_BASE));
```

- **范围按 M1 收窄**：只支持 System 种类、一次一个槽位。其它种类回 NotSupported。
  Zircon 在 `Storage` 里为 MMIO、IRQ、I/O 端口做区间重叠检查，这里等真正需要这几类时再做，所以 `storage` 参数先收下不用。
- **槽位编号照搬 Zircon**：`ZX_RSRC_SYSTEM_VMEX_BASE = 1`，0 号（Zircon 的 hypervisor 槽）空着，不重新编号。
  这样从 Zircon 移植过来的代码，问的还是同一个槽位。
- **handle_info 的参数填槽位编号**：和 Zircon 的 userboot 给 system resource 打标签的方式一样。
  接收方按 `SystemResource` 加槽位号取句柄。
- **传递链照设计 §4.6**：内核只交给第一个进程 userboot；userboot 原样转交给 next（dev 是 init，dbg_1 是 shell）；
  init 自己留着，不再外发。所以 dev 下由 init 启动的 shell 拿不到 vmex，这是预期行为。

**参照**
- Zircon `ResourceDispatcher::Create()`：对 `ZX_RSRC_KIND_SYSTEM` 同样只接受槽位表内的 base、大小为 1；
- Zircon userboot 用 `get_resource_handle()` 铸造 system resource，放进 bootstrap 消息；
- 槽位常量见 `zircon/system/public/zircon/syscalls/resource.h`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：
  - dev：`init: vmex resource ok`；init 起的 shell 打出 `shell: started without vmex`；
  - dbg_1：userboot 起的 shell 打出 `shell: vmex resource ok`；
  - 两个镜像的 shell 检查都没有 FAILED，`usercopy.test` 都是 18/18；句柄配额测试仍是 512 个 channel 后触顶，
    因为 vmex 不是 shell 自己创建的，不计入它的配额（K11 的记法）。
- clang-tidy：新文件无告警；`resource_dispatcher.hpp` 里有两条原本就有的告警。
- 反向检查（没有 vmex 时被拒绝、传错类型的句柄回 BadType）要等 K19 有了 `vmo_replace_as_executable` 才能做。

**故意没做**
- 没有资源相关的系统调用（Zircon 的 `resource_create`）。按设计，M1 里只有内核铸造的这一个槽位。
- userboot 还不能用 vmex，K19 之后它才会先复制 BootFS，再把其中一份换成可执行的（U8）。

## vDSO 支持 7 个参数的桩（G2，2026-09-28）

**文件**
- `kernel/lib/userabi/vdso/vdso.S`：`ours_syscall` 宏加 `nargs == 7` 分支，注释里的栈布局图补上 7 参数的情形
- `build/abi/gen_syscalls.py`：删掉 `UNSUPPORTED_ARG_COUNTS` 和它的检查
- `system/public/ours/syscalls.gni`：`args` 的说明去掉“不能正好 7 个”
- `docs/design/260924/userspace-filesystem.md`：G2 标为已落地

**症状**
`vmar_map(vmar, options, vmar_offset, vmo, vmo_offset, length, *mapped_addr)` 有 7 个参数，
生成器拒绝这种描述，vDSO 宏也没有对应的桩。

**原因**
缺功能。当初故意留空：写一个没有调用者的桩，没有东西能验证它。

**改法**

```asm
.elseif \nargs == 7
    push %r12              // 调用方的 %r12 是 callee-saved，要还
    mov %rcx, %r10         // 第 4 个参数：SYSCALL 会覆盖 %rcx
    mov 16(%rsp), %r12     // push 之后：0 = 旧 %r12，8 = 返回地址，16 = 第 7 个参数
...
.elseif \nargs == 7
    pop %r12
```

- 只保存、装入 `%r12`。`%r13` 不携带参数，内核既不读它也不改它（syscall 帧保存后原样恢复），
  所以桩不欠调用方什么。
- 偏移 16（0x10）与 Zircon 的 `m_syscall` 宏对 7 个参数用的偏移相同。

**参照**
Zircon `kernel/lib/userabi/vdso/syscalls-x86.S` 的 `m_syscall` 宏：7 个参数时 `push_reg %r12`，
从 `0x10(%rsp)` 取第 7 个参数。

**验证**
- 两个镜像均构建通过。改动前后的 `vdso.image.elf` 逐字节相同（`cmp`）：现在没有 7 个参数的调用，
  已有桩一字节不变。
- 把宏单独汇编，实例化一个 7 参数、一个 8 参数的桩，用 `llvm-objdump` 反汇编：
  7 参数桩是 `push %r12; mov %rcx,%r10; mov 0x10(%rsp),%r12; mov $n,%eax; syscall; pop %r12; ret`，
  8 参数桩和改动前一致。
- `boot_check.py` 两个镜像都 PASS（dbg 12 passed；rel 11 passed、4 skipped）。

**故意没做**
- 7 参数桩在真实调用下的验证，留给 K21 的 `vmar_map`：它的第 7 个参数是输出地址，
  取错了，shell 的映射检查就会失败。

## 先建进程、后写消息：VmaRoot 进唯一那条 processargs，删掉临时的补发通道（2026-09-28）

**文件**
- `kernel/lib/userabi/src/userboot.cpp`、`include/userabi/userboot.hpp`：`launch()` 多一个出参，把子进程的根 VMA 句柄交回调用方；
  删掉 `deliver_root_vma()`；`make_bootstrap()` 拆成 `make_bootstrap_channel()` 和 `write_bootstrap()`，`userboot_init()` 先 launch、后写消息
- `kernel/object/include/ours/object/channel_dispatcher.hpp`、`channel_dispatcher.cpp`：删掉 `deliver_first()`
- `kernel/lib/syscall/src/process.cpp`、`system/public/ours/syscalls.gni`、`system/lib/ou/include/ou/process.hpp`、`src/process.cpp`：
  `process_spawn_boot` 多一个出参 `root_vma`
- `system/lib/runtime/src/start.cpp`：回到只读一条消息（还原到 K21b 之前的版本，再改一段注释）
- 三个用户态启动者改成先 spawn、后写消息：`kernel/lib/userabi/userboot/userboot.cpp` 的 `start_next()`、
  `system/service/init/init.cpp`（新增 `spawn_then_write()`，`start_autorun()`、`start_shell()` 用它）、`examples/shell/shell.cpp` 的 `run_probe()`
- `docs/design/260924/userspace-filesystem.md`：K21、K23 行

**症状**
K21b 为了让进程拿到根 VMA，加了一条临时通道：`launch()` 往子进程的 bootstrap channel 队首
（`deliver_first()`）塞一条只带 VmaRoot 的消息，runtime 读两条。用户问：为什么要先写消息、再建进程，这合理吗？

**原因**
不合理，这是临时的 `process_spawn_boot` 带出来的顺序：它一次做完建进程、装程序、启动线程，而且什么都不返回，
启动者只能事先把消息备好；内核启动 userboot 也沿用了同一个 `launch()`，同样先写消息。
可 VmaRoot 这类句柄只有进程建好之后才存在，“先写消息”的顺序注定放不进它，于是才要内核再补一条。
正确的顺序是 launcher 的顺序（Zircon `process_builder`）：先 `process_create` 拿到进程和根 VMAR，再写消息、再启动。

**改法**
在 K23 之前，“建进程”这一步仍在内核里，但顺序已经和最终方案一致：

```cpp
// 用户态启动者（init、userboot、shell）
channel_create(&here, &there);
process_spawn_boot(job, name, there, &root_vma);             // 先建进程，拿回根 VMA
channel_write(here, message, length, {..., root_vma}, n);     // 再写唯一那条消息，VmaRoot 在里面
handle_close(here);

// 内核启动 userboot
auto bootstrap = make_bootstrap_channel(kernel_end);
launch(root_job, program, ustl::move(bootstrap), &root_vma);  // 先建进程
write_bootstrap(*kernel_end.dispatcher(), program, ustl::move(root_vma));
```

几处取舍：
- **消息晚于线程启动，没关系**：runtime 本来就先等 READABLE 或 PEER_CLOSED 再读（照 Zircon 的
  `ld::ProcessargsBuffer`）。启动者如果写失败或根本没写，关掉自己那端，子进程看到 PEER_CLOSED、读不到消息，以 `EXIT_START_FAILED` 退出。
- **句柄的记账**：`process_spawn_boot` 在调用者的系统调用里建根 VMA 句柄，按 K11 记在调用者名下，
  随后随消息转移给子进程。值先写给调用者再入表（设计 4.1）；写不出去时调用者拿到错误，子进程已在运行，
  但等不到消息，会随调用者关掉那端而退出。
- **根 VMA 句柄在线程启动前建好**：建不出来（配额满）就让 launch 失败，而不是留下一个没人能把区域交给它的进程。
- 与 K21b 的做法相比，少了一个只为过渡而存在的内核接口（`deliver_first`）和 runtime 里“读两条、谁先谁后”的约定。

**参照**
- Zircon `process_builder`（`ProcessBuilder::Start`）：`zx_process_create` → 装段 → 写 processargs（含 `PA_VMAR_ROOT`）→ `zx_process_start`。
- Zircon 内核启动 userboot（`kernel/lib/userabi/userboot.cc`）：先建进程和根 VMAR，再往 bootstrap channel 写消息。

**验证**
- 两个镜像均构建通过；`boot_check.py` 两个镜像都 PASS（dbg 14 passed；rel 13 passed、5 skipped）。
- 启动链上每一对都走了新顺序：内核 → userboot → init（dev）/ shell（dbg_1），init → shell、autorun，shell → 各 fault_probe；
  shell 的 `vma_map checks passed`、`vma_allocate checks passed` 照旧（VmaRoot 现在来自启动者写的那条消息），探针全部照常。

**故意没做**
- `process_spawn_boot` 的 `root_vma` 出参随 K25 删掉整个调用时一起消失，不单独登记。

**怎么发现的**
用户追问“为什么先写消息再创建进程”，回头看顺序才发现临时通道只是在弥补一个错误的顺序。

**用户的预测**
无。

**你应该能回答的三个问题**
- 为什么“先写消息、后建进程”的顺序放不进 VmaRoot？哪些句柄也有同样的问题？
- 子进程的线程如果先于消息开始运行，runtime 为什么不会读空？启动者写失败时子进程会怎样？
- 根 VMA 句柄为什么要在子进程线程启动之前建好，而不是之后？
