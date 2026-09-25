# userabi 改动记录（2026-09-25）

## 构建生成用户可见文件的白名单 `config/user_bootfs`，随内核包打入（2026-09-25）

**文件**
- `system/user_executable.gni` — 每个 `user_executable()` 以 metadata `bootfs_user_visible` 声明自己的安装名
- `kernel/lib/userabi/vdso/BUILD.gn` — vDSO 同样声明为用户可见
- `kernel/kernel_package.gni` — 新参数 `user_bootfs`：为真时用 `generated_resource` 收集 deps 的 `bootfs_user_visible`，
  写成 `config/user_bootfs` 打进包里
- `kernel/products/image/dev/BUILD.gn`、`kernel/products/image/dev.dbg_1/BUILD.gn` — 两个镜像的内核包打开 `user_bootfs`
- `docs/repairs/260925/obi.md` — 新增第 4 条（见"验证"）

**症状**
无，这是 `docs/repairs/260925/userabi.md` 第 1 条定下的黑白名单做法的第一步：内核要知道哪些文件可以交给用户态，得有一份名单。

**原因**
按主人的决定（2026-09-25），用户态看得见什么由显式的白名单决定，默认不放行；名单由构建从 GN metadata 生成，不手写。

**改法**
改前：内核包里没有任何"哪些对用户可见"的信息。

改后：

```gn
    metadata = {
      bootfs_user_visible = [ _install_name ]
      ...
    }
```

```gn
  if (_user_bootfs) {
    _user_visible = "$target_name.user_visible"
    generated_resource(_user_visible) {
      ...
      data_keys = [ "bootfs_user_visible" ]
      output_conversion = "list lines"
      outputs = [ "config/user_bootfs" ]
    }
  }
```

- **标记放在产出文件的 target 上**：`user_executable()` 一律标记，vDSO 显式标记（用户态加载器要读它的 `.dynsym`）。没人标记的——
  内核映像与补丁表、名单本身、以后新加的任何东西——默认留在内核这边。
- **显式开启**：一个镜像的所有 kernel-storage 条目会被合并成一个 BootFS，第一版无条件生成名单时，physboot 的包也生成了一份（空的），
  两个同名的 `config/user_bootfs` 并列进了包里。改为只由承载用户程序的包开启；某个包该开而没开时，内核拿不到名单，结果是什么也不给
  用户态——出错的方向是"少给"。

**参照**
Fuchsia 没有对应的名单：它的内核包与用户 bootfs 本来就是两类 ZBI 条目（`zircon/kernel/phys/kernel-package.cc:65`），分开打包。
这里在同一个包里用名单达到同样的隔离；长期仍按设计 D7 拆成独立条目。

**验证**
- 生成的名单：dev 为 `userboot vdso shell init`，dbg_1 为 `userboot vdso shell fault_probe`。
- 两个镜像启动时内核打印的包清单里各有且只有一行 `config/user_bootfs`（25 / 32 字节）；dev `init: shell closed its channel`，
  dbg_1 `probes done (11 killed, 0 survived)`。
- 第一版（无条件生成）时清单里出现过两个同名条目，打包器没有报错——记为 `docs/repairs/260925/obi.md` 第 4 条。

**故意没做**
内核读这份名单、只把名单里的文件组成交给用户态的 BootFS VMO：后两步（先补 `kernel.name` 启动选项，再建 VMO）。

## 按白名单新建交给用户态的 BootFS VMO，内核自己的文件以黑名单兜底（2026-09-25）

**文件**
- `kernel/lib/userabi/src/user_bootfs.hpp`、`user_bootfs.cpp` — 新增 `make_user_bootfs()` 与 `USER_BOOTFS_RIGHTS`
- `kernel/lib/userabi/src/userboot.cpp` — `userboot_init()` 在第一个进程启动前建好它，存进 `resident_bootfs`
- `kernel/lib/userabi/BUILD.gn` — 加入新文件，依赖 `//kernel/lib/bootfs`
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `VmoDispatcher::read()`、`write()`，内核缓冲区版
- `docs/repairs/260925/userabi.md` — 第 1 条修掉，删除（该文件只有这一条）
- `docs/design/260924/userspace-filesystem.md` — 那一条的验收挪进 K14 的"验证"列

**症状**
潜伏，当前触发不到：还没有任何 BootFS VMO 交给用户态。按原计划 K12 会把**整个**内核包做成 VMO 交出去，
里面有 `vmours/image.elf`——内核带 KASLR，映像就是布局，只差一个基址（`docs/repairs/260925/userabi.md` 第 1 条）。

**原因**
内核包与"用户态看得见的文件"在镜像里是同一个 BootFS。设计原本让 init 在建 `/boot` 目录树时藏起内核的条目，
但那只挡得住经 `/boot` 访问的程序：持有原始 VMO 的 userboot 和 init 用 `vmo_read` 按偏移照样读得到。
**按名字隐藏、却把原始字节交出去，等于没藏。**要挡住，字节就不能进那个 VMO。

**改法**
改前：`userboot_init()` 解压内核包、留着它、启动第一个程序，没有任何要交给用户态的 BootFS。

```cpp
    // Kept, so that programs started later come out of the same package.
    resident_package = ustl::some(package.unwrap());
```

改后：紧接着新建一个 VMO，只放白名单里的文件，头部、条目表、名字表全部重写：

```cpp
    resident_package = ustl::some(package.unwrap());
    static_cast<void>(make_user_bootfs(*resident_package, boot::options().kernel_name(),
                                       resident_bootfs, resident_bootfs_rights));
```

```cpp
    auto is_kernel_private(std::string_view const name, std::string_view const kernel_name)
        -> bool {
        if (!kernel_name.empty() && name.starts_with(kernel_name) &&
            (name.size() == kernel_name.size() || name[kernel_name.size()] == '/')) {
            return true;
        }
        auto const slash = name.rfind('/');
        auto const base = slash == std::string_view::npos ? name : name.substr(slash + 1);
        return base == phys::KernelPackage::IMAGE_ELF || base == phys::KernelPackage::CODE_PATCHES;
    }
```

- **两道判断，顺序固定**：先看白名单（构建从 GN metadata 生成的 `config/user_bootfs`，上一提交），没列的一律不给；
  再看黑名单，列了也拒绝，并在启动时报 error。黑名单有两条，各补对方的漏：
  - 按**位置**：`kernel.name` 目录下的一切。今天那里只有映像和补丁表，但以后放进去的任何私有文件也自动被挡——
    这正是主人要"名单而不是特判"的理由。
  - 按**文件名**：`image.elf`、`code-patches` 不论在哪个目录。挡住包里第二个内核，以及 bootloader 改了
    `kernel.name` 时 phys 实际装的那个（vmours 看不到 bootloader 的命令行，见上一条 kernel-boot 记录）。
- **失败的方向是"少给"**：没有名单、名单为空、名单里的名字不在包里、同名出现两次、被黑名单拒绝，都只记日志、跳过，
  其余照常；VMO 本身建不起来时第一个程序没有 BootFS 可拿，但照样启动。
- **两遍扫描**：第一遍数文件、名字字节、数据页数，得出总大小；第二遍写。扫描是确定性的，两遍看到同一组文件；
  只有第一遍打日志。这样不用在两遍之间存任何东西，也不需要堆分配。
- **数据页对齐、尾部补零**：每个文件从页边界开始，占整页。于是没有两个文件共享一页，以后按页映射或切 Slice
  一个文件，不会带出另一个的字节。宿主侧的 `bootfs::Builder` 还是 8 字节对齐（改在 U4）；这一份由内核自己排版，
  可以一开始就对。VMO 是 Pinned、创建时整体提交的，页先清零再写，所以名字表之后、每个文件之后的空隙读出来都是零，
  不是帧里的旧内容。
- **头部最后写**，写完用 `VmoDispatcher::read()` 读回 magic、文件数、总大小核对——用户态的 `vmo_read` 以后也走这条路，
  这是"字节真的落进了 VMO"的唯一检查。
- **rights 为 Basic|Read|Map|GetProperty**：不给 Write，所有程序读同一份字节，能写就能改别人要跑的程序；不给 Execute，
  可执行权经 vmex 换得（设计 D3），换的时候去掉 Write。创建时的默认 rights 与它取交集。
- `VmoDispatcher` 补上 `read()`/`write()`：userabi 只经对象层碰 VMO，不直接调 `mm::VmObject`。只收内核缓冲区是有意的——
  mm 在 VMO 锁下拷贝，不能缺页；系统调用 `vmo_read`（K13）自己经一页中转缓冲再拷给用户。

被否掉的做法：只跳过 `kernel.name` 这一个目录（写死今天已知的那一项，明天新加的私有文件会悄悄漏出去）；
给内核映像加密（密钥只能和密文放在同一个镜像里，而且用户拿到的本来就是解密后放进内存的那份）。

**参照**
Fuchsia 的内核包与用户 bootfs 本来就是两类 ZBI 条目，STORAGE_KERNEL 与 STORAGE_BOOTFS
（`zircon/kernel/phys/kernel-package.cc:65`），交给 userboot 的 bootfs VMO 里没有内核的东西，不需要过滤。
这里两者在同一个条目里，所以在内核里过滤；长期仍按设计 D7 拆成独立条目。`VmoDispatcher::read/write` 对应
Zircon `VmObjectDispatcher::Read()`/`Write()`，区别是那边收用户指针。

**验证**
- 两个镜像构建、启动。dev：`userabi: bootfs vmo 36864 bytes, 4 files, magic 0x42545346`，`init: shell closed its channel`；
  dbg_1：`userabi: bootfs vmo 32768 bytes, 4 files, magic 0x42545346`，`shell: probes done (11 killed, 0 survived)`。
- 大小与手算一致。dev 的四个文件 userboot 1024、vdso 1472、shell 14848、init 4544 字节，按页取整为 1+1+4+2 页，
  加上头部、条目表、名字表占的 1 页，共 9 页 = 36864；dbg_1 的 fault_probe 3888 字节占 1 页，共 8 页 = 32768。
- 黑名单与各条失败路径：临时给 dbg_1 加一个只带 metadata 的 group，往白名单里塞 `vmours/image.elf`、`vmours/code-patches`、
  `vmours`、`other/image.elf`、`missing`、`shell`（重复）。启动日志逐条报出：前四个 `is the kernel's own (kernel.name=vmours); refused`，
  `missing` `is on the BootFS whitelist but not in the kernel package`，`shell` `is on the BootFS whitelist twice`；VMO 仍是
  32768 字节、4 个文件，探针照常跑完。临时改动已还原，未提交。
- 用户态看到的内容（条目表里没有 `vmours/`）要等 K13 的 `vmo_read` 和 K14 把 VMO 交出去才能从用户态核对，已写进 K14 的验证列。

**故意没做**
- 把 VMO 交给用户态：第一个进程今天只收一个句柄（%rdi 里的根 job），processargs 在 K14。
- 释放解压缓冲：每个程序仍从它装载（`launch()`），内核自己的文件也在里面；而且 vfree 还没有 TLB shootdown。
- 按设计 D7 把用户 BootFS 拆成独立的 OBI 条目：需要改打包器和 phys，名单已经挡住了泄露，放到以后。
