# userabi 改动记录

## userboot 改走对象层建进程与映射（2026-09-06）

**文件**
- `kernel/lib/userabi/userboot.cpp` — 建地址空间/做映射的两处改成走 dispatcher
- `kernel/lib/userabi/BUILD.gn` — 新增 `//kernel/object` 依赖，并说明为什么 `//kernel/mm` 还在
- `kernel/object/include/ours/object/handle.hpp` — `dispatcher()` 补非 const 重载

**症状**
不是崩溃，是层次错了。`userboot.cpp` 全文没有一处 `Dispatcher`：

```cpp
auto status = mm::VmAspace::create(mm::VmAspaceKind::User, mm::VmasFlags::None, "userboot",
                                   &out.aspace);
...
auto root = out.aspace->root_vma();
auto mapping = root->map_phys(...);
auto stack = root->map_anon(...);
```

跑得通，但 `ProcessDispatcher` / `VmaDispatcher` 因此从来没有被任何真实调用者用过
——前者连 `create()` 的定义都没有，而这件事直到有人调用才会以链接错误的形式暴露。

**原因**
`kernel/object` 这一层是**为用户服务的内核对象**，其余子系统的对象
（`mm::VmAspace` / `VmArea` / `VmMapping`、`task::Thread`）是为内核自身服务的。
userboot 是第一个*用户*程序，构造它的那条路径就应该是将来系统调用代用户走的那条。
绕过去的代价不是当下的 bug，而是对象层永远停在桩的形态：每一个必须经过的
dispatcher，才会被真实调用者逼出确定的签名、确定的失败路径、确定的所有权。

具体到映射：`mm::VmArea` 知道一个映射*被允许*成为什么（它的 VmaFlags 上限），
但它不可能知道**是谁在问**。而 rights 挂在句柄上，不挂在对象上。将来每一个
`sys_vmar_map` 都要回答"这个调用者能不能在这里映射"，能回答的只有把调用者带进来的
那个句柄——所以映射入口必须长在 dispatcher 上。今天 `VmaDispatcher::map_phys()`
只是直接转发给 `VmArea`，看不出差别，而这正是关键：位置对了，检查才有地方放。

顺带撞出一条：`ustl::Rc` 的 `operator->` 是**深 const** 的（`Rc<T> const` 给出
`T const *`），这一点和 `fbl::RefPtr`、`std::shared_ptr` 都不一样，后两者的 const 是
浅的。`KernelHandle::dispatcher()` 原本只有 const 重载，于是即便持有一个非 const 的
句柄，拿到的也是 `T const *`，而 dispatcher 的方法全是非 const 的（它们要改自己代表
的那个对象）——结果是**通过拥有它的句柄反而调不到任何方法**。

**改法**
1. 进程和它的 root VMA 一次建出来：

   改前
   ```cpp
   auto status = mm::VmAspace::create(mm::VmAspaceKind::User, mm::VmasFlags::None, "userboot",
                                      &out.aspace);
   ```
   改后
   ```cpp
   object::KernelHandle<object::ProcessDispatcher> process;
   object::KernelHandle<object::VmaDispatcher> root_vma;
   Rights process_rights;
   Rights root_vma_rights;
   auto status = object::ProcessDispatcher::create(object::JobDispatcher::root_job(), "userboot",
                                                   process, process_rights, root_vma,
                                                   root_vma_rights);
   ```

   两个对象一起交出来，因为单独哪一个都没用：没有 root VMA 的进程拿不到任何内存，
   没有进程的 VMA dispatcher 指着一段谁也不会在里面跑的区域。两份 rights 目前没有
   消费者——它们是句柄表存在之后 `Handle::make()` 的实参，那一步才是"把对象交给
   userboot"而不是"用对象把 userboot 搭出来"。

2. 映射改走 dispatcher：

   改前
   ```cpp
   auto root = out.aspace->root_vma();
   auto mapping = root->map_phys(mm::PhysMap::virt_to_phys(backing), ...);
   auto stack = root->map_anon(...);
   ```
   改后
   ```cpp
   auto mapping = root_vma.map_phys(mm::PhysMap::virt_to_phys(backing), ...);
   auto stack = root_vma.map_anon(...);
   ```

3. `LoadedImage` 持有的从地址空间改成进程：

   改前
   ```cpp
   struct LoadedImage {
       ustl::Arc<mm::VmAspace> aspace;
   ```
   改后
   ```cpp
   struct LoadedImage {
       object::KernelHandle<object::ProcessDispatcher> process;
   ```

   地址空间仍然拿得到，经由拥有它的进程：`thread->set_aspace(loaded.process.dispatcher()->aspace())`。

4. `KernelHandle::dispatcher()` 补非 const 重载，理由见上。

`root_vma` 在 `userboot_init()` 结束时析构，唯一一个引用也就没了——这是对的：它做出来
的映射活在地址空间里，而地址空间归进程所有；dispatcher 是"被允许做映射的那个对象"，
不是"持有映射的那个东西"。代码里写了这条注释，因为它读起来像泄漏而其实不是。

**参照**
- `Zircon userboot.cc` 的 `ProcessDispatcher::Create(...)`：同样一次拿回进程和 root
  VMAR 两个 KernelHandle 加两份 rights。
- `Zircon VmAddressRegionDispatcher::Map()`：同样把映射入口放在 dispatcher 上而不是
  VmAddressRegion 上。
- `Zircon KernelHandle::dispatcher()`：Zircon 只有 const 一个重载就够用，因为
  `fbl::RefPtr` 的 const 是浅的；ours 的 `ustl::Rc` 深 const，所以需要两个。

**验证**
- `ninja` 通过。`clang-tidy` 对 `userboot.cpp` 只剩 `USERBOOT_IMAGE` 的命名告警
  （SCREAMING_SNAKE 是本树约定）。
- QEMU 实测 `kernel.dev.dbg_1.obi`——这是 `ProcessDispatcher::create()`、
  `VmaDispatcher::create()` 和两个 `map_*` **第一次有真实调用者**：

  ```
  [info] userboot: found 'userboot', 776 bytes of ELF
  [info] userboot: mapped [200000, 203000) into its own aspace, entry at 201270
  [info] userboot: entering ring 3 at 201270, stack 1200000
  [trace] Exception[interrupt] from user space      （反复出现）
  ```

  用的是 `KERNEL_DEBUG_LEVEL=1` 的镜像，所以这同时验证了上一条改动里
  `KernelHandle` 那个必然失败的 `DEBUG_ASSERT`——它原本会在这里第一次炸。
- 注：本树的 boot 日志含终端转义与 NUL 字节，`grep` 会当二进制文件静默跳过，
  查日志要用 `grep -a`。

**故意没做**
- **`get_frames_n()` 和 `PhysMap` 还在**，所以 `//kernel/mm` 依赖还没去掉。镜像和
  解压后的内核包目前仍是裸帧，退掉它们要 `VmoDispatcher`——那是下一步，BUILD.gn 的
  注释里写明了这一点，免得这条依赖看起来像是没人管。
- **线程仍然是 `task::Thread::spawn` + 手工 `set_aspace`**。换成
  `ThreadDispatcher::create/start` 是再下一步，而且换完之后 `set_aspace` 这行会直接
  消失：`task::Thread` 在有 ThreadDispatcher 时本来就会去问
  `m_user_thread->process().aspace()`（`thread.cpp` 的 `Thread::Current::fault`）。
- **没有把这两个 KernelHandle 装进任何句柄表**，userboot 因此还没有"拿到"自己的
  对象。那需要 `Handle::make()` 有定义和 arena，设计已定
  （`docs/architecture/object/handle-lookup.md`），是独立的一块。
- **`map_phys` / `map_anon` 里仍然没有 rights 检查**。理由同上一条改动记录：现在没有
  持句柄的调用者可查，恒真的检查比没有更糟。
