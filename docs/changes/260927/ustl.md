## 新增 ustl::function::Function：目标就地存放、最多 24 字节、超了编译报错（2026-09-27）

**文件**
- `system/lib/ustl/include/ustl/function/function.hpp` — 新增 `Function<R(Args...)>` 和 `detail::FitsInline` concept
- `system/lib/ustl/test/function_test.cpp` — 主机测试
- `system/lib/ustl/BUILD.gn` — 登记头文件，新增 `function-test` 并加进 `tests` 组

**症状**
无运行时症状。缺的是一样东西：ustl 里唯一的类型擦除可调用对象是
`ustl::function::Fn`，而它只是 `std::function` 的别名：

```cpp
template <typename Signature>
using Fn = std::function<Signature>;
```

`std::function` 的目标一旦超出它的小缓冲区，就悄悄搬到堆上。持自旋锁、关中断的
代码不能碰堆，所以这类代码只剩两条路：要么写成模板（`VmObjectPaged::read_write`
就是这样，页遍历因此实例化了 4 份），要么退回"函数指针 + `void *`"（线程入口、
Dpc 都这么做）。

**原因**
类型擦除的代价有两部分：一次间接调用，外加目标对象放在哪里。间接调用在内核里
可以接受，"放在哪里"不能是一个看不见的 `new`。`std::function` 把缓冲区大小当成
实现细节，调用方没法知道自己的 lambda 会不会分配，而捕获列表多一个成员就可能
越过那条线。这个错误不会在编译期出现，只会在运行期某次分配失败，或者在原子上下文
里睡眠时才暴露。

所以大小必须是接口的一部分：放得下就就地存，放不下就编译失败。

**改法**

```cpp
namespace detail {
template <typename T, std::size_t Size, std::size_t Align>
concept FitsInline =
    sizeof(T) <= Size && alignof(T) <= Align && std::is_nothrow_move_constructible_v<T>;
} // namespace detail

template <typename R, typename... Args>
class Function<R(Args...)> final {
  public:
    constexpr static std::size_t INLINE_SIZE = 24;
    constexpr static std::size_t INLINE_ALIGN = alignof(void *);

    template <typename F>
        requires INVOCABLE<F> && detail::FitsInline<std::decay_t<F>, INLINE_SIZE, INLINE_ALIGN>
    Function(F &&callable) noexcept;
    ...
  private:
    struct Ops {
        auto (*invoke)(void *target, Args &&...args) -> R;
        auto (*relocate)(void *dst, void *src) noexcept -> void;
        auto (*destroy)(void *target) noexcept -> void;
    };
    Ops const *m_ops = &EMPTY_OPS;
    alignas(INLINE_ALIGN) mutable std::array<std::byte, INLINE_SIZE> m_storage;
};
```

几个取舍：

- **24 字节**：三个指针，够装函数指针，或者捕获 `this` 加一个缓冲区指针和长度
  的 lambda。整个对象 32 字节，半条 cache line，按值传递也便宜。大小固定，不做成
  模板参数，这样同一个签名的 `Function` 无论装什么都是同一个类型。
- **超限用 concept 拒绝**，不用 `static_assert`，也不用 `constexpr bool`。用 concept
  的话，`std::is_constructible_v` 会如实返回 false，测试就靠这一点；clang 也会把
  失败的 concept 拆到具体子句。实测一个 32 字节的 lambda 报的是
  `'sizeof((lambda)) <= 24UL' (32 <= 24) evaluated to false`。第一版用的是
  `constexpr static bool`，报错只有 `ACCEPTS<...> evaluated to false`，看不出原因，
  所以改成了 concept。concept 不能是类成员，所以放在 `detail`。
- **只能移动，不能拷贝**，同 Zircon `fit::function` 和 C++23
  `std::move_only_function`。如果允许拷贝，就要求每个目标都可拷贝，持有资源的
  可调用对象就进不来了。移动是把目标重新构造到对方的缓冲区里，源对象随后变空。
- **空状态不是空指针**，而是一张 invoke 为 `__builtin_trap()` 的表。调用路径上
  没有分支，调用空 `Function` 会立刻停下，而不是跳到 0。
- **`operator()` 是 const**，这点与 `std::function` 一致。const 保护的是"装着哪个
  目标"，不是目标自身的状态，所以 `mutable` lambda 经 const 引用调用时依然能
  改自己的计数。存储区因此标成 `mutable`。
- **空函数指针或成员指针**构造出空 `Function`，与 `std::function` 相同。

**参照**
Zircon `fit::inline_function<Signature, inline_target_size>`
（`sdk/lib/fit/include/lib/fit/function.h`）：同样把目标就地存放、放不下就
`static_assert`，同样只能移动。区别在于这里大小固定，拒绝走 concept 而不是
`static_assert`。libc++ 的 `std::move_only_function`：只能移动的语义与这里相同，
但它仍会在堆上分配。

**验证**
- 主机测试 `host/obj/system/lib/ustl/function-test` 通过，退出码 0。覆盖范围：
  - 24 字节能构造，25 字节不能；16 字节对齐的目标不能；
  - 三指针 lambda 能构造，四指针不能；
  - 函数指针、带捕获 lambda、返回值转换、`void` 丢弃返回值；
  - 只能移动的参数，经 const 调用 `mutable` 目标；
  - 移动后源对象为空，自移动；
  - 目标在移动、重新赋值、`= nullptr` 各环节都恰好析构一次（`Tracked` 计数）。
- 另写探针确认：主机测试的 `assert` 是生效的；调用空 `Function` 以 SIGTRAP
  终止；超大 lambda 的报错如上所述。
- 内核工具链：取 `kernel/mm/vm_object_paged.cpp` 的编译参数对一个实例化了
  `Function` 的 TU 做 `-fsyntax-only`，通过。说明头文件在 freestanding 下
  可用（`<functional>` 里的 `std::invoke_r`、`<array>`、`<memory>`）。
- clang-format、clang-tidy（加 `--header-filter` 覆盖头文件）均无新告警。
- 没有启动 QEMU：内核里还没有调用方。

**故意没做**
- **没替换 `Fn`，也没改 `VmObjectPaged::read_write`。** 那是下一步，每个调用方
  单独提交。`Fn` 目前还有使用者，删掉它是另一件事。
- **没做可拷贝版本，也没做不持有目标的 `FunctionRef`。** 只在同步回调里用、
  不需要保存的场景，`FunctionRef` 更轻（两个指针），真有调用方需要时再加。
- **没对平凡可复制的目标走 memcpy 快路径。** 每种目标一张表，relocate 就是
  一次移动构造加析构，对平凡类型编译器本身就会折叠成拷贝。

## Function 的大小常量改用 usize（2026-09-27）

**文件**
- `system/lib/ustl/include/ustl/function/function.hpp`：`INLINE_SIZE`、`INLINE_ALIGN`，以及 `detail::FitsInline` 的两个模板参数
- `system/lib/ustl/test/function_test.cpp`：`Bytes<N>` 的模板参数

**症状**
无运行时症状，是写法问题。项目约定整数类型用 prelude 里的别名，而新写的
`Function` 用了 `std::size_t`。

**原因**
prelude（`kernel/include/ours/prelude.hpp`）会被强制包含进所有 C++ 编译：
内核工具链通过 `//kernel/products:headers`，主机工具链通过 `host-prelude`，
用户程序通过 `//system:user.headers`。所以 ustl 头文件里直接写 `usize` 在哪个
工具链下都能编译，`ustl/io/direct_memory.hpp` 早就是这么写的。

**改法**

之前：`constexpr static std::size_t INLINE_SIZE = 24;`

之后：`constexpr static usize INLINE_SIZE = 24;`

`<cstddef>` 保留，`std::byte` 和 `std::nullptr_t` 还要用。

**参照**
无，本项目自有约定。

**验证**
- 主机测试 `function-test` 通过。
- `kernel.dev.obi` 构建通过。

**故意没做**
ustl 里其它 44 个头文件的 `std::size_t` 这次没动。那是一次单独的全库扫描，
不和这次混在一起。
