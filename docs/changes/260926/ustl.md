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
