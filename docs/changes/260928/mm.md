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
