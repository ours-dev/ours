## PmNode::Current 的三个分配入口共用 local_node_for()（2026-09-28）

PmNode 结构整理第 5 步，也是最后一步。

**文件**
- `kernel/mm/pm_node.cpp`：新增 `PmNode::Current::local_node_for()`；`alloc_frames`、`alloc_frames_fragment`、
  `alloc_frames_bulk` 改为调用它
- `kernel/mm/include/ours/mm/pm_node.hpp`：声明

**症状**
无运行时症状，是重复代码。三个不带节点的分配入口，函数体逐行相同：
1. 应用线程的 NUMA 策略；
2. 找本地节点，找不到返回 `BadState`，旁边还跟着同一段三行注释；
3. 校验 canary；
4. 转交给节点。

唯一的区别是最后调用节点的哪个方法。

**原因**
同一套步骤写了三遍，改一处就得同步改三处。第 1 步里缩小 `resource_lock` 持锁范围时，就是在这三份里
各删了一遍同样的 `Guard`。再加第四个入口，就会有第四份拷贝。

**改法**

```cpp
auto PmNode::Current::local_node_for(Gaf &gaf) -> ktl::Result<PmNode *> {
    if (auto const status = apply_thread_policy(gaf); Status::Ok != status) {
        return ustl::err(status);
    }
    auto *const local_node = Current::resolve_local_node();
    if (!local_node) {
        return ustl::err(Status::BadState);
    }
    local_node->m_canary.verify<Self>();
    return ustl::ok(local_node);
}

auto PmNode::Current::alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    auto const node = local_node_for(gaf);
    if (!node) {
        return ustl::err(node.unwrap_err());
    }
    return (*node)->alloc_frames(gaf, order, fallback);
}
```

`gaf` 按引用传入，因为 `apply_thread_policy()` 会根据策略改写它，转交给节点的必须是改写后的值，
这一点和原来一致。

**参照**
Linux 没有这层包装：`alloc_pages()` 直接取 `numa_node_id()` 和 mempolicy，然后调用 `__alloc_pages()`。
合并成一个共同的前置函数，是本项目自己的整理。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过，QEMU 实测两个镜像 `usercopy.test` 都是 18/18。
- NUMA：`out/numa.x86` dbg_1 镜像用 `NUMA=1` 启动，跑到 shell，无异常；回退链与整理开始前逐行 `diff` 一致。

**故意没做**
无。PmNode 结构整理的五步到此完成：
1. 修 6 个潜伏 bug；
2. `Router` 和 `Topology` 改为内部类；
3. `bootstrap()` 挪进 `pm_node_init.cpp`；
4. `ZoneQueues` 收拢为 `ZoneList`；
5. 本节。

## 新增 pmnode.test：物理内存节点层的 kshell 测试（2026-09-28）

**文件**
- `kernel/commands/pm_node_test.cpp`（新）：`pmnode.test` 命令
- `kernel/commands/BUILD.gn`：加入新文件
- `kernel/mm/include/ours/mm/priv/zone_iterator.hpp`（新）：`ZoneIterator` 和 `AllocationContext` 从 `pm_node.cpp` 挪到这里
- `kernel/mm/pm_node.cpp`：删掉上面两个结构
- `kernel/mm/include/ours/mm/pm_node.hpp`：`PmNode::Router` 从私有区挪到公开区

**症状**
PmNode 结构整理一共动了五步，但其中几块没有任何东西覆盖：
- `ZoneIterator` 和 `AllocationContext` 没有调用者，改写后只做过编译检查；
- `Router::for_each_node()`、`contains()`、fragment 分配也都没有调用者；
- NUMA 下的回退顺序靠人工 diff 转储来确认，没有自动检查。

**原因**
这些代码要么没人调用，要么只在多节点机器上才表现出差别，启动和 `usercopy.test` 都碰不到。
另外有两处测试代码根本够不着：`ZoneIterator` 和 `AllocationContext` 写在 `pm_node.cpp` 里，外部看不到；
`Router` 是 PmNode 的私有内部类。

**改法**
- `ZoneIterator` 和 `AllocationContext` 挪进 `priv/zone_iterator.hpp`，与 `priv/frame_buddy.hpp` 是同样的用法：
  mm 内部使用，测试可以包含。
- `Router` 改为公开。它只做一件纯计算的事（按距离排出其它节点），公开没有风险。
- `pmnode.test` 按节点逐个检查，所以同一份测试在单节点上跑 20 项，在四节点上跑 56 项：
  - **Topology**：当前 CPU 能解析到一个存在的节点；`current_node()`、`cpu_to_node()`、`mm::current_node()` 三者一致；
    所有 CPU 都还在固件分配的节点上；`num_online_nodes()` 等于在线掩码的位数。
  - **ZoneList**：本地 zone 每种类型一个、属于本节点、按类型顺序遍历；回退链以本地 zone 开头；
    没有重复的 zone；远端 zone 按距离从近到远排列。
  - **Router**：其它有内存的节点恰好各出现一次、不包含自己、距离不减；候选集为空时路由为空；
    `for_each_node()` 访问的节点与遍历一致，而且会把额外参数转发给回调。
  - **ZoneIterator**：能跳过按类型数组里的空位；节点掩码过滤生效；类型上限过滤生效。
  - **AllocationContext**：带 `ThisNode` 只走本地 zone，不带则走完整条回退链。
  - **`contains()`**：结果和对应 zone 一致；没有该类型 zone 时为 false；超出最大 order 时为 false。
  - **fragment 和 bulk 分配**：n = 2、6、8、5、1 时，块数等于 n 的二进制中 1 的个数，总页数等于 n；
    bulk 分配返回 n 个互不相同的单页。
  - **禁止分配**：`disable()` 之后，不带节点和带节点两条路径都被拒绝；`enable()` 之后恢复正常。

写测试的过程中发现两处是测试自己错了，已改正：
- 比较了两个不同临时对象的 `begin()` 和 `end()`。单节点上碰巧相等，四节点上不相等。
- 以为 `GAF_KERNEL` 不带 `ThisNode`。其实它带着（`gaf.hpp` 里 `Kernel = ThisNode | Reclaim | Write`），
  要测"整条回退链"就得先去掉这一位。单节点上本地 zone 就是整条回退链，所以没暴露出来。

**参照**
无。写法沿用 `usercopy.test` 的模式：kshell 命令，逐项打印 pass/FAIL，最后给出汇总。

**验证**
- dev：`pmnode.test` 20/20，`usercopy.test` 18/18；dbg_1：`pmnode.test` 20/20。
- NUMA（`out/numa.x86`，`NUMA=1`，四节点）：`pmnode.test` 56/56。
- 变异检查，确认测试能抓到真正的回归：
  - 把 fragment 循环改回原来的错误写法：测试卡在 n=2，没有打出汇总行；
  - 让 `ZoneIterator` 忽略节点掩码：dev 和 NUMA 上都报 `FAIL ZoneIterator's node mask filters out the excluded node`。
  两处改动都已还原。
- clang-tidy：新文件无告警。

**故意没做**
- 没测运行期换节点（`PmNode::Current::set_nid()`）：它会改动全局的 CPU 归属，在测试里改了再改回来，会影响同时在跑的其它 CPU。
- 没断言分配前后空闲页数完全相等：其它 CPU 同时在分配，结果会时好时坏。

## 清理三条已修复却仍留在 repairs 里的旧条目（2026-09-28）

**文件**
- `docs/repairs/260919/kernel-mm.md`：删掉第 1 条（`alloc_frames_fragment()` 死循环），原第 2 条改为第 1 条
- `docs/repairs/260910/mm.md`：删除（唯一的一条是 `NUM_ONLINE_NODES` 从未赋值）
- `docs/repairs/260906/mm.md`：删掉第 3 条（`VmObjectPaged::read()` / `write()` 是 `Unimplemented`）

**症状**
这三条都已经修掉了，repairs 里却还留着。repairs 只该写"还没修的"，
留着已修的条目，会让认领的人去修一个不存在的问题。

**原因**
修的时候是从 PmNode 结构整理或 VMO 读写的角度动手的，没有回头查早先日期的 repairs 文件里是否有同一条。

**改法**
逐条对照当前代码确认已修，然后删掉：
- fragment 死循环：`260927/mm.md` 里的 `e09aa491`；
- `NUM_ONLINE_NODES`：`260927/mm.md` 里的 `1c9e9bb7`，计数器已删，改为数在线掩码；
- `read()` / `write()`：`92da681e`（kernel 缓冲区版本），之后又有 `8b84de4c` 的 `read_user()` / `write_user()`。

**参照**
无。

**验证**
对照代码逐条确认：`pm_node.cpp` 的 fragment 循环在步进里右移；`PmNode::num_online_nodes()`
返回 `node_online_mask().count()`；`vm_object_paged.cpp` 里 `read()`、`write()` 都有实现。
只动了文档，不涉及构建。

**故意没做**
同一批文件里还没修的条目都保留：`260919/kernel-mm.md` 的 `map_physical()` 物理偏移，
`260906/mm.md` 的第 1、2 条。
