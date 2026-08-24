# kernel/lib/acpi 重新设计（提案）

配套文档：[REVIEW.md](REVIEW.md)。术语按 `.agents/skills/engineering/codebase-design`
（module / interface / depth / seam / adapter / leverage / locality）。

## 设计目标

1. 表结构布局与 ACPI 6.5 严格对齐，每个结构体都有 `static_assert(sizeof)`。
2. 每张表只查找一次、只映射一次（消灭 README 里记的那条 issue）。
3. 用一个泛型的 typed-entry range 取代六个 `enumerate_*` + 六个 `Fn` typedef。
4. 长度检查集中到一处，让 C2/C3 那类越界读**在结构上不可能发生**。
5. 能在 host toolchain 上跑单元测试。

## 分层

```
acpi/signature.hpp          Signature（u32 存储，不做 union 双关）
acpi/tables/*.hpp           纯 POD 布局 + kSignature / kType，零内核依赖
        ↑
acpi/table_set.hpp          TableSet —— 本次设计的深模块
        ↑
acpi/numa.hpp               只剩 hi/lo 拼接这类领域解码
acpi/debug.hpp              要么实现，要么删掉
```

`HpetRegs` / `HpetTimerRegs` 迁出到 `kernel/arch/x86/kernel/time/`
（它们是 MMIO 寄存器，不是 ACPI 表）。

## 核心接口

```cpp
namespace acpi {

/// 真接缝：两个 adapter —— kernel/phys 的恒等映射、kernel proper 的 PhysMap。
/// 契约（当前实现的隐含前提，这次写进接口）：
///   - 返回的映射在内核生命周期内一直有效，调用方不负责 unmap。
///   - 成功即表示 [addr, addr + size) 整段连续可读。
class PhysMapper {
  public:
    virtual auto map(PhysAddr addr, usize size) -> ktl::Result<void const *> = 0;
  protected:
    PhysMapper() = default;
    virtual ~PhysMapper() = default;
};

/// 深模块：构造时一次性把表目录物化成 (signature, header*) 数组；
/// 之后 PhysMapper 不再被需要，也不再被持有 —— 没有 vtable，没有假接缝。
class TableSet final {
  public:
    static auto from_rsdp(PhysMapper &, PhysAddr rsdp) -> ktl::Result<TableSet>;

    /// 唯一的查表入口。一处完成：签名匹配 + length >= sizeof(T) + 校验和。
    /// C2/C3 那种"忘了查长度就 reinterpret_cast"在这里没有表达方式。
    template <typename T>
    [[nodiscard]] auto find() const -> T const *;

    /// 调试/诊断用：遍历全部表头。
    [[nodiscard]] auto headers() const -> ustl::views::Span<TableHeader const *const>;

  private:
    TableSet() = default;   // 私有，杜绝 C5 的未初始化实例
    ustl::Array<TableHeader const *, kMaxTables> tables_{};
    usize count_{0};
};

/// 取代全部 enumerate_*：类型化的子条目 range。
/// 调用方可以 break，可以拿到具体类型，没有类型擦除开销。
template <typename Table, typename Entry>
[[nodiscard]] auto entries(Table const &) -> EntryRange<Entry>;

} // namespace acpi
```

调用点从

```cpp
acpi::enumerate_io_apics(*parser, [&](acpi::madt::IoApicEntry const &e) -> Status {
    ...
    return Status::Ok;   // 这个返回值现在被丢弃
});
```

变成

```cpp
auto const *madt = tables.find<acpi::madt::Table>();
for (auto const &e : acpi::entries<acpi::madt::IoApicEntry>(*madt)) {
    ...
}
```

## 接口收缩对照

| | 现在 | 提案 |
|---|---|---|
| 自由函数 | 7 个 `enumerate_*` / `count_*` / `get_table_by_*` | 1 个 `entries<E>()` |
| 回调 typedef | 6 个 `...CommitFn` | 0 |
| 虚接口 | `IPhysToVirt` + `IAcpiParser` | 只保留 `PhysMapper` |
| 每张表的查找 | 每次调用 O(N) 扫描 + 重复 phys_to_virt | 构造时一次 |
| 长度检查 | 分散在各调用点（且有遗漏） | 集中在 `find<T>()` |

## 测试

新增 `kernel/lib/acpi/test/`，沿用 `//system/lib/ustl` 的 host 测试模式
（`executable(...) { testonly = true }` + `<cassert>` + 挂到 `//:default`）。
在内存里合成 RSDP/RSDT/XSDT/MADT/SRAT/SLIT，覆盖：
- C1 签名半段匹配必须被拒绝
- C2/C3 截断的 SLIT 必须返回错误而不是越界读
- C4 disabled 的处理器条目必须被跳过而非报错
- C6/C7 每个结构体的 `sizeof` 和字段偏移
- XSDT 优先、XSDT 损坏时回退 RSDT
- 校验和失败、length 撒谎、entry 长度撒谎等畸形输入
