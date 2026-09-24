# object 改动记录（2026-09-24）

## 句柄 arena 的查找不再有分支（Spectre V1）（2026-09-24）

**文件**
- `kernel/object/handle.cpp` — `HandleArena::lookup()` 改为 `confine_array_index` + `conditional_select_nospec_eq`；占用中的槽记录加 `HANDLE_LIVE_MARK`；`reserve()` 不再跳过 generation 0
- `docs/architecture/object/handle-lookup.md` — 常量名改为本项目的写法；补上随机数须预左移；第 3、4 道关按现状重写，改正第 4 道关的理由

**症状**
潜伏。句柄查找在每个带句柄参数的系统调用入口执行，输入是用户给的数字，而查找里有三个
由它决定的条件跳转：generation 是否为 0、下标是否越界、槽里的值是否相等。

**原因**
条件跳转会被预测。比较结果出来之前，CPU 沿预测的一侧执行：预测"相等"时，后面的代码会拿着
用户随便编的数字所指的那个槽——越界时甚至是 arena 之外的内存——当作 `Handle` 用，读它的对象
指针、它的权限，这些访存在缓存里留下的痕迹在回滚之后仍可测得。这就是 Spectre V1
（bounds check bypass）。原来的 TODO 把这个比较的风险说成"程序可以靠计时探测哪些槽被占用"，
理由写错了：占用情况不是值得设防的秘密，要防的是推测路径上对用户所选对象的使用。

**改法**
改前：

```cpp
        auto lookup(u32 const base_value) -> Handle * {
            if ((base_value & HANDLE_GENERATION_MASK) == 0) {
                return nullptr;
            }
            auto const index = base_value & HANDLE_INDEX_MASK;
            if (index >= HANDLE_ARENA_SLOTS) {
                return nullptr;
            }
            // TODO(P1): a constant-time comparison, ...
            if (m_live_base_value[index].load(std::memory_order_acquire) != base_value) {
                return nullptr;
            }
            return std::launder(reinterpret_cast<Handle *>(&m_storage[index]));
        }
```

改后：

```cpp
        auto lookup(u32 const base_value) -> Handle * {
            static_assert(USER_ASPACE_BASE == MB(2), ...);
            auto const index =
                arch::confine_array_index(base_value & HANDLE_INDEX_MASK, HANDLE_ARENA_SLOTS);
            auto const live = m_live_base_value[index].load(std::memory_order_acquire);
            auto const address = arch::conditional_select_nospec_eq(
                live, base_value | HANDLE_LIVE_MARK, reinterpret_cast<usize>(&m_storage[index]), 0);
            auto *const handle = reinterpret_cast<Handle *>(address);
            return handle ? std::launder(handle) : nullptr;
        }
```

三个分支分别这样消掉：

- **越界**：不检查，而是夹住。越界的下标变成 0，接下来的比较自然失败——探针里仍是原下标，
  槽 0 里任何记录都不可能等于它。
- **相等比较**：用条件传送选出"槽地址或 0"，比较未决时结果不可能是槽地址。
- **generation 为 0**：原来要拒绝它，是因为空槽记录 0，而下标 0、generation 0 的 base 值也是 0。
  现在占用中的槽记 `base_value | HANDLE_LIVE_MARK`（第 31 位——base 值只有 30 位，这一位
  永远空闲），探针也带这一位，空槽的 0 永远比不中。于是 generation 0 不必再保留，`reserve()`
  回绕时不再跳过它，与 Zircon 的 `NewHandleValue()` 一致。

失败时返回的 0 在推测路径上被解引用也安全：调用者的 `if (!handle)` 本身又是一个分支，误预测
时可能按小偏移读空指针；`USER_ASPACE_BASE`（2 MiB）以下在任何地址空间都不映射，读不到数据、
也不会填缓存行。`static_assert(USER_ASPACE_BASE == MB(2))` 把这个前提钉在代码里。

`std::launder` 之前的空指针判断是对**选择结果**的判断，推测路径上看到的只能是 0；编译器也
把它折掉了（见验证）。

**参照**
Zircon `Handle::FromU32()`（`kernel/object/handle.cc`）：`arena_.Confine(handle_addr)` 夹住地址，
`fbl::conditional_select_nospec_eq(handle->base_value(), value, handle_addr, 0)` 选出结果。
Zircon 把 base 值存在槽内对象里；这里存在槽外的 `m_live_base_value`，所以用"活标记"区分空槽，
而不是靠 generation 0。

**验证**
- 两个内核（dbg.0、dbg.1）里 `Handle::from_base_value` 的反汇编，从取下标到返回没有任何条件跳转：

  ```
  andl  $0x3ffff, %eax
  cmpq  %rax, %rdx          ; 0x1000 vs index
  cmovaq %rax, %rsi         ; confine
  movl  0x6088(%rax,%rsi,4), %edx   ; live record
  orl   $0x80000000, %edi   ; probe | LIVE_MARK
  ...
  cmpq  %rdx, %rdi
  cmovneq %rcx, %rax        ; slot address or 0
  retq
  ```

- 两个镜像启动正常，shell 的 `close again = BadHandle` 与通道检查照常（关闭后的陈旧值、
  以及正常查找都走的是这条新路径）。

**故意没做**
- 第 5 道关（句柄的表 koid 等于本表 koid）仍是普通比较：到那一步时拿到的已是一个真实存在的
  `Handle`，推测路径上最多看到"别的进程的一个合法句柄"，这与 Zircon 相同。
- arena 惰性提交推迟到 P1-C（2026-09-24 定）。
