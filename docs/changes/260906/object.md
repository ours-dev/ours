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
