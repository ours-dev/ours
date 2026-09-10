# mm —— 待修项（2026-09-10）

## `PmNode::NUM_ONLINE_NODES` 从来没有被赋过值，`num_online_nodes()` 返回垃圾

**严重程度**：中。目前**没有调用者**，所以是潜伏的；但它是个 public 访问器，
下一个用它的人会拿到一个看着像数字的垃圾，而且不会有任何报错。

**怎么发现的**
写 lldb 的 `nodes` 命令时顺手读的：

```
(lldb) p ours::mm::PmNode::NUM_NODES
(usize) 1
(lldb) p ours::mm::PmNode::NUM_ONLINE_NODES
(usize) 1879048187          ← 0x6FFFFFFB
```

一个节点的机器，"在线节点数" 是 18 亿。

**证据**
全仓搜索，这个变量只有声明和定义，**没有任何一处赋值**：

```
$ grep -rn 'NUM_ONLINE_NODES' kernel/mm --include='*.cpp' --include='*.hpp'
kernel/mm/pm_node.cpp:200:decltype(PmNode::NUM_ONLINE_NODES) PmNode::NUM_ONLINE_NODES;
kernel/mm/include/ours/mm/pm_node.hpp:240:        return NUM_ONLINE_NODES;
kernel/mm/include/ours/mm/pm_node.hpp:440:    static usize NUM_ONLINE_NODES;
```

对照旁边的 `NUM_NODES`，它在 `pm_node.cpp:310` 有 `NUM_NODES += 1;`。

更进一步：**这个符号在镜像里根本不存在**——

```
$ llvm-nm --demangle .../vmours | grep -c NUM_ONLINE_NODES
0
$ llvm-nm --demangle .../vmours | grep 'PmNode::NUM_NODES'
0000000000088c58 b ours::mm::PmNode::NUM_NODES
```

没有引用它的代码（唯一的读者 `num_online_nodes()` 是个 header 里的 inline，
而 `kernel/mm/include/ours/mm/pmm.hpp:82` 那层包装也没人调），存储被回收掉了。
调试器仍能按 DWARF 里的位置读出一个数——就是上面那个 18 亿。

**该怎么修**
两条路，选一条：

1. **补上赋值**。节点上线时递增，对标 Linux 的 `nr_online_nodes` / `node_states[N_ONLINE]`。
   赋值点应当和 `NUM_NODES += 1`（`pm_node.cpp:310`）在一起，或者在
   `PmNode::init()` 成功之后。
2. **删掉它**，连同 `num_online_nodes()` 和 `pmm.hpp:82` 的包装。一个没人赋值、
   没人读的计数器，留着只会让下一个人以为它是可信的。

倾向 1：NUMA 起来之后一定需要区分 "有多少节点" 和 "有多少在线节点"
（`mm_numa = true` + 四节点 QEMU 拓扑就是为此存在的）。

**顺带一提**
`nodes` 命令**故意不打印这个值**，就是因为它不可信；修好之后可以加进去。
