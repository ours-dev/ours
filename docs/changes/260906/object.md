# object 改动记录

## VmAreaDispatcher 有了实现，KernelHandle 有了移动赋值（2026-09-06）

**文件**
- `kernel/object/include/ours/object/vm_area_dispatcher.hpp` — 从空类变成真正的 dispatcher
- `kernel/object/vm_area_dispatcher.cpp` — 新文件，`create` / `map_phys` / `map_anon`
- `kernel/object/include/ours/object/handle.hpp` — `KernelHandle` 补上移动赋值
- `kernel/object/BUILD.gn` — 加入新的源文件

**症状**
潜伏，当前触发不到——因为没有任何调用者。

`kernel/lib/userabi/userboot.cpp` 全文没有一处 `Dispatcher`：它直接调
`mm::VmAspace::create()` 建地址空间、直接在 `aspace->root_vma()` 上做映射、
直接 `task::Thread::spawn()` 起线程、直接 `arch_enter_uspace()` 进 ring 3。
`BUILD.gn` 里 `deps = [ "//kernel/mm", ... ]` 是这件事的书面证据。

这样能跑，但对象层因此一直没有被任何真实调用者逼出形状：`VmAreaDispatcher`
是一个空类

```cpp
class VmAreaDispatcher: public VmAreaDispatcherBase {};
```

而 `ProcessDispatcher::create()` 只有声明没有定义（链接期才会发现）。

**原因**
`kernel/object` 这一层是**为用户服务的内核对象**，其余子系统的对象
（`task::Thread`、`mm::VmAspace` / `VmArea` / `VmMapping`）是为内核自身服务的。
userboot 的意义正在于它是第一个*用户*程序——创建它的路径应该就是将来系统调用
代它走的那条路径。绕过 dispatcher，对象层就永远停在桩的形态，坐在它上面的
syscall 层也就永远没有确定的形状。

具体到 VMAR：**权利（Rights）挂在句柄上，不挂在对象上**。`mm::VmArea` 知道一个
映射*被允许*成为什么（它的 VmaFlags 上限），但它不可能知道是谁在问。将来每一个
`sys_vmar_map` 都要回答"这个调用者能不能在这里映射"，能回答的只有把调用者带进来
的那个句柄——所以映射入口必须长在 dispatcher 上，而不是长在 VmArea 上。这也是
为什么 `create()` 必须把 Rights 作为**单独的出参**交出去：Handle 携带 rights，
Dispatcher 不携带，工厂函数唯一能做的就是把构造 Handle 用的那个值给调用者。

组合而不是继承，这里有超出项目惯例的具体理由：VmArea 由它的地址空间拥有，寿命
长过每一个指向它的句柄；dispatcher 由持有句柄的人拥有，最后一个句柄关闭时就死。
继承会把这两条寿命焊成一条，于是"进程关掉了它的 VMAR 句柄"就变成了"地址空间少
了一个区域"——那不是关句柄的语义。

顺带撞出来的第二个缺陷：`KernelHandle` 有 `USTL_NOCOPYABLE`，而删除拷贝构造会
连带抑制隐式的移动操作。结果是这个类型**没有任何办法赋值给一个已经存在的
KernelHandle**，只能构造新的。可这一层每个工厂函数都用 `KernelHandle<T> &` 出参
报告结果（`ProcessDispatcher::create` 一次要两个），而那正是赋值。也就是说这个
类型无法按它自己的接口所要求的方式被使用。编译器的说法是：

```
error: overload resolution selected deleted operator '='
```

**改法**
1. `VmAreaDispatcher` 持有一个 `ustl::Arc<mm::VmArea>`，并暴露 mm 侧那对映射入口。

   改前
   ```cpp
   class VmAreaDispatcher: public VmAreaDispatcherBase {};
   ```

   改后（节选）
   ```cpp
   class VmAreaDispatcher: public VmAreaDispatcherBase {
     public:
       static auto create(ustl::Arc<mm::VmArea> vma, KernelHandle<Self> &handle_out,
                          Rights &rights_out) -> Status;

       auto map_phys(PhysAddr phys_base, mm::VmPlacement where, usize size, mm::MmuFlags mmuf,
                     mm::VmMapOption option, char const *name)
           -> ktl::Result<ustl::Arc<mm::VmMapping>>;

       auto map_anon(mm::VmPlacement where, usize size, mm::MmuFlags mmuf, mm::VmMapOption option,
                     char const *name) -> ktl::Result<ustl::Arc<mm::VmMapping>>;

     private:
       ustl::Arc<mm::VmArea> m_vma;
   };
   ```

   `map_phys` / `map_anon` 现在**不做任何权利检查**，并且注释里写明了原因：还没有
   持句柄的调用者可查——userboot 是内核自己搭起来的。检查属于 syscall 入口，等
   句柄解析那一步存在之后再加。

2. `KernelHandle` 补上移动赋值。

   改前——只有一个从 `KernelHandle<U>&&` 来的构造函数，没有任何 `operator=`。

   改后
   ```cpp
   FORCE_INLINE
   auto operator=(KernelHandle &&other) -> KernelHandle & {
       m_dispatcher = ustl::move(other.m_dispatcher);
       return *this;
   }
   ```

   没有改用现成的 `reset()`：它内部调的是 `m_dispatcher->on_zero()`，而 `Dispatcher`
   上的函数叫 `on_zero_handles()`。`reset()` 是成员模板，从来没被实例化过，所以这个
   错误一直是隐形的——已记入 `docs/repairs/260906/object.md`。

**参照**
- `Zircon VmAddressRegionDispatcher::Create()`（`kernel/object/vm_address_region_dispatcher.cc`）：
  同样是 `KernelHandle<T>*` + `zx_rights_t*` 两个出参，同样持有
  `fbl::RefPtr<VmAddressRegion>` 而不是继承它。
- `Zircon VmAddressRegionDispatcher::Map()`：Zircon 只有一个 `Map()`，因为在 Zircon
  里可映射的东西全都是 VMO。ours 的 `VmArea` 还留着"直接映射一段物理"的捷径
  （userboot 的镜像现在正走这条），所以这里暂时保留两个入口，而不是假装 VMO 层
  已经到位。
- `Zircon KernelHandle::operator=(KernelHandle&&)`：同样的声明，同样的理由。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过（两个 switchset 都编）。
- QEMU 实测 `kernel.dev.dbg_1.obi`：启动照旧走到 userboot 进入 ring 3，日志重复
  出现 `Exception[interrupt] from user space`——时钟中断正在抢占用户态的自旋循环，
  说明 ring 3 是真的进去了且没有回归。结束于 `terminating on signal 15`（是我们杀的，
  不是三重故障）。
- 本次没有调用者，所以是编译/链接层面的验证；行为验证在下一步接上 userboot 时做。

**故意没做**
- **没有做权利检查**。理由见上：没有句柄可查。加一个恒真的检查比不加更糟，它会
  让人以为这条路已经守住了。
- **没有从 VmaFlags 推导 rights**。Zircon 会按 VMAR 自己的 `CAN_MAP_READ/WRITE/EXECUTE`
  放宽 rights；ours 的 `VmAreaOrMapping` 把 `VmaFlags` 藏在 private 且没有访问器，
  读不到上限，所以先给 `default_right()`——诚实的默认值好过猜一个更窄的。代码里
  留了 TODO 说明这一点。
- **没有 `allocate()`（建子 VMA）、`protect()`、`unmap()`、`destroy()`**。这个 slice
  用不到；等到有 syscall 需要它们时再按需长出来，那时才知道签名该长什么样。
- **没有顺手修 `KernelHandle::reset()`**。它是另一件事，进 repairs 单独认领。

## VmAreaDispatcher 改名为 VmaDispatcher（2026-09-06）

**文件**
- `kernel/object/include/ours/object/vma_dispatcher.hpp` — 由 `vm_area_dispatcher.hpp` 改名
- `kernel/object/vma_dispatcher.cpp` — 由 `vm_area_dispatcher.cpp` 改名
- `kernel/object/include/ours/object/types.hpp`、`process_dispatcher.hpp`、`BUILD.gn` — 跟随改名

**症状**
无，纯命名。

**原因**
对象层这一族的名字要成对读得通：内存对象是 `VmoDispatcher`，地址区域是
`VmaDispatcher`。`VmAreaDispatcher` 把底层 mm 类型的全称 `VmArea` 直接搬进了对象
层的名字里，于是同一族里一个用缩写（VMO 将来必然缩写，没人会写
`VmObjectDispatcher` 又同时写 `VmAreaDispatcher`）一个用全称。

Zircon 那边这一族叫 `VmObjectDispatcher` / `VmAddressRegionDispatcher`，两个都是
全称，自洽；ours 选缩写，那就两个都缩写。

**改法**
逐条对应改名，`git mv` 保留文件历史：

| 改前 | 改后 |
|---|---|
| `class VmAreaDispatcher` | `class VmaDispatcher` |
| `using VmAreaDispatcherBase` | `using VmaDispatcherBase` |
| `RIGHTS_DEFAULT_VMAR` | `RIGHTS_DEFAULT_VMA` |
| `gktl::magic("VmAreaDispatcher")` | `gktl::magic("VmaDispatcher")` |
| `DISPATCHER_VMAR_CREATE_COUNT` / `"dispatcher.vmar.create"` | `DISPATCHER_VMA_CREATE_COUNT` / `"dispatcher.vma.create"` |
| `OURS_OBJECT_VM_AREA_DISPATCHER_HPP` | `OURS_OBJECT_VMA_DISPATCHER_HPP` |
| `vm_area_dispatcher.{hpp,cpp}` | `vma_dispatcher.{hpp,cpp}` |

注释里的 "VMAR" 分两种，只改了其中一种：说的是 ours 自己的对象时改成 VMA
（`/// Rights a freshly created VMA handle carries.`），说的是 Zircon 的
`VmAddressRegionDispatcher` 及其 `CAN_MAP_*` 标志时保持 VMAR 不动——那是对照物的
正确称呼，改了反而对不上。

**参照**
`Zircon VmAddressRegionDispatcher` / `VmObjectDispatcher`：同一族的两个名字，
ours 取其缩写形式。

**验证**
`ninja` 通过；`clang-tidy` 对本文件只剩 `GKTL_COUNTER` 宏声明变量的命名告警，
与 `process_dispatcher.cpp` / `thread_dispatcher.cpp` 现状一致。QEMU 实测
`kernel.dev.dbg_1.obi` 启动无回归，仍反复出现 `Exception[interrupt] from user space`。

**故意没做**
`rights.hpp` 里那条 `TODO: RIGHTS_DEFAULT_VMO has no home yet` 没有动——它要等
`VmoDispatcher` 建出来才有地方搬，那是下一步的事。

## ProcessDispatcher::create() 有了定义，root job 有了实体（2026-09-06）

**文件**
- `kernel/object/process_dispatcher.cpp` — `create()` 与私有构造函数的实现
- `kernel/object/include/ours/object/process_dispatcher.hpp` — `m_aspace` 改为拥有式，新增 `m_job` / `m_name`
- `kernel/object/job_dispatcher.cpp` — 新文件，root job 单例与它的 init hook
- `kernel/object/include/ours/object/job_dispatcher.hpp` — 补上 `MAGIC`
- `kernel/object/BUILD.gn` — 加入新源文件

**症状**
潜伏。`ProcessDispatcher::create()` 和 `create_shared()` 一直只有声明没有定义，
`JobDispatcher::root_job()` 同样。它们没有被发现，是因为没有人调用——一旦有人调，
拿到的是链接错误而不是编译错误，位置也不在出错的那一行。

**原因**
两处需要说清楚。

**其一，为什么地址空间必须是拥有式的。** 改前的成员是裸指针：

```cpp
    /// Unique address space per each process.
    mm::VmAspace *m_aspace;
```

而 `mm::VmAspace::create()` 交出来的是 `ustl::Arc<VmAspace>`——地址空间是引用计数
对象，除了 `create()` 的返回值之外没有任何地方引用它。裸指针成员意味着唯一的那个
Arc 停在 `create()` 的栈帧上，函数一返回引用计数归零，地址空间当场被释放，进程手里
留下的是野指针。这不是风格问题，是这个成员一旦真的被写入就必然发生的事。Zircon 的
`ProcessDispatcher` 用的是 `fbl::RefPtr<VmAspace> normal_aspace_`，同样的理由。

`aspace()` 仍然返回裸指针，这是有意的：它的两个调用者是缺页路径
（`Thread::Current::fault`）和上下文切换，两者都在这个进程的某个线程正在运行时执行
——线程正在跑本身就是"进程还活着"的证明。给它们 Arc 只会让这两条热路径去动一个
它们不需要的引用计数。

**其二，为什么要先有 job。** `create()` 的第一个参数就是
`ustl::Arc<JobDispatcher> &job`：进程属于某个 job，这是签名一开始就写死的事实。
而 `root_job()` 没有定义，所以在补出它之前 `create()` 根本无法被调用。

root job 用 init hook 建，而不是首次调用时懒创建。理由和
`VmAspace::init_kernel_aspace()` 一样：只有启动序列知道堆什么时候起来；懒创建要么
得做线程安全，要么得靠注释声明"只能在启动期调"——而 hook 把后者免费变成了强制。
挂在 `Vmm + 1` 而不是 `Heap`：对象本身只是一次堆分配，但第一个会问它要东西的是进程
创建，那需要 VMM 已经就绪；挂得更早只是白白拉长"半成品系统能够摸到它"的窗口。

顺带补 `MAGIC`：`SoloDispatcher` 的构造函数要读 `Derived::MAGIC` 来建 canary，
`JobDispatcher` 没有，所以它从来就实例化不出来。

**改法**
改前——`process_dispatcher.cpp` 里只有 `attach_thread` / `detach_thread` 两个函数，
没有 `create`。

改后（节选）：

```cpp
auto ProcessDispatcher::create(ustl::Arc<JobDispatcher> &job, char const *const name,
                               KernelHandle<Self> &process_out, Rights &process_rights,
                               KernelHandle<VmaDispatcher> &root_vma_out, Rights &root_vma_rights)
    -> Status {
    ustl::Arc<mm::VmAspace> aspace;
    auto status = mm::VmAspace::create(PRIVATE_ASPACE_BASE, PRIVATE_ASPACE_SIZE,
                                       mm::VmAspaceKind::User, mm::VmasFlags::None, name, &aspace);
    if (Status::Ok != status) {
        return status;
    }

    auto root_vma = aspace->root_vma();
    if (!root_vma) {
        return Status::InternalError;
    }

    status = VmaDispatcher::create(ustl::move(root_vma), root_vma_out, root_vma_rights);
    if (Status::Ok != status) {
        return status;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(job, name, ustl::move(aspace)));
    ...
}
```

三步的顺序是有讲究的：进程对象放在最后建，因为它是接管 aspace 所有权的那一步。在它
之前的每一处失败都只需要丢掉 Arc 就能干净退出；在它之后 aspace 有了活得比这个栈帧
长的所有者。倒数第二步失败时 `root_vma_out` 已经握着一个 dispatcher，调用者的
`KernelHandle` 析构会释放它——aspace 也随之而死，因为 root VMA 是最后一个引用它的
东西。

地址范围取 `PRIVATE_ASPACE_SIZE`（到 2^46）而不是 `USER_ASPACE_SIZE`（到 2^47 的
canonical 分界）：用户半区的上半留给 `create_shared()` 将来发的共享地址空间，而这
只有在一开始就按那个边界切分才成立，事后再缩是缩不动的。这两个常量本来就躺在这个
文件的匿名 namespace 里，是当初留下的挂点。

**参照**
- `Zircon ProcessDispatcher::Create()`（`kernel/object/process_dispatcher.cc`）：同样
  一次交出进程和它的 root VMAR 两个 KernelHandle 加两份 rights，同样把 VmAspace 用
  RefPtr 持有。
- `Zircon GetRootJobDispatcher()`：同样是启动期建好的单例。
- `VmAspace::init_kernel_aspace()`（本树）：init hook 建单例的既有写法。

**验证**
- `ninja` 两个 switchset 均通过。
- QEMU 实测两个镜像。新增的 init hook 在 `Vmm + 1` 真的跑了（跑不通会 panic 在
  `Failed to create the root job`），两个镜像都照旧走到 userboot 进入 ring 3：
  `dev.dbg_1` 停在反复的 `Exception[interrupt] from user space`，`dev` 停在
  `entering ring 3 at 201270, stack 1200000`，都以 signal 15 结束。
- `create()` 本次仍无调用者，行为验证在下一步 userboot 切过来时做。

**故意没做**
- **`create_shared()` 仍未实现**。它要的共享地址空间语义（两个进程共享上半区）没有
  任何调用者，签名之外的部分现在写只能靠猜。
- **job 不记录它的子进程**。反向的进程列表在 Zircon 里是为了让 job 能被整体 kill 和
  枚举，这两个操作这里都还不存在；先加一个没人读的链表没有意义。
- **没有 handle table，没有把这两个 KernelHandle 装进任何进程的句柄表**。那需要
  `Handle::make()` 有定义和 arena 分配器，是独立的一块（设计已定，见
  `docs/architecture/object/handle-lookup.md`）。

## KernelHandle 的调试断言断在一个必然为空的指针上（2026-09-06）

**文件**
- `kernel/object/include/ours/object/handle.hpp` — 断言改为检查移动之后的成员
- `kernel/object/include/ours/object/vma_dispatcher.hpp` — 类与析构函数标 `final`

**症状**
潜伏，但触发条件极窄且必然：任何一次用 `ustl::Arc` 构造 `KernelHandle`，在
`KERNEL_DEBUG_LEVEL=1` 的镜像里都会当场断言失败。至今没有暴露，只是因为在
`VmaDispatcher::create()` 出现之前，树里没有任何一处这样构造过。

**原因**

```cpp
    template <typename U>
    KernelHandle(ustl::Arc<U> object)
        : m_dispatcher(ustl::move(object)) {
        DEBUG_ASSERT(object);
    }
```

成员初始化列表先于函数体执行，所以进到 `DEBUG_ASSERT` 时 `object` 已经被移走了。
而 `ustl::Rc` 的移动构造是

```cpp
    Rc(Rc &&other) noexcept : m_pointer(std::exchange(other.m_pointer, nullptr)) {}
```

——它把源指针置空。也就是说这个断言检查的是一个**保证为空**的指针：不是"可能失败"，
是构造成功时必然失败。值得检查的是活过构造函数的那个状态，也就是 `m_dispatcher`。

上面那个 `KernelHandle(T *object)` 重载没有这个问题：它接的是裸指针，没有移动发生。

`final` 是同一轮 clang-tidy 顺出来的第二条：`Dispatcher` 的析构是虚的，
`VmaDispatcher` 的析构因此是在覆盖它，但没有写出来。把类和析构都标 `final` 之后，
将来签名一旦改到不再覆盖，编译器会说话。注意两者是绑定的——只标析构会得到
`class with destructor marked 'final' cannot be inherited from`，因为 `final` 析构
要求类本身也是 `final`。

**改法**

改前
```cpp
        : m_dispatcher(ustl::move(object)) {
        DEBUG_ASSERT(object);
    }
```
改后
```cpp
        : m_dispatcher(ustl::move(object)) {
        DEBUG_ASSERT(m_dispatcher);
    }
```

以及
```cpp
class VmaDispatcher final: public VmaDispatcherBase {
    ...
    ~VmaDispatcher() final;
```

**参照**
`Zircon VmAddressRegionDispatcher`：`class VmAddressRegionDispatcher final : public
SoloDispatcher<...>` 加 `~VmAddressRegionDispatcher() final;`，同样的写法。
断言那条无对应物，是本树自有的缺陷。

**验证**
`ninja` 通过（`final` 那条第一次改错时正是靠 `-Werror` 拦下的）；QEMU 实测
`kernel.dev.dbg_1.obi` 无回归。断言本身要等下一步 userboot 真的去构造 KernelHandle
才会被走到——那正是它原本会炸的地方。

**故意没做**
`KernelHandle::reset()` 里调用不存在的 `Dispatcher::on_zero()` 那条没有一起改，它是
语义问题不是笔误，留在 `docs/repairs/260906/object.md` 第 1 条。
