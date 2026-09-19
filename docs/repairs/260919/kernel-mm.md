# kernel/mm 待修项（2026-09-19）

这两条是排查 userabi 缺页时顺路读到的，和那条缺页无关，各自独立可认领。

## 1. `alloc_frames_fragment()` 对任何偶数 n 都是死循环

`kernel/mm/pm_node.cpp:595`：

```cpp
    for (usize order = 0; n > 0; order += 1) {
        if ((n & 1) == 0) {
            continue;
        }

        if (auto frame = this->alloc_frames(gaf, order, fallback)) {
            list.push_back(*frame.unwrap());
            n >>= 1;
        } else {
            ...
        }
    }
```

`n >>= 1` 只写在取到 frame 的那条分支里。`n` 是偶数时走 `continue`，`n` 一位都不
移，下一轮 `n & 1` 仍然是 0，循环条件 `n > 0` 永远成立——`order` 一直加，`n` 一直
不动，卡死在这里，而且是在持有分配路径的锁时卡死。

n 为奇数时也只是碰巧对：它靠「取到就移一位」凑出了正确的位遍历，但这个不变量和
`order` 的递增是两回事，写在两个地方迟早会分叉。

改法是把移位提到循环的步进里，让「第 order 位」和 `order` 由同一处推进：

```cpp
    for (usize order = 0; n > 0; order += 1, n >>= 1) {
        if ((n & 1) == 0) {
            continue;
        }
        ...
    }
```

对照 Linux 的 `__alloc_pages_bulk()` 不是同一个东西（那是批量同阶），这里更像把
n 按二进制拆成若干 2^order 块，拆位的循环就该由位本身驱动。

## 2. `map_physical()` 给每个保护区段都从同一个物理页重新开始

`kernel/mm/vm_mapping.cpp:243-249`：

```cpp
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf, options);
        for (usize offset = 0; offset < region_size; offset += PAGE_SIZE) {
            if (auto const appended = coalescer.append(phys_base + offset);
```

`offset` 每进一个保护区段就重新从 0 开始，而 `phys_base` 是整个 `[base, base+size)`
的起点。于是第二个区段又从 `phys_base` 映起：一段物理内存被映到两个不同的虚拟区段
上，而本该映在后半段的物理页一个都没被映到。

只有当整个映射恰好落在单一保护区段里（目前的常见情形）才看不出来，所以它能一直活着。

物理偏移要相对整个映射算，不是相对区段：

```cpp
        auto const region_offset = region_base - base;
        for (usize offset = 0; offset < region_size; offset += PAGE_SIZE) {
            coalescer.append(phys_base + region_offset + offset);
```

顺带一提，同一文件 `map()` 上方已经挂着一条 FIXME 说它不取 `m_aspace->get_lock()`，
而 `protect()`/`unmap()` 都取——那是另一条，不要和这条合并修。
