# kernel/lib/gktl — 改动记录

## 命令树不再走 std::allocator，改用 ustl 的可失败分配器（2026-09-24）

**文件**
- `kernel/lib/gktl/include/gktl/radix_tree.hpp` — 分配器协议从 `std::allocator_traits` 换成 ustl 的 `Rebind` / `try_allocate` / `deallocate`，与 `ustl::collections::BTree` 同一套
- `kernel/lib/gktl/src/command.cpp` — 命令树改用 `ustl::collections::Allocator<Command>`，并显式指定 `ktl::kernel_memory_resource()`
- `kernel/lib/gktl/BUILD.gn` — `gktl` 依赖 `//kernel/lib/ktl`；`headers` 公开依赖 `//system/lib/ustl:headers`

**症状**
在 `kshell> ` 输入任何命令（例如 `log`），串口先打 `CPU[1..3] suspended`、一段 ring dump，
最后是 `[... cpu0 error] operator new`，内核停住。每条控制台命令都是第一次分发就 panic。

**原因**
`gktl::run_command()` 第一次被调用时由 `build_radix_tree()` 建树，树的类型是

```cpp
// kernel/lib/gktl/src/command.cpp（改前）
using CommandTree =
    RadixTree<Command, std::allocator<Command>, CommandKeyOfValue, CommandKeyEncoder>;

CommandTree TREE{};
```

`RadixTree` 通过 `std::allocator_traits` 把它 rebind 成节点分配器，`reserve()` 里调
`NodeAllocatorTraits::allocate()`，最终落到 `std::allocator::allocate()` →
全局 `::operator new(size_t)`。这个内核里**没有**真正的 `operator new(size_t)`：
内核的 `new` 只有带 `ours::mm::Gaf` 的 placement 形式（`heap/new.hpp`），不带 gaf 的那个
只是 `kernel/lib/libc/cpp_dependencies.cpp` 里的弱符号桩，函数体就是
`ours::panic(__func__)`——所以串口上看到的 panic 文本恰好是 `operator new`。
objdump 可以直接看到：改前 `gktl::run_command(int, char**)` 里有一条 `call _Znwm`。

更深一层，std 分配器协议本身和这棵树的用法就对不上：

- `std::allocator::allocate()` 失败时抛 `std::bad_alloc`，不会返回空指针；内核是
  `-fno-exceptions`，所以 `reserve()` 里的 `if (m_nodes == nullptr)` 永远不是它的失败路径。
- std 协议的默认实现 `std::allocator` 在 freestanding 内核里没有可用的后端，
  任何人只要照着 std 惯例写 `std::allocator<T>` 就会踩到同一个桩。

**改法**

树改为说 ustl 的可失败分配器协议——和 `ustl::collections::BTree` 完全一样：
`typename A::template Rebind<Node>` 做 rebind，`try_allocate(n)` 返回
`ustl::Result<Node *, Error>`，失败是一个值而不是异常；构造/析构用
`std::construct_at` / `std::destroy_at`。

```cpp
// radix_tree.hpp（改前）
using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
using NodeAllocatorTraits = std::allocator_traits<NodeAllocator>;
...
m_nodes = NodeAllocatorTraits::allocate(m_node_allocator, capacity);
if (m_nodes == nullptr) {
    return false;
}

// radix_tree.hpp（改后）
using NodeAllocator = typename Allocator::template Rebind<Node>;
static_assert(requires(NodeAllocator &allocator, Node *node, usize count) { ... },
              "RadixTree allocators must use the USTL fallible allocation interface.");
...
auto allocation = m_node_allocator.try_allocate(capacity);
if (allocation.is_err()) {
    return false;
}
m_nodes = allocation.value();
```

ustl 分配器之间没有跨元素类型的转换构造，只共享 resource，所以带分配器的构造函数
照 BTree 的做法从 `allocator.resource()` 重建节点分配器：
`m_node_allocator(*allocator.resource())`。

命令树这边：

```cpp
// command.cpp（改后）
using CommandAllocator = ustl::collections::Allocator<Command>;
using CommandTree = RadixTree<Command, CommandAllocator, CommandKeyOfValue, CommandKeyEncoder>;

CommandTree TREE{CommandAllocator{ktl::kernel_memory_resource()}};
```

为什么显式写 `ktl::kernel_memory_resource()` 而不用默认构造的分配器：
`ustl::collections::Allocator` 在**构造时**就把当时的默认 resource 记下来；`TREE` 是
命名空间作用域的全局对象，由 `start_kernel()` 第一行的
`ccrt::init_global_static_objects()` 构造，而把默认 resource 换成内核堆的
`ktl::init_memory_resource()` 要到 `init_pmm()` 里才跑。默认构造会记下 null resource，
`reserve()` 返回 OutOfMemory，每条命令都变成静默的"找不到"——比 panic 更难查。
显式指定 resource 只是记下"用哪个堆"，第一次真正分配发生在第一次分发命令时，那时堆早已就绪。

被否掉的方案：

- **复用 `ktl::Allocator`**。它能过 `std::allocator_traits`（`RebindT` 不被识别，但
  `Allocator<U>` 形式的模板会被 traits 自动替换首参），但它的契约是内核自己的：
  `allocate(n)` 用 `new (gaf) T[n]()` 交回 n 个**已值初始化**的元素，`deallocate()` 用
  `delete[]` 把它们析构掉——`ustl::collections::Vec` 和 `CircularBuffer` 依赖这一点。
  树自己用 `construct` 逐个构造节点，套上它就是对每个节点构造两次、析构两次。
  把 `ktl::Allocator` 改成标准语义又会打破 Vec/CircularBuffer，所以不动它。
- **在 command.cpp 里写一个局部的 std 风格分配器**。能用，但 ustl 的可失败协议已经是
  内核容器的通用分配器（BTree、VmaSet 都用它），再造一个只会多一套失败语义。

**参照**
无直接对应物，本项目自有设计。协议照的是本仓 `ustl::collections::BTree::create_node()` /
`destroy_node()`（`system/lib/ustl/include/ustl/collections/detail/btree.hpp`）。
Zircon 的内核容器同样不走 std 分配器，而是 `fbl::AllocChecker` + 带检查的 `new`，
失败显式返回——同一个理由：内核里分配失败必须是一个可以检查的值。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-tidy 在改动行上只剩 `TREE` /
  `TREE_BUILT` 的命名告警，那是原有名字（仓库里可变全局一律大写），不在这次改动内。
- objdump `vmours.with-tests`：`gktl::run_command()` 里不再有 `_Znwm`。镜像里剩下的
  7 处 `operator new(unsigned long)` 调用全在 libc++ 的 `std::format` 缓冲和
  `basic_string` 里，与本改动无关。
- QEMU（`scripts/qemu_boot.py`，dev 镜像）：`kshell> ` 下输入 `log`、`log level bogus`
  都回到提示符，不再 panic。**但命令其实没有执行**——`log level bogus` 应当打出的
  `log: unknown level 'bogus'` 没有出现。原因是下一层的缺陷：整棵树一次要约 50 个
  552 字节的节点（≈27 KiB），而 `ktl::KernelMemoryResource` 直接调 `heap::kmalloc()`，
  超过 4 KiB 一律返回空，`reserve()` 因而失败。这一条单独修，见下一节。

**故意没做**
- 没有把 `TREE`/`TREE_BUILT` 改成小写：仓库里可变全局的惯例就是大写，改名是另一件事。
- 没有让 `ustl::collections::Allocator` 延迟到第一次分配时才取默认 resource。
  "构造时记下 resource"是 `std::pmr::polymorphic_allocator` 同样的语义，改它影响所有
  ustl 容器；全局对象显式指定 resource 就够了。
- 没有缩小 `Node`（66 个子指针，552 字节）。树的形状是另一个设计问题，与分配失败无关。
