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

## `reclaim_init_area()` 和它的调用者搬出 `.init` 段（2026-09-03）

**文件**
- `kernel/mm/early_mem.cpp` — `EarlyMem::reclaim_init_area()` 去掉 `INIT_CODE`。
- `kernel/mm/init_pmm.cpp`、`kernel/mm/include/ours/mm/init.hpp` — 未被调用的包装
  `free_init_area()` 同样去掉。
- `kernel/top/main.cpp` — `labour_routine()` 去掉 `INIT_CODE`，并改写那句说反了的注释。

**症状**

**潜伏，当前触发不到。** 启动停在更早的地方（见下文「验证」），根本走不到这里。
这一条是静态发现的，不是从崩溃回溯出来的。

触发条件：只要执行流真的走到 `labour_routine()` 里的 `reclaim_init_area()`，
它就会把自己正在执行的那段代码交还给伙伴分配器。

**原因**

`reclaim_init_area()` 把 `.init*` 整段（镜像内 `[__init_start, __init_end)`）还给伙伴分配器。
问题是**做这件事的代码自己就住在这段里**：

```
__init_start        = 0x7e000
__init_end          = 0x9d000
labour_routine()    = 0x7e220   <- 段内
reclaim_init_area() = 0x84140   <- 段内
```

（改动前 `llvm-nm` 的实测值。）

释放的那一刻**不会出任何事**——`free_frames()` 只改伙伴分配器的记账，页表映射原封不动，
CPU 继续从同样的虚拟地址取指，取到的还是原来的字节。窗口一直开着，直到**第一次把这些
页重新分配出去并写入的操作**，才把仍然要返回穿过的指令覆盖掉。`labour_routine()` 在
`reclaim_init_area()` 之后还要执行两次 `Thread::spawn()`，而线程内核栈就是紧接着的下一次
分配——最可能的覆盖者。写坏之后既不缺页也不 `#GP`，只是执行流走进被改写的字节，
表现为"没有断言、没有异常、QEMU 还活着、输出停了"。

这也是"释放正在使用的内存"这类错误的共性：**释放本身是无声的，代价由下一个使用者付。**
把它当成"释放时会崩"来找，永远找不到。

原来那句注释写反了：

```cpp
// Ok, now there is no any routine to access the `init` area.
```

写这句话的那一行本身就在 `init` 区里。注释说的是"没有别的例程还要用它"，漏掉了
"正在跑的这个例程"。

**改法**

把两端都搬出去：`reclaim_init_area()` 不再是 `INIT_CODE`，`labour_routine()` 也不再是。
后者是关键——即使 `reclaim_init_area()` 搬走了，返回地址仍然指回 `labour_routine()` 段内的
指令，问题原样存在。`init_arch()` / `init_platform()` 保持 `INIT_CODE` 不变：它们是被调用的，
在释放发生之前就已经返回，栈上不再有它们的帧。

顺带确认了释放之后仍被引用的 `.init` 内容是安全的：`CpuLocal` 的链接期模板确实在
`.init` 里，但按 `cpu_local.cpp` 里 `canonical_base()` 的注释，它只作为拷贝源和地址原点，
`init_early()` 之后不再被读写；`labour_routine()` 里 spawn 出去的 `gibberish` lambda 的
thunk 经 `llvm-nm` 确认落在普通 `.text`（`0x7e40`），不在 `[0x7e000, 0x9d000)` 内。

**否掉的方案**：把 `reclaim_init_area()` 的调用挪到某个非 init 的辅助函数里。没用——
调用者 `labour_routine()` 还是要被返回到。真正的条件是「释放发生时，栈上不能有任何
`.init` 帧」，这只能由调用链本身满足。

**参照**

`Linux free_initmem()`（`init/main.c`、各架构的 `arch/*/mm/init.c`）。它**不是** `__init`，
调用它的 `kernel_init()` 也不是 `__init` 而是 `__ref`——所有 `__init` 的活都塞在
`kernel_init_freeable()` 里，先返回，再释放。分界和这次完全一致，值得对照着看一遍。

Zircon 无对应物：它不回收 init 代码段。

**验证**

- 构建通过；`clang-format`、`clang-tidy -p out/bringup.x86` 对四个文件跑过，无 error。
- **静态验证**（这是本条的主要证据）：改后 `llvm-nm` 显示
  `labour_routine() = 0x7e30`、`reclaim_init_area() = 0x2be20`，都落在 `.text`
  （`0x6000`–`0x3f3bb`）内，已在 `[__init_start=0x7e000, __init_end=0x9d000)` 之外。
- **运行时未实测**，因为走不到：启动停在更早的地方。用临时 `log::trace` 打点定位到
  确切位置（打点已全部还原，未提交）：
  1. `init_platform()` 正常返回；
  2. `set_init_level(InitLevel::Platform)` 跑该级别的 init hook，
     `free_bootstrap16_buffer()` 释放 D000/E000/F000 三页后返回；
  3. `prof_init()`（`kernel/commands/prof.cpp:88`）进入，打印
     `eh size=28600`，随后**卡死在 `build_fde_index()` 里**，不再有任何输出。
  把 `free_bootstrap16_buffer()` 临时改成空操作，卡死点不变，确认与那三次释放无关。
  所以 `reclaim_init_area()` 这条路径当前不可达，本条改动只有静态证据。

**故意没做**

- 没有碰 `prof_init()` / `unwind::FdeIndex::build()` 的死循环。那是 `kernel/lib/prof` 与
  unwind 库的事，且该模块树上有另一会话的大量未提交改动。已单独记入
  `docs/repairs/260903/mm.md` 作为交叉引用（它挡住了 mm 这条路径的运行时验证）。
- 没有动 `free_bootstrap16_buffer()`。它用 `EarlyMem::allocate()` 拿的页、用
  `mm::free_frames()` 还给伙伴分配器，而且还完之后 `create_bootstrap16()` 建的那两处
  恒等映射并没有解除——这两点都值得看，但都不是当前的卡死点，一并记入 repairs。
- `start_kernel()` 仍是 `INIT_CODE`。它不返回（末尾 `become_idle()`），idle 线程栈上那个
  指回 `.init` 的返回地址永远不会被用到。

---

## 2. VMA 的范围遍历改用 B 树查找，并实现 `VmaSet::Enumerator`（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — 给 `Enumerator` 补上接口；改正 `lower_bound` 的错误注释
- `kernel/mm/vm_area_or_mapping.cpp` — 实现 `Enumerator` 的两个构造函数与 `next()`
- `kernel/mm/include/ours/mm/vm_area.hpp` — `can_protect_locked` 去掉 `const`；加 `friend class VmaSet`
- `kernel/mm/vm_area.cpp` — `find_mapping_locked`、`unmap_locked`、`protect_locked`、`can_protect_locked` 四处改走 `Enumerator`

**症状**

潜伏，当前规模下量不出来。`VmaSet` 是一棵以 `base` 为键、带 `GapAugment` 增广的 B 树，
但四个范围操作全都从 `begin()` 线性扫到底。根 VMA 现在只有 9 个子节点，所以看不出问题；
`find_mapping_locked()` 在缺页热路径上，复杂度要按真实地址空间算。

另有一处不是性能而是正确性：`unmap_locked()` 一边遍历一边销毁完全覆盖的映射，靠"先前进
再销毁"来保护迭代器。

**原因**

B 树把两件事都给了：`lower_bound()` 是 O(log n) 的定位，而且同一个文件里的 `has_range()`
已经写出了正确的区间查找写法——先 `lower_bound(base)` 拿到第一个 `base >= 目标` 的，再回退
一个看它是否 `end() > base`（跨界进来的）。四处调用点各自手写扫描，等于把这棵树当链表用。

`lower_bound()` 的文档注释写的是"Find the first existent VMA whose end < addr"，而
`lower_bound_node()` 的实现是标准语义"第一个 base >= addr 的"。注释描述的是另一个函数。
这多半就是没人用它的原因——照着这句注释根本推不出该怎么用。

"先前进再销毁"这条对 B 树不成立。`VmAreaOrMapping::destroy()` 会 `m_parent->m_subvmas.erase(*this)`，
而 `VmaSet::erase()` 调 `m_tree.erase(base)`。B 树的删除会重平衡节点，**树里任何位置的迭代器
都可能失效**，不只是指向被删元素的那个。侵入式链表里"提前前进一格"是充分的，树里不是。

**改法**

把遍历收进 `VmaSet::Enumerator`——这个类之前只有声明，两个构造函数和成员都在，就是没有实现，
连 `next()` 都没声明。构造函数用 `has_range()` 那套定位，`next()` 在交出元素之前先前进迭代器，
并保证交出的每个元素都真正与窗口相交，调用方不必再判一次。

两种走法分开：

- `protect_locked` / `can_protect_locked` / `find_mapping_locked` 只读，一个 enumerator 走到底，
  O(log n + k)。
- `unmap_locked` 会销毁子节点，所以每一步重新定位一次，O(k log n)。销毁前先把 `cursor` 推到
  `child->end()`——子节点与 `[cursor, end)` 相交意味着它的 end 严格大于 cursor，循环一定推进。
  这是"边走边删"的代价，不是可以省的。

`can_protect_locked` 从 `const` 改成非 `const`：`Enumerator` 只接受可变的 `VmaSet`。它其实什么
都不改，等到第二个只读区间遍历出现时再给 `Enumerator` 补 const 形式。

**参照**

`Linux find_vma()`（`mm/mmap.c`）——同样是先在 maple tree / rbtree 里定位再走，而不是从头扫。
`Zircon VmAddressRegion` 的 `subregions_` 是 `WAVLTree`，`FindRegionLocked()` 用 `upper_bound`
再回退一格，回退的理由和这里完全一样。

**验证**

构建通过；`clang-format` 与 `clang-tidy -p out/bringup.x86` 对四个文件跑过，无新告警。
QEMU 实测无回归，九个预置 VMA 照常建立。

遍历本身启动路径走不到，所以用一个临时探针在 `dump_locked()` 里实测后撤掉，`kernel.dev.dbg_1.obi`：

```
PROBE full-window: enumerator=9 tree=9      全窗口枚举出全部子节点
PROBE straddler:   got=k:init want=k:init   窗口开在最后一个子节点内部一字节，仍能接住它
PROBE first-byte:  got=k:physmap            单字节窗口精确命中
```

第二条是关键——只用 `lower_bound` 不回退的写法会在这里悄悄漏掉一个子节点。

`unmap_locked()` 的重新定位循环**未实测**，启动路径不解除任何映射。

**故意没做**

- 没给 `Enumerator` 补 const 形式，见上。
- 没有动 `dump_locked()`，它本来就要走全部子节点，线性是对的。
- 探针顺带发现根 VMA 的 `end()` 会溢出（`0xFFFFFF8000000000 + 0x8000000000` 正好是 2^64，
  回绕成 0）。`contains()` 和 `check_range()` 都是用偏移量算的、不受影响，但 `end()` 本身是
  `m_base + m_size`。没有在这一条里改，记在 `docs/repairs/260903/mm.md`。

---

## 3. 遍历只在真正删除时才重新定位；`seek()` 收拢三处重复（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — 新增 `VmaSet::seek()` 与 `erase(IterMut)`
- `kernel/mm/vm_area_or_mapping.cpp` — 实现两者；`has_range()` 与 `Enumerator` 构造函数改用 `seek()`
- `kernel/mm/vm_area.cpp` — `unmap_locked()` 改为携带单个迭代器

**症状**

第 2 条里的 `unmap_locked()` 是 O(k log n)：每处理一个子节点就构造一个 `Enumerator`，
也就是重新 `lower_bound` 一次。但**只有真正销毁子节点的那一步需要重新定位**，
下降到子区域、或者只裁剪部分覆盖的映射，都不会从集合里删任何东西。

**原因**

先说一个查证结果，因为它否定了本来打算做的优化。`ustl` 的 B 树确实有
`erase(iterator) -> iterator`，签名和 `std::map` 一样，看着像是 O(1) 摊还的删除，
实际实现是：

```cpp
auto next = pos; ++next;
next_key.emplace(key_of(*next));
erase(key);                          // 按键删，O(log n)
return next_key.has_value() ? lower_bound_node(*next_key) : end();  // 再查回去
```

它就是把"记下后继的键 → 删 → 重新查找"包了一层。所以**删除那一步降不下来**，
O(log n) 是这个容器的下限，除非改 `ustl` 本身。

但这不妨碍另一半：不删除的步骤本来就不需要重新定位，`++` 在 B 树上是摊还 O(1)
（叶子内部是 `slot + 1`，跨节点才向上爬，而一趟 k 步的中序遍历每条边最多走两次）。
第 2 条那版对**每个**子节点都重新定位，等于为不删除的步骤白付 O(log n)。

顺带发现"先 `lower_bound` 再回退一格接住跨界者"这段逻辑已经被抄了三份：
`has_range()`、`Enumerator` 的构造函数、以及 `unmap_locked()` 里那个 `cursor`。
回退那一格是最容易漏掉的半句，漏掉不会报错，只会静悄悄少走一个区域。

**改法**

`VmaSet::seek(addr)` 收拢那段定位逻辑，三处都改用它；`has_range()` 因此从两个分支
塌成一个——`seek()` 返回的已经包含跨界者，剩下只需判断它是否在窗口结束之前开始。

`VmaSet::erase(IterMut) -> IterMut` 删除并交回后继位置。`unmap_locked()` 于是只持有
一个迭代器：不删除的步骤走 `++`，只有销毁完全覆盖的映射时才付那次 O(log n)。

销毁顺序是先出集合再 `destroy()`：只有集合能说出walk该从哪儿继续，`destroy()` 报不了。
出集合会掉一个引用、而那可能是最后一个，所以先 `retain()` 撑住，`destroy()` 完再
`release()`。`destroy()` 内部还会再删一次，但 `m_tree.erase(key)` 返回 0，
`VmaSet::erase()` 只在返回非 0 时才 `release()`，所以不会重复释放。

**参照**

`std::map::erase(iterator) -> iterator` 是这个接口的原型，区别在于标准库的实现是真的
O(1) 摊还。`Linux vma_iter_*`（`mm/vma.c`）在 maple tree 上同样是"删除时重新定位、
其余步骤沿游标走"。

**验证**

构建通过，`clang-tidy` 无新告警，QEMU 无回归。

遍历启动路径走不到，仍用临时探针实测后撤掉。这次直接测新原语：把 9 个真实子节点插进一个
临时 `VmaSet`（`insert()` 加引用、`erase()` 减引用，对真实对象净变化为零），
然后沿 `erase(IterMut)` 返回的后继一路删下去。`kernel.dev.dbg_1.obi`：

```
PROBE seek/erase: inserted=9 first=k:physmap erased=9 left=0 ordered=1
PROBE enumerator: walked=9 tree=9
```

`erased=9 left=0` 且 `ordered=1`（基址严格递增）说明后继在重平衡之后依然有效，
既没有走丢也没有重复。

`unmap_locked()` 的完整路径仍**未实测**——启动不解除任何映射。这次测的是它依赖的两个原语。

**故意没做**

- 没有去改 `ustl` 的 `erase(iterator)`。让它成为真正的 O(1) 摊还删除，需要节点算法在重平衡
  过程中维持一个有效的后继位置，那是共享容器里一处精细改动，属于 ustl 的范围，
  记在 `docs/repairs/260903/mm.md`。

---

## 4. 给第 2、3 条补上实测（2026-09-03）

**文件**
- `kernel/mm/tests/vma_walk_bench.cpp` — 新增宿主基准
- `kernel/mm/tests/BUILD.gn`、`BUILD.gn` — 接进构建

**症状**

第 2、3 条都标了 `perf`，却只有复杂度推导，没有一个数字。复杂度推导会漏掉常数，
而常数在 n 很小的时候就是全部——这个内核的根 VMA 现在只有 9 个子节点。

**改法**

`kernel/mm/tests/vma_walk_bench.cpp`，跟着 `frame_buddy_smoke.cpp` 的宿主测试约定走。
它不建在 `VmaSet` 上——那会拖进 `VmAreaOrMapping`、地址空间和 canary，而要测的是树。
容器、键类型、键提取器的形状相同。真实的 `VmaSet` 还带 `GapAugment`，基准没带：
增广让插入和删除各多一点开销，对遍历没有影响。

宿主 arm64、`-O2`，内核是 x86-64。**比值可以搬，绝对值不能。**

```
     n | operation                    |    before      after  speedup
-------+------------------------------+--------------------------------
     8 | point lookup (fault path)    |      15.1       17.0    0.89x
     8 | range walk (8 areas)         |      23.6       21.3    1.11x
     8 | 8 steps: re-seek vs ++       |      52.9       25.0    2.12x
     8 | erase all (per area)         |      99.0       46.9    2.11x
    64 | point lookup (fault path)    |      61.5       31.7    1.94x
    64 | range walk (8 areas)         |      85.4       51.6    1.66x
    64 | 8 steps: re-seek vs ++       |     127.9       58.2    2.20x
    64 | erase all (per area)         |      54.7       40.4    1.36x
   512 | point lookup (fault path)    |     374.7       40.0    9.36x
   512 | range walk (8 areas)         |     540.4       79.2    6.82x
   512 | 8 steps: re-seek vs ++       |     222.8       79.0    2.82x
   512 | erase all (per area)         |      33.7       28.2    1.20x
  4096 | point lookup (fault path)    |    3859.0       65.4   59.03x
  4096 | range walk (8 areas)         |    3333.5       68.0   49.01x
  4096 | 8 steps: re-seek vs ++       |     292.2       70.5    4.15x
  4096 | erase all (per area)         |      20.3       17.7    1.15x
```

**结论，包括打自己脸的那条**

**第 2 条（扫描改查找）在当前规模上没有收益，甚至略亏。** n=8 时点查找是 0.89×——
比原来慢一点点。原因不神秘：查找要下降到叶子再回退一格，扫 8 个元素反而更便宜。
分水岭在 8 和 64 之间，到 512 才有 9.4×，4096 有 59×。

所以第 2 条那句"最要紧的是 `find_mapping_locked`，因为它在缺页热路径上"，
**渐进意义上对，当下不对**。热路径这个事实放大的是未来的规模，不是今天的 9 个子节点。
这个改动仍然该做——树就是为真实地址空间准备的，而不是为 9 个内核 VMA——但它是笔投资，
不是当期收益。这一点上一条没写清楚。

**第 3 条（只在删除时重新定位）在每个规模上都成立。** 每步 2.12×–4.15×，n=8 也有 2.12×。
它不依赖 n 变大，因为它省掉的是每步一次树下降，而不是一次线性扫描。

**还有一条也是我说错的**：第 3 条断言 `erase(iterator)` 因为内部是重新查找，所以
"删除那一步降不下来"。实测是 1.15×–2.11×，确实更快。手写那版要 `seek()`（下降加回退判断）
再 `erase(key)`（又一次下降）；库里那版是 `++`（廉价）加 `erase(key)` 加一次 `lower_bound`。
两边都是两次下降，但库里省掉了回退判断，也少一层 `Option` 包装。
比不上真正的摊还 O(1)，但不是零。

**故意没做**

- 没有因为 n=8 的 0.89× 回退第 2 条。理由见上：那是规模投资。但从此该在提交里说清楚
  "现在不快，将来快"，而不是笼统写 perf。
- 没有测 `VmaSet` 本身（含 `GapAugment`）。要测得把 `VmAreaOrMapping` 拆得能在宿主机上构造，
  那是另一件事。
- 没有在内核里测。宿主比值足以支撑"曲线怎么走"，而这正是这几条改动的论点。

---

## 5. 基准移到 `benches/`（2026-09-03）

**文件**
- `benches/kernel/mm/vma_walk_bench.cpp` — 由 `kernel/mm/tests/` 迁入
- `benches/BUILD.gn`、`benches/kernel/mm/BUILD.gn` — 新增
- `kernel/mm/tests/BUILD.gn`、`BUILD.gn` — 撤掉旧目标，根改指 `//benches:benches`

**原因**

第 4 条把基准放进了 `kernel/mm/tests/`，跟着那里已有的宿主测试约定走。放错了：
仓库顶层的 `benches/` 就是给基准准备的。它当时只有一个空 `.gitignore`（Bazel 迁 GN 时留下的），
看着像没人用，但空目录在这个仓库里不代表没用途——和那些空类、桩声明一样，是留好的位置。

区分标准是**程序输出什么**，不是它测的代码在哪：`tests/` 下的目标非通过即失败，
基准打印的是时间，所以即使测的是 mm 的代码，也该待在 `benches/`。

**改法**

`benches/` 下按源码树镜像，`benches/kernel/mm/vma_walk_bench.cpp` 与它测的代码同路径，
以后加别的模块不必再定规矩。`benches/BUILD.gn` 提供一个 `benches` group，
根 `BUILD.gn` 只引这一个标签，不再逐个点名。

group 存在的意义是让基准保持可编译——烂掉的基准应该由构建发现，
而不是由下一个需要这组数字的人发现。

**验证**

`gn gen` 通过，`ninja host/obj/benches/kernel/mm/vma-walk-bench` 构建并运行，
输出与第 4 条同量级（n=8 点查找 0.98×、每步 2.25×，run-to-run 抖动之内）。全量构建无错。

## 6. `PageQueues` 真正持有页：入队是"移动"而不是"插入"，并补上出队（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `page::VmmBody` 增加 `u8 queue`，
  以及 `PageFrame::page_queue()` / `set_page_queue()`
- `kernel/mm/include/ours/mm/page_queues.hpp` — `QueueType` 定宽到 `u8`；
  `mrugen_to_queue_type()` / `lrugen_to_queue_type()` 换成
  `reclaimable_queue(gen)` + `mru_queue()` / `lru_queue()`；新增公开的 `remove()`
  与私有的 `dequeue_locked()`
- `kernel/mm/page_queues.cpp` — `set_queue_backlink_locked()` 先出队再入队；
  实现 `remove()` 与 `dequeue_locked()`

**症状**

潜伏，当前启动触发不到——没有任何调用方驱动这三个 setter。用临时探针驱动之后立刻暴露：
把一个已经在 Anonymous 队列上的页改填到 Pinned 队列，第二次调用当场 `#UD`。

```
[info ] probe[filed 4 anon / 2 pinned / 2 reclaimable]: anon list/size=4/4 pinned=2/2 mru(gen 0)=2/2
[info ] probe: backlink of page 3 = 0x1003 pgoff 3
[error] #UD Invalid opcode: vector=6, error_code=0x0
[error]   rip=0xffffff8840030485 (+0x30485)  cs=0x10  rsp=0xffffff884007e210
```

另有一处更晚才会发作：`ReclaimableStart + m_mrugen` 没有取模，`m_mrugen` 是自由递增的
`u32`，而环只有 8 个槽，所以第 8 次轮转之后下标就走出了 `m_queues` 的末尾。

**原因**

三件事，一件比一件深。

**其一，帧描述符只有一个链表钩子。** `PageFrame::m_hook` 是所有角色共用的一个
`ListMemberHook`——pm_frame.hpp 里已经写明了理由：一个帧同一时刻要么在伙伴分配器的空闲
链表上，要么在页队列上，要么在 slab 的 partial 链表上，三者互斥，所以一个钩子够了，而且
指向联合体成员的成员指针在语言上不成立，钩子也放不进 `VmmBody`。

一个钩子的直接后果是：**"把页放到队列 X 上"永远是一次移动，不可能是一次插入**。原来的
`set_queue_backlink_locked()` 无条件 `push_back`：对一个钩子还挂在别的链表上的节点做插入，
新链表的 `prev/next` 被写进这个节点，而旧链表的前后邻居仍然指着它——两条环形链表就此穿到
一起，谁遍历谁走丢。上面那个 `#UD` 就是侵入式链表的自检断言编译成 `ud2` 后被撞上的。

**其二，"页当前在哪条队列上"没有地方记。** 侵入式链表的节点无法反查自己的容器，所以既然
入队要先出队，就必须在描述符里记下这一位。这也是 `m_sizes` 能维护的前提：钩子是
`AutoUnlink` 模式，`unlink()` 不经过链表头，减不了任何计数器；而 `m_queues` 声明成
`ConstantTimeSize<false>`（伙伴分配器要 O(1) 的 splice），Boost 系的侵入式容器本来就禁止
auto-unlink 与缓存长度并存，正是因为 unlink 够不到头。计数只能由这里手工维护，而手工维护
需要知道要减哪一个。

Zircon 的 `vm_page_t::object.page_queue` 就是同一个字节，理由完全相同。

**其三，只有入口没有出口。** 三个 setter 会把一个裸 `VmCowPages *` 写进描述符，而没有任何
接口能把页摘下来。页一旦被还回伙伴分配器，描述符里既留着一个可能已经析构的所有者指针，
链表里也还留着一个节点，指向一段马上会被别的角色 `become()` 掉的存储。所以 `remove()`
不是"顺手加的功能"，它是入队的另一半；缺了它，入队本身就是错的。

**改法**

`VmmBody` 加一个字节 `queue`，取值就是 `QueueType` 的枚举值。`QueueType::None` 是 0，
而 `become<VmmBody>()` 会把整个 payload 清零，所以"刚变成 VMM 页的帧未入队"这件事不需要
任何人显式写一次。`VmmBody` 原本 17 字节（对齐后 24），加一字节仍在 `BODY_SIZE = 32` 之内，
现有的 `static_assert` 覆盖了这一点。

字段用普通 `u8` 而不是 `AtomicU8`：它的每一次读写都在 `PageQueues::m_lock` 之下，做成原子
只会对外宣告一个并不存在的无锁协议。`VmmBody` 必须是平凡可复制、平凡可析构的，普通 `u8`
和原子都满足，所以这里是纯粹的语义选择。

`set_queue_backlink_locked()` 开头无条件调 `dequeue_locked()`。无条件而不是断言，是因为
"重填一个已经在队列上的页"是正常路径而不是异常路径——老化和 `mark_accessed()` 做的就是这件事。

`dequeue_locked()` 在 `queue == None` 时直接返回。这条早退不是省事，而是安全边界：帧可能正
挂在伙伴分配器的空闲链表上（同一个钩子），此时把它 `unlink()` 走就是从别人的链表里偷节点。

`remove()` 额外把 backlink 清空（`vmo = nullptr`、`pgoff = 0`），对齐 Zircon
`PageQueues::Remove()` 的做法。`dequeue_locked()` 不清，因为 setter 紧接着就要覆盖它。

`ReclaimableStart + gen` 改成 `ReclaimableStart + gen % NUM_RECLAIMABLE`，收进
`reclaimable_queue(u32)` 一个函数里。顺带修掉 `mrugen_to_queue_type()` 读 `m_lrugen` 的
复制粘贴错误——两个访问器读的是同一个计数器，MRU 与 LRU 因此永远指向同一条队列，代际方案
从一开始就不可能成立。名字也一并换掉：它们返回队列而不是"把代数转成类型"。

**被否掉的方案**：给 `m_queues` 换一套 `ConstantTimeSize<true>` 的链表类型，用链表自己的
长度代替 `m_sizes`，这样就不需要在描述符里记队列。否掉的原因是它和 `AutoUnlink` 互斥，而
`AutoUnlink` 是 `Slab::destroy()` 依赖的性质（它在不知道页挂在哪条链表上的情况下摘页），
改不动。

**参照**

- `Zircon PageQueues::SetQueueBacklinkLocked()` 与 `PageQueues::MoveToQueueLocked()`
  （`zircon/kernel/vm/page_queues.cc`）：同样是"先从旧队列摘下、再挂到新队列"，同样把队列
  编号存在页描述符里（`vm_page_t::object.page_queue`）。
- `Zircon PageQueues::Remove()`：清 backlink 的那两行就是照它写的。
- Linux 侧对应的是 `mm/swap.c` 的 `lruvec_add_folio()` / `lruvec_del_folio()`，但 Linux 的
  `folio->lru` 是专属钩子而不是与伙伴分配器共用的那一个，所以它没有"入队必须先出队"这条约束，
  参照价值不如 Zircon。

**验证**

构建通过（`kernel.dev.obi` 与 `kernel.dev.dbg_1.obi`），两个镜像 QEMU 实测启动到与改动前
相同的位置，无回归。`clang-format` 与 `clang-tidy -p out/bringup.x86` 对三个文件跑过，
新增行无告警。

没有调用方驱动这些队列，所以启动本身证明不了任何事。证据来自一个临时的内核内探针
（`GKTL_INIT_HOOK` 挂在 `InitLevel::Heap`，8 个静态 `VmPage`，一个独立的 `PageQueues`，
提交前已删除）：

```
[info] probe[filed 4 anon / 2 pinned / 2 reclaimable]: anon list/size=4/4 pinned=2/2 mru(gen 0)=2/2
[info] probe: backlink of page 3 = 0x1003 pgoff 3
[info] probe[moved page 0 anon -> pinned]: anon list/size=3/3 pinned=3/3 mru(gen 0)=2/2
[info] probe[removed all]: anon list/size=0/0 pinned=0/0 mru(gen 0)=0/0
[info] probe: backlink of page 3 after remove = 0x0 pgoff 0
[info] probe: page 3 still linked = false
```

`list/size` 两个数是分别得来的：`list` 是把那条侵入式链表从头走一遍数出来的，`size` 是
`m_sizes` 里的计数器。两者一致才说明链表本身没有被穿坏——只看计数器是看不出链表已经错乱的。
把 `dequeue_locked()` 那一行注释掉（即 HEAD 的行为）重新构建，第二次 `set_pinned` 就是上面
「症状」里那个 `#UD`，探针的第三行永远打不出来。

**故意没做**

- **没有做 host 单元测试。** `PageQueues` 直接坐在 `sync::SpinLock` 上，而这把锁的四个方法
  都是 out-of-line 定义在 `spin_lock.cpp` 里、依赖 `mm::CpuLocal::current_cpu()` 与
  `GKTL_COUNTER`，`Guard<SpinLock, IrqSave>` 又要求 `arch::AutoInterruptDisabler` 是完整类型。
  本机是 arm64，`-DARCH_X86` 下 `arch/x86/interrupt.hpp` 的 `cli/pushfq` 内联汇编汇编不过，
  不带 `-DARCH_X86` 时 `arch/interrupt.hpp` 的兜底分支只声明了不完整的 `ArchCpuState`，
  `AutoInterruptDisabler` 的成员就没法定义。要让它上 host，得先把 `PageQueues` 与这把锁解耦，
  那是另一件事。已记进 `docs/repairs/260903/mm.md`。
- **没有让任何生产代码调用这三个 setter。** VMO 侧的页仓库（待修第 1 条）还没建起来，
  现在接上去只会往队列里塞一批没人认领的页。

---

## 6. 区间比较改用闭区间末地址，`end()` 换成 `last()`（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — `end()` 换成 `last()`；`GapAugment`
  的 `m_max_end` 换成 `m_max_last`，接缝计算随之改写；`Enumerator` 的窗口改成闭区间
- `kernel/mm/vm_area_or_mapping.cpp` — `seek()`、`has_range()`、`Enumerator::next()`、
  `find_spot()` 的空隙记账全部改成闭区间形式
- `kernel/mm/vm_area.cpp` — `unmap_locked`、`protect_locked`、`can_protect_locked`、
  `find_mapping_locked` 四处窗口算术改成闭区间

**症状**

`VmAreaOrMapping::end()` 是 `m_base + m_size`。内核根 VMA 是
`[0xFFFFFF8000000000, 0x8000000000)`，两者相加**正好**是 2^64，无符号回绕成 0，
所以 `root_vma->end()` 恒为 0。

后果不是算错一位，是整个窗口消失。`Enumerator::next()` 的停止条件是
`area->base() >= m_max`，而任何"整个根区域"的窗口传进来的 `m_max` 就是这个 0，
于是第一个子节点就满足停止条件——**枚举返回零个子节点，不报错**。
本次实测（`kernel.dev.obi`，临时探针，已撤掉）在同一棵树上把新旧两种写法并排跑：

```
PROBE base=0xFFFFFF8000000000 size=0x8000000000 last=0xFFFFFFFFFFFFFFFF
PROBE full-window: enumerator=9 tree=9
PROBE old exclusive-end form: end=0x0 walked=0
```

`walked=0` 而树里有 9 个，就是这条缺陷的全部形状。

当前没有真实调用方拿整个根区域当窗口，所以它还是潜伏的；但地址空间转储、拆除记账、
范围 commit/decommit 都天然要这么写，第一个这么写的人会拿到一个"这个地址空间是空的"
的答案，而且一路无声。

**原因**

问题不在 `end()` 这一个函数，在"用开区间末地址描述一个能顶到地址空间顶端的区间"这件事本身。
地址空间的顶端没有"再往后一个地址"可言：`base + size` 要么溢出，要么需要一个比 `VirtAddr`
更宽的类型来存。而 `end()` 只是这种算术里最显眼的一处——`unmap_locked()`、`protect_locked()`
里各自写着的 `VirtAddr const end = base + size;` 是同一个表达式，只是没写成成员函数；
`VmArea::protect(0, m_size)` 打到根 VMA 上，回绕的是那一句，不是 `end()`。所以把 `end()`
改成饱和到 `VirtAddr(-1)` 治不了本：调用点自己算的窗口末地址照样回绕，而且饱和还会悄悄
丢掉最后一个字节——地址空间最顶上那一页从此不可寻址，没有任何地方会说一声。

闭区间的末地址 `base + size - 1` 则对任何非空区间都不溢出，因为它至多等于 2^64 - 1，
恰好是 `VirtAddr` 能表示的最大值。而"非空"是这一层本来就有的不变量：
`create_subaom_preamble()` 在区域存在之前就把零长度请求挡掉了，所以 `last()` 可以直接
`DEBUG_ASSERT(m_size)`，把这个前提写死在代码里而不是留在注释里。

**改法**

`end()` 删掉，换成 `last()`（区间内最后一个地址）。所有跨对象的区间比较改写：

| 旧（开区间） | 新（闭区间） |
| --- | --- |
| `child->base() >= end` | `child->base() > last` |
| `min(child->end(), end)` | `min(child->last(), last)` |
| `overlap_end - overlap_base` | `overlap_last - overlap_base + 1` |
| `mapping->end() <= end` | `mapping->last() <= last` |
| `(*previous)->end() > addr` | `(*previous)->last() >= addr` |
| `Enumerator{set, va, va + 1}` | `Enumerator{set, va, va}` |

**包含性判断没有跟着改**：`contains()`（`addr - m_base < m_size`）和 `check_range()`
本来就是偏移量形式，同样不回绕，而且"这个地址是不是我的"用偏移写更直白。这两半的分工
是有意的：偏移形式回答"属于谁"，闭区间末地址回答"两段区间怎么交叠"——后者要拿两个不同
基址的对象比大小，改成偏移就得先归一到同一个原点，读起来不再像地址。

`GapAugment` 的聚合值 `m_max_end` 一并改成 `m_max_last`。接缝长度原来是
`right.m_min_start - left.m_max_end`，现在写成
`right.m_min_start - left.m_max_last - 1`，条件仍是 `right.m_min_start > left.m_max_last`。
**没有**写成更直观的 `left.m_max_last + 1 < right.m_min_start`——那个 `+1` 正是要躲的回绕。
相邻两段算出接缝 0，而请求长度恒大于 0，所以不需要为相邻单开一个分支。

`find_spot()` 的 `m_prev_end` 语义保持"已看过的部分之后第一个空闲地址"（空隙的头必须是
开区间形式，它本来就是个可用地址而不是已占用地址），但推进改走新的 `advance_past(last)`：

```cpp
auto advance_past(VirtAddr last) -> void {
    auto const first_free = last < m_high_limit ? last + 1 : m_high_limit;
    if (first_free > m_prev_end) {
        m_prev_end = first_free;
    }
}
```

这里顺手补掉了一个同源的潜伏缺陷：原来是 `m_prev_end = area->end()`，如果某个区域
一直占到地址空间顶端，`end()` 回绕成 0，之后每一次空隙判断都是**从地址空间底部量起**的，
`fits()` 会愉快地返回一个早就被占着的地址。现在顶到 `m_high_limit` 上，`fits()` 自己
会以"起点不低于上限就放不下任何非空请求"把后续全部否掉。

**参照**

`Zircon VmAddressRegion::UnmapInternalLocked()`（`zircon/kernel/vm/vm_address_region.cc:873`）
——它的 Zircon 内核地址空间是同一组数字（`KERNEL_ASPACE_BASE 0xffffff8000000000`、
`KERNEL_ASPACE_SIZE 0x0000008000000000`），处理方式就是本条采用的这一种：

```cpp
vaddr_t end_addr_byte = 0;
DEBUG_ASSERT(size > 0);
bool overflowed = add_overflow(base, size - 1, &end_addr_byte);
ASSERT(!overflowed);
for (auto itr = subregions_.IncludeOrHigher(base);
     itr.IsValid() && (*itr).second->base() <= end_addr_byte; ++itr) {
```

连"非空所以加法不会溢出"这条断言都对得上。分工也一样：
`Zircon VmAddressRegion::is_in_range()`（`vm_address_region.h:142`）是偏移形式
`base >= base_ && offset < size_ && size_ - offset >= size`，和我们的 `check_range()`
逐字同构，负责包含性；闭区间末地址只用在子区域遍历上。Linux 那边没有对应物——
`vm_area_struct` 是 `vm_start`/`vm_end` 开区间，但 Linux 的 VMA 只在用户地址空间里，
`TASK_SIZE` 远低于 2^64，顶不到会回绕的位置。

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 对三个文件跑过，
无新告警（`vm_area_or_mapping.hpp:253` 那条 `modernize-use-override` 是既有的析构函数，
不在本次改动里）。

QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全，一路推进到线程调度
（`Thread |laborer| is preparing exiting` 后持续打印 `I'm Thread 2`）；`kernel.dev.obi`
跑到 `prof: no architectural PMU`。两者都与改动前的基线逐行一致。

回绕本身启动路径走不到，用临时探针在 `dump_locked()` 里实测后撤掉，见上面「症状」一节的
三行输出。`straddler` 和 `has_range` 两条也在同一次实测里：

```
PROBE straddler: got=k:init want=k:init
PROBE has_range(whole root)=true
```

`straddler` 是把窗口开在最后一个子节点的最后一个字节上（`[last, last]`，闭区间下这是个
合法的单字节窗口；开区间下要写成 `[last, last + 1)`，而 `last + 1` 正是可能回绕的那个加法）。

`unmap_locked()`、`protect_locked()`、`find_spot()` 里被顶到地址空间顶端的那条分支
**未实测**——现在没有任何区域占到顶端，构造一个需要另建一个地址空间。

**故意没做**

- 没有选"让 `end()` 饱和到 `VirtAddr(-1)`"。理由在「原因」里：它治不了调用点自己算的
  窗口末地址，还会无声吃掉最顶上那一页。
- 没有把区间判断全改成偏移量形式。偏移适合包含性，不适合两个不同基址的对象互相比较，
  Zircon 的分工也是这样。
- 没有动 `VmArea::unmap_locked()` 里 `mapping->unmap_locked(overlap_base - mapping->base(), ...)`
  这处**已确认的缺陷**：`VmMapping::unmap_locked()` 收的是绝对地址（`VmMapping::unmap()`
  传的是 `m_base + offset`，`VmArea::protect_locked()` 传给
  `VmMapping::protect_locked()` 的也是绝对地址），这里却减掉了 `mapping->base()`，
  传进去一个偏移量。这正是「偏移与绝对地址混用」那条待修项的实际代价，留给它一起修，
  因为让参数不可能被误解才是修法，改一行减法只是补救。

---

## 7. `VmCowPages` 成为真正的页面仓库：游标带位置，`require_*_page` 先查再分配（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — 整体重写，底层从 `ktl::xarray::XArray` 换成
  ustl 的 B 树，`insert_page()` 改为返回 `Status`
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — `Cursor` 增加 `m_offset`/`m_end` 与
  `index()`/`advance()`，`require_page()` 补上 `will_write` 参数；构造函数参数名
  `num_pages` 改回 `size`
- `kernel/mm/vm_cow_pages.cpp` — `Cursor` 构造函数初始化位置；`require_owned_page()` /
  `require_read_page()` 先查 `m_pages`；补上 `is_type_page()` / `is_type_zero()` 的定义；
  `alloc_pages()` 加 `Gaf::Zero`
- `kernel/mm/vm_mapping.cpp` — `map_paged()` 传给 `make_cursor()` 的改成 VMO 内偏移

**症状**

潜伏，当前启动触发不到，但按需分页做不下去。同一个偏移问两次会拿到两页，而且两页都不属于
这个 VMO。临时探针实测（`kernel.dev.dbg_1.obi`，探针未提交），对同一个 4 页匿名 VMO
用两个游标各走一遍，打印每个偏移解析出的物理地址：

```
改前  probe[walk 1]: 24A000 24B000 24C000 24D000
      probe[walk 2]: 24E000 24F000 250000 251000
      probe: same offset twice -> same page = 0, four offsets distinct = 1

改后  probe[walk 1]: 24C000 24D000 24E000 24F000
      probe[walk 2]: 24C000 24D000 24E000 24F000
      probe: same offset twice -> same page = 1, four offsets distinct = 1
```

改前第二遍走出来的是四页全新的页，第一遍那四页没有任何东西记着，就此泄漏。

**原因**

这条缺陷是三处独立的空缺叠在一起，缺任何一处都足以让"第二次拿到同一页"不成立：

1. **`VmPageMap` 根本不能用**。它是 `ktl::xarray::XArray` 的一层薄壳，而 XArray 的
   `load()` / `store()` / `erase()` / `make_entry()` **只有声明，从来没有定义**——
   本该定义它们的 `kernel/lib/ktl/include/ktl/xarray.tcc` 没有被任何文件 include，
   而且里面写的是 `Xarray`（小写 a）、`kMaxSlots`、`index_`、`owner_` 这些当前
   `xarray.hpp` 里不存在的名字，`Cursor` 也没声明过 `expand`/`create_slots`/`load`
   任何一个，就算 include 进来也编不过。实测把 `get_page()` 调起来，链接器给的是：

   ```
   ld.lld: error: undefined hidden symbol:
     ktl::xarray::XArray<ktl::Allocator<char>, ...>::load(unsigned long)
   >>> referenced by vm_page_map.hpp:24
   ```

   所以 `VmPageMap` 全树没有一处读写，不是"忘了用"，而是**用了就链接不过**。这也解释了
   为什么问题一直没被发现：编译期没有任何信号，只有真的去调它才会暴露。

2. **游标没有位置**。构造函数是 `Cursor(VmCowPages *target, VirtAddr offset, usize sizer_pages)
   : m_target(target) {}`，两个参数收下就扔了，`m_offset` 从不初始化也从不前进。而它的两个
   调用方——`map_paged()` 和 `commit_range_locked()`——都是 `for` 循环里每页调一次
   `require_owned_page(1, ...)`，从不说自己要的是哪一页。也就是说**位置只能由游标来记**，
   调用方的写法已经假定它会自己前进了。

3. **`require_owned_page()` 从不查**。它直接 `alloc_pages()`，成功就返回，谁也不记。

三者合起来，VmCowPages 不是仓库而是分配器的一层壳。这在"所有映射都一次性 Commit、此后
再不缺页"的世界里看不出来，因为每个偏移只被问一次。按需分页把这个前提取消了：解析一次缺页
就**是**第二次问同一个偏移。如果第二次拿到的是新页，那么第一次缺页写进去的数据就没了，
同一个 VMO 的两个映射也会看到两块不同的内存。

顺带暴露出来的第四处：`map_paged()` 把 `base` 直接交给 `make_cursor()`，而 `base` 是**绝对
虚拟地址**（`m_base + offset` 对齐后的结果），不是 VMO 内偏移。旁边的 `map_physical()` 传的
是 `base - m_base`，是对的。游标丢掉偏移的时候这个错没有后果；一旦游标开始用它，页面仓库
就会被 `0xFFFFFF88...` 这种键值索引，同一个 VMO 的两个映射永远对不上同一个槽。

**改法**

`VmPageMap` 底下换成 `ustl::collections::BTreeMap<PgOff, Slot>`——VmaSet 和
`ProtectionRanges` 已经在用的同一棵树，可靠且有基准数据。槽是一个字：页描述符指针，
Tag 折进低位（`PageFrame::ALIGNMENT` 是 64，低 6 位恒为零；`Tag::Borrowed = BIT(0)` 这个
写法本来就是冲着位打包去的）。

没有选"把 XArray 补完"：那是 `kernel/lib/ktl` 里的共享容器，写一棵完整的基数树是另一条线的
工作量，而 `.tcc` 里那份半成品与现在的 `xarray.hpp` 已经对不上，不是可以接着写的底子。
也没有选 Zircon 那种 16 槽分块（见"故意没做"）。

`require_owned_page()` 的顺序改成"先查槽 → 再看有没有页源 → 最后才分配"：

```cpp
// 改前
VmPage *page = nullptr;
auto status = m_target->alloc_pages(0, &page, nullptr);
if (Status::Ok == status) {
    return ustl::ok(page);
}
return ustl::err(create_read_request(nr_pages, request->get_read()));

// 改后
if (auto *page = m_target->m_pages.get_page(index())) {
    advance();
    return ktl::ok(page);
}
if (m_target->m_page_source) {
    return ktl::err(create_read_request(nr_pages, request->get_read()));
}
VmPage *page = nullptr;
if (auto const status = m_target->alloc_pages(0, &page, nullptr); Status::Ok != status) {
    return ktl::err(status);
}
if (auto const status = m_target->m_pages.insert_page(index(), page, VmPageMap::Tag::Owned);
    Status::Ok != status) {
    free_frames(page, 0);
    return ktl::err(status);
}
advance();
return ktl::ok(page);
```

三处值得说明的取舍：

- **分配失败不再变成"发个读请求"**。改前 `alloc_pages()` 一失败就掉进
  `create_read_request()`，于是内存不足被报告成 `Unimplemented`。匿名 VMO 没有页源，
  没有任何人可问，`OutOfMem` 就是 `OutOfMem`。有页源那条分支保留原样（仍是
  `Unimplemented`），形状留着，将来填进去即可。
- **先记账再交出去**。`insert_page()` 失败就把页还回去。一页分配出去却没记进仓库，比
  没分配更糟：调用方把它映射上了，VMO 不知道它存在，下一次同偏移缺页会再分配一页盖上去。
- **`insert_page()` 拒绝覆盖已占用的槽**（返回 `AlreadyExists`），而不是替换。槽里那页
  正被这个 VMO 的每一个映射指着，直接丢掉既泄漏它又让映射指向孤儿页。

`alloc_pages()` 加上 `Gaf::Zero`。VMO 里没人供过的字节按定义读出来是零，而伙伴分配器交回来的
帧里是上一个所有者留下的内容。当前所有 VMO 都用 `GAF_KERNEL` 建（`GAF_USER` 才隐含
`Gaf::Zero`），所以这个保证只能落在这里，不能指望调用方记得。

`is_type_page()` 现在真的去查槽；`is_type_zero()` / `is_type_reference()` 仍返回 false，
因为 `VmPageMap` 只存页——没有零页标记，也没有父 VMO 引用项（`VmCowPages` 连 parent 成员
都还没有）。注释写明了这三个函数将来是解码槽类型的地方。

**参照**

- `Zircon VmCowPages::LookupCursor`（`vm_cow_pages.cc`）——同样带 `offset_` / `end_offset_`，
  `RequireOwnedPage()` / `RequireReadPage()` 结束时前进游标；`RequirePage(bool write, ...)`
  就是我们补上 `will_write` 的那个参数。
- `Zircon VmPageList` / `VmPageListNode`（`vm_page_list.h`）——页面仓库的对应物，
  以 16 槽为一块建 WAVL 树。
- `Zircon VmPageOrMarker`——把类型打包进同一个字的低位，本次 `Tag` 的做法同源。

**验证**

- `prebuilt/ninja/ninja -C out/bringup.x86` 全绿。
- `clang-format` / `clang-tidy -p out/bringup.x86`：改动文件本身无新增告警
  （`vm_mapping.cpp` 里 `MappingCoalescer` 的命名告警与三处 `use nullptr` 是既有的）。
- QEMU 实测两个镜像。`kernel.dev.dbg_1.obi`（DEBUG_ASSERT 打开）与 `kernel.dev.obi` 均无
  断言、无异常，跑到线程调度正常运转（`I'm Thread 2` 循环、`laborer` 正常退出）。
- 上面那张探针对比表就是本条改动的直接证据；探针在提交前已删除。

**故意没做**

- **没做 `PageSource` / `PageRequest` 协议**。理由见 `docs/repairs/260903/mm.md` 第 3 条：
  那套复杂度是"供页者在另一个进程里"的造价，我们没有这条边界。匿名缺页永不产生
  `ShouldWait`，`PageRequest::wait()` 保持 `Unimplemented` 也没人会走到。
  `PageRequest` 仍由调用方在栈上分配、`MultiPageRequest *` 仍一路穿下来，
  `AnonymousPageRequest` 空类保留——都是脚手架，不是死代码。
- **没做 Zircon 那种 16 槽分块**。当前一页一个 B 树条目，1 GiB 的 VMO 就是 26 万个条目，
  顺序遍历每页一次 O(log n) 查找。Zircon 的 `VmPageListNode` 把 16 页并成一个树节点正是
  为了摊掉这个。做法清楚但属于另一次改动，已记进 `docs/repairs/260903/mm.md`。
- **没补 `~VmCowPages()` 释放自己拥有的页**。改前页根本没被记下来，谈不上释放；改后仓库
  知道自己有哪些页，才第一次有条件释放。这是一次独立的改动（还要连带 `decommit`），
  已记进待修。
- **没动 `VmObjectPaged::commit_range_internal()` 里那个 O(n²) 的外层循环**
  （它按页数循环，每次都提交整个 `size`）。改前它让一个 4 页 VMO 分配 16 页，改后第二遍
  起全部命中，只剩多余的查找。是另一条独立缺陷，已记进待修。
- **没改 `ktl::xarray::XArray`**，也没有删它。它属于 `kernel/lib/ktl` 那条线。

## 7. 代际老化真的会老化：`rotate_reclaimable_queues()` 前进 MRU，环满时把最老的一代折进下一代（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/page_queues.hpp` — 声明 `advance_lru_locked()`；
  给 `rotate_reclaimable_queues()` 和 `mark_accessed()` 补文档注释
- `kernel/mm/page_queues.cpp` — 实现 `advance_lru_locked()` 与 `mark_accessed()`；
  重写 `rotate_reclaimable_queues()`；`PageQueues()` 不再是 `= default`

**症状**

潜伏，当前启动触发不到。`rotate_reclaimable_queues()` 原来只有三行有效动作：

```cpp
Guard<sync::SpinLock, IrqSave> guard{get_lock()};
m_mrugen.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
m_last_age_time = time::SteadyClock::now();
m_last_age_reason = reason;
```

`m_lrugen` 从头到尾没有任何代码写过。用探针驱动之后：环填满（8 代各 1 页）之后每一次轮转
都在往一个仍然装着页的槽里塞新页，八代只剩一代。

**原因**

这里最容易写错的地方，恰恰是**这段代码本来对了一半**——值得说清楚它对在哪。

代际 LRU 的核心是：**一页的"年龄"是它离最新一代的距离，而不是它自己身上记的某个时间戳。**
所以"让所有页老一岁"这件事不需要碰任何一个页——把 MRU 代数往前推一格就够了，O(1)，
一个原子加。这正是 Linux 的 `inc_max_seq()` 做的事，也正是原来那行 `m_mrugen.fetch_add(1)`
做对了的事。会觉得"只递增一个计数器"是敷衍，是因为直觉以为老化必须搬页；它不必。

真正缺的是环的另一端。槽只有 `NUM_RECLAIMABLE`（8）个，活着的代是闭区间 `[lru, mru]`，
占用 `mru - lru + 1` 个槽。当它等于 8 时，再推进 MRU 就会撞上 LRU 所在的那个槽——新一代和
最老一代共用一条链表，从此两者再也分不开，代际方案退化成"一条链表加一个没人看的计数器"。
（配合第 6 节修掉的越界：在取模之前，撞上的还不是 LRU 槽，而是 `m_queues` 数组的外面。）

所以老化的完整形状是两件事而不是一件：**推进 MRU（不搬页）**，以及**在环满时先退休最老的
一代（要搬页）**。Linux 把这两件事分成 `inc_max_seq()` 和 `inc_min_seq()`，Zircon 分成
`RotateReclaimQueues()` 和 `ProcessLruQueue()`，界线画在同一个地方。

还有一处初始化问题：`PageQueues() = default` 让 `m_lrugen`/`m_mrugen` 处于未定义值——
`ustl::sync::Atomic` 的默认构造函数是 `= default`，不清零。当前唯一的实例在 `PmNode` 里，
落在 `.bss` 上碰巧是零，但整套环的判断全建立在 `mru - lru + 1` 上，靠链接段的性质来保证
它一开始等于 1 是不能接受的。

**改法**

`rotate_reclaimable_queues()`：

```cpp
if (usize(mru - lru) + 1 == NUM_RECLAIMABLE) {
    advance_lru_locked();
}
m_mrugen.store(mru + 1, MemoryOrder::Relaxed);
```

减法故意做在自由递增的计数器上而不是取模后的槽下标上：`u32` 的模运算让这个差值跨越计数器
自身回绕时依然正确，而两个槽下标根本区分不出"环满"和"环空"。

`advance_lru_locked()` 把最老一代整条链表交给下一代，然后 `++m_lrugen`。重接链表本身是一次
O(1) 的 `splice`；**走一遍链表只是为了重打标签**——第 6 节引入的那个队列字节是
`dequeue_locked()` 用来决定减哪个计数器的依据，只 splice 不改标签，下一次摘页就会把数减到
错误的槽上。Zircon 在这里付得更多：`ProcessLruQueue()` 是一页一页重新入队的。

我们没有交换分区、没有供页者、没有压缩，所以**没有一页能真的被回收**，最老的一代只能整体
折进下一代。代价是环最远端的那两代分不出先后；除此之外没有别的损失。Linux 的
`inc_min_seq()` 在能回收时先回收，回收不掉的同样往前折，形状是一样的。

`mark_accessed()`（原本只有声明没有定义）：页在可回收队列上且不在 MRU 代时，把它重新填到
MRU 代。这是代际方案唯一的输入信号——没有它，代数记录的是"第一次入队的时刻"而不是
"最后一次被访问的时刻"，老化就退化成 FIFO。

**被否掉的方案**：照 Zircon 的 `MarkAccessed()` 写成无锁的——用一次 compare-exchange 把新
代数写进描述符，页却留在旧链表上，等 `ProcessLruQueue()` 事后调和。它买到的是一条不用取锁的
快路径，代价是描述符里的队列字节故意与真正持有它的链表不符。我们这里每条路径本来就在
`m_lock` 之下，无锁快路径无处可买，而"字节即链表"这条不变量是 `dequeue_locked()` 的全部依据，
比那条快路径值钱。

**参照**

- `Linux inc_max_seq()` 与 `inc_min_seq()`（`mm/vmscan.c`，multi-gen LRU）——"推进 MRU 不搬页、
  环满时折叠最老一代"这个分工直接照它。
- `Linux folio_mark_accessed()` / `folio_update_gen()`——`mark_accessed()` 的语义对应物。
- `Zircon PageQueues::RotateReclaimQueues()` 与 `PageQueues::ProcessLruQueue()`
  （`zircon/kernel/vm/page_queues.cc`）——同样的两段划分，但它的 LRU 处理是逐页重新入队，
  且 `MarkAccessed()` 走无锁路径，两处都没有照抄，理由见上。

**验证**

构建通过，两个镜像 QEMU 实测启动到与改动前相同的位置，无回归。`clang-format` 与
`clang-tidy -p out/bringup.x86` 对两个文件跑过，新增行无告警。

功能证据同样来自一个临时探针（16 个静态 `VmPage`，一个独立的 `PageQueues`，提交前已删除）。
`slots(list)` 是把 8 条可回收链表逐条走一遍数出来的，`slots(size)` 是 `m_sizes` 里的计数器，
`total` 是两者各自的合计：

```
probe 3 gens, before aging: lru=gen0 mru=gen2 slots(list)=[1,1,1,0,0,0,0,0] slots(size)=[1,1,1,0,0,0,0,0] total=3/3
probe 3 gens, after aging : lru=gen0 mru=gen3 slots(list)=[1,1,1,0,0,0,0,0] slots(size)=[1,1,1,0,0,0,0,0] total=3/3
probe ring full   : lru=gen0 mru=gen7 slots(list)=[1,1,1,1,1,1,1,1] slots(size)=[1,1,1,1,1,1,1,1] total=8/8
probe after fold  : lru=gen1 mru=gen8 slots(list)=[0,2,1,1,1,1,1,1] slots(size)=[0,2,1,1,1,1,1,1] total=8/8
probe filed gen 8 : lru=gen1 mru=gen8 slots(list)=[1,2,1,1,1,1,1,1] slots(size)=[1,2,1,1,1,1,1,1] total=9/9
probe: page 3 queue byte before mark_accessed = 6
probe: page 3 queue byte after  mark_accessed = 3
probe marked p3   : lru=gen1 mru=gen8 slots(list)=[2,2,1,0,1,1,1,1] slots(size)=[2,2,1,0,1,1,1,1] total=9/9
probe aged (ring full, so it folds again): lru=gen2 mru=gen9 slots(list)=[2,0,3,0,1,1,1,1] slots(size)=[2,0,3,0,1,1,1,1] total=9/9
probe removed all : lru=gen2 mru=gen9 slots(list)=[0,0,0,0,0,0,0,0] slots(size)=[0,0,0,0,0,0,0,0] total=0/0
```

逐条读：

1. 前两行是「老化不搬页」的直接证据：`mru` 从 gen2 走到 gen3，8 个槽的内容一个字节没动。
2. `ring full` 之后那次轮转触发折叠：槽 0（gen0 那一页）并进槽 1，槽 1 变成 2，`lru` 走到
   gen1，总数仍是 8——一页都没丢。腾出来的槽 0 随即被 gen8 用上（8 % 8 = 0），第三行填进去。
3. `mark_accessed` 那两行是队列字节本身：6 是 `ReclaimableStart(3) + 3`，即 gen3 所在的槽；
   改成 3 即槽 0，也就是当前的 MRU 代 gen8。对应的链表变化是槽 3 清空、槽 0 从 1 变 2。
4. 最后一次轮转时环仍是满的（`mru - lru + 1 = 8 - 1 + 1 = 8`），所以它又折了一次：
   槽 1 的 2 页并进槽 2，槽 2 从 1 变 3。这一行原本是想演示"不折叠的老化"，结果演示了
   "环满就必折"，两件事第 1 条已经单独证过了。
5. 每一行 `slots(list)` 与 `slots(size)` 逐位相同，`total` 两个数也一致——折叠用的是整条链表
   的 `splice`，如果重打标签那一步漏了，最后 `removed all` 会把计数减到别的槽上，全零就不会
   出现。

**故意没做**

- **没有回收。** `advance_lru_locked()` 里没有"挑一页扔掉"的分支，因为没有地方可扔：
  没有交换分区、没有供页者、没有压缩。写一个当前只能返回失败的回收路径，等于把设计决定
  伪装成代码。
- **没有人调用 `mark_accessed()`。** 它的输入是页表的 accessed 位，需要缺页路径与页表遍历器
  一起提供，那两样分别属于别的会话。
- **没有引入"最低 LRU 代"的请求机制。** Zircon 的 `LruRunner` 是被
  `RequireLruGen()` 之类的请求驱动的（回收方说"我要看到比第 N 代更老的页"），那套背压只有在
  真的有回收方时才有意义。

---

## 7. VMA 接口统一收绝对地址，不再收相对 `m_base` 的偏移（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/vm_area.hpp` — `create_mapping`/`create_subvma`/`unmap`/`protect`/
  `reserve`/`create_subaom_preamble` 六个签名的首参改成 `VirtAddr base`
- `kernel/mm/vm_area.cpp` — 六处实现随之改写；修掉 `unmap_locked()` 传偏移给
  `VmMapping::unmap_locked()`、`create_mapping()` 把 VMA 偏移当 VMO 偏移传两处缺陷
- `kernel/mm/include/ours/mm/vm_mapping.hpp`、`kernel/mm/vm_mapping.cpp` — `map`/`protect`/
  `unmap` 三个公开入口同样改收绝对地址
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — `Builder` 的标量成员补默认初始化
- `kernel/mm/init_vmm.cpp`、`kernel/mm/stack.cpp` — 调用方去掉换算

**症状**

一半是可读性，一半是两处真缺陷。

同一层里两套坐标：`unmap`/`protect`/`reserve`/`create_subvma`/`create_mapping` 收相对
`m_base` 的偏移，而 `map_at`/`map` 交回绝对地址、`contains`/`find_mapping_locked` 以及全部
`*_locked` 内部实现收绝对地址。于是每次跨越都要换算，`init_vmm.cpp` 里就写着
`phys_vma.base - root_vma->base()`。

两处换算是错的，而且都不可能在调用点看出来——两种读法都是一个裸 `u64`：

1. `VmArea::unmap_locked()` 把 `overlap_base - mapping->base()` 传给
   `VmMapping::unmap_locked()`，后者收的是绝对地址。
2. `VmArea::create_mapping(vma_ofs, size, vmo_ofs, ...)` 把 `vma_ofs` 传进
   `.set_vmo_offset()`，而 `vmo_ofs` 收下之后一次也没用。

第 2 条当前潜伏（所有调用方都拿一个新建 VMO 的偏移 0）。第 1 条不是潜伏，是只差一个调用方：
本次用临时探针把这条路走通了，`kernel.dev.obi` 实测，**修之前**：

```
PROBE   VmMapping::unmap_locked got base=0x2000 size=0x1000, mapping is [0xFFFFFF8804000000, 0xFFFFFF8804004000)
PROBE unmap(trim page 2) -> InvArgs
```

`0x2000` 是偏移量被当成地址。`VmMapping::unmap_locked()` 拿它去问自己的
`ProtectionRanges`——那里存的是绝对区间——窗口整个落在映射之外，
`m_protection_ranges.update()` 拒绝，部分解除映射**静默失败并把 InvArgs 一路交回**。

**原因**

偏移这套坐标在这一层是没有立足之地的。理由不是"绝对地址更好看"，是三件具体的事：

一、**内部实现本来就是绝对的**。`unmap_locked`、`protect_locked`、`can_protect_locked`、
`find_mapping_locked`、`VmaSet::seek`、`Enumerator` 无一例外收绝对地址，而且必须如此：
它们要拿子节点的 `base()`／`last()` 和窗口比大小，子节点的基址是绝对的，改成偏移就得先把
两边归一到同一个原点。所以偏移只存在于最外面一层公开签名上，每次进出都换算一次，
而换算本身不产生任何信息。

二、**偏移 0 是个陷阱**。它同时是"这个区域的开头"和"我没意见"。把 LAPIC 的 MMIO 放到
physmap 起点那个 bug（存档第 36 节）的第一推动力就是这个二义：调用方本意是"随便挑"，
写出来是 `0`，而 `0` 在偏移坐标下是个完全合法的具体地址。换成绝对地址至少让"这个区域的
开头"必须显式写出来（`vma->base()`），二义暴露在字面上——真正拆掉它是下一条的事。

三、**两种偏移长得一模一样**。`create_mapping` 同时收 VMA 偏移和 VMO 偏移，都是 `usize`，
编译器帮不上忙。改成 `VirtAddr base` 之后，剩下的唯一偏移是 `vmo_ofs`——它确实只能是偏移，
因为 VMO 没有地址——一个接口里只留一种偏移，才谈得上认错了会硌手。

顺带说明为什么 `unmap()`/`protect()` 里那句 `if (offset > m_size) return InvArgs;` 可以直接
删掉：它存在的唯一目的是防止 `m_base + offset` 加法回绕到 `m_base` 以下，而
`check_range()` 用的是减法（`base - m_base <= m_size - size`），本身就防回绕，
绝对地址不需要这道前置。这是同一件事的第二面：换算是错误的来源，去掉换算连带去掉了
为换算准备的防御。

**改法**

六个 `VmArea` 入口 + 三个 `VmMapping` 入口的首参统一成 `VirtAddr base`。调用方：

```cpp
// 之前
root_vma->create_subvma(phys_vma.base - root_vma->base(), phys_vma.size, ...);
vma->reserve(0, phys_vma.size, ...);
mapping->protect(0, size, mmuf);
mapping->unmap_locked(overlap_base - mapping->base(), ...);

// 之后
root_vma->create_subvma(phys_vma.base, phys_vma.size, ...);
vma->reserve(vma->base(), phys_vma.size, ...);
mapping->protect(aligned_base, size, mmuf);
mapping->unmap_locked(overlap_base, ...);
```

绝对地址让两处从前问不出口的检查成为可能，都补上了：

- `reserve()` 原来**完全没有范围检查**。偏移形式下也问不了——`m_base + vma_off` 总能算出
  一个地址，只是不一定在这个区域里，而没有任何地方会发现。现在加 `check_range()`。
- `create_subaom_preamble()` 的 Fixed 分支加 `check_range()`。非 Fixed 分支不加，
  因为那条分支根本不读 `base`，检查一个马上要被丢掉的值只会误伤。

`Builder` 的标量成员补上默认初始化。`create_subvma()` 原来给 `set_vmo_offset()` 传
`vma_ofs`（子区域根本没有 VMO），现在那一行删掉了——删掉之后 `m_vmo_ofs` 就成了未初始化
读，虽然 `build()` 在无 VMO 分支里不看它。与其留一个"靠没人读所以没事"的成员，不如
让所有成员都有确定值。

**参照**

`Linux` 全线是绝对地址：`do_munmap(struct mm_struct *mm, unsigned long start, size_t len)`、
`mprotect_fixup(..., unsigned long start, unsigned long end, ...)`、
`vm_area_struct::vm_start`/`vm_end`，从不出现"相对 VMA 起点的偏移"。
`Zircon` 是对照组，它**保留**偏移：`VmAddressRegion::CreateVmMapping(size_t mapping_offset, ...)`
的偏移是相对 VMAR 起点的，因为 VMAR 属于某个进程、起点可以被 ASLR 随机化，用户态拿不到也
不该依赖绝对地址。我们这一层是内核地址空间，起点是编译期常量（`KERNEL_ASPACE_BASE`），
调用方手里本来就全是绝对地址（`PhysMap::VIRT_BASE`、`ARCH_CPU_LOCAL_VIRT_BASE`、
`mapping->base()`），偏移只是把它们减一遍再加回去。所以这里跟 Linux 而不跟 Zircon。
需要注意 Zircon 的下半层同样是绝对的：`VmAddressRegion::UnmapInternalLocked(vaddr_t base, ...)`
收的是 `vaddr_t`——它的分界线和我们改之后一致，只是公开层的那一侧不同。

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 对七个文件跑过，
新增行无新告警（`vm_mapping.cpp` 里剩下的 `readability-identifier-naming` 与
`modernize-use-nullptr` 都在 `MappingCoalescer` 和 arch unmap 调用上，不是本次改的行）。

QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全，一路到线程调度；
`kernel.dev.obi` 到 `prof: no architectural PMU`。这次启动本身就是正向证据，
`create_subvma`(Fixed 绝对基址)、`reserve`、`create_mapping` + `map` 都在启动路径上。

`unmap`/`protect` 启动不走，用临时探针实测后撤掉：在根 VMA 空闲处（per-CPU 窗口正上方）
建一个 4 页的 `k:probe` 子区域，`reserve()` 出一个覆盖全区的映射，然后逐条走。
**修之后**（`kernel.dev.obi`）：

```
PROBE create_subvma(0xFFFFFF8804000000) -> Ok base=0xFFFFFF8804000000 size=0x4000
PROBE reserve -> Ok
PROBE unmap(outside) -> InvArgs
PROBE   VmMapping::unmap_locked got base=0xFFFFFF8804002000 size=0x1000, mapping is [0xFFFFFF8804000000, 0xFFFFFF8804004000)
PROBE unmap(trim page 2) -> Ok
PROBE protect(page 0, R) -> Ok
PROBE unmap(all) -> Ok children_left=0
```

逐条对上：Fixed 请求落在**要求的绝对地址**上；`unmap` 一个区域外的绝对地址被
新加的 `check_range()` 挡下（偏移形式下这个请求根本无法表达）；`VmMapping::unmap_locked()`
收到的是 `0xFFFFFF8804002000` 而不是修前的 `0x2000`，部分裁剪从 `InvArgs` 变成 `Ok`；
`VmArea::protect()` 的递归下降**首次实测通过**；全覆盖的 `unmap` 把映射销毁并从集合里摘掉
（`children_left=0`），即第 3 条那个 `erase(IterMut)` 走查循环也一并验到了。

未实测：嵌套子区域的递归（探针只有一层）、`FixedNoReplace` 的 `AlreadyExists` 分支。

**故意没做**

- 没有拆 `map_at()`/`map()` 那个既当输入提示又当输出结果的 `VirtAddr *`。本条只统一坐标系，
  "此处"与"随便挑"的二义是另一条。`stack.cpp` 里 `rvma->create_mapping(rvma->base(), ...)`
  那句注释就是这个二义的现场——写着一个地址，实际不落在那里。
- 没有动 `VmMapping::map()` 缺锁那条既有 FIXME。
- 没有把 `VmObject` 一侧的偏移改成什么别的。VMO 的偏移本来就该是偏移，
  见 `docs` 里「VmObject 的 offset 是字节偏移」那条约定。

## 8. `LruRunner` 的循环与关停握手：条件不再反着写，状态不再少一个；但仍然不启动线程（2026-09-03）

**文件**
- `kernel/mm/page_queues.cpp` — 重写 `LruRunner`（三态状态机、`run()`、`request_shutdown()`、
  `state()`）；给 `MruRunner` 补上"它是干什么的"的注释；实现 `start_runners()`；
  构造函数补上 `m_last_age_time` 与两个轮转周期

**症状**

潜伏且不可达——没有任何东西创建过 `LruRunner`，`m_thread` 上标着 `[[maybe_unused]]`，
`start_runners()` 只有声明没有定义。

```cpp
enum class State {
    RequestShutdown,
    Shutdown,
};

auto PageQueues::LruRunner::run() -> void {
    while (State::Shutdown == m_state.load(MemoryOrder::Relaxed)) {
    }
}
```

**原因**

两层错，第二层比第一层要紧。

**表层：条件反了。** 工作线程的循环跑到"被关停"为止，而不是"在被关停期间"跑。这里写的是
后者：`m_state` 是 `Shutdown` 时空转，一旦不是就立刻退出——正好把两种情形都做反。而且循环
体是空的，所以即使条件对了它也不是一个循环，只是一个占满 CPU 的忙等。

**里层：状态少一个，两种情形没法都表示。** 枚举只有 `RequestShutdown` 和 `Shutdown`，**没有
一个值表示"正在运行"**。于是不管比较写成 `==` 还是 `!=`，总有一个状态是循环永远不可能处在
的状态——这不是笔误可以修好的，条件反不反都不对。

关停握手需要区分的正是两件事：**"有人要求停下"** 和 **"循环真的已经离开、不再碰任何队列"**。
只有后者之后，拆掉队列才是安全的。所以第三个枚举值不是补充，它是握手本身。

**改法**

`State { Running, ShutdownRequested, Shutdown }`，`m_state` 在构造函数里初始化成 `Running`。

`run()`：

```cpp
while (State::Running == m_state.load(MemoryOrder::Acquire)) {
    auto const next = m_owner->m_last_age_time + m_owner->m_min_rotate_time;
    task::Thread::Current::sleep_until(time::Deadline::at(next), task::Interruptible::Yes);

    if (State::Running != m_state.load(MemoryOrder::Acquire)) {
        break;
    }
    m_owner->rotate_reclaimable_queues(AgeReason::Timeout);
}
m_state.store(State::Shutdown, MemoryOrder::Release);
```

三处值得说明：

1. **睡到一个时刻，而不是睡一段时长。** 下一次老化的到期时刻是"上一次老化实际发生的时刻加
   一个最小周期"，所以节拍不会被轮转自身的耗时推着漂移，而且一次手动轮转会把下一次自动轮转
   往后推，而不是两次挤在一起。
2. **醒来之后重新读一次状态，而不是直接落到循环条件上。** 可中断的睡眠正是关停请求打断等待
   的方式；落到循环条件上会先多做一次轮转才退出，那是对着调用方正在拆的队列白干一次。
3. **退出时的 `store` 用 Release，`state()` 用 Acquire。** 这一对是"看见 Shutdown 就等于
   循环对队列做的一切都已经发生"的全部依据，也就是这个状态"意味着什么"的来源。

`request_shutdown()` 用 `compare_exchange_strong` 只从 `Running` 转出，而不是无条件 store：
重复请求会把循环已经发布的 `Shutdown` 覆盖回去，等于把已经告诉等待者的事情收回。

**故意保留 `start_runners()` 不启动任何线程。** 它现在有定义（原来只有声明，是一个潜在的
链接错误），函数体是一段注释：队列的每个操作都是同步的，不需要这个线程；而一个线程在今天
只会去老化一个永远空着的环——没有任何东西往这些队列里填页（那要等 `VmCowPages` 变成真正的
页仓库），也没有任何东西读回代数，因为没有回收方：没有交换分区、没有供页者、没有压缩，
回收出来的页无处可去。老化的意义是给回收方排一个序，没有消费者的排序就是一次为了什么都不做
的定时唤醒。

顺带补上构造函数里两处未初始化：`m_last_age_time` 值初始化成时钟纪元（**不是** `now()`——
它跑在 `PmNode` 的构造函数里，那时平台还没把时钟带起来），效果是第一次到期时刻已经过去，
真启动的话会立刻轮转一次再进入节拍；`m_min_rotate_time` / `m_max_rotate_time` 取 Zircon 的
默认值 10 秒 / 60 秒，并在常量旁写明这是占位值——真正的数字来自看着某个回收方因为代数太粗
或太细而选错页，而现在没有回收方可看。

**参照**

- `Zircon PageQueues::MruThread()` / `LruThread()`（`zircon/kernel/vm/page_queues.cc`）：
  同一个形状——循环、按时限等待、退出前发布状态。Zircon 等在一个 event 上，因此关停是立刻
  生效的；这里等在可中断的 `sleep_until` 上，关停延迟最坏是一个轮转周期。没有引入 event，
  是因为 `sync::Event` 默认构造出来的 canary 从来没有人初始化过，而这段代码目前不可达，
  为一段不可达的代码去验证一个新对象的构造语义不划算。这一点记进了待修。
- Zircon 的分工是：MRU 线程按时钟老化，LRU 线程在回收方要求看到更老的页时退休最老的一代。
  这里只有前半件事存在，而骨架把 `run()` 放在了 `LruRunner` 上，所以老化循环暂时留在那里；
  `MruRunner` 的类注释写明了这件事，等回收方出现时两者归位。
- `Linux kswapd()`（`mm/vmscan.c`）是另一个参照，但 Linux 的老化不是由独立线程按时钟驱动的
  （MGLRU 的 `inc_max_seq()` 由回收路径自己调），所以对"该不该有这个线程"这个问题，
  Linux 的答案其实是"不该"，与这里最终不启动线程的结论一致。

**验证**

构建通过，两个镜像 QEMU 实测启动到与改动前相同的位置，无回归。`clang-format` 与
`clang-tidy -p out/bringup.x86` 对两个文件跑过，新增行无告警。

握手用临时探针实测（提交前已删除）。它在栈上造一个 `LruRunner`，不起线程，只走状态机：

```
probe runner: fresh                 -> Running
probe runner: after request         -> ShutdownRequested
probe runner: after run() returned  -> Shutdown
probe runner: after second request  -> Shutdown
probe runner: min rotate = 10000000000 ns, max = 60000000000 ns
```

第三行是关键：状态已经是 `ShutdownRequested` 时进入 `run()`，它一次都不进循环体就返回，
并留下 `Shutdown`。改动前的代码在同样的位置会永远空转（`Shutdown != ShutdownRequested`，
所以那个反着写的条件立刻退出——但它退出时什么也不发布，等待者永远等不到握手）。
第四行是重复请求不会把 `Shutdown` 覆盖回去。

**明说没验证到的**：`run()` 的循环体——`sleep_until` 加 `rotate_reclaimable_queues`——
**一次都没有跑过**，因为没有线程，而 `sleep_until` 在非线程上下文里不能调。它是照着
Zircon 的形状写的，静态可读，但没有运行时证据。这是不启动线程的代价，已记进
`docs/repairs/260903/mm.md`。

**故意没做**

- **没有启动线程**，理由见上。
- **没有引入 event 让关停立刻生效。** 现在关停最坏要等一个轮转周期。等真有线程时再补。
- **没有把老化循环搬到 `MruRunner`**，虽然按 Zircon 的分工它属于那里。现在搬只是把不可达的
  代码从一个类挪到另一个类，等回收方出现、`LruRunner` 拿到自己那一半工作时一起做。
- **没有实现 `to_string(AgeReason)`**（它是个只有友元声明的函数）。没有调用方，而且
  `AgeReason` 是私有嵌套类型，类外根本没法命名它——那是一个只能由成员通过 ADL 调用的函数，
  等到真有日志要打的时候一并处理。

---

## 8. `VmMapping::fault()` 从桩到实现，缺页真的能解析了（2026-09-03）

**文件**
- `kernel/mm/vm_mapping.cpp` — 实现 `fault()`
- `kernel/mm/vm_aspace.cpp` — `page_fault_internal()` 里 `m_last_fault` 的空 `Arc` 判空；
  改为在栈上构造 `VmFault` 并把它的地址传下去
- `kernel/mm/include/ours/mm/vm_fault.hpp` — `page_request` 由 `PageRequest` 改成
  `MultiPageRequest`；`num_pages` 由 `VirtAddr` 改成 `usize`；补上字段注释

**症状**

按需分页整个不工作。`fault()` 是

```cpp
auto VmMapping::fault(VmFault *vmf) -> ustl::Pair<Status, usize> {
    m_canary.verify<Self>();
    return {Status::NotSupported, 0};
}
```

它上面那一整套——x86 的 `#PF` 分发、`Thread::Current::page_fault()`、
`VmAspace::page_fault_internal()` 的重试循环、`ShouldWait` 机制——全都是围绕一个不存在的
操作写的。同一条路径上另有两处会在**第一次**缺页就触发：

- `m_last_fault->check_range(va, 1)`：`m_last_fault` 是空 `Arc`，直到第一次查找成功才有值。
  空 `Arc` 的 `operator->` 交出的是 `nullptr`，而 `check_range()` 是非虚内联成员，于是它在
  `this == nullptr` 上去读 `m_base`/`m_size`——内核跑出的第一次缺页就撞在这里，而且这一次
  恰恰是这个缓存唯一不可能命中的一次。
- `m_last_fault->fault(0)`：字面量 `0` 当 `VmFault *`。

**原因**

三处是同一个问题的三个面：**这条路径从来没有跑过**。`VmFault` 从未被构造过，所以它的
`page_request` 是 `PageRequest` 还是 `MultiPageRequest`、`num_pages` 是什么类型，都没有
被任何调用检验过；`m_last_fault` 的初始状态没有被想过，因为没有"第一次"；`fault(0)` 能编过，
是因为 `0` 是合法的空指针常量。一条只写不跑的路径，编译器能查的只有类型，而这里连类型
都没对上——`require_page()` 这一串收的是 `MultiPageRequest *`，`VmFault` 里放的却是
`PageRequest`。

**改法**

`fault()` 的顺序是：校验 → 查权限 → 查页表 → 要页 → 装页表。

权限从 `m_protection_ranges` 读，不从页表反查：它是权限的唯一写入者，而且区间铺满整个窗口，
所以对一页做枚举必然只得到一个区间。零权限区间是 `unmap()`/`protect()` 表示"这里已经没有
映射"的写法，所以它是拒绝（`AccessDenied`）而不是缺页。写缺页撞上不可写的区间同样拒绝。

**查页表这一步是被 arch 层逼出来的**。缺页可能落在一个已经装好的页上——另一个 CPU 在本
CPU 等地址空间锁的时候先解析了同一次缺页，或者本 CPU 还在用陈旧的 TLB 项。本该由
`MapControl::OverwriteIfExisting` 表达，但 `PageTable::map_pages()`
（`kernel/arch/x86/mm/page_table.cpp:86`）把 `control` 直接 `(void)` 掉了，注释里写明
"engine always rejects a page that is already mapped"。所以无条件 `map()` 会把一次**已经
解析完的**缺页报成 `AlreadyExists`——这正是本次探针第二发缺页抓到的：

```
改前  probe: fault#2 = AlreadyExists, pa = 24D000, same page = 1, data still D0DECAFE
改后  probe: fault#2 = Ok,            pa = 24D000, same page = 1, data still D0DECAFE
```

于是先 `query()`：已有表项就地返回 `Ok` 与"新映射 0 页"，只在权限位与区间不符时调
`arch_aspace.protect()` 补齐（`protect()` 是唯一能原地改权限的入口，`map()` 不能，
而先 unmap 再 map 会开一个"页对所有人都不存在"的窗口）。

`VmFault` 挪到 `page_fault_internal()` 的栈上，在 do-while **之外**构造：一次重试里发出的
请求，必须和下一次等待、完成的是同一个对象。`page_request` 改成 `MultiPageRequest`，
因为下面整条 `require_page()` 链收的就是它。这两点都是第 3 条待修里"必须保留"的形状——
缺页路径不能分配内存，否则可能递归触发回收再触发缺页。

`m_last_fault` 的判空写成 `m_last_fault && m_last_fault->check_range(...)`：先问有没有映射，
再问覆不覆盖。

**参照**

- `Zircon VmMapping::PageFault()` / `PageFaultLockedObject()`（`vm_mapping.cc:1375` / `:1173`）
  ——同样是取游标、`RequirePage`、再装页表，同样在持有对象锁的前提下运行。
- `Zircon VmAspace::PageFault()` / `PageFaultInternal()`（`vm_aspace.cc:552` / `:569`）
  ——`page_fault_internal()` 的形状来自这里，包括 `ShouldWait` 之后 `page_request.Wait()`
  再重试那一段。

**验证**

- `prebuilt/ninja/ninja -C out/bringup.x86` 全绿；`clang-format` / `clang-tidy`
  改动文件无新增告警。
- QEMU 实测两个镜像（`kernel.dev.dbg_1.obi` / `kernel.dev.obi`），无断言、无异常，
  跑到线程调度正常运转。
- 临时探针（提交前已删除）：建一个 `VmoFlags::Lazy` 的 2 页 VMO，**不带
  `VmMapOption::Commit`、也不调 `mapping->map()`** 地映射进内核地址空间，然后用
  `VmAspace::soft_fault()` 逐条驱动：

```
probe: mapped [FFFFFF8804000000, FFFFFF8804002000) lazily, pa before any fault = 0
probe: fault#1 = Ok, pa = 24D000
probe: wrote D0DECAFE, read back D0DECAFE
probe: fault#2 = Ok, pa = 24D000, same page = 1, data still D0DECAFE
probe: page 1 fault = Ok, pa = 24F000, distinct from page 0 = 1
probe: read-only mapping, write fault = AccessDenied, read fault = Ok, pa = 250000
probe: fault on an unmapped address = NotFound
```

逐条对应：映射建好时页表里什么都没有；第一次缺页装上了一页；这一页是真的、可写的；
第二次缺页拿到同一页且数据还在（第 7 条那个仓库在起作用）；同一映射的第二页是另一页；
只读映射拒绝写缺页、接受读缺页；不存在的地址得到 `NotFound`。`fault#1` 是内核生命周期里的
第一次缺页，所以它同时是 `m_last_fault` 空 `Arc` 那条判空的实测。

**没验证到的**，说清楚：探针调的是 `VmAspace::soft_fault()`，**没有走硬件 `#PF`**。
硬件那一段现在走不通——`Thread::Current::fault()`（`kernel/kernel/task/thread.cpp:712`）
开头就是 `if (mm::is_kernel_address(va)) return Status::NotFound;`，内核地址的缺页根本到不了
VMM，而当前还没有用户地址空间。这不是本次改动引入的，已记进待修。所以本条验证的是
`page_fault_internal()` → `VmMapping::fault()` → 游标 → arch `map()` 这一段，
`#PF` → `x86_handle_page_fault()` → `Thread::Current::page_fault()` 那一段未实测。

**故意没做**

- **一次缺页只装一页**。`vmf->num_pages` 说的是调用方愿意接受几页，提前多映射几页是值得做的，
  但它要求游标能一次交出一段连续的页，而不是一页一页——就是 `map_paged()` 那条 TODO 要的
  批量。在那之前多要几页只会变成同一把锁下更多次单页工作。
- **没做 CoW**。写缺页落在一个已存在且区间允许写的页上，在有 CoW 的内核里是分裂私有副本的
  地方；我们没有父子 VMO 链，所以那条分支现在就是"表项已经对了，直接返回"。
- **没动 `Thread::Current::fault()` 的内核地址挡板**，也没动 `PageTable::map_pages()`
  对 `MapControl` 的忽略。前者属 task，后者属 arch，都已记进待修。
- **没实现 `PageSource` / `PageRequest` 协议**（第 3 条）。匿名缺页永不返回 `ShouldWait`，
  所以 `page_fault_internal()` 里那圈 do-while 至今一次都没转过第二遍——形状保留，将来是
  往里填而不是重写。

---

## `map_at()` 的地址参数拆成 `VmPlacement`，`VmMapOption::Fixed` 随之消失（2026-09-03）

> 这一节不带序号：`docs/changes/260903/mm.md` 有多个会话并发追加，序号 6/7/8 都已经被
> 撞了不止一次。本文件是只增不改的，别人的节次不能动，所以这里改用模板本来的无序号标题
> （文件中间那几节也是这么写的）。

**文件**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — 新增 `VmPlacement`；
  `VmMapOption` 去掉 `Fixed` 与 `FixedNoReplace`
- `kernel/mm/include/ours/mm/vm_area.hpp` — `map_at`/`map` 改名并改签名成
  `map_phys`/`map_anon`；`create_mapping`/`create_subvma`/`create_subaom_preamble`/
  `map_with_vmo` 首参改成 `VmPlacement`
- `kernel/mm/vm_area.cpp` — 上述实现；`create_subaom_preamble()` 重写成两条互斥分支
- `kernel/mm/init_vmm.cpp`、`kernel/mm/stack.cpp`、`kernel/arch/x86/apic/local_apic.cpp`、
  `kernel/arch/x86/apic/io_apic.cpp`、`kernel/arch/x86/kernel/descriptor.cpp`、
  `kernel/arch/x86/kernel/idt.cpp`、`kernel/arch/x86/kernel/time/hpet.cpp` — 七个调用方

**症状**

`map_at()` 的第二个参数是 `VirtAddr *virt_base`，一个参数身兼三职：

1. 输入提示——调用方想要的地址；
2. 输出结果——实际落点写回这里；
3. 空指针表示"你挑地址"。

而"提示到底算不算数"由**另一个参数** `VmMapOption::Fixed` 决定。于是"想要某个地址"和
"随便挑"这两件事，在签名上没有区别，只在两个参数的组合里。

代价已经付过一次：`init_local_apic()` 写的是 `map_at(APIC_PHYS_BASE, 0, PAGE_SIZE, ...)`，
第二个参数那个 `0` 意思是"随便挑"，但 `0` 同时也是一个合法的地址值。加上当时
`create_subaom_preamble()` 把 `alloc_spot()` 的结果算完就扔（存档第 36 节），LAPIC 的 MMIO
就落到了这个 VMA 的起点，也就是 physmap 的起点。那个 bug 的直接原因是丢了一次赋值，
**但让它写得出来的是这个签名**。

还有一处更隐蔽的，Fixed 分支：

```cpp
if (m_subvmas.has_range(base_aligned, size_aligned)) {
    if (is_fixed_noreplace) {
        return ktl::err(Status::AlreadyExists);
    }
    // Overwrite the primitive regions.
    DEBUG_ASSERT(false, "Now we don't support to overwrite a group of existent pages");
}
```

`DEBUG_ASSERT` 在 `KERNEL_DEBUG_LEVEL=0` 下展开成空（`system/public/ours/assert.hpp:51`），
所以 release 构建里这个分支**什么都不做然后往下走**，用请求的地址建区域。
`VmaSet::insert()` 只在基址完全相同时报 `AlreadyExists`，部分重叠它看不见——
于是 `Fixed` 打到一段部分被占的地址上，会静默建出一个重叠的 VMA。

**原因**

一个参数只该有一个意思，这是表层。往下一层是：**"随便挑"这件事根本没有地址可以填**，
而旧签名逼着调用方填一个。填什么都不对——`nullptr` 和 `0` 都是"没有意见"的编码，
而 `0` 又是个合法地址，两种读法在类型上完全同形。凡是"用某个取值当哨兵"的设计，
只要那个取值本身也在值域里，就迟早会被当成真值用一次。Linux 的 `mmap(NULL, ...)` 能用
同样的手法，只是因为用户地址空间永远不映射第 0 页——我们这里没有这条豁免，内核根 VMA 的
起点是 physmap 的起点，是个天天有人用的地址。

输出那一半则是多余的。映射建出来之后，它自己知道自己在哪，`(*result)->base()` 就是答案，
而且是唯一的一份。五个调用方里已经有两个（`local_apic.cpp`、`io_apic.cpp`）就是这么读的，
它们传进去的那个出参从头到尾没被看过。剩下三个（`descriptor.cpp`、`idt.cpp`、`hpet.cpp`）
先声明一个 `VirtAddr virt_addr = 0`、把地址取回来、再用——多出来的那个局部变量是一份
可以和映射脱节的拷贝。

名字也一起改了。`map_at` 与 `map` 读起来像是"指定地址"与"不指定地址"的区别，
实际区别是**背后是什么**：前者包一个 `VmObjectPhysical`（已经存在的物理内存），
后者包一个 `VmObjectPaged`（匿名页）。两个正交的维度被写成了一个，而且写的是错的那个。

**改法**

新增 `VmPlacement`，两个具名构造函数，没有第三种状态可写：

```cpp
VmPlacement::anywhere()      // VMM 挑，本身不携带任何地址
VmPlacement::at(VirtAddr)    // 就这个地址，否则失败
```

`base()` 上带 `DEBUG_ASSERT(m_fixed)`：`anywhere()` 没有地址可读，读它是个错误，
而不是"读出个 0 然后当地址用"。

`VmMapOption::Fixed` 与 `FixedNoReplace` 从枚举里删掉。留着就是留下第二种说法，
而两种说法迟早会打架——`VmPlacement::at()` 已经把"就这个地址"说清楚了。
`at()` 撞上已占用的范围一律返回 `AlreadyExists`：覆盖式映射没有实现，
拒绝是 VMM 现在唯一兑现得了的答案；等有人实现覆盖，它该拿一个自己的具名构造函数
（`VmPlacement::replacing(base)`），而不是给 `at()` 加第二种含义。

`create_subaom_preamble()` 因此塌成两条互斥分支，非 Fixed 那条提前 return，
不再有"先按请求算一个 base，再看要不要覆盖掉"的中间态——那个中间态正是丢掉
`alloc_spot()` 结果的地方。尺寸对齐提到分支之前单独做，因为两条分支对起点有分歧、
对长度没有。

`map_at`/`map` 改名成 `map_phys`/`map_anon`，出参删掉：

```cpp
// 之前
VirtAddr virt_addr = 0;
auto result = rvma->map_at(mm::virt_to_phys(VirtAddr(PIDT)), &virt_addr, PAGE_SIZE,
                           MmuFlags::Readable, VmMapOption::Commit | VmMapOption::Pinned, "k:IDT");
PIDT = reinterpret_cast<decltype(PIDT)>(virt_addr);

// 之后
auto result = rvma->map_phys(mm::virt_to_phys(VirtAddr(PIDT)), VmPlacement::anywhere(),
                             PAGE_SIZE, MmuFlags::Readable,
                             VmMapOption::Commit | VmMapOption::Pinned, "k:IDT");
PIDT = reinterpret_cast<decltype(PIDT)>((*result)->base());
```

七个调用方全部改完，其中六个是 `anywhere()`，只有 `init_vmm.cpp` 的九个预置 VMA 是
`at(phys_vma.base)`——那正是唯一真正要求固定地址的地方，现在它在字面上就与别人不同。

**参照**

`Zircon VmAddressRegion::CreateVmMapping()` 的 `ZX_VM_SPECIFIC` 标志位就是被替换掉的那个
形状：一个偏移参数加一个决定它算不算数的标志位，二者分离。Linux 的 `mmap()` 是
`MAP_FIXED` 加 `addr`，同构。两者都受制于系统调用 ABI——标志位是位掩码，参数是整数，
表达不了"这个参数不存在"。我们这一层是内核内部的 C++ 接口，没有这个约束，
所以可以直接把"不存在"编码进类型。这是本条与两个参照物**有意不同**的地方，
不是疏漏。`Zircon VmAddressRegion::CreateSubVmarInternal()` 里
`if (!(vmar_flags & VMAR_FLAG_SPECIFIC)) { ... AllocSpotLocked ... }` 的分支结构则是照搬的。

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 对十个文件跑过，
新增行无新告警（剩余的 `readability-identifier-naming` 全在既有的 `GDT`/`PIDT`/`IOAPICS`
等旧式命名上，不是本次改的行）。

QEMU 实测无回归，而且这次启动路径本身覆盖了两种落法：`kernel.dev.dbg_1.obi` 九条
`Reserve area` 齐全且地址与改动前逐条相同（`at()` 路径），LAPIC / IO-APIC 24 条中断线 /
GDT / IDT / HPET 全部映射成功并推进到线程调度（`anywhere()` 路径）；
`kernel.dev.obi` 到 `prof: no architectural PMU`。

新增的拒绝语义启动不走，用临时探针实测后撤掉（`kernel.dev.obi`，即 `DEBUG_ASSERT` 编译掉
的那个构建，正是旧代码会静默重叠的配置）：

```
PROBE at(free) -> Ok base=0xFFFFFF8804000000
PROBE at(occupied, same base) -> AlreadyExists
PROBE at(occupied, partial overlap) -> AlreadyExists
PROBE at(outside root) -> InvArgs
PROBE map_phys(anywhere) -> Ok base=0xFFFFFF8804002000
PROBE root children now = 11
```

第三行是关键：部分重叠正是 `VmaSet::insert()` 看不见、旧 `Fixed` 分支会放过去的那一种。
最后一行的 `anywhere()` 落在 `0xFFFFFF8804002000`，即刚建的探针区域之后的第一个空位，
说明它确实走了 `alloc_spot()` 而不是拿了什么默认地址。

未实测：`anywhere()` 在地址空间碰满时返回 `NotFound` 的分支。

**故意没做**

- 没有实现覆盖式映射。`at()` 现在一律拒绝，`VmPlacement::replacing()` 留给真的需要它的人。
- 没有给 `VmPlacement` 加对齐或范围约束（Zircon 的 `align_pow2`、上下界）。
  现在只有 `alloc_spot(size, PAGE_SIZE, m_base, m_base + m_size - 1)` 一种走法，
  参数化一个没有第二个调用方的维度只会先猜错。
- 没有动 `VmMapping::map()` 缺锁那条既有 FIXME，也没有动 `VmObjectPhysical` 一侧。

---

## `find_mapping_locked()` 改成游标下降，不再按嵌套深度递归（2026-09-04）

> 无序号：本文件多会话并发追加，6/7/8 已经各撞过两三次，见上一节的说明。

**文件**
- `kernel/mm/vm_area.cpp` — `find_mapping_locked()` 由自递归改为 `while` 循环

**症状**

潜伏，当前触发不到。`find_mapping_locked()` 找到的子节点若是子区域，就对它再调一次自己：

```cpp
if (child->is_mapping()) {
    return static_cast<VmMapping *>(child);
}
return static_cast<VmArea *>(child)->find_mapping_locked(va);
```

栈帧数等于 VMA 的嵌套深度。内核地址空间现在只有两层（root → 九个预置区域 → 映射），
所以看不出问题。但**没有任何东西给嵌套深度设上限**——`create_subvma()` 会在子区域里
再建子区域，要多深有多深——而这个函数是 `VmAspace::page_fault_internal()` 上的那个查找，
栈帧落在谁触发缺页谁的栈上。用户地址空间一旦能用，深度就是用户态说了算的，
这条路径直通内核栈溢出。

**原因**

递归在这里从头到尾没有换来任何东西，这是关键：**下降只会跟着一个子节点走**。
子节点互不相交，而一个地址是个单字节窗口，所以每一层至多有一个子节点能回答。
没有分叉要记，也就没有栈要留——递归调用保存的返回地址和栈帧全是纯开销。

这与真正需要栈的树遍历不同（那种要在一个子树走完之后回到兄弟节点上），
所以这一条能用最便宜的手法解决，也因此值得先做：改动最小，而收益最直接
（它在缺页热路径上）。`protect`/`unmap` 那两处要遍历一个范围内的**全部**子节点，
是真的有分叉，去递归要另一套手法，分开做。

**改法**

游标换成局部变量，递归调用变成给游标赋值：

```cpp
auto *area = this;
while (true) {
    VmaSet::Enumerator enumerator{area->m_subvmas, va, va};
    auto *const child = enumerator.next();
    if (!child) {
        return nullptr;
    }
    if (child->is_mapping()) {
        return static_cast<VmMapping *>(child);
    }
    area = static_cast<VmArea *>(child);
}
```

`contains(va)` 仍然只在入口查一次：`Enumerator` 交出来的子节点保证与 `[va, va]` 相交，
也就一定含有 `va`，逐层再查是多余的。

**参照**

`Zircon VmAddressRegion::FindMappingLocked()`（`zircon/kernel/vm/vm_address_region.cc:508`）
是同一个循环，理由也一样：

```cpp
const VmAddressRegion* vmar = this;
while (const VmAddressRegionOrMapping* next = vmar->subregions_locked().FindRegion(va)) {
    if (auto mapping = next->as_vm_mapping_ptr()) {
        return const_cast<VmMapping*>(mapping);
    }
    vmar = next->as_vm_address_region_ptr();
}
return nullptr;
```

Zircon 的 VMAR 四个范围操作（`FindMappingLocked`、`Protect`、`UnmapInternalLocked`、
`DestroyLocked`）**一个都不递归**，三种不同手法，这是最简单的那种。
Linux 没有对应物：它的 VMA 不嵌套，每个 `mm` 一棵平的 maple tree，从源头没有这个问题。

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 跑过，无新告警。

QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到线程调度，
`kernel.dev.obi` 到 `prof: no architectural PMU`。

启动路径的嵌套只有两层，探不出"多走一步"，所以用临时探针在根 VMA 空闲处搭了一棵
root → a → b → mapping 的三层树，实测后撤掉（`kernel.dev.obi`）：

```
PROBE build depth-3 -> Ok
PROBE find(depth 3) -> k:probe-m base=0xFFFFFF8804000000
PROBE find(hole) -> <none>
PROBE find(outside) -> <none>
```

第二行是正向证据：循环连下三层拿到了叶子映射。后两行是两个否定面——
落在 `a` 里但 `b` 没覆盖的那一页（真空洞），以及根区域之外的地址。

**故意没做**

- 没有给 `create_subvma()` 加嵌套深度上限。去掉递归之后深度不再是栈的问题；
  真要限制也该是策略层的事，且现在没有第二个理由。
- 没有动 `unmap_locked`/`protect_locked`/`can_protect_locked` 三处递归，它们要遍历
  范围内的全部子节点，需要一个深度优先的 `Enumerator`（靠 `m_parent` 爬回去），
  另起一条。

---

## `VmArea::destroy()` 向上爬的是 `this` 的父节点，不是游标的（2026-09-04）

> 无序号，理由同前。

**文件**
- `kernel/mm/vm_area.cpp` — `destroy()` 的爬升改读 `curr->m_parent`；补上这段循环的注释

**症状**

不是潜伏，是"没人调用所以没暴露"。`destroy()` 是一个下降—爬升的迭代遍历，
爬升那一步写的是：

```cpp
auto *const parent = m_parent;   // this 的父节点，不是 curr 的
curr->Base::destroy();
...
curr = parent ? ustl::adopt<Self, ...>(parent) : ustl::Arc<Self>();
```

`m_parent` 是 `this->m_parent`——不管游标 `curr` 已经下降到多深的子孙，
爬升永远回到**被调用者自己的父节点**。

后果比"少拆几个节点"严重得多：对一个子区域调 `destroy()`，游标下到孙子、拆完、
然后爬到**根 VMA**，于是循环接着把整个内核地址空间往下拆。本次用临时探针实测
（`kernel.dev.obi`，根 VMA 空闲处搭 root → a → {b → 映射, c → 映射} 两条分支，
对 a 调 `destroy()`），**修之前**：

```
PROBE built -> Ok root=10 a=2 b=1 c=1
[cpu0 error] Unhandled page fault: vector=14, error_code=0x0
  rip=0xffffff884002f52b  rsi=0x800000000  r8=0xffffff8800000000
```

第二条 PROBE 根本没打出来。`rsi` 是 `0x800000000`（physmap 的长度）、
`r8` 是 `0xFFFFFF8800000000`（per-CPU 窗口的基址），也就是说它当时正在解除 physmap 的映射
——把自己脚下的地址空间拆掉，然后踩空。

**修之后**：

```
PROBE built -> Ok root=10 a=2 b=1 c=1
PROBE destroy(a) -> Ok root=9 a=0 b=0 c=0
PROBE find under a -> <none>
```

`root=10 → 9` 是 a 从根上摘掉，`a=b=c=0` 是两条分支都拆干净了，
第三行确认叶子映射也不在了。

**原因**

值得说的是这个错为什么活了下来。`m_parent` 和 `curr->m_parent` 在**一层深**的树上
是同一个东西：`curr` 从来没离开过 `this`，两种写法结果相同。内核地址空间正好只有两层，
而且 `destroy()` 至今没有真实调用方，所以既没有人写出会分叉的树，也没有人跑过这条路径。

这正是**用游标代替递归**这类改写要特别小心的地方：递归版本里"我的父节点"就是调用栈，
语言帮你记着，不可能记错；改成显式游标之后，"当前节点"和"起点"变成两个必须由人分清的变量，
而它们的成员访问写出来长得几乎一样（`m_parent` vs `curr->m_parent`），编译器不会有任何意见。
这条与本轮另外几条去递归的改动是同一件事的两面：去掉递归是对的，但显式游标要求每一处
成员访问都显式说明是"谁的"。

**改法**

一行：`auto *const parent = curr->m_parent;`。另外把那段循环的注释补厚——
说清楚 `m_parent` 在这里就是栈的替代物（每个节点都知道回去的路，所以下降靠子节点、
爬升靠父指针，既不需要栈帧也不需要数组），以及为什么读父指针必须在 `Base::destroy()`
**之前**（`Base::destroy()` 会把 `m_parent` 清成 nullptr）。原来那句
"If not a mapping, recursive to keep destroying" 也改掉了：这里没有递归，
而且不能在这里直接销毁，因为 `Base::destroy()` 会在它自己的子节点还指着它的时候把它摘掉。

**参照**

`Zircon VmAddressRegion::DestroyLocked()`（`zircon/kernel/vm/vm_address_region.cc:409`）
是同一个形状，而且它把这个区别写对了：

```cpp
fbl::RefPtr<VmAddressRegion> cur(this);
while (cur) {
    ...
    if (child_region) {
        cur = child_region;                 // 下降
    } else {
        if (cur->parent_) { cur->parent_->subregions_.RemoveRegion(cur.get()); }
        VmAddressRegion* cur_parent = cur->parent_;   // ← cur 的，不是 this 的
        cur->parent_ = nullptr;
        cur.reset((cur.get() == this) ? nullptr : cur_parent);
    }
}
```

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 跑过，
无新告警（`vm_area.hpp:143` 那条 `modernize-use-override` 是既有的析构声明）。

QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到线程调度，
`kernel.dev.obi` 到 `prof: no architectural PMU`。前后对比见上面「症状」一节。

`rip=0xffffff884002f52b` **没有符号化成功**——`llvm-nm` 在这个偏移附近只有 `__rodata_start`，
多半是被内联掉的局部函数。没有继续追，因为寄存器已经把现场说清楚了，
而且修完之后这条路径不再存在。

**故意没做**

- 没有动 `destroy()` 的引用计数。查过 `ustl::Rc(T*)` 是 **retain 一次**而不是接管一个既有的
  强引用（`system/lib/ustl/include/ustl/rc.hpp:283`），所以 `Arc<Base> aom(...)` +1、
  `adopt` 给 `curr` +1、`aom` 析构 -1，净额平衡，是对的。
- 没有让 `destroy()` 在中途失败时回滚。它现在遇到子节点销毁失败就直接返回，
  留下一棵拆了一半的树。这与 `unmap()` 的取舍一致（重复销毁是幂等的），
  但和 `protect()` 的"先问后写"不同，值得在有真实调用方时再定。

---

## `protect` 的两趟改走深度优先枚举器，不再按嵌套深度递归（2026-09-04）

> 无序号，理由同前。

**文件**
- `kernel/mm/include/ours/mm/vm_area.hpp` — 新增 `VmArea::MappingEnumerator`
- `kernel/mm/vm_area.cpp` — 实现它；`can_protect_locked()` 与 `protect_locked()` 改成平循环

**症状**

潜伏。`can_protect_locked()` 与 `protect_locked()` 遇到子区域就对它再调一次自己，
栈帧数等于 VMA 的嵌套深度，而嵌套深度没有上限——`create_subvma()` 会在子区域里再建子区域，
要多深有多深。与 `find_mapping_locked()` 那条同源，只是这两处不在缺页路径上，
所以排在它后面。

**原因**

这两处和 `find_mapping_locked()` 不一样：那一条的下降**只跟着一个子节点**，
所以根本没有分叉要记，改成 `while` 循环就完了。范围操作要走窗口内的**全部**子节点，
是真的有分叉——一个区域有多个子节点，每个子节点又可能有自己的子节点，
走完一棵子树之后必须回到兄弟节点上。这才是需要"栈"的那种遍历。

但栈不必是调用栈，也不必是数组：**`m_parent` 就是那个栈**。每个节点都知道回去的路，
所以"走完一层就爬一格，然后接着往下找兄弟"这件事，一个游标就够了。
爬回去之后怎么知道刚才走到哪儿了？不记，重新找——树是以 `base` 为键的，
`seek(finished->base())` 必然精确落在刚走出来的那个子节点上
（`lower_bound()` 命中它的键，而回退那一步不会触发，因为它前面的兄弟结束于该基址之下），
然后 `++` 就是下一个兄弟。这样每一层的位置都是 O(log n) 重新算出来的，
而不是 O(depth) 存下来的——用一点时间换掉全部空间，正是"没有上限的深度"要的那种取舍。

另一半同样重要：**范围操作真正动手的只有叶子**。子区域自己不持有页表，
`protect()` 下降到它只是为了够到它下面的映射。所以树的形状对调用方来说是纯粹的记账，
把它在枚举器里摊平之后，两个函数各自塌成一个平循环——
`can_protect_locked()` 甚至不再需要裁剪窗口，因为它只问权限、不问范围。

**改法**

新增 `VmArea::MappingEnumerator`：给定一个区域和一个闭区间窗口，
按地址序交出窗口内的全部**叶子映射**，中间的子区域对调用方不可见。

```cpp
auto VmArea::MappingEnumerator::next() -> VmMapping * {
    while (true) {
        if (m_iter == m_area->m_subvmas.end() || (*m_iter)->base() > m_last) {
            if (m_area == m_root) {
                return nullptr;
            }
            auto *const finished = m_area;
            m_area = finished->m_parent;                          // 爬升
            m_iter = m_area->m_subvmas.seek(finished->base());     // 重新找，不记
            ++m_iter;
            continue;
        }
        auto *const child = *m_iter;
        if (child->is_mapping()) {
            ++m_iter;
            return static_cast<VmMapping *>(child);
        }
        m_area = static_cast<VmArea *>(child);                     // 下降
        m_iter = m_area->m_subvmas.seek(m_first);
    }
}
```

下降时**不**推进父层的迭代器，因为爬升那一步会靠 `seek(finished->base())` 重新定位并
`++` 越过它；两边都推进就会漏掉一个兄弟。终止性：每次下降严格变深，每次爬升严格变浅
且把父层的迭代器往前推一格，所以不会原地打转。

下降后从 `m_first` 而不是从子区域自己的基址开始 seek，两个方向都对：窗口开在子区域内部时
能接住跨界的那个子节点；窗口开在子区域之下时，所有子节点的基址都高于 `m_first`，
`seek()` 落在 `begin()` 上而回退不会触发。

`can_protect_locked()`：

```cpp
MappingEnumerator enumerator{*this, base, base + size - 1};
while (auto *const mapping = enumerator.next()) {
    if (!mapping->validate_mmuflags(mmuf)) {
        return false;
    }
}
return true;
```

`protect_locked()` 同样，只多一步把窗口和映射求交后交给 `VmMapping::protect_locked()`。

**故意只交叶子**：也交子区域的模式是地址空间转储、拆除记账想要的
（`docs/repairs/260903/mm.md` 第 5 条），等它有调用方时再加，就像 Zircon 用
`VmAddressRegionEnumeratorType` 区分 `VmarsAndMappings` 与 `MappingsOnly` 那样。

**参照**

`Zircon VmAddressRegionEnumerator`（`zircon/kernel/vm/include/vm/vm_address_region_enumerator.h`）
是同一件东西：迭代式，靠 `up = up->parent_` 爬回去（`next()` 里那段
`do { ... up = up->parent_; } while (depth_-- != kStartDepth);`），另带一个 `depth_` 计数。
`Zircon VmAddressRegion::Protect()`（`vm_address_region.cc:1135`）用它跑
`MappingsOnly` 的两趟——先一趟检查每个映射能否接受新权限，再一趟应用——
和我们这里的 `can_protect_locked()` / `protect_locked()` 结构完全一致。
Zircon 的 VMAR 四个范围操作一个都不递归，三种手法：`FindMappingLocked()` 是单路循环、
`Protect()` 是这个枚举器、`DestroyLocked()` 是下降—爬升循环。
Linux 没有对应物，它的 VMA 不嵌套。

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 跑过，
无新告警（`vm_area.hpp:170` 那条 `modernize-use-override` 是既有的析构声明）。

QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到线程调度，
`kernel.dev.obi` 到 `prof: no architectural PMU`。

启动路径不 protect 也不嵌套，用临时探针在根 VMA 空闲处搭了一棵**深度参差**的树后实测撤掉
（`kernel.dev.obi`）：

```
a -> b -> m0        （第 0 页，深度 3）
a -> m1             （第 1 页，深度 2）
a -> c -> d -> m2   （第 2 页，深度 4）
a -> m3             （第 3 页，深度 2）
```

```
PROBE   walk[0] m0 base=0xFFFFFF8804000000
PROBE   walk[1] m1 base=0xFFFFFF8804001000
PROBE   walk[2] m2 base=0xFFFFFF8804002000
PROBE   walk[3] m3 base=0xFFFFFF8804003000
PROBE walk total=4
PROBE   walk[0] m1 base=0xFFFFFF8804001000   ← 窗口只盖中间两页
PROBE   walk[1] m2 base=0xFFFFFF8804002000
PROBE walk total=2
PROBE protect(whole, R) -> Ok
```

四个叶子按地址序全部走到，深度在 2/3/4 之间来回，说明下降、爬升、找兄弟三步都对；
第二组是窗口裁剪，只交出该交的两个。

**未实测**：`can_protect_locked()` 返回 false 那条分支。构造不出来——
所有区域和映射的 `VmaFlags` 现在都等于根 VMA 的 `PermMask`，没有一个叶子能拒绝任何权限。
原因是 `create_subvma()`／`create_mapping()` 把自己的 `vmaf` 参数丢掉、一律用 `m_vmaf`，
已记入 `docs/repairs/260903/mm.md`。

**故意没做**

- 没有给 `MappingEnumerator` 加 const 形式，`can_protect_locked()` 因此仍然不是 const。
  与 `VmaSet::Enumerator` 保持一致，等第二个只读遍历出现时一起做。
- 没有加 `depth` 计数。Zircon 需要它是因为 `VmEnumerator` 的回调要报深度，
  我们这里没有调用方关心。
- 没有动 `unmap_locked()`。它边走边销毁子节点，B 树的 erase 会让迭代器失效，
  而这个枚举器的爬升还依赖"刚走出来的子节点仍能按 base 找回来"——两条都不成立，
  要另一套办法，单独一条。

---

## `unmap()` 拒绝部分跨越子区域，最后一处按深度递归的路径消失（2026-09-04）

> 无序号，理由同前。

**文件**
- `kernel/mm/vm_area.cpp` — `unmap_locked()` 加一趟只读的可行性检查；主循环不再下降，
  整体覆盖的子节点（区域与映射一视同仁）直接销毁

**症状**

潜伏。`unmap_locked()` 遇到部分覆盖的子区域就对它再调一次自己，栈帧数等于嵌套深度，
而嵌套深度没有上限。这是 VMA 这一层最后一处按深度递归的路径。

**原因**

前两条能纯机械地去递归，这一条不能，值得说清楚为什么。

`find_mapping_locked()` 的下降只跟着一个子节点，没有分叉；`protect` 有分叉，
但它只读，可以用 `m_parent` 当栈爬回去。`unmap` 两条都不占：它**边走边销毁**，
而 B 树的 erase 会重平衡，树里任何位置的迭代器都可能失效——`MappingEnumerator` 的爬升
恰恰依赖"刚走出来的子节点还能按 base 找回来"，销毁之后这个前提就没了。
所以照搬第二条的手法在这里是错的。

剩下两条路：要么做一个可暂停/可恢复的遍历（每次销毁之后从一个记住的地址重新定位，
Zircon 的 `pause()`/`resume()` 就是干这个的），要么**让"需要下降"这件事不再发生**。

选了后者，而且理由不只是省事：**子区域是一段地址空间的预约，不是一袋页**。
从外面往一个子区域中间捅一个洞，语义上没有明显答案——预约是缩小、分裂，还是原样留个空洞？
Zircon 面对同一个问题的选择是不猜：`UnmapInternalLocked()` 直接拒绝部分跨越子 VMAR 的范围，
真想拆掉它的调用方应当点名它、在它里面 unmap。一旦子区域只能被整体覆盖，
就没有什么可下降的了——整体覆盖的子区域直接销毁，而 `destroy()` 本身已经是游标遍历而非递归。

这是**语义收紧**，不是纯重构：改之前 `unmap` 部分覆盖一个子区域会下降进去把里面的映射解掉、
留下一个空的子区域；改之后返回 `InvArgs`。当前没有任何调用方（`VmArea::unmap()` 是个
公开接口但全树无人调用），所以这个窗口是免费的——趁没人依赖旧语义时定下更严的那一种。

**改法**

两趟，形状与 `protect()` 一致：

```cpp
// 一、只读，问"这个范围合法吗"
{
    VmaSet::Enumerator enumerator{m_subvmas, base, last};
    while (auto *const child = enumerator.next()) {
        if (child->is_mapping()) {
            continue;                       // 映射是一串页，两端都可以裁
        }
        if (child->base() < base || child->last() > last) {
            return Status::InvArgs;         // 子区域被切了一半
        }
    }
}

// 二、动手
auto iter = m_subvmas.seek(base);
while (iter != m_subvmas.end()) {
    auto *const child = *iter;
    if (child->base() > last) break;
    if (base <= child->base() && child->last() <= last) {
        child->retain();
        iter = m_subvmas.erase(iter);       // 交回后继，因为 erase 会让迭代器失效
        auto const status = child->destroy();
        child->release();
        ...
        continue;
    }
    DEBUG_ASSERT(child->is_mapping(), "a partly covered sub-area survived the check pass");
    ...                                     // 裁剪，不删除，走 ++
}
```

先问后写的理由和 `protect()` 一样：主循环销毁掉第一个子节点之后就没法回滚了，
所以"范围切了子区域一半"这种错必须在动手之前整体拒绝，而不是半途返回。
整体覆盖的分支现在对区域和映射一视同仁——`destroy()` 是虚的，`VmArea` 的重写版本
自己用 `m_parent` 把子树下降—爬升地拆完。

**参照**

`Zircon VmAddressRegion::UnmapInternalLocked()`（`zircon/kernel/vm/vm_address_region.cc:873`）
就是这两趟：

```cpp
for (auto itr = subregions_.IncludeOrHigher(base);
     itr.IsValid() && (*itr).second->base() <= end_addr_byte; ++itr) {
  ...
  if (!(*itr).second->is_mapping() &&
      (!can_destroy_regions || (*itr).second->base() < base || itr_end_byte > end_addr_byte)) {
    return ZX_ERR_INVALID_ARGS;
  }
}
while (true) {
  auto itr = subregions_.IncludeOrHigher(base);
  ...
  if (unmap_base == curr_region->base() && unmap_size == curr_region->size()) {
    zx_status_t status = curr_region->DestroyLocked();
  } else {
    ... mapping->UnmapLocked(unmap_base, unmap_size);
  }
}
```

Zircon 比我们多一个 `can_destroy_regions`（来自 `VmAddressRegionOpChildren`）：
系统调用路径传 `No`，于是范围只要碰到任何子 VMAR 就拒绝；内核内部路径传 `Yes`，
允许整体销毁。这个区别要有系统调用路径才有意义，我们还没有，所以没有实现——
相当于恒为 `Yes`。

Linux 不需要这一层判断：VMA 不嵌套，`do_munmap()` 面对的永远是一串平的 VMA，
部分覆盖就地拆分（`__split_vma()`）。

**验证**

构建通过（两个镜像）；`clang-format` 与 `clang-tidy -p out/bringup.x86` 跑过，无新告警
（`vm_area.hpp:170` 那条 `modernize-use-override` 是既有的析构声明）。

QEMU 实测无回归：`kernel.dev.dbg_1.obi` 九条 `Reserve area` 齐全并推进到线程调度，
`kernel.dev.obi` 到 `prof: no architectural PMU`。

启动不 unmap，用临时探针在根 VMA 空闲处搭树后实测撤掉（`kernel.dev.obi`），
`a` 是 8 页，下面混着两个子区域和两个直属映射：

```
PROBE built -> Ok
PROBE   leaf[0] m0 ... leaf[3] m3      PROBE leaves=4 children=4
PROBE unmap(half of sub-area b) -> InvArgs
PROBE   leaf[0] m0 ... leaf[3] m3      PROBE leaves=4 children=4   ← 一个都没动
PROBE unmap(trim m3 tail) -> Ok
PROBE unmap(all of sub-area c) -> Ok
PROBE   leaf[0] m0  leaf[1] m1  leaf[2] m3   PROBE leaves=3 children=3
```

三件事各自对上：切了子区域 `b` 一半的范围被拒，**而且树完全没动过**（拒绝是整体的，
不是走到一半退出来）；裁剪直属映射 `m3` 的尾巴被允许，不删除任何节点；
整体覆盖子区域 `c` 的范围把它连同里面的 `m2` 一起销毁，`a` 的子节点数 4 → 3。

至此 `kernel/mm/vm_area.cpp` 里 `static_cast<VmArea *>(child)->...` 形式的自递归调用
一处不剩（`grep` 为空）。

**故意没做**

- 没有实现 Zircon 的 `VmAddressRegionOpChildren::No`。见「参照」：要有系统调用路径才有意义。
- 没有做可暂停/可恢复的遍历。见「原因」：拒绝部分跨越让它变得不必要，
  真到了需要"从外面挖穿子区域"的那天，该先回答那个语义问题，而不是先写遍历。
- 没有让 `unmap` 在中途失败时回滚。第一趟已经挡掉了唯一一种不可恢复的错法；
  剩下的失败（映射自己解除失败）重来一次是幂等的。
