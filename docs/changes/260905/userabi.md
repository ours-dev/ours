# kernel/lib/userabi — 改动记录

格式见 `docs/changes/TEMPLATE.md`。设计决定在 `docs/architecture/object/handle-lookup.md`。

这一支要做的是一条纵切：**打包一个极小的 userboot → 找到它 → 建进程和地址空间 →
映射 → 进 ring 3 → 一次系统调用 → 退出**。一步一验，每步单独提交。

---

## 1. 内核第一次在 BootFS 里找到 userboot（2026-09-05）

**文件**
- `kernel/lib/userabi/userboot.cpp` — `userboot_init()` 从空壳改成真的去定位 userboot。
- `kernel/lib/userabi/BUILD.gn` — 补上 handoff / mm / logz4 依赖。
- `kernel/top/main.cpp` — 在 `labour_routine()` 里接上调用。

**症状**

`userboot_init()` **没有任何调用者**，所以链接器整个丢掉了它——这也是为什么它里面调用
一个只有声明、没有定义的 `ProcessDispatcher::create` 却从来不报链接错误。打包这一侧
（`user-x86` 工具链、`kernel_elf_binary("userboot")`、`common_package_items`）早就好了，
内核这一侧一行没有。

**原因**

不是缺陷，是没写。但有两个不明显的坑，值得写下来：

其一，**文件名不能从内核自己的名字推出来**。vmours 在包里是 `vmours/image.elf`，
而 userboot 是**裸的 `userboot`**：

```gn
kernel_elf_binary("userboot") {
  has_patches = false   # ← 就是这一行造成的差别
  sources = [ "userboot.cpp" ]
}
```

`kernel_elf_image()` 对「没有 code patches」的二进制装成一个裸文件，而不是一个 `<name>/`
子树。两者在同一个 BootFS 里形状不同。

其二，**解压缓冲不能用 `ktl::Vec<u8>`**：

```cpp
// 错的做法
ktl::Vec<u8> buffer;
buffer.reserve(memory_size);   // memory_size 是兆字节级
```

ktl 的分配器是 kmalloc，最大尺寸档 4 KiB，超额请求**返回 nullptr 而不是回退到按页分配**；
`Vec` 随后在 nullptr 上构造元素，`reserve()` 里就页故障。`kernel/commands/prof.cpp`
的 `FdeTable` 正是被这个坑过（注释还在）。解压后的 BootFS 装着整个内核 ELF，必然超。

**改法**

按帧分配、经 physmap 读：

```cpp
// 大小只有 OBI item 头知道：memory_size 是「解压后」的长度。
// BootObi::kernel_package_item() 存在的理由就是这个两段式。
auto const bytes = static_cast<usize>(item.unwrap().memory_size());

out.nr_frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
out.base = mm::get_frames_n<u8>(mm::GAF_KERNEL, out.nr_frames);

auto result = boot::kernel_package({out.base, bytes});
```

这些帧**不释放**：包里每个文件的字节都是**指进这块缓冲的视图**，丢掉它就全成悬垂。

另外特意不直接 `find("userboot")`，而是先把整包列一遍：名字是 GN 打包规则定的，
一旦装的和找的对不上，`find()` 只会答 `NotFound`，完全不告诉你**里面到底有什么**。

**参照**

`Zircon`：userboot 同样是 `STORAGE_KERNEL` BootFS 里的一个文件，而不是独立的 OBI item
（`kernel_package("zircon")`）。

**验证**

QEMU dbg 实测，内核第一次看见了整个包：

```
userboot: kernel package holds 4 files
userboot:   userboot (776 bytes)
userboot:   vmours/code-patches (0 bytes)
userboot:   vmours/image.elf (487704 bytes)
userboot:   physboot (82760 bytes)
userboot: found 'userboot', 776 bytes of ELF
```

**故意没做**

- 没有建 VMO、进程、地址空间，没有映射，没有进 ring 3。这一步只证明「字节拿得到」。
- 那块解压缓冲现在**永不释放**。等 ELF 装进 VMO 之后应当把它还回去，已记进待修。
