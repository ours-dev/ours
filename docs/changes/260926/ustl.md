# ustl 改动记录（2026-09-26）

## `Option<T &>` 能隐式收窄成 `Option<T const &>`，intrusive 容器的 const 访问器终于能编译（2026-09-26）

**文件**
- `system/lib/ustl/include/ustl/option.hpp` — `Option<T &>` 加一个转换构造：`U *` 能转成 `T *` 时，`Option<U &>` 隐式转成 `Option<T &>`
- `system/lib/ustl/test/option_test.cpp` — 用 static_assert 钉住转换方向，运行时检查转换后仍指向原对象、NONE 仍是 NONE
- `system/lib/ustl/test/intrusive_const_accessors_test.cpp` — 新增：在 const 容器上调用 List/Slist/Sgtree/Treap/Hashtable 的 const 访问器
- `system/lib/ustl/BUILD.gn` — 新 `:intrusive-const-accessors-test`，并入 `:tests`
- `BUILD.gn` — 默认构建带上 `option-test` 和 `intrusive-const-accessors-test`（宿主工具链）

**症状**
在 const 容器上调用下面任何一个都编译失败：

```
list.hpp:174:16: error: no viable conversion from returned value of type
  'Option<lockdep::AcquiredLockEntry &>' to function return type
  'Option<const lockdep::AcquiredLockEntry &>'
```

受影响的是 `List::front() const`、`List::back() const`、`Slist::front() const`，以及 `Sgtree`、`Treap`、`Hashtable`
的 `find() const`（外面包的 `SgSet`/`TreapSet`/`UnorderedSet` 的 `find() const` 连带失败）。lockdep 在
`ThreadLockState::check_irq_safety_across_states` 里撞上，只好经一个非 const 指针去调 `back()` 绕开。

**原因**
这些 const 访问器从 const 容器里拿到的是一个**可变**的元素引用，再指望在 return 处变成 const——而 `Option<T &>` 不会变。

- **const 容器里走的是可变节点。**节点算法（`CircularListAlgorithms` 等）只收 `NodePtr`，所以 const 成员函数里的 `header()`
  先把表头 const_cast 掉：

  ```cpp
  constexpr auto header() const noexcept -> NodePtr {
      return PointerTraits<NodePtr>::pointer_to(const_cast<Node &>(m_header));
  }
  ```

  接下来 `to_value_ptr(NodePtr)` 选中的是非 const 重载，得到 `T *`，`some_ref(*ptr)` 推导出 `Option<T &>`：

  ```cpp
  constexpr auto back() const noexcept -> ustl::Option<T const &> {
      const auto node = NodeTraits::get_previous(header());
      return node == header() ? ustl::NONE : ustl::some_ref(*ValueTraits::to_value_ptr(node));
  }
  ```

- **这个形状本身没错，Boost.Intrusive 就是这么写的**：`list::front() const` 是
  `return *priv_value_traits().to_value_ptr(NodeTraits::get_next(this->get_root_node()));`，拿到 `T &`，在 return 处由语言
  隐式转成 `T const &`。ustl 自己的 const 迭代器也一样：`ListIter<.., true>::operator->()` 拿到 `T *`，靠 `T *` → `T const *`
  的隐式转换返回 `ElementPtr`。
- **缺的是 `Option<T &>` 这一环。**它内部就是一个可空的 `T *`，但只有 `Option()`、`Option(NoneValue)`、`explicit Option(T &)`
  三个构造函数，裸引用和裸指针白送的"加 const"转换它没有，于是 return 那一步不成立。
- **为什么一直没暴露**：类模板的成员函数只在被调用时才实例化，这些 const 重载在没人调用之前一直"编译通过"。现有测试只在非 const
  容器上调用；而且 `system/lib/ustl/test/` 下 88 个测试文件只有 16 个在 `BUILD.gn` 里有 target，其中只有 bitfields、bitset、rc
  三个进了默认构建——`ustl:tests` 这个组没有任何人引用，连 `option-test` 都从没被默认构建编过。
- **bstree/rbtree/avltree/splaytree 为什么没事**：它们在调用处先 cast 一下，`some_ref` 就推导出 `Option<T const &>`：

  ```cpp
  : ustl::some_ref(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
  ```

  这个 cast 只为凑签名而存在——正是"API 缺了什么"的信号。list/slist/sgtree/treap/hashtable 没写这个 cast，就挂了。

**改法**
改前，`Option<T &>` 的构造函数只有这些：

```cpp
constexpr Option() noexcept = default;
constexpr Option(NoneValue) noexcept {}
constexpr explicit Option(T &value) noexcept : m_value(std::addressof(value)) {}
```

改后，加一个和 `Box<U>`、`Rc<U>` 同一约束的隐式转换构造（外加 `template <typename> friend class Option;` 以读对方的 `m_value`）：

```cpp
template <typename U>
    requires(std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>)
constexpr Option(Option<U &> const &other) noexcept : m_value(other.m_value) {}
```

容器代码一行没动：五个 const 访问器原样编译，返回类型就是声明的 `Option<T const &>`。

- **隐式是故意的。**`some_ref` 的规矩是"别名必须在调用处写出来"；这里 `other` 已经是别名，转换不产生新的别名，只收窄访问权限，
  不破坏那条规矩。显式的话 return 处照样不成立，等于没修。
- **约束照抄 `Box`/`Rc`**：`U *` 能转 `T *` 就放行，所以能加 const、能从派生看成基类（和 `Derived &` → `Base &` 一样）；
  去 const、向下转、无关类型都不行。`!std::is_same_v` 把同类型留给拷贝构造。
- **赋值不用另写。**`Option<T &>` 的拷贝/移动赋值是隐式生成的，`const_opt = mut_opt;` 先经转换构造出临时量再移动赋值，测试里覆盖了。

否掉的方案：**在每个容器里走 const 的 `to_value_ptr(ConstNodePtr)`。**`ContainerValueTraits` 确实有这条路，但
(1) 它只修容器，用户代码照样不能把 `Option<T &>` 交给收 `Option<T const &>` 的函数——lockdep 那种写法以后还会再撞；
(2) sgtree/treap/hashtable 用的是各自私有的 `to_value(NodePtr) -> T *`，没有 const 版本，要给五个容器各铺一条 ConstNodePtr 的路；
(3) 树里那 12 处 `static_cast<T const &>` 就是这个方案的调用处版本，已经说明补在调用处要到处补。

也否掉了"只许加 const"（`std::span` 用的 `U (*)[]` 转 `T (*)[]` 技巧）：ustl 里指针语义的类型（`Box`、`Rc`）都放行向上转换，
`Option<T &>` 单独收紧反而不一致。

**参照**
Boost.Intrusive `list::front() const`（仓库里 `system/lib/boost.intrusive/include/boost/intrusive/list.hpp`）：可变节点取值、在
return 处收窄成 const 引用，本改动让 `Option<T &>` 支持同样的收窄。约束形状照 ustl 自己的 `Box(Box<U> &&)`、`Rc(Rc<U, Policy> const &)`。
Rust 的 `Option<&mut T>` 不会隐式变成 `Option<&T>`（要 `.as_deref()`）；这里跟 C++ 的引用走，因为 `Option<T &>` 要替代的是 `T &`/`T *`。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86`：258 个依赖 `option.hpp` 的编译单元都按新头文件重编，ninja 无剩余工作。
- 宿主上跑 `host/obj/system/lib/ustl/option-test` 和 `intrusive-const-accessors-test`，退出码都是 0。
- 反向验证：把 `option.hpp` 换回改前版本（用前置 `-I` 遮住）重编两个测试，都编不过——option_test 报 5 个错（2 个 static_assert、
  3 个转换/赋值），intrusive_const_accessors_test 报 List×2、Slist、Sgtree、Treap、Hashtable 共 6 个 return 转换错误。测试确实抓得住。
- `clang-format -i` 三个 C++ 文件；`clang-tidy -p out/bringup.x86`（header-filter 覆盖 `option.hpp` 和 intrusive 头文件）零告警。
- QEMU 各跑 30 秒：`kernel.dev.dbg_1.obi` 起到 shell 和 fault_probe，`kernel.dev.obi` 起到 init 和 shell、shell 自检全过；两个都没有
  panic/assert。

**故意没做**
- 树里 12 处 `static_cast<T const &>`（bstree/rbtree/avltree/splaytree 的 `find`/`lower_bound`/`upper_bound` const 版）现在多余了，
  没删：它们是对的，删掉是纯清理，单独一次改动。记在 `docs/repairs/260926/ustl.md`。
- lockdep 里经非 const 指针调 `back()` 的绕法（`.claude/worktrees/lockdep/include/lockdep/thread_lock_state.hpp`）现在可以去掉，
  但那是另一个会话的 worktree，没碰。
- 拥有型 `Option<T>` 仍然没有 `Option<U>` → `Option<T>` 的转换：没有需求，不顺手加。
- 没把 `ustl/test/` 下其余没接进构建的测试接进去（list/slist/sgtree/treap/unordered_set 等在宿主上都能编过且通过），那是单独一件事，
  记在 repairs。

## ustl 的 88 个测试全部进默认构建，根 `BUILD.gn` 改成只挂 `ustl:tests` 一个组（2026-09-26）

**文件**
- `system/lib/ustl/BUILD.gn` — 给 71 个没有 target 的测试各建一个 `executable`；`:tests` 组补全到 88 个、按字母排；option-test 的说明从根 `BUILD.gn` 挪到它自己的 stanza 上；`intrusive_slist_parity_test.cpp` 不建 target，在原位留注释
- `BUILD.gn` — `default` 里逐个点名的 5 个 ustl 测试换成一行 `//system/lib/ustl:tests($host_toolchain)`
- `docs/repairs/260926/ustl.md` — 删掉原第 1 条（就是这条）；新增第 1 条：`Slist` 的 swap 在空表上把表头接错

**症状**
潜伏。`system/lib/ustl/test/` 有 89 个测试文件，`ustl/BUILD.gn` 只给 17 个建了 `executable`，默认构建只编其中 5 个
（bitfields、bitset、rc、option、intrusive-const-accessors）。其余 84 个不随任何构建编译，头文件把它们改坏了也没人知道：
上一条的 const 访问器编译错误能潜伏下来，这是原因之一；这次接进来时又牵出一个真 bug——`intrusive_slist_parity_test`
永远不返回（见 repairs 第 1 条）。

**原因**
一个测试要进构建，得同时出现在三份手写清单里：`ustl/BUILD.gn` 的 `executable`、`ustl:tests` 组、根 `BUILD.gn` 的
`default`。漏掉任何一份，测试就无声地不编：

- GN 没有 glob，目录里多一个 `.cpp` 没有 target 认领，GN 不会发现。
- 非默认工具链里的 target 只有从默认图引用到才会生成 ninja 规则。`enum-bits-test` 这类有 `executable`、也在
  `ustl:tests` 里、但 `ustl:tests` 没人引用的，在 `out/bringup.x86/host/` 下根本没有规则，连 `ninja all` 也碰不到。
- 根 `default` 绕过 `ustl:tests` 逐个点名，于是每加一个测试要改两个文件，第二个文件总被忘掉——点到 5 个为止。

后两份清单其实是同一件事记了两遍。`gktl`、`acpi`、`obi`、`libc`、`ffl` 在根里都只挂自己的 `tests` 组，ustl 是例外。

**改法**
根 `BUILD.gn` 改前，逐个点名：

```gn
    "//system/lib/logz4:logz4-test($host_toolchain)",
    "//system/lib/ustl:bitfields-test($host_toolchain)",
    "//system/lib/ustl:bitset-test($host_toolchain)",
    "//system/lib/ustl:rc-test($host_toolchain)",

    # Option<T &> widening to Option<T const &>, and the intrusive
    # containers' const accessors that depend on it. Both are largely
    # static_assert and overload checks, so building them is most of the
    # test.
    "//system/lib/ustl:option-test($host_toolchain)",
    "//system/lib/ustl:intrusive-const-accessors-test($host_toolchain)",
```

改后，挂组：

```gn
    "//system/lib/logz4:logz4-test($host_toolchain)",

    # Every //system/lib/ustl test, through ustl's own group rather than a
    # line per test. This list used to name them one by one and reached five
    # of the 89; the rest, most of the intrusive containers' tests among them,
    # were compiled by nothing, so a header change could break them unseen.
    # Plain hosted main()s, hence the host toolchain as for the tests above.
    "//system/lib/ustl:tests($host_toolchain)",
```

以后加测试只动 `ustl/BUILD.gn`：一个 stanza 加组里一行。新 stanza 和原有的同一形状，target 名是文件名去掉 `.cpp`、
下划线换短横线：

```gn
executable("any-hook-test") {
  testonly = true
  sources = [ "test/any_hook_test.cpp" ]
  deps = [ ":headers" ]
}
```

先试再接：用 `rc-test.ninja` 里的宿主编译、链接参数，把 84 个没进默认构建的测试逐个单独编译、链接、运行（超时 120 秒）。
83 个退出码 0；`intrusive_slist_parity_test` 超时，插桩看卡在 `Slist::sort()`，根因是
`CircularSlistAlgorithms::swap_nodes` 在空表上把表头接错。它记进 repairs，不建 target，断言一行没动。

否掉的方案：
- **在根 `default` 里逐个点名全部 88 个**（原计划的写法）：正是造成这条问题的写法，两份清单要同步，还要在根里多 83 行。
- **一份名单加 `foreach` 生成 stanza 和组**：能把后两份清单并成一份，但和文件里原有的逐个写 stanza 不一致；讨论后保留
  逐个写，代价是组里仍要手写一行。
- **把 `//system/lib:tests` 挂进根**：它会一起拉进 derive、hsien 等别的库的测试组，那些编不编得过没查，超出这次范围。

**参照**
无，本项目自有的构建组织问题。仓库里的先例是根 `BUILD.gn` 对 `//kernel/lib/gktl:tests($host_toolchain)`、
`//kernel/lib/acpi:tests($host_toolchain)` 的写法：库自己维护 `tests` 组，根只引用组。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 成功；`out/bringup.x86/host/obj/system/lib/ustl/` 下 88 个测试二进制逐个运行，
  全部退出码 0。
- 默认构建多出 83 个宿主编译单元，单独试编时合计约 130 秒 CPU（`-O2 -g2`）。
- `gn format`：新增部分没有差异。`ustl/BUILD.gn` 的 `public` 头文件列表、根 `BUILD.gn` 的 `//benches:benches` 原本就会被
  `gn format` 改写，没动。
- 没改 C++ 文件，不涉及 clang-format/clang-tidy；内核和 phys 的源码与 target 都没动，没跑 QEMU。

**故意没做**
- 没修 `Slist` 的 swap bug：那是 ustl 源码的改动，还要补用例，单独一次。修法（两处常量对调）和已做过的验证写在
  `docs/repairs/260926/ustl.md` 第 1 条。
- 没整理 `ustl/BUILD.gn` 里 `public` 头文件列表的顺序：和本次无关的纯格式改动。

## `Slist` 的 swap 在空表上把表头接错：每次移动、swap 和 sort 都会弄坏链表（2026-09-26）

**文件**
- `system/lib/ustl/include/ustl/collections/intrusive/circular_slist_algorithms.hpp` — `swap_nodes()` 末尾两句改成一条两个方向共用的规则，被换空的表头指回自己
- `system/lib/ustl/test/circular_slist_algorithms_test.cpp` — 补空表头的 `swap_nodes()` 用例：两边都空、一边空（两个参数顺序各一次）
- `system/lib/ustl/test/slist_test.cpp` — 补移动构造（空表、非空表）、移动赋值、`swap()`；结尾的三元表达式改成 `if`
- `system/lib/ustl/BUILD.gn` — 接进 `intrusive-slist-parity-test`，删掉原位的说明注释
- `docs/repairs/260926/ustl.md` — 删掉第 1 条；新增第 3 条（线性 `Slist` 不能 swap）

**症状**
- `intrusive_slist_parity_test` 运行不返回（-O0、-O2 一样），插桩定位在 `left.sort(compare)`。
- 移动一个空 `Slist` 后，源表和目标表都"非空"；移动一个非空 `Slist` 后，源表 `empty()` 仍返回 false。小程序输出：

  ```
  move empty:     source.empty()=0 dest.empty()=0     # 应为 1 1
  move non-empty: source.empty()=0 dest.size()=1      # 应为 source.empty()=1
  ```

- 内核里潜伏：唯一持有 `Slist` 的 `gktl::ChainBuffer` 还是空壳，没有调用路径。

**原因**
环形单链表里"空"就是表头指向自己：`init_header()` 写 `set_next(node, node)`，`is_empty()` 判 `get_next(node) == node`。
没挂在任何链上的节点是另一种状态：`init()` 把 next 置空指针。

`swap_nodes(node, other)` 交换两个节点在链里的位置，所以每个节点接过对方原来的后继。只有两种例外，都是那个后继恰好是
参与交换的两个节点之一：

- 对方的后继就是对方自己，即对方是空表头。这时接过来的是"空"这个状态，自己应该指向**自己**。
- 对方的后继就是自己，即对方紧挨在自己前面。交换后两者前后倒过来，自己的后继变成对方。

移植时第一种例外写反了，而且两句一起错：

```cpp
NodeTraits::set_next(node,
                     other_is_empty ? other : (other_next == node ? other : other_next));
NodeTraits::set_next(other, is_empty ? node : (next == other ? node : next));
```

对方为空时，`node` 被指向 `other` 而不是自己，另一句对称。Boost 原版是 `other_empty ? this_node`、`this_empty ? other_node`。

为什么错一个指针就让整张表失效：环形链表的每一次遍历都靠"走回表头"终止，终止条件永远是 `current != header`。
被换空的表头指向别人的表头，从它出发的遍历全都走进别人的环，再也回不来；`empty()` 也判它非空。

- 一边空：拿到元素的一边是对的，被换空的一边坏了。`clear()`、`get_previous_node()`、`merge()` 的
  `while (!source.empty())` 都不终止。
- 两边都空：两个表头互相指，形成一个两节点的环，各自把对方的表头当成一个元素。`clear()` 会把对方的表头当成 hook
  去改写，下面反向验证里 slist_test 的 SIGSEGV 就是这么来的。

撞上它的路径比看上去多：`Slist(Slist &&other) : Slist() { swap(other); }`，新建的表必然是空的，**每次移动构造都中**；
`operator=(Slist &&)` 先 `clear()` 再 `swap()`，同样；`sort()` 的第一步就把 carry 和一个空 bin 交换，carry 从此是坏的，
下一次 `merge()` 就停不下来。

为什么一直没发现：`circular_slist_algorithms_test` 只拿一个 `init()` 过的节点（next 为空指针）和链上节点交换，
碰不到"空表头"那一支；`slist_test` 不移动、不 swap、不 sort；覆盖 sort 的 parity 测试直到上一条改动之前从没编过。
双向链表的 `CircularListAlgorithms::swap_nodes()` 是照 Boost 的另一种写法（先 `init_header()`，再交换前后指针）搬的，
没有这个问题，`intrusive-list-parity-test` 也一直能过。

**改法**
改前就是上面那两句。改后：

```cpp
const auto next_in_place_of = [](NodePtr const mover, NodePtr const place,
                                 NodePtr const place_next) noexcept -> NodePtr {
    if (place_next == place) {
        return mover;
    }
    if (place_next == mover) {
        return place;
    }
    return place_next;
};
NodeTraits::set_next(node, next_in_place_of(node, other, other_next));
NodeTraits::set_next(other, next_in_place_of(other, node, next));
```

`next_in_place_of(mover, place, place_next)` 就是 `mover` 站到 `place` 的位置之后该有的后继。规则只写一次，两个方向各调
一次，`node` 和 `other` 只在调用处出现。和 Boost 逐项核对过：三种情况（对方空、相邻、其余）的结果相同，对方是未挂链的
节点（next 为空指针）时也一样，接过来的是空指针。

否掉的方案：**只把两个常量对调。**改动最小，也和 Boost 逐字对应，但规则仍然抄两份、藏在嵌套三元表达式里，这次正是
两份抄本在同一处一起写错。仓库规范也不许三元表达式。

两个测试文件补的用例：

- 算法层：两个空表头交换后仍然都空；左边有两个元素、右边空，交换后元素到右边且环回到右表头，左表头指回自己；
  再换回来，确认反方向也对。两个参数顺序正好走到规则的两次调用。
- 容器层：从空表移动构造、从非空表移动构造（源表变空且还能再用，目标表的环回到自己的表头）、移动赋值（旧元素被
  摘下）、`swap()` 把空表放在参数一侧。三处 `NOLINTNEXTLINE(bugprone-use-after-move)`：被移走的表是什么状态，
  正是要测的东西。

**参照**
Boost.Intrusive `circular_slist_algorithms::swap_nodes()`
（`system/lib/boost.intrusive/include/boost/intrusive/circular_slist_algorithms.hpp:292`）。
Linux 没有对应的坑：它的单链表（`llist`、`hlist`）以空指针结尾，没有"指向自己的空表头"这种状态。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 成功（242 步，包括 include 了 `slist.hpp` 的内核编译单元）。
- `out/bringup.x86/host/obj/system/lib/ustl/` 下 89 个测试逐个运行，全部退出码 0，`intrusive-slist-parity-test` 在内。
- 反向验证：三个测试用改前的头文件（HEAD 版本，用前置 `-I` 遮住）重编运行。`circular_slist_algorithms_test` 退出 12，
  即两边都空的那条；`slist_test` 以 SIGSEGV 退出（-11）；parity 测试 10 秒超时。新用例确实抓得住。
- `clang-format` 三个 C++ 文件无差异；`clang-tidy -p out/bringup.x86`（`--header-filter` 覆盖
  `circular_slist_algorithms.hpp` 和 `slist.hpp`）零告警，3 条 use-after-move 被上面 3 个 NOLINT 压住。
- QEMU 各跑 30 秒：`kernel.dev.dbg_1.obi` 起到 shell 和 fault_probe，日志里 6 行缺页全是 fault_probe 故意触发、被正常
  终止的用户态缺页；`kernel.dev.obi` 起到 init 和 shell，shell 自检全过。两个都没有 panic/assert。

**故意没做**
- `swap_nodes()` 前面两句 `if` 里的三元表达式没改：它们是对的，改写是纯风格清理。
- 线性 `Slist`（`Linear<true>`）的 swap：`LinearSlistAlgorithms` 根本没有 `swap_nodes()`，一实例化就编不过。
  那是另一件事，记在 `docs/repairs/260926/ustl.md` 第 3 条。
