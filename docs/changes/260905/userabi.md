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

---

## 2. 用户地址空间的基址落在内核半区（2026-09-05）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp` — `USER_ASPACE_BASE` /
  `USER_ASPACE_SIZE` / `USER_ASPACE_RESTRICTED_SIZE`。

**症状**

潜伏，但**只要进 ring 3 就必炸**。当前触发不到，因为还没有任何东西进过用户态。

**原因**

```cpp
#define KERNEL_ASPACE_BASE 0xffffff8000000000UL
...
#define USER_ASPACE_BASE 0xffffff8000000000UL   // ← 和上面一字不差
#define USER_ASPACE_SIZE 0x0000008000000000UL
#define USER_ASPACE_RESTRICTED_SIZE 0
```

x86-64 把 canonical 地址劈成两半：63..47 位必须全等于第 47 位，于是地址要么在
`0x0000'0000'0000'0000`–`0x0000'7fff'ffff'ffff`（用户半区），要么在
`0xffff'8000'0000'0000`–`0xffff'ffff'ffff'ffff`（内核半区）。用户映射**不可能**在分界之上：
ring 3 访问那样的地址，**无论页表怎么写**都要出错。

而这里的 `USER_ASPACE_BASE` 正是 `KERNEL_ASPACE_BASE`，也就是 physmap 的起点。

`USER_ASPACE_RESTRICTED_SIZE` 是 0 也是错的：`process_dispatcher.cpp` 里
`PRIVATE_ASPACE_SIZE = USER_ASPACE_RESTRICTED_SIZE`，于是「私有地址范围」是零字节。

**改法**

```cpp
#define USER_ASPACE_BASE MB(2)
#define USER_ASPACE_SIZE ((1UL << 47) - PAGE_SIZE - USER_ASPACE_BASE)
#define USER_ASPACE_RESTRICTED_SIZE ((1UL << 46) - USER_ASPACE_BASE)
```

基址取 2 MiB 而不是 0：让空指针、以及从空指针加一个小偏移得到的地址，都落进一个
**永远不会被映射的洞**里——和 Zircon 的 `USER_ASPACE_BASE` 同一个理由、同一个数字。
大小停在 2^47 分界**下面一页**：最顶上的映射再往后一个地址就非 canonical 了，留一页出来，
边界算术才不用到处特判。

**参照**

`Zircon arch/x86/include/arch/kernel_aspace.h`：

```c
#define USER_ASPACE_BASE 0x0000000000200000UL  // 2MB
#define USER_ASPACE_SIZE ((1ULL << 47) - 4096 - USER_ASPACE_BASE)
#define USER_RESTRICTED_ASPACE_SIZE ((1ULL << 46) - USER_ASPACE_BASE)
```

**验证**

构建通过；QEMU dbg 启动无回归（无断言、52.9 万行日志、QEMU 存活）。**真正的验证要等到
有东西进 ring 3**，那时这三个常量才第一次被用上——现在只能说它不再自相矛盾。
