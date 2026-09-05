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

---

## 3. `VmasFlags::User` 的值是 0，于是每个「用户」地址空间都是内核地址空间（2026-09-05）

**文件**
- `kernel/mm/include/ours/mm/vmas_flags.hpp` — 三个 kind 各给一个独立位，其余位顺移。

**症状**

`VmArea::map_phys()` 在用户地址空间里映射 `[0x200000, 0x203000)` 被拒，`InvArgs`。
探针打出根 VMA 的范围，答案就在那里：

```
PROBE root=[FFFFFF8000000000,0) req base=200000 size=3000
```

`0xFFFFFF8000000000` 是 `KERNEL_ASPACE_BASE`，`+0x8000000000` 是 `GB(512)` = `KERNEL_ASPACE_SIZE`
（加起来正好回绕成 0）。这个「用户」地址空间，从头到尾就是个内核地址空间。

**原因**

```cpp
enum class VmasFlags : usize {
    User,            // ← 0
    Kernel,          // ← 1
    Guest,           // ← 2
    OomIgnored = 2_bit,
    ...
};
```

前三个是没写值的枚举量，于是取 0、1、2。而全树**每一处**都用按位与来测它：

```cpp
if (bool(flags & VmasFlags::User)) {          // vm_aspace.cpp:45
if (!!(VmasFlags::Kernel & m_flags)) {        // arch_vm_aspace.cpp:51
} else if (!!(VmasFlags::Guest & m_flags)) {  // arch_vm_aspace.cpp:59
```

`x & 0` 对任何 x 都是 0——**`User` 这一测永远为假**。`VmAspace::create()` 因此每次都走
else 分支，把 `KERNEL_ASPACE_BASE`/`KERNEL_ASPACE_SIZE` 装进了「用户」地址空间。

`Kernel` 和 `Guest` 碰巧能用，只因为 1 和 2 恰好也是单个位；这套编号离出错只差一个枚举量
（下一个 kind 会是 3，同时命中 Kernel 和 Guest 两个测试）。

**改法**

```cpp
User = 0_bit,
Kernel = 1_bit,
Guest = 2_bit,

OomIgnored = 3_bit,
AreaMergeable = 4_bit,
Shared = 5_bit,
Active = 6_bit,
```

全树只有 5 处引用这些枚举量，且全部用 `&` 测试，所以改编号不改任何调用点。

否掉的方案：保留 0/1/2 并加一个 `KindMask`，把测试改成相等比较。那要改动每一处调用点，
而且把「kind 是一个 2 位字段、其余是标志位」这件事留在类型里继续等着下一个人踩。

**验证**

同一探针，改动后：

```
PROBE root=[200000,7FFFFFFFF000) req base=200000 size=3000 phys=96C000
userboot: mapped [200000, 203000) into its own aspace, entry at 201270
```

---

## 4. userboot 的 ELF 装进了它自己的用户地址空间（2026-09-05）

**文件**
- `kernel/lib/userabi/userboot.cpp` — `load_image()`。

**症状**

不是缺陷，是这条纵切的下一步。

**原因**

无。

**改法**

userboot 是 PIE（`ET_DYN`），三个 PT_LOAD（R / RX / RW），跨度 0x3000，入口在文件内偏移
`0x1270`。用 `llvm-readelf -r` 确认过它**一条重定位都没有**——它不引用自身之外的任何东西——
所以装到任何页对齐的基址上都能直接跑。这就是这段代码短的原因：没有重定位处理、
没有动态链接、没有符号解析。

```cpp
// 跨度按 memsz 而不是 filesz 量：两者之差是 .bss，占地址不占文件字节。
lowest = std::min(lowest, VirtAddr(phdr.vaddr));
highest = std::max(highest, VirtAddr(phdr.vaddr + phdr.memsz));
...
// 先整体清零，再把各段的文件字节盖上去 —— .bss 的零就是这么来的：
// filesz 到 memsz 之间那段此后再没被写过。
std::memset(backing, 0, out.span);
...
std::memcpy(backing + (VirtAddr(phdr.vaddr) - lowest), bytes.data() + phdr.offset, phdr.filesz);
```

映射用 `map_phys` 而不是 `map_anon` + 拷贝：帧是刚分配的、内容已经在 physmap 里摆好了，
直接把这段物理内存映进用户地址空间，就不需要「让目标地址空间生效之后再往用户地址写」
那一套（那还要对付 SMAP）。

`MmuFlags::User` 是这里的关键一位：没有它，页是 supervisor-only，ring 3 取第一条指令就出错。

ELF 头是本地声明的，没有复用 `//kernel/lib/eftl` 或 `ours::phys::ElfImage`：eftl 是为动态
链接做的，段要穿过一层 traits 的 variant 才拿得到；`ElfImage` 的 load plan 正合用，但它在
phys 的 `physload.core` source_set 里，没链进内核。装一个静态 PIE 就是二十来行算术，
把这些行摊开看得见，比省下它们更值。

**参照**

`Zircon`：userboot 同样在内核里被装载并进入，`ProcessDispatcher` 之后才参与。

**验证**

```
userboot: mapped [200000, 203000) into its own aspace, entry at 201270
```

启动无断言、QEMU 存活。

**故意没做**

- **三段共用一套 RWX 权限**。R / RX / RW 三个段现在被当成一整段映射，所以这块内存**同时
  可写可执行**——正是 W^X 要禁止的。逐段映射是下一步该做的，已记进待修。
- 没有建线程、没有进 ring 3。这块映射目前没有任何东西会去执行。

---

## 5. 地址空间的 kind 不再是标志位，而是真正的 kind（2026-09-05）

**文件**
- `kernel/mm/include/ours/mm/vmas_flags.hpp` — 拆出 `VmAspaceKind`。
- `kernel/mm/include/ours/mm/vm_aspace.hpp` / `kernel/mm/vm_aspace.cpp` — `create()` 收 kind。
- `kernel/arch/x86/mm/{include/ours/mm/arch_vm_aspace.hpp,arch_vm_aspace.cpp}` — 同上。
- `kernel/mm/cpu_local.cpp`、`kernel/lib/userabi/userboot.cpp` — 调用点。

**症状**

第 3 条把 `User/Kernel/Guest` 从 0/1/2 改成三个独立位，症状消失了。但那只是**修好了一个
实例，留下了产生它的形状**——这一节修的是形状。

**原因**

「是哪一个地址空间」是**三选一**，而标志字表达不了这件事：`User | Kernel` 是一个合法的
`VmasFlags` 值，却没有意义，每个读它的地方都得自己发明一套规矩。而两处读它的地方，各自
发明的规矩都是**用一个 catch-all 兜底**，且方向相反：

```cpp
// vm_aspace.cpp —— 不是 User 就当内核
if (bool(flags & VmasFlags::User)) {
    base = USER_ASPACE_BASE;   size = USER_ASPACE_SIZE;
} else {
    base = KERNEL_ASPACE_BASE; size = KERNEL_ASPACE_SIZE;
}

// arch_vm_aspace.cpp —— 不是 Kernel 也不是 Guest 就当用户
if (!!(VmasFlags::Kernel & m_flags)) { ...沿用 phys 交接的内核页表... }
else if (!!(VmasFlags::Guest & m_flags)) { ...init_ept()... }
else [[likely]] { ...新建 MMU 页表并别名内核... }
```

于是一个 Guest 地址空间会**同时**拿到内核的 base/size 和一张 EPT。而 `User` 那一位为 0
的缺陷之所以能藏这么久，正是因为有这个 `else` 替它把错误答案补齐了——**兜底掩盖了缺陷**。

**改法**

kind 独立成枚举，两处都改成穷尽的 `switch`：

```cpp
enum class VmAspaceKind { User, Kernel, Guest };

enum class VmasFlags : usize {   // 只剩真正互相独立的位
    None = 0,
    OomIgnored = 0_bit,
    AreaMergeable = 1_bit,
    Shared = 2_bit,
    Active = 3_bit,
    Aslr = 31_bit,
};
```

```cpp
switch (kind) {
    case VmAspaceKind::User:   base = USER_ASPACE_BASE;   size = USER_ASPACE_SIZE;   break;
    case VmAspaceKind::Kernel: base = KERNEL_ASPACE_BASE; size = KERNEL_ASPACE_SIZE; break;
    case VmAspaceKind::Guest:
        // 客户机的范围是 hypervisor 的事，跟着它的 EPT 一起来，不该借用内核的数字。
        return Status::Unimplemented;
}
```

`is_user()` 也从 `!!(m_flags & Flags::User)` 变成 `m_kind == VmAspaceKind::User`。

收益不是「这次对了」，而是**下一次也对**：每个 kind 都被点名，加第四种时编译器会在这两处
报错，而不是让它悄悄变成内核地址空间。

**参照**

`Zircon` 拆得更开：`VmAspace::Type`（User/Kernel/GuestPhysical/Low）是枚举，
`VmAspace::Create(Type, name)` 按它取范围，标志位是另一个参数。

**验证**

构建通过；QEMU dbg 启动无回归（无断言、QEMU 存活），userboot 仍然装载到位：

```
userboot: mapped [200000, 203000) into its own aspace, entry at 201270
```

**故意没做**

- `Guest` 现在返回 `Unimplemented` 而不是继续借内核的范围。树里还没有 hypervisor，
  这条路本来就没人走；让它明确失败，好过继续给出一个错误答案。

---

## 6. 进 ring 3：四个各自潜伏的缺陷（2026-09-05）

**文件**
- `kernel/arch/x86/kernel/uspace.S` — `iret` → `iretq`。
- `kernel/arch/x86/kernel/include/ours/arch/x86/descriptor.hpp` — 用户选择子补 RPL 3。
- `kernel/arch/x86/kernel/init.cpp` — CpuLocal 之后重建 TSS 描述符。
- `kernel/mm/vm_aspace.cpp` / `kernel/include/ours/task/thread.hpp` — 空 aspace 与 `set_aspace()`。
- `kernel/lib/userabi/userboot.cpp` — 用户栈、入口线程、`arch_enter_uspace()`。

**症状**

`userboot: entering ring 3 at 201270` 之后 QEMU 自行退出（三重故障），**日志一个字都没有**，
gdbstub 下 `arch_handle_exception` 的断点**一次都没命中**。

「异常连投递都失败」这一点是整条线索的关键：它把嫌疑从「ring 3 里跑错了」缩到
「特权级切换本身，或者切换所依赖的那张表」。

**原因**

四个，都只在「第一次进用户态」这一刻才生效，所以此前谁也碰不到：

**其一，`iret` 是 32 位形式。** 反汇编说了实话：

```
71dc: cf                    iretl        ← x86_enter_uspace
  72: 48 cf                 iretq        ← interrupt_common 的返回路径
```

64 位下裸写 `iret` 汇编成 `iretl`，按**四字节**弹 EIP/CS/EFLAGS/ESP/SS，而压进去的每项是
八字节。返回地址、代码选择子、标志位全取自错误的半边。

**其二，用户选择子没有 RPL。**

```c
#define X86_GDT_USER_CODE64 (6 << 3)   // 0x30，index 6，RPL 0
```

选择子 = GDT 下标 **+ 请求特权级**，低两位就是那个级别。`iretq` 往下降特权级时会检查
CS.RPL，选择子写着 ring 0 就拒绝下降。

**其三，也是最隐蔽的一个：TSS 描述符指向 CpuLocal 搬迁之前的那份。**

`x86_init_percpu(0)` 在 `init_arch_early()` 里就跑了 `x86_setup_tss_percpu()`，那时
`mm::CpuLocal::init()` 还没发生，于是描述符里写的、`ltr` 装载的，是 `X86_PCPU` 的**模板副本**
（在 physmap 里）。CpuLocal 随后给 CPU 0 分了自己的 chunk，而 `x86_set_tss_sp()` 是通过
**运行中的 CPU** 解析 `X86_PCPU` 的——它一直在往一个 CPU 根本不看的对象里写 `sp0`。
把 GDT dump 出来和实时地址并排一看就清楚了：

```
PROBE tss(now)=FFFFFF88000000E0 sp0=FFFFFF8804021000     ← x86_set_tss_sp 写的
TSS64 busy | 0xFFFFFF801FD400E0 | 0x2068 | ring 0        ← CPU 实际用的
```

内核一直待在 ring 0 时这毫无代价——`sp0` 只在**特权级切换**时才被读。第一次 `iretq` 进
ring 3 把它变成致命的：下一个中断从这份陈旧 TSS 里取到 `RSP0 = 0`，把中断帧压到地址 0，
而投递由此产生的 #PF 又要读 `RSP0`——double fault，接着 triple，**任何处理函数都还没来得及跑**。
这正是「日志一个字都没有」的由来。

**其四，`Thread::m_aspace` 有读无写。** 它在构造函数里被初始化成空，**全树没有任何赋值点**，
所以从来没有线程跑在自己的地址空间里，调度器的 `curr->aspace() != next->aspace()` 恒为假。
补了 `set_aspace()`；顺带 `VmAspace::switch_aspace()` 对空指针取 `&prev->m_arch` 是 UB
（`switch_context()` 其实完全不看 `from`，且已把空 `to` 当作内核页表），一并改成空指针安全。

**改法**

前三条各自一行到一段，见上；第四条加一个 setter。userboot 这边：映射一页用户栈
（**不带 Executable**——可执行的栈正是缓冲区溢出变成代码执行的经典路径），起一个内核线程，
**在 `resume()` 之前**装上 aspace（调度器切换到它时才会加载正确的 CR3，顺序不能反），
线程入口构造用户帧并 `arch_enter_uspace()`——它不返回：`iretq` 到一个伪造的中断帧，
这是进 ring 3 的唯一办法，x86 没有「直接降特权级」的指令。

**参照**

`Zircon`：`USER_CODE_64_SELECTOR (0x30 | 3)`；`x86_uspace_entry` 同样以 `iretq` 收尾；
TSS 的 `sp0` 同样在上下文切换时更新（`x86_set_tss_sp`）。

**验证**

QEMU dbg，**userboot 在 ring 3 里运行**：

```
userboot: mapped [200000, 203000) into its own aspace, entry at 201270
userboot: entering ring 3 at 201270, stack 1200000
Exception[interrupt] from user space      ← 897 次
```

897 次是时钟中断反复抢占 ring 3 里 `pause` 自旋的 userboot，内核每次都正确判定中断来自
用户态并返回它。无断言，QEMU 存活。

**故意没做**

- 没有系统调用。userboot 目前只是自旋；证明「到了 ring 3」用不着系统调用，
  用户态中断本身就是证据。
- 三段仍共用一套 RWX 映射（待修第 1 条）。
