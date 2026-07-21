# GoogleTest 用户接口到 Hsien Annotation 映射

本文按 GoogleTest 官方用户文档整理公开用户接口，并给出适合 hsien
freestanding/hosted 双环境的映射。GoogleTest 的宏只是语法入口；hsien
用 C++26 annotation 表达声明性元数据，运行时行为仍由 `Context`、
fixture、runner 和 reporter 提供。

参考：

- https://google.github.io/googletest/primer.html
- https://google.github.io/googletest/advanced.html
- https://google.github.io/googletest/reference/testing.html
- https://google.github.io/googletest/reference/assertions.html
- https://google.github.io/googletest/reference/matchers.html

## 1. 测试声明与注册

| GoogleTest | 语义 | Hsien 映射 | 方式 |
| --- | --- | --- | --- |
| `TEST(Suite, Name)` | 无 fixture 的普通测试 | `[[=hsien::test]]` | 当前实现：函数名作为测试名；后续可增加 suite/name annotation |
| `TEST_F(Fixture, Name)` | 使用 fixture 的测试 | `[[=hsien::test]] [[=hsien::fixture<Fixture>]]` | annotation 选择 fixture，runner 创建新实例 |
| `TEST_P(Fixture, Name)` | value-parameterized fixture 测试 | `[[=hsien::test]] [[=hsien::fixture<Fixture>]] [[=hsien::values<...>]]` | 当前实现已支持无 fixture 的整型值版本 |
| `TYPED_TEST(Fixture, Name)` | 对已知类型列表重复测试 | `[[=hsien::test]] [[=hsien::types<...>]]` | 每个类型生成一个 record |
| `TYPED_TEST_P` + `REGISTER_TYPED_TEST_SUITE_P` | 先定义类型测试模式，稍后实例化 | `[[=hsien::test_pattern]]` + `[[=hsien::instantiate_types<...>]]` | 需要独立的 pattern/instantiation 模型 |
| `RegisterTest(...)` | 运行时动态注册 | 不提供用户接口 | hsien 注册必须在编译前由 annotation 发现完成 |

GoogleTest 的 `TEST`/`TEST_F` 会隐式注册测试；hsien 也保持隐式注册，但
注册产物是构建期生成的 `.hsien.tests` 段，而不是静态初始化对象或用户
手写 registry。

建议的长期声明形式：

```cpp
[[=hsien::test]]
[[=hsien::suite<"Pmm">]]
[[=hsien::name<"AllocatesOneFrame">]]
void allocates_one_frame(hsien::Context&);
```

没有 `suite/name` annotation 时，V1 继续使用函数名作为稳定测试名。

## 2. Fixture 与生命周期

GoogleTest 的 fixture 接口包括：

- `SetUp()` / `TearDown()`：每个 test instance 前后执行。
- `SetUpTestSuite()` / `TearDownTestSuite()`：一个 suite 的首个/最后一个
  instance 前后执行。
- `Environment::SetUp()` / `TearDown()`：整个 test program 前后执行。
- 每个 `TEST_F` 都创建新 fixture 对象，不复用前一个测试对象。

Hsien 应映射为结构化 annotation，而不是要求用户继承 GoogleTest 风格
基类：

```cpp
struct PmmFixture {
    auto set_up(hsien::Context&) -> void;
    auto tear_down(hsien::Context&) -> void;
    PmmState state;
};

[[=hsien::fixture<PmmFixture>]]
struct PmmTests {};

[[=hsien::test]]
[[=hsien::fixture<PmmFixture>]]
void releases_frame(hsien::Context&, PmmFixture&);
```

建议的 hsien 生命周期：

```text
environment setup
  -> suite setup
    -> fixture construction + setup
      -> test body
    -> fixture teardown + destruction
  -> suite teardown
environment teardown
```

内核测试必须允许 fixture 使用静态 buffer、boot allocator 或显式资源，
不能隐式要求 heap、thread 或 exception runtime。

## 3. Value-Parameterized Tests

GoogleTest 通过 `TestWithParam<T> + TEST_P + INSTANTIATE_TEST_SUITE_P` 完成
值参数化；参数可以来自 `Values`、`ValuesIn`、`Bool`、`Combine` 或自定义
生成器，并且每个值有独立测试实例名。

Hsien 当前已经实现最小对应形式：

```cpp
namespace hsien {
    template <auto... Values>
    struct values_t {};

    template <auto... Values>
    inline constexpr values_t<Values...> values {};
}

[[=hsien::test]]
[[=hsien::values<1, 2, 3>]]
void checks_size(hsien::Context&, int value);
```

scanner 生成：

```text
checks_size/0   value = 1
checks_size/1   value = 2
checks_size/2   value = 3
```

下一步应按以下顺序扩展：

1. 多个整型/枚举参数，使用 structural tuple。
2. `ValuesIn` 等静态数组/`ustl::Array` 数据源。
3. 自定义实例名 annotation，例如 `[[=hsien::param_name<...>]]`。
4. `Combine` 的笛卡尔积展开。
5. 非整型 structural parameter（固定字符串、枚举配置结构体）。

V1 不应复制 GoogleTest 的运行时 generator 对象；参数必须在构建期
展开成 section record，内核 runner 只执行已经生成的 adapter。

## 4. Typed Tests

GoogleTest 有两种类型测试：

- typed tests：定义时已经知道类型列表，用 `TYPED_TEST_SUITE`。
- type-parameterized tests：先定义测试模式，稍后用
  `INSTANTIATE_TYPED_TEST_SUITE_P` 提供类型列表。

Hsien 可使用 structural type-list annotation：

```cpp
template <typename TypeParam>
struct QueueContract {
    static auto run(hsien::Context&) -> void;
};

[[=hsien::test]]
[[=hsien::types<int, unsigned, long>]]
void queue_contract(hsien::Context&);
```

生成器为每个类型生成一个 adapter，并把类型显示在实例名中：

```text
queue_contract/<int>
queue_contract/<unsigned>
queue_contract/<long>
```

这类能力依赖 P2996 的 type reflection/splicing；它应作为参数化测试之后
的独立增量，不应在 V1 中把任意类型编码成字符串再交给 runtime。

## 5. Assertions

GoogleTest assertion 家族包括：

- Boolean：`EXPECT_TRUE/FALSE`、`ASSERT_TRUE/FALSE`。
- Comparison：`EQ`、`NE`、`LT`、`LE`、`GT`、`GE`。
- String：`STREQ`、`STRNE`、`STRCASEEQ` 等。
- Floating point：`FLOAT_EQ`、`DOUBLE_EQ`、`NEAR`。
- Predicate：`EXPECT_PRED*`、`ASSERT_PRED*`。
- Result：`AssertionSuccess()`、`AssertionFailure()`。
- Unconditional：`SUCCEED`、`FAIL`、`ADD_FAILURE`。
- Skip：`GTEST_SKIP`。
- Trace/property：`SCOPED_TRACE`、`RecordProperty`。
- Matcher assertion：`EXPECT_THAT`、`ASSERT_THAT`。

这些不是 declaration metadata，因此不应强行设计成 annotation。建议
采用 freestanding-friendly 的函数式接口：

```cpp
context.expect(condition, std::source_location::current());
context.expect_eq(actual, expected, std::source_location::current());
context.expect_that(actual, hsien::eq(expected),
                    std::source_location::current());
context.skip("requires initialized MMU");
context.trace("page allocation phase");
```

与 GoogleTest 的重要差异已经确定：hsien 不使用 exception；所有失败只
记录并继续执行，不提供会中止当前函数的 `ASSERT_*` 语义。若之后需要
保护性 fatal 行为，应该另命名为 `context.abort_current_test()`，不要把
它伪装成 GoogleTest `ASSERT_*`。

## 6. Matchers

GoogleTest matcher 主要由 `EXPECT_THAT` 使用，公开类别包括：

- 通配：`_`、`A<T>`。
- 比较：`Eq`、`Ne`、`Lt`、`Le`、`Gt`、`Ge`。
- 浮点：`FloatEq`、`DoubleEq`、`Near`、NaN-sensitive variants。
- 字符串/正则：`HasSubstr`、`StartsWith`、`EndsWith`、`MatchesRegex`。
- 容器：`Contains`、`Each`、`SizeIs`、`IsEmpty`。
- 结构：`FieldsAre`、`Property`、`Field`、`Pair`、`Key`。
- 指针/智能指针：`IsNull`、`NotNull`、`Pointee`、`Pointer`。
- 组合：`AllOf`、`AnyOf`、`Not`、`Conditional`。
- 适配：`ResultOf`、`Truly`、`MatcherCast`、`SafeMatcherCast`。
- 自定义：`MATCHER`、`MATCHER_P*` 或手写 monomorphic/polymorphic matcher。

Hsien matcher 应是 header-only、无 RTTI、无 exception 的纯函数对象：

```cpp
context.expect_that(value, hsien::all_of(
    hsien::ge(0), hsien::lt(10)));
```

Matcher 名称不是 annotation；annotation 只描述“测试有哪些实例”。

## 7. Death、Exception 与 Skip

GoogleTest death tests 包括 `ASSERT_DEATH`、`EXPECT_DEATH`、
`ASSERT_EXIT`、`EXPECT_EXIT`，通过子进程/clone 验证进程终止、退出码和
stderr。GoogleTest 还提供 `EXPECT_THROW`、`ASSERT_THROW`、
`EXPECT_NO_THROW` 等 exception assertions。

Hsien 内核默认不复制这两类语义：

- exception assertions 与“永远禁用 exception”冲突，明确不提供。
- freestanding kernel 没有安全的进程 fork/clone death-test seam。
- 若将来需要验证 kernel panic，应设计独立的 hosted process adapter 或
  QEMU fault-test backend，不能把它表示成普通 `[[=hsien::test]]` 参数。
- `GTEST_SKIP` 可映射为 runtime `Context::skip()`；skip 只改变当前
  instance 状态，仍继续后续 instances。

## 8. Runner、过滤与输出

GoogleTest 运行控制公开接口包括：

- `InitGoogleTest()`：解析命令行 flags。
- `RUN_ALL_TESTS()`：运行 link unit 中的所有测试并返回状态。
- `--gtest_filter`：正/负模式过滤 suite/test 名。
- `--gtest_list_tests`：只列出测试。
- `--gtest_repeat`：重复运行。
- `--gtest_shuffle`、`--gtest_random_seed`：随机顺序。
- `--gtest_break_on_failure`、`--gtest_throw_on_failure`、
  `--gtest_catch_exceptions`：失败/异常策略。
- sharding flags：`GTEST_TOTAL_SHARDS`、`GTEST_SHARD_INDEX`。
- XML/JSON output、colored output、brief output、stream result。

Hsien 映射为 runner configuration，而不是 annotation：

```cpp
struct RunConfig {
    TestFilter filter;
    bool list_only;
    unsigned repeat_count;
    bool shuffle;
};

auto run_section(RunConfig const&, Reporter&) -> Summary;
```

内核第一版可以只支持固定顺序、全量运行和串口 reporter；hosted runner
再逐步增加 filter/list/repeat。测试 annotation 不应该携带命令行运行
状态。

## 9. Event Listeners、Reporter 与结果属性

GoogleTest 的 `TestEventListener` / `EmptyTestEventListener` 接收：

- program start/end
- suite start/end
- test start/end
- assertion part result
- environment setup/teardown

Hsien 直接把它们设计成 `Reporter` seam 的事件方法，避免继承复杂的
虚接口：

```cpp
struct Reporter {
    auto on_test_start(TestInfo const&) -> void;
    auto on_failure(Failure const&) -> void;
    auto on_test_end(TestResult const&) -> void;
};
```

hosted 可以提供 console/XML/JSON reporter；kernel 可以提供串口 reporter
或静默计数 reporter。`RecordProperty` 对应 `Context::property(key,value)`，
但内核版应使用固定容量存储，不能隐式分配。

## 10. 其他公开接口

| GoogleTest 接口 | Hsien 建议 |
| --- | --- |
| `FRIEND_TEST` | 不做 annotation；使用普通 C++ `friend` 或公开一个测试 seam |
| `StaticAssertTypeEq` | 直接使用 `static_assert(std::same_as<T,U>)` 或项目已有 traits |
| `PrintToString` / value printer | 提供 `hsien::format`/reporter formatter，先支持 kernel 基础类型 |
| `SCOPED_TRACE` | `hsien::TraceScope` RAII，零 heap、失败时附加当前 trace 栈 |
| `RecordProperty` | `Context::property`，生成测试结果元数据 |
| `AddGlobalTestEnvironment` | `[[=hsien::environment]]` 或隐藏 build-level environment record |
| `RegisterTest` | 不开放；annotation 扫描是唯一注册路径 |
| `UnitTest`/`TestSuite`/`TestInfo`/`TestResult` | 作为只读 runtime result views，避免暴露内部 registry 容器 |
| gMock `MOCK_METHOD` / `EXPECT_CALL` | 独立 mocking 子系统，不应混入 hsien test annotation 设计 |

## 11. 推荐的 Hsien 接口层次

```text
Declaration metadata
  [[=hsien::test]]
  [[=hsien::fixture<...>]]
  [[=hsien::values<...>]]
  [[=hsien::types<...>]]
          |
          v
Build-time scanner -> TestRecord section
          |
          v
Runtime Context -> assertions / matchers / skip / properties
          |
          v
Reporter -> hosted console/XML/JSON or kernel serial output
          |
          v
Runner config -> filter/list/repeat/shuffle
```

核心原则：

1. annotation 只描述测试声明、fixture、参数和类型实例化。
2. 断言、matcher、skip、trace、property 是 runtime interface。
3. runner/filter/reporter 是执行配置，不进入 ELF test record 的用户声明。
4. GoogleTest 的 fatal/exception/death 语义不直接移植到内核；不匹配的
   能力要显式标记为 hosted-only 或独立 backend。
5. 每个新增 annotation 都必须能被 Python AST scanner 映射成稳定、可删除
   的 `.hsien.tests` record，而不是引入新的手工注册入口。
