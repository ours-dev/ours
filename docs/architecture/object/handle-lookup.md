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

arena = **一段地址连续、按固定步长切成同尺寸槽位的内存**，只装 `Handle` 一种东西。
地址空间一次预留，物理页惰性提交。

这不是随手选的分配器，而是**让 handle 值能成立的前提**：槽位等宽且连续，于是
`地址 ↔ 下标` 是一次纯算术（`(addr - base) / sizeof(Handle)`），一个小整数就能指认一个
`Handle`。`Handle::m_baseval` 存的就是那个下标。

被否掉的替代方案：每进程维护一张「数字 → `Handle *`」的映射。它多一层间接、每进程多一份
内存、且每次查找多一处要加锁——而 handle 查找在**每一次系统调用的入口**都要发生一遍，
是整个内核里最热的路径之一。查询速度在这里压倒其它考虑，这是本决定的理由。

## 用户看到的值怎么算

照 Zircon（`kernel/object/handle.cc`、`handle_table.cc`）。分两层，别合并着看：

**第一层，`base_value`（32 位，仅内核可见）**：

```
 [31..30] 保留，必须为 0     HANDLE_RESERVED_BITS = 2
 [29..18] generation         HANDLE_GENERATION_MASK
 [17..0]  arena 下标         HANDLE_INDEX_MASK = HANDLE_MAX_COUNT - 1
```

`HANDLE_MAX_COUNT = 256 * 1024`，**必须是 2 的幂**（`static_assert`），下标才能用掩码取。
`HANDLE_GENERATION_SHIFT = log2(HANDLE_MAX_COUNT) = 18`。和 Zircon 一样另有 `static_assert`
要求 generation 至少能数到 255，否则位宽不够用。（Zircon 里对应的名字是 `kHandleReservedBits`、
`kHandleGenerationMask`、`kMaxHandleCount`。）

保留的第 31 位在内核里另有一个用处：arena 给**占用中**的槽记录 `base_value | HANDLE_LIVE_MARK`
（第 31 位），空槽记 0。查找拿 `base_value | HANDLE_LIVE_MARK` 去比，空槽的 0 永远比不中——
所以不再需要把 generation 0 留作"空"的记号，查找也少了一道判断（见下文第 4 道关）。

**第二层，用户看到的值**：

```
handle 值 = ((base_value << 2) | 0b11) XOR 该表的随机数
```

左移 2 位正是保留位存在的理由：`base_value` 的高 2 位为零，移位才不丢信息。

每表随机数**必须预先左移 2 位**（低 2 位为零）：XOR 之后低 2 位仍得是 `0b11`，
第 1 道关才对合法值成立。Zircon 生成随机数时同样先左移 `kHandleReservedBits`。
（P1-A 里随机数仍取 0；CPRNG 在 P1-E。）

## 槽位复用：generation 是必须的

一个 arena 槽被释放后会再分配给别人。`GetNewBaseValue()` 读出**空闲槽里存着的旧
`base_value`**，把 generation 加一再写回（`NewHandleValue()`），所以同一个下标每次复用都
得到不同的 `base_value`。

解析时 `Handle::FromU32()` 由值算出下标、取到槽地址，**再比对
`handle->base_value() == value`**——陈旧的值下标虽然对，generation 对不上，解析失败。

（本文档早先的版本写着「不加 generation 位，靠随机数和表 koid 挡」。那是错的：
随机数是每张表一个常量，同一张表里前后两次复用同一槽位算出的值会完全相同，
表 koid 也一样对得上。没有 generation，`close` 之后的陈旧值会解析到新主人的对象上。）

## 解析的五道关，各防一件事

1. **低 2 位必须是 1** —— 挡掉 0（`ZX_HANDLE_INVALID`）和随手编的小整数。
2. **XOR 回每表随机数** —— 用户猜不出别人的值，也看不出 arena 布局。
3. **下标落在 arena 内** —— Zircon 是 `arena_.Committed()`（惰性提交，下标落在未提交的页上
   就失败）。这里 arena 目前固定 `HANDLE_ARENA_SLOTS = 2^12` 个槽、启动时就有，所以这一道是
   `arch::confine_array_index()`：越界的下标被夹成 0，而不是用一个分支拒绝——夹成 0 之后
   第 4 道关自然失败，因为探针里仍是原来的下标。惰性提交推迟到 P1-C。
4. **`base_value` 全等比较** —— generation 对不上就失败，见上一节。和 Zircon 一样用
   `conditional_select_nospec_eq()` 做：结果是槽地址或 0，由条件传送产生，而不是由分支决定。
   防的是 **Spectre V1**：分支会被预测，误预测的路径会拿着用户选的 `Handle`（它的对象、
   它的权限）先跑一段，把选择留在缓存里。本文档早先写"防用旁路探测 arena 布局"，理由写错了——
   哪些槽被占用并不是值得设防的秘密。失败时返回的 0 在推测路径上被访问也无害：
   `USER_ASPACE_BASE` 以下（2 MiB）在任何地址空间里都不映射。这一步不能图省事写成普通 `if`。
5. **`handle->handle_table_id() == 本表 koid`** —— A 进程的合法值在 B 进程里解析不出来。

分工：**第 5 条是进程间隔离，第 4 条是时间上的隔离（复用后的陈旧值）**，1、2、3 是加固。
将来谁想省掉一步，先看清省的是哪一类。

## 容量与耗尽

- `HANDLE_MAX_COUNT = 256 * 1024`（全系统，不是每进程）是编码能表示的上限，2 的幂；
  当前真正有内存的是 `HANDLE_ARENA_SLOTS = 2^12` 个槽。
- Zircon 另有 `kHighHandleCount = kMaxHandleCount * 7 / 8`：越过这条线就打印一次告警，且**带节流**
  （`handle_count_high_log_.Ready()`），否则告警本身会淹掉串口。本项目还没有这条线。
- 耗尽时 `Alloc()` **返回 nullptr，不 panic**：记一个计数器、打印一条告警，让调用方把
  失败当作普通错误往上报。用户进程能耗尽的资源不该能把内核打死。

## 这条决定对 `mm` 提出的要求

`mm::ObjectCache`（`object_cache.hpp:152`）是本树「一类对象一个池」的现成设施，
`ThreadCache`、`VmaCache` 都建在它上面。但它和 arena 差一个性质：**它不保证地址连续，
也没有对外暴露「对象在池内的序号」**。所以两条路选一：

- 给 `Handle` 单独做一块连续 arena（Zircon 的 `fbl::GPArena<PreserveSize, sizeof(Handle)>`）；
- 或者给 `ObjectCache` 增加「取出对象在池内的序号 / 由序号取回对象」的能力。

后者影响面更大且属于 `kernel/mm` 的 lane，要动就得先和 mm 会话谈。前者更可控。

## 还没定的

（暂无。`Rights` 的位定义早已存在于 `system/public/ours/rights.hpp`；容量与耗尽行为见上。）
