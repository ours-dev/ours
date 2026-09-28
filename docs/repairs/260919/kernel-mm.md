# kernel/mm 待修项（2026-09-19）

这一条是排查 userabi 缺页时顺路读到的，和那条缺页无关，可独立认领。

## 1. `map_physical()` 给每个保护区段都从同一个物理页重新开始

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
