# Handle 的查找路径：从用户寄存器里的一个数字到 `Dispatcher`

这份文档记的是**已定的设计决定**和它的理由，不是实现记录。实现由 `user/boot` 会话按此进行。

## 三层，别混起来

| 层 | 是什么 | 谁拥有它 | 生命周期 |
|---|---|---|---|
| `Dispatcher` | 内核对象本体：koid、signals、observers、引用计数 | 由 `ustl::Arc` 共享 | 最后一个 `Arc` 消失时析构 |
| `Handle` | 内核侧的一条记录 = (对象, 权限, 属于哪张表) | 一张 `HandleTable` | 从表里移除时释放 |
| handle 值 | 用户寄存器里那个 32 位数字 | 用户进程 | 由进程自己 close |

要点：**权限绑在 `Handle` 上，不绑在对象上**。同一个 `Dispatcher` 可以有多个 `Handle` 指向它、
各带不同的 `Rights`。这就是为什么 `kernel/lib/syscall/task.cpp` 里的 `sys_*` 收的是
`object::Handle *` 而不是 `Dispatcher *`——权限检查发生在解析 handle 的那一刻，一次，
在系统调用的入口，而不是散落在每个对象方法里。

`KernelHandle<T>` 是第四种东西，容易和 `Handle` 混：它是**还没装进任何表**之前，内核自己
持有对象的那种引用（Zircon 同名同义）。造完对象、还没决定给谁之前，拿的是它。

## 决定：`Handle` 住在一块 arena 里，查找是 O(1)

arena = **启动时一次划出、按固定步长切成同尺寸槽位的连续内存**，只装 `Handle` 一种东西。

这不是随手选的分配器，而是**让 handle 值能成立的前提**：槽位等宽且连续，于是
`地址 ↔ 下标` 是一次纯算术（`(addr - base) / sizeof(Handle)`），一个小整数就能指认一个
`Handle`。`Handle::m_baseval` 存的就是那个下标。

被否掉的替代方案：每进程维护一张「数字 → `Handle *`」的映射。它多一层间接、每进程多一份
内存、且每次查找多一处要加锁——而 handle 查找在**每一次系统调用的入口**都要发生一遍，
是整个内核里最热的路径之一。查询速度在这里压倒其它考虑，这是本决定的理由。

## 用户看到的值怎么算

照 Zircon（`kernel/object/handle_table.cc:14-40`）：

```
handle 值 = ((base_value << 2) | 0b11) XOR 该表的随机数
```

解析时反着来，然后三道关，各防一件事——顺序和分工都要保住：

1. **最低两位必须是 1**：把 0 和随手编的小整数立刻挡掉（`ZX_HANDLE_INVALID` 是 0）。
2. **XOR 每表随机数**：用户既猜不出别人的 handle 值，也看不出 arena 的布局。
3. **`handle->handle_table_id() == 本表 koid`**：A 进程的合法值在 B 进程里解析不出来。

**只有第 3 条是隔离**，前两条是加固。把它们的分工记清楚，将来谁想省掉一步时才知道省的是什么。

## 随之定下的：不给 handle 值加 generation 位

槽位复用后的陈旧值，由第 2、3 条挡：随机数让陈旧值算出来的下标基本落不到有效槽，
即使落到了，表 koid 也对不上。Zircon 同样不设 generation 位。

## 这条决定对 `mm` 提出的要求

`mm::ObjectCache`（`object_cache.hpp:152`）是本树「一类对象一个池」的现成设施，
`ThreadCache`、`VmaCache` 都建在它上面。但它和 arena 差一个性质：**它不保证地址连续，
也没有对外暴露「对象在池内的序号」**。所以两条路选一：

- 给 `Handle` 单独做一块连续 arena（Zircon 的 `fbl::GPArena<PreserveSize, sizeof(Handle)>`）；
- 或者给 `ObjectCache` 增加「取出对象在池内的序号 / 由序号取回对象」的能力。

后者影响面更大且属于 `kernel/mm` 的 lane，要动就得先和 mm 会话谈。前者更可控。

## 还没定的

- `Rights` 的位定义。
- arena 的容量上限，以及耗尽时的行为（Zircon 有 `kMaxHandleCount`，并在接近上限时告警）。
