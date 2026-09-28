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
