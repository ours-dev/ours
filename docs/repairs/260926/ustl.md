# ustl 待修项（2026-09-26）

第 2 条是修 `Option<T &>` → `Option<T const &>`（见 `docs/changes/260926/ustl.md`）时看到的；第 1 条是把 `ustl/test/`
的测试全部接进构建时，唯一一个跑不通的测试牵出来的。都不是那两次改动引入的。按严重程度排。

---

## 1. `CircularSlistAlgorithms::swap_nodes` 碰到空表就把表头接错：`Slist` 的 swap、移动、sort 全坏

**现象**：`test/intrusive_slist_parity_test.cpp` 编得过，但运行不返回（-O0、-O2 一样），所以没接进构建，`ustl/BUILD.gn`
里在它该在的位置留了注释。插桩定位在第 74 行 `left.sort(compare)`。另写了个小程序测移动构造，结果：

```
move empty:     source.empty()=0 dest.empty()=0     # 应为 1 1
move non-empty: source.empty()=0 dest.size()=1      # 应为 source.empty()=1
```

**原因**：`system/lib/ustl/include/ustl/collections/intrusive/circular_slist_algorithms.hpp:145-147`，一方为空时的两个
目标写反了：

```cpp
NodeTraits::set_next(node,
                     other_is_empty ? other : (other_next == node ? other : other_next));
NodeTraits::set_next(other, is_empty ? node : (next == other ? node : next));
```

Boost 的原版是 `other_empty ? this_node`、`this_empty ? other_node`。环形单链表里"空"就是表头指向自己，所以对方为空时，
交换后的自己也该指向**自己**，这里却指向了对方的表头：

- 两边都空：两个表头互相指，两个表都"非空"，各自把对方的表头当成一个元素。
- 一边空：拿到元素的那一边是对的；被换空的那一边指向对方表头，从它出发的遍历再也回不到它自己的表头——`clear()`、
  `get_previous_node()`、`merge` 的 `while (!source.empty())` 都不终止。

撞上它的路径：`Slist::swap()`；`Slist(Slist &&)` 是 `: Slist() { swap(other); }`，新表永远是空的，**每次移动构造都中**；
`operator=(Slist &&)` 先 `clear()` 再 `swap()`，同样；`Slist::sort()` 第一步就把 carry 和空的 bin 交换。

测试为什么没抓到：`circular_slist_algorithms_test` 只交换了一个 `init` 过的节点（next 为空指针）和链上元素，没覆盖
"表头指向自己"的空表；`slist_test` 不移动、不 swap、不 sort；覆盖 sort 的 `intrusive_slist_parity_test` 以前根本没编。
双向的 `CircularListAlgorithms::swap_nodes` 是照 Boost 另一种写法（先 `init_header` 再交换）搬的，没有这个问题。

**内核影响**：潜伏。内核里唯一持有 `Slist` 的是 `gktl::ChainBuffer`，还是空壳。

**改法**：两处常量对调，即 `other_is_empty ? node : ...` 和 `is_empty ? other : ...`。在 scratch 副本上验证过：
`intrusive_slist_parity_test` 退出码 0，上面的移动构造输出变成 `1 1` / `1`。

修的时候一起做：

- 给 `intrusive-slist-parity-test` 建 `executable` 并入 `ustl:tests`，删掉 `ustl/BUILD.gn` 里那段注释；
- `circular_slist_algorithms_test` 补两个空表头的 `swap_nodes` 用例（两边都空、一边空），`slist_test` 补移动构造和 swap。

**参照**：Boost.Intrusive `circular_slist_algorithms::swap_nodes`
（`system/lib/boost.intrusive/include/boost/intrusive/circular_slist_algorithms.hpp:292`）。

## 2. 树容器里 12 处 `static_cast<T const &>` 已经多余（清理）

`bstree.hpp`、`rbtree.hpp`、`avltree.hpp`、`splaytree.hpp` 各 3 处，都在 `find`/`lower_bound`/`upper_bound` 的 const 版本里：

```cpp
: ustl::some_ref(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
```

这个 cast 当初是为了让 `some_ref` 推导出 `Option<T const &>`，因为 `Option<T &>` 不能转 `Option<T const &>`。现在能转了，
去掉 cast 写成和 list/sgtree 一样的 `ustl::some_ref(*ValueTraits::to_value_ptr(node))`，返回类型不变。
不影响正确性，只是让 intrusive 容器只剩一种写法。改完用 `intrusive-const-accessors-test` 的做法给这四棵树也钉一下返回类型。
