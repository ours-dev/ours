# kernel/mm — 改动记录

格式见 `docs/changes/TEMPLATE.md`。待修项在 `docs/repairs/260903/mm.md`。
2026-09-02 及更早的 mm 改动在只读存档 `docs/repairs-20260830.md` 里（第 31–40 节）。

---

## 1. `VmArea::protect()` 从声明到实现，`reserve()` 不再绕过映射层（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_mapping.hpp` — 声明 `protect_locked()`
- `kernel/mm/vm_mapping.cpp` — 把 `protect()` 拆成校验 + 加锁的外壳与 `protect_locked()` 内核
- `kernel/mm/include/ours/mm/vm_area.hpp` — 声明 `protect_locked()` 与 `can_protect_locked()`
- `kernel/mm/vm_area.cpp` — 实现区域级 `protect()`；`reserve()` 改走映射层

**症状**

潜伏，当前启动触发不到。`VmArea::protect()` 直接 `return Status::Unimplemented`，所以
映射一旦建立就再也改不了权限——这是 VMA 接口里唯一没有实现的入口。

同时 `VmArea::reserve()` 越过自己的子节点，直接调 `m_aspace->arch_aspace().protect()` 写页表。
这一条现在能"正常工作"，但它让页表和映射自己记的 `ProtectionRanges` 成为两个独立的真相来源，
一旦有人再调一次映射级的 `protect()`，以哪个为准是没有定义的。

**原因**

权限这件事的真正拥有者是 `VmMapping`：它有 `ProtectionRanges`，把自己的地址窗口铺成一串不相交、
相邻且同权限即合并的区间，`protect()` 先更新这串区间，再逐区间下发到页表，并且把"没有任何权限"
的区间翻译成解除映射而不是设权限。`VmArea` 手里没有页表，只有子节点——所以区域级的 protect
不可能自己动手，只能下发。原来它没有下发，于是要么什么都不做（`Unimplemented`），要么像
`reserve()` 那样伸手越过子节点去写页表。

拆 `protect_locked()` 是被锁逼出来的：`get_lock()` 返回的是整个地址空间那一把锁，而且不可重入。
区域在持锁下降子节点时不能再调公开的 `protect()`，因为后者要自己取锁。`unmap`/`unmap_locked`
早就是这个分层，这里只是补齐对称。

**改法**

`VmArea::protect()` 校验后取锁，然后**先问后写**：`can_protect_locked()` 递归确认每个覆盖到的
子节点都接受这组权限，全部通过才由 `protect_locked()` 统一下发。

两趟而不是一趟，是因为 protect 半途失败无法恢复：前几个子节点已经改写，旧权限没有留底，返回值
也说不出边界落在哪里，调用方既不知道改到哪也没法回滚。`unmap` 不需要这个待遇——半解除的范围
再解除一次就行，操作是幂等的。

`reserve()` 末尾的直接页表写改成 `mapping->protect(0, size, mmuf)`。语义等价（`VmMapping::create()`
已经用同一个 `mmuf` 初始化了整个窗口的 `ProtectionRanges`，这次 update 是空操作），但它顺带
接上了零权限即解除映射那条分支，并且让页表只有一个写入者。

**参照**

`Zircon VmAddressRegion::Protect()`（`zircon/kernel/vm/vm_address_region.cc`）。它同样是先遍历
子节点做一次可行性检查再应用，理由和这里一样。下发到叶子的对应物是
`Zircon VmMapping::ProtectLocked()`。Linux 那边是 `mprotect_fixup()`，但 Linux 的 VMA 是平的，
没有嵌套，所以没有"递归下降"这一层。

**验证**

构建通过，`clang-format` 与 `clang-tidy -p out/bringup.x86` 对四个文件跑过，新增行无新告警。

QEMU 实测（`kernel.dev.dbg_1.obi`）：`init_vmm_preheap()` 的九个预置 VMA 全部经由新的
`reserve()` → `mapping->protect()` → `protect_locked()` 路径建立成功，日志里九条
`Reserve area` 齐全，启动继续推进到线程创建。这是 `protect_locked()` 拆分的正向证据。

`VmArea::protect()` 的递归下降**未实测**——当前没有任何调用方，启动路径走不到。它与已经验证
可用的 `unmap_locked()` 同构（同样的子节点遍历、同样的区间裁剪、同样的映射／区域分派），但这只是
类比，不是证据。

**故意没做**

- 没有让 `protect()` 要求范围被完全覆盖。Zircon 要求范围内不能有空洞，我们的 `unmap()` 不要求，
  两者保持一致比单独向 Zircon 看齐更重要。等到有真实调用方时再定。
- 没有动 `map_at()` 那个既当输入提示又当输出结果的 `VirtAddr *virt_base`。它是之前把 LAPIC 的
  MMIO 放到 physmap 起点那个 bug 的温床，该改，但那是接口签名的改动，会牵动所有调用方，单独一条。
- 没有统一"偏移 vs 绝对地址"。`unmap`/`protect`/`reserve`/`create_subvma` 收相对 `m_base` 的偏移，
  而 `map_at`/`map` 返回绝对地址、`contains`/`find_mapping_locked` 收绝对地址，调用方要反复换算
  （`init_vmm.cpp` 里就写着 `phys_vma.base - root_vma->base()`）。同样是签名级改动。
- 没有补遍历接口。目前只有 `find_mapping_locked()` 能找叶子，没有找子区域的，也没有对外枚举，
  地址空间转储、范围操作、拆除记账都无从下手。列在 `docs/repairs/260903/mm.md`。

## 补回 `MemCache::init()` 里被复制粘贴吃掉的 `NodeCache::on_mem_cache_init()`（2026-09-03）

**文件**
- `kernel/mm/object_cache.cpp` — `MemCache::init()` 里第二次初始化调用错写成 `CpuCache::`，
  改回 `NodeCache::`；顺手把这个函数里三处 `status` 检查统一成仓库主流的单行
  if-with-initializer 写法。

**症状**

启动在 `gktl::InitLevel::Heap` 就停住：

```
[... cpu0 error] Failed to create the object cache for Thread
```

来自 `kernel/kernel/task/thread.cpp:45`，即 `ThreadCache::init_from_option()` 返回非 `Ok`。
`init_pmm()`／`init_vmm()` 全程没有任何异常，日志一路正常，失败点看上去和内存子系统无关。

这个错只存在于工作区，没有进过历史：HEAD 上的 `MemCache::init()` 是对的，是一次把
`auto status = f(); if (Status::Ok != status)` 改写成 `if (auto status = f(); ...)` 的
机械重构在第二处把被调用者也一起复制了。代码和它正上方的注释因此互相矛盾——那段注释整段
都在解释「CpuCache 必须排在 NodeCache 前面」以及为什么，而底下根本没有 NodeCache 这一步。

**原因**

值得写厚的不是「少调了一个函数」，而是**为什么少调它以后一切看起来都还好，直到很后面才炸**。

`m_node_caches` 在构造函数里就是 `m_node_caches{}`，全零。`NodeCache::on_mem_cache_init()`
是唯一往里面填指针的地方。它不跑，这个数组就永远是一排 nullptr。而 `MemCache::init()`
本身**返回 `Status::Ok`**——它检查的是两个 `on_mem_cache_init()` 的返回值，第二个压根没调用，
自然也没有返回值可以检查。于是缓存被登记进 `GLOBAL_CACHES`，对外宣称自己可用。

真正的分水岭在 `MemCache::allocate(Gaf, NnId, CpuCache *)` 的开头：

```cpp
auto *const node_cache = m_node_caches[nid];
if (!node_cache) [[unlikely]] {
    // This cache keeps no slabs on that node.
    return nullptr;
}
```

这条快速返回原本表达的是一个**合法**状态：某个缓存按 `SLAB_NODES` 只在部分节点上放 slab，
问到别的节点上就该说「没有」。它没有能力区分「这个节点本来就不服务」和「这个缓存从来没有
被初始化过」——两者在 `m_node_caches[nid] == nullptr` 上完全同形。所以缺失的初始化不会断言、
不会报错、不会崩，只会让**这个缓存的每一次分配都返回 nullptr**，把错误一路推迟到第一个
真正检查分配结果的调用方。

顺着这条路把影响面走完，一共两层，都在 `init_pmm()` 里发生：

1. `MemCache::init()`（静态那个）建起 `NODE_CACHE` 和 `SELF_CACHE`。两者的 `m_node_caches`
   都是空的，但 `create_boot()` 一路返回 `Ok`。注意 `NODE_CACHE` 自己不可用这件事没有立刻
   暴露，恰恰是因为唯一会调用 `NODE_CACHE->allocate()` 的就是 `NodeCache::on_mem_cache_init()`
   ——它不跑，就没人去踩这个坑。缺陷把自己的证据一起吃掉了。
2. `init_pmm()` 紧接着调 `ktl::init_kmalloc()`，用 `MemCache::create_boot()` 建起全部
   `NUM_MALLOC_CACHES` 个 size class。每一个都返回 `Ok`，函数里那句
   `DEBUG_ASSERT(Status::Ok == status)` 一次都不会响。**内核堆就此整个死掉**：此后任何
   `kmalloc()`／`operator new` 都会拿到 nullptr，而没有任何一行日志说过出了问题。

之所以第一个可见的失败偏偏是 ThreadCache，只是因为它是启动顺序上**第一个检查返回值**的
消费者：`InitLevel::Heap` 的 hook 调 `ThreadCache::init_from_option()` → `MemCache::create()`
→ `SELF_CACHE->allocate(GAF_KERNEL)` → nullptr → `Status::NoMemory` → `panic()`。
如果它当初写成不检查，第一个失败会是某处对 nullptr 的解引用，现场会离原因更远。
换句话说：**这个缺陷是 fail-closed 的，不会写坏内存，但它的可观测点和它的成因之间隔了
两个初始化阶段**，而且中间那一段（kmalloc）是彻底静默的。

**改法**

把第二处改回 `NodeCache::on_mem_cache_init(*this)`，让代码和它上面那段解释顺序的注释一致。

同时把该函数里三处检查统一成 `if (auto const status = f(); Status::Ok != status)` 的单行形式。
本文件 192、496 行和 `kernel/mm/` 其余文件里都是这个写法，原来的重构留下了三种不同的形状
（`auto status` 一种、`Status const status` 两种，还带一处行尾空格和多余折行）。统一之后，
`status` 也不再是函数作用域的可变变量，三次检查各自独立，互相之间不可能再看到对方的值。

**否掉的方案**：本来想在 `MemCache::init()` 末尾加一条断言，确认 `SLAB_NODES` 里每个节点
都拿到了 NodeCache，这样同类缺失会当场响而不是推迟两个阶段。它是对的，但它是另一件事
（是给 `allocate()` 那条二义性的 nullptr 快速返回补护栏，不是修这次的复制粘贴），按一次改动
一次提交的规矩单独列进 `docs/repairs/260903/mm.md`。

**参照**

Linux `mm/slub.c` 的 `kmem_cache_open()`，它同样是「先建 per-node 结构、再建 per-cpu 结构」
这一对步骤，对应物分别是 `init_kmem_cache_nodes()` 和 `alloc_kmem_cache_cpus()`；
node 那一侧的自举递归由 `early_kmem_cache_node_alloc()` 单独处理。我们这里两步的先后
和 Linux 相反（CpuCache 在前），理由写在函数内的注释里：`NodeCache::on_mem_cache_init()`
要从 `NODE_CACHE` 里分配，而当正在初始化的缓存**就是** `NODE_CACHE` 时，那是一次对本对象
`allocate()` 的重入，会读 `m_cpu_caches`——所以 cpu 侧必须先就位。Linux 用一个专门的
early 分配路径绕开同一个递归，我们用排序绕开。

Zircon 无对应物：它没有 slab/object-cache 这一层，堆是 Scudo。

**验证**

- 构建：`prebuilt/ninja/ninja -C out/bringup.x86` 全绿（335/335）。
- `clang-format -i`、`clang-tidy -p out/bringup.x86` 对 `kernel/mm/object_cache.cpp` 跑过，
  改动所在的 556–600 行区间无告警。tidy 报的 8 个 error 全部来自另一会话正在进行的
  `Status::InvalidArgument` → `Status::InvArgs` 改名（`ktl/name.hpp`、`bootmem/*`），与本改动无关。
- QEMU 实测 `kernel.dev.dbg_1.obi`：五个对象缓存全部建起来，
  `ThreadCache / VmaCache / VmCowPagesCache / VmMappingCache / VmoPagedCache has been created`
  五条日志齐全，启动越过 `InitLevel::Heap` 与 `init_vmm()`，推进到 VMO commit 才停在
  `gktl` 的 magic 断言上——那已经在 kernel/task 一侧。
- QEMU 实测 `kernel.dev.obi`（断言编译掉）：走得更远，经 irq、HPET 时钟源选定、APIC timer
  标定，最后停在调度器一侧的 `#DE`。两个镜像都完整穿过 kernel/mm，无本模块回归。
- 说明：验证期间树上另一会话的 `Status::InvArgs` 改名尚未改完 `kernel/phys`、`kernel/lib/acpi`、
  `kernel/lib/obi`，整树编译不过。为了跑通构建，临时在 `status.hpp` 里加了一对旧名别名，
  验证结束后按字节校验（sha1 `785ed2f1`）还原，**未提交**。

**故意没做**

- 没有认领同一文件里第 158 行的 `Status::InvalidArguments` → `Status::InvArgs`。那是另一会话
  的全树改名，暂存时用 `git hash-object` + `git update-index --cacheinfo` 只把本节的三处
  改动放进索引，工作区里它那一处原样留着。
- 没有给 `m_node_caches` 加完备性断言，见上文「否掉的方案」，已列入 repairs。
- 没有动 `MemCache::create()`（约 626 行）里同类的 `auto status = ...; if (...)`。它没有错，
  只是这次 if-initializer 转换没扫到；不属于本次修复，也不值得单独一次构建—启动循环。
- 没有排查 `kernel.dev.dbg_1.obi` 停下来的那条 magic 断言和 `kernel.dev.obi` 的 `#DE`。
  两者都在 kernel/task，由对应会话负责。

## canary 的种子改由类型给出，`VmArea`/`VmMapping` 统一成 `verify<Self>()`（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — 删掉 `AREA_MAGIC`／`MAPPING_MAGIC`
  两个字符串常量，改由派生构造函数传 `gktl::magic<Self>()`。
- `kernel/mm/vm_area.cpp` — 构造函数改传 `gktl::magic<Self>()`，十二处 `verify(AREA_MAGIC)`
  改 `verify<Self>()`。
- `kernel/mm/vm_mapping.cpp` — 同上，构造函数与八处 `verify(MAPPING_MAGIC)`。

**症状**

启动死在 `init_vmm()` 里：

```
[... cpu0 trace] Reserve area k:head: [FFFFFF8840000000, FFFFFF8840006000)
[... cpu0 error] Assertion failed on m_magic == gktl::magic<T>() with message: Error type magic
```

lldb 经 gdbstub 取到的栈：`start_kernel` → `init_vmm` → `init_vmm_preheap` →
`entrol_preset_vma` → `VmArea::reserve()` → `VmMapping::protect()` →
`gktl::Canary::verify<VmMapping>()`。

看上去像是 `reserve()` 拿到了一个没构造过的、或者根本不是 `VmMapping` 的指针。**不是。**
对象是好的，是这句检查本身要错了数。

**原因**

同一个类型在这套代码里有**两种身份拼法**，而且它们的值不相等。

`VmAreaOrMapping` 的构造函数收一个 `u32 magic` 去播种 canary——基类必须在派生对象还不存在
的时候就把 canary 填好，这是这个参数存在的理由。原来传进去的是本类里声明的两个常量：

```cpp
constexpr static auto MAPPING_MAGIC = gktl::magic("VmMapping");
```

也就是**裸字符串字面量**的哈希。而 `Canary::verify<T>()` 算的是
`gktl::magic<T>() = magic(TypeName<T>::value())`，`TypeName` 是从 `__PRETTY_FUNCTION__` 里
把类型名抠出来的，给的是**带命名空间限定的** `ours::mm::VmMapping`。`magic()` 是 base-63
哈希，`:` 编码成 63，两者必然不同：

| 表达式 | 值 |
| --- | --- |
| `gktl::magic("VmMapping")`（对象实际携带的） | `0xe7c3c57c` |
| `gktl::magic<VmMapping>()`（`verify<Self>()` 要求的） | `0x5ebda4f5` |

两边都是 `u32`，调用点看不出任何区别——这是它能潜伏的全部原因。只要全树一直只写
`verify(MAPPING_MAGIC)`，播种和检查用的是同一个常量，自洽，永远不响；一旦有人开始把检查
改写成 `verify<Self>()`，被改到的那几处就变成必然失败。树上正好有这么一次未提交的统一，
改了 `vm_mapping.cpp` 的三处（217／248／276），同文件另外五处和 `vm_area.cpp` 的十二处没动。

`VmArea::reserve()` 之所以成为现场，只是因为 276 行的 `protect()` 是启动路径上第一个被走到的
坏检查点。`a47527c`（`reserve()` 改为经由 `mapping->protect()` 下发权限）没有错，它只是第一段
执行到这个点的代码。

**这一族坑在本仓库已经咬过两次，这是第三次**，很值得记住：

1. `gktl/type_info.hpp` 的注释记着，`TypeName` 曾经有一条把名字返回成空串的分支，于是
   `gktl::magic<T>()` 对所有 `T` 都是 `magic("")`，全内核每一次 `verify<T>()` 都是 0 和 0 相比——
   机制看着在跑，其实什么都没检查。
2. `VmObject` 曾经把类型标签和 canary 合成同一个字，`VmObjectPaged` 用 `verify<Self>()`
   去比一个装着字符串字面量哈希的 canary，即本次同样的错。它已经修好了，修法是让派生
   构造函数传 `gktl::magic<Derived>()`（见 `vm_object.hpp` 中 `enum class Type` 上方的注释，
   以及 `vm_object_paged.cpp:16`、`vm_object_physical.cpp:6`）。
3. 就是本次。讽刺的是 `VmObject` 那次留下的注释写着"这是 `VmAreaOrMapping` 已经在用的形状"
   ——而 `VmAreaOrMapping` 恰恰是没有改过来的那个。注释写的是意图，不是事实。

共同的形状是：**canary 的种子和 canary 的校验各自独立地去拼同一个类型的名字。**
只要这两条路径分开，它们就会在某次重构里错开，而错开时两边都是 `u32`，编译器不会说话。

**改法**

按用户的选择统一到 typed 写法，并且照抄 `VmObject` 已经验证过的那个形状：**删掉常量**，
让派生构造函数传 `Base(gktl::magic<Self>(), ...)`，二十处 `verify(XXX_MAGIC)` 全改
`verify<Self>()`。

**否掉的方案**：把那三行改回 `verify(MAPPING_MAGIC)`。它能让这次的启动过去，改动也最小，
但它把分歧原样留着，下一个写 `verify<Self>()` 的人会再踩一次——这已经是第三次了。

**也没有选**只把常量改成 `gktl::magic<VmArea>()` / `gktl::magic<VmMapping>()`（第一版就是这么
写的）。它同样能让两种拼法相等，但常量还在，还是两条通往同一个身份的路，仍然可以被人再写歪。
删掉之后"一个类型一个身份"是构造上成立的，不是靠约定成立的。

**参照**

本仓库内部前例：`VmObject`／`VmObjectPaged`／`VmObjectPhysical`，见上文。这比外部参照更有用，
因为它连修法带注释都已经在树里了，本次只是把漏掉的那个类补齐。

Zircon `fbl::Canary<fbl::magic("VMAR")>`：magic 是**模板非类型参数**，只有一种拼法，
不存在种子与校验各自求值的问题。它是从结构上消灭这个错，比我们靠"两处都调 `magic<T>()`"
更强一档；不过那需要 canary 的类型随宿主类型参数化，会牵动 `VmAreaOrMapping` 的布局，
这次没做。

Linux 无对应物：`struct vm_area_struct` 没有 canary，同类校验靠 `CONFIG_DEBUG_VM` 的
`VM_BUG_ON_VMA()`。

**验证**

- 构建：`prebuilt/ninja/ninja -C out/bringup.x86` 全绿。
- `clang-format -i`、`clang-tidy -p out/bringup.x86` 对两个 `.cpp` 跑过，无 error；
  `vm_area_or_mapping.hpp:226` 的 `modernize-use-override` 是析构函数上的既有告警，
  只是因为我加的注释而行号平移。
  注意 `clang-format` 作用于该头文件时会顺带重排 include 块并改写另外两处与本次无关的
  `[[nodiscard]]` 换行，已还原，只保留本节的改动。
- QEMU 实测，两个镜像都**不再出现任何 `Error type magic`**（`grep -c` 为 0），
  且都完整穿过 `init_vmm()`：
  - `kernel.dev.dbg_1.obi`：越过 VMA 预置、irq、HPET 时钟源、APIC timer 标定，进入
    `task::init()`，两条 `Allcoated Kernel stack` 之后停在
    `thread->state() & (Parked | Sleeping)` 断言上。
  - `kernel.dev.obi`：同样越过 `init_vmm()`，停在调度器一侧的 `#DE`。
  两处都在 kernel/task，由该会话负责（对方的 `3892d65` / `4fe4b82` 此前从未实测过）。

**故意没做**

- 没有认领 `vm_area.cpp` / `vm_mapping.cpp` 里另一会话的 `Status::InvalidArguments` →
  `Status::InvArgs` 改名。暂存时用 `git hash-object` + `git update-index --cacheinfo`
  只把 canary 相关的改动放进索引。
- 没有动其他用 canary 的类。`PmNode`／`PmZone` 用 `m_canary(this)`（`Canary(T *)` 推导出
  同一个 `magic<T>()`），`VmAspace` 用 `gktl::Canary m_canary{this}`，`VmObject` 一族已经修过，
  它们本来就自洽。
- 没有改成 Zircon 那样把 magic 做成模板参数，理由见「参照」。
- 没有排查后面 kernel/task 的两个停点。
