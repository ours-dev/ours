# Fuchsia 内核 VM：类层次与关系

> 基于 `/home/developer/fuchsia/zircon/kernel/vm/` 源码分析。

---

## 1. 四层架构总览

```mermaid
flowchart TB
    subgraph L1["Layer 1: 用户可见层 (VMO Handle)"]
        VOP["VmObjectPaged<br/>• cow_pages_: RefPtr<VmCowPages><br/>• cow_range_: 可见窗口<br/>• parent_: VMO 级父指针<br/>• reference_list_: slice/reference 子节点<br/><br/>职责: 用户 syscall 入口, 委托页面操作"]
    end

    subgraph L2["Layer 2: 页面管理层 (不可见)"]
        VCP["VmCowPages<br/>• parent_/children_list_: 克隆树<br/>• page_list_: VmPageList<br/>• page_source_: demand paging<br/>• paged_ref_: 回指 VmObjectPaged<br/><br/>├── LookupCursor — 缺页遍历<br/>├── DeferredOps — 批量延迟操作<br/>└── CloneCowPageLocked — COW 核心<br/><br/>职责: COW 克隆树, 压缩解压, 缺页处理"]
    end

    subgraph L3["Layer 3: 页面存储层"]
        VPL["VmPageList<br/>btree<uint64_t, VmPageListNode><br/>每节点 16 槽 (64KB)<br/><br/>└── VmPageOrMarker (uint32_t)<br/>    Page | Reference | Marker<br/>    Interval | ParentContent<br/><br/>├── VmPageOrMarkerRef — 可变引用<br/>└── VMPLCursor — btree 迭代器"]
    end

    subgraph L4["Layer 4: 压缩子系统"]
        VCOM["VmCompression (全局单例)<br/>• VmCompressedStorage — 存储后端<br/>• VmCompressionStrategy — 算法 (lz4)<br/><br/>└── VmCompressor (单实例, instance_lock_ 串行化)<br/>    状态机: Ready → Started → Compressed → Finalized"]
    end

    L1 -->|"cow_pages_<br/>委托"| L2
    L2 -->|"page_list_<br/>持有"| L3
    L2 -->|"Pmm::Node()<br/>GetPageCompression()"| L4

```

---

## 2. VmObjectPaged ↔ VmCowPages：委托关系

```mermaid
flowchart TB
    subgraph UserHandle["用户句柄层"]
        H1["zx_handle_t<br/>(VMO 句柄)"] --> VOP1["VmObjectPaged<br/>cow_range_ = [0, size)<br/><br/>这是用户直接操作的 VMO"]
        H2["zx_handle_t<br/>(slice 句柄)"] --> VOP2["VmObjectPaged<br/>cow_range_ = [100, 50)<br/><br/>窗口视图 slice"]
    end

    subgraph CowTree["VmCowPages 克隆树 (内核内部)"]
        HIDDEN["VmCowPages (Hidden)<br/>is_hidden = true<br/>paged_ref_ = ✗<br/>page_list_ = [共享页面]"]
        ORIG["VmCowPages<br/>visible ✓<br/>paged_ref_ → VOP1"]
        CLONE["VmCowPages<br/>visible ✓<br/>paged_ref_ → VOP2"]

        HIDDEN --> ORIG
        HIDDEN --> CLONE
    end

    VOP1 -.->|"cow_pages_ RefPtr"| ORIG
    VOP2 -.->|"cow_pages_ RefPtr"| CLONE

```

**关键关系**:
- `VmObjectPaged.cow_pages_` → 指向它挂载的 `VmCowPages` 节点（RefPtr，共享所有权）
- `VmCowPages.paged_ref_` → 回指它"属于"的 `VmObjectPaged`（raw pointer，无所有权）
- Hidden 节点 `paged_ref_ == null`：不对应任何用户可见 VMO
- 多个 `VmObjectPaged` 可以共享同一棵 `VmCowPages` 树（通过 slice/reference + `cow_range_`）

---

## 3. VmCowPages 克隆树结构

### 3.1 节点属性对比

```mermaid
flowchart TD
    ROOT["Root (VmCowPages)<br/>━━━━━━━━━━━━━━<br/>parent_ = null<br/>page_source_ = ✓ (optional)<br/>paged_ref_ = ✓<br/>is_hidden = false<br/>page_list_ = [Page A, Page B, ...]<br/>━━━━━━━━━━━━━━<br/>🌳 树的顶层"]
    ROOT --> HIDDEN

    HIDDEN["Hidden (VmCowPages)<br/>━━━━━━━━━━━━━━<br/>parent_ = Root<br/>page_source_ = ✗<br/>paged_ref_ = ✗ (无 VMO 回指!)<br/>is_hidden = true (kHidden flag)<br/>page_list_ = [共享页面 C, D, ...]<br/>━━━━━━━━━━━━━━<br/>🔒 纯中间节点, 不可见"]

    HIDDEN --> CHILD1
    HIDDEN --> CHILD2

    CHILD1["Child 1 (VmCowPages)<br/>━━━━━━━━━━━━━━<br/>paged_ref_ → VmObjectPaged ✓<br/>page_list_ = [空/ParentContent]<br/>parent_offset_ = 0<br/>━━━━━━━━━━━━━━<br/>🍃 原 VMO, 可能有 page_source_"]

    CHILD2["Child 2 (VmCowPages)<br/>━━━━━━━━━━━━━━<br/>paged_ref_ → VmObjectPaged ✓<br/>page_list_ = [空/ParentContent]<br/>parent_offset_ = offset<br/>━━━━━━━━━━━━━━<br/>🍃 Clone, 子范围视图"]

```

### 3.2 坐标转换

```mermaid
flowchart LR
    P0["Parent offset 空间<br/>0 ───── size"] -->|"+ parent_offset_"| P1["Child 可见空间<br/>offset ───── offset+limit"]
    P0 -->|"超出 parent_limit_<br/>停止向上查找"| P2["Root (或 Hidden)"]
    P3["root_parent_offset_<br/>= 所有 parent_offset_ 之和"] -.->|"溢出保护"| P0
```

> 查找 child offset X → 在 parent 中查 `X + parent_offset_`。`parent_limit_` 指示向上查找的边界，`root_parent_offset_` 是累积偏移，用于防止 64-bit 溢出。

---

## 4. LookupCursor：缺页遍历生命周期

```mermaid
sequenceDiagram
    actor Fault as MMU 缺页中断
    participant VOP as VmObjectPaged
    participant VCP as VmCowPages
    participant Cursor as LookupCursor
    participant Tree as 克隆树

    Fault->>VOP: GetPage(offset, will_write)
    VOP->>VCP: GetLookupCursorLocked(range)
    VCP->>Cursor: new LookupCursor(target, range)
    
    Cursor->>Cursor: RequirePage(will_write)
    Note over Cursor: will_write → RequireOwnedPage<br/>!will_write → RequireReadPage

    Cursor->>Tree: EstablishCursor()
    Tree->>Tree: FindPageContentLocked()<br/>沿 parent_ 链自底向上查找
    Tree-->>Cursor: owner_info_ {cursor, owner, owner_offset}

    alt Slot = Reference
        Cursor->>VCP: MakePageFromReference()
        Note over Cursor: Decompress → Page
    else Slot is Page and TargetIsOwner
        Cursor-->>VOP: return {page, writable=true}
    else Slot is Page and not TargetIsOwner
        Note over Cursor: COW! CloneCowPageLocked<br/>or TargetAllocateCopyPageAsResult
        Cursor-->>VOP: return {page, writable=true}
    else Slot = Marker/ContentZero
        Note over Cursor: ForkMarker / 零页分配
        Cursor-->>VOP: return {page, writable=true}
    else Slot = Empty
        Note over Cursor: ReadRequest → 阻塞等 UserPager
    end

    Cursor->>Cursor: ~LookupCursor() 释放所有锁
```

---

## 5. VmPageOrMarker：槽位五态

```mermaid
stateDiagram-v2
    [*] --> Empty

    state "Page (0b000)" as Page
    state "Reference (0b010)" as Reference
    state "Marker (0b001)" as Marker
    state "Empty" as Empty
    state "Interval (0b011)" as Interval
    state "ParentContent (0b100)" as ParentContent

    note right of Page
        vm_page_t* 物理页
        高 29 bit = PMM 页索引
    end note
    note right of Reference
        VmCompression 索引
        高 29 bit = 压缩数据句柄
    end note
    note right of Marker
        零页标记
        高 29 bit = share_count
    end note
    note right of Interval
        稀疏零区间
        Start/End/Slot sentinel
    end note
    note right of ParentContent
        内容在 parent 的
        page_list 中
    end note

    Empty --> Page : 分配物理页
    Empty --> Reference : 压缩页面迁入
    Empty --> Marker : 零页标记
    Empty --> ParentContent : clone 创建时置入

    Page --> Reference : SwapPageForReference<br/>(压缩)
    Reference --> Page : SwapReferenceForPage<br/>(解压)
    Page --> Empty : 释放
    Reference --> Reference : SwapReferenceForReference

    Marker --> Page : ForkMarkerLocked<br/>(写时 COW)
    ParentContent --> Page : COW 分配新页面
```

### 5.1 VmPageList 内部结构

```mermaid
flowchart TD
    VPL["VmPageList<br/>btree<uint64_t, unique_ptr<VmPageListNode>>"]
    
    VPL --> N0["VmPageListNode<br/>base = 0<br/>覆盖 [0, 64KB)"]
    VPL --> N1["VmPageListNode<br/>base = 64KB<br/>覆盖 [64KB, 128KB)"]
    VPL --> N2["VmPageListNode<br/>base = ... <br/>按需分配, 稀疏存储"]

    N0 --> S00["pages_[0]  Page A"]
    N0 --> S01["pages_[1]  Empty"]
    N0 --> S02["pages_[2]  Reference → compressed"]
    N0 --> S03["..."]
    N0 --> S15["pages_[15] Marker (zero)"]

    N1 --> S10["pages_[0]  ParentContent ⤴"]
    N1 --> S11["pages_[1]  Empty"]
    N1 --> S12["..."]
    N1 --> S15["pages_[15] Interval(Start)"]

```

> `kPageFanOut = 16`，每个节点覆盖 16 页 = 64KB。`VMPLCursor` 封装 btree 迭代器，提供连续遍历能力。

---

## 6. 压缩子系统

```mermaid
flowchart TD
    subgraph CompressionSubsystem["VmCompression 压缩子系统"]
        COMP["VmCompression (全局单例)<br/>PmmNode::page_compression_<br/><br/>创建时注入:"]
        STORAGE["VmCompressedStorage<br/>压缩数据存储后端<br/>CompressedData(ref) → {src, metadata}<br/>Free(ref)"]
        STRATEGY["VmCompressionStrategy<br/>压缩算法接口<br/>Compress(src, dst)<br/>Decompress(src, len, dst)"]
        COMPRESSOR["VmCompressor (单实例)<br/>instance_lock_ 串行化<br/>━━━━━━━━━━━<br/>State 状态机:<br/>Ready → Started → Compressed → Finalized<br/><br/>Arm() → 预分配 spare page<br/>Start() → 返回临时 Reference<br/>Compress() → 无锁压缩<br/>TakeCompressionResult() → 最终结果"]

        COMP --> STORAGE
        COMP --> STRATEGY
        COMP --> COMPRESSOR
    end

    subgraph CowPath["VmCowPages 调用路径"]
        DIR1["ReclaimPageForCompression<br/>Page → Reference"] --> COMPRESSOR
        DIR2["MakePageFromReference<br/>Reference → Page"] --> COMP
    end

```

---

## 7. 完整关系总图

```mermaid
flowchart TB
    SYSCALL["用户态 syscall<br/>zx_vmo_create / read / write / clone"] --> VOP

    subgraph UserVisible["用户可见层"]
        VOP["VmObjectPaged<br/>handle + 窗口范围"]
    end

    subgraph PageMgmt["页面管理层"]
        VCP["VmCowPages<br/>COW 树节点"]
        CURSOR["LookupCursor<br/>缺页遍历"]
        DEFERRED["DeferredOps<br/>批量延迟操作"]
    end

    subgraph PageStorage["页面存储层"]
        VPL["VmPageList<br/>btree 稀疏数组"]
        SLOT["VmPageOrMarker<br/>Page | Reference | Marker<br/>Interval | ParentContent"]
    end

    subgraph Compression["压缩子系统"]
        VMCOM["VmCompression"]
        VMCOMP["VmCompressor"]
    end

    VOP -->|"cow_pages_ →"| VCP
    VOP -->|"GetLookupCursorLocked()"| CURSOR
    VCP -->|"page_list_"| VPL
    VPL -->|"槽位"| SLOT
    VCP -->|"Pmm::Node().GetPageCompression()"| VMCOM
    VMCOM -->|"instance_"| VMCOMP
    CURSOR -->|"EstablishCursor →<br/>FindPageContentLocked"| VCP
    VCP -->|"paged_ref_ ←"| VOP
    DEFERRED -->|"FreedList, RangeChange"| VCP

```

---

## 8. 关键成员变量速查表

### VmObjectPaged

| 成员 | 类型 | 说明 |
|------|------|------|
| `cow_pages_` | `fbl::RefPtr<VmCowPages>` | 指向页面管理节点 |
| `cow_range_` | `VmCowRange` | 此 VMO 在 cow_pages_ 中的可见窗口 |
| `parent_` | `VmObjectPaged*` | slice/reference 的父 VMO 指针 |
| `reference_list_` | `ReferenceList` | 所有 slice/reference 子节点链表 |

### VmCowPages

| 成员 | 类型 | 说明 |
|------|------|------|
| `options_` | `VmCowPagesOptions` | kHidden, kPageSourceRoot, kUserPagerBackedRoot |
| `parent_` | `fbl::RefPtr<VmCowPages>` | 克隆树父节点 (null = 树的根) |
| `children_list_` | `TaggedDoublyLinkedList` | 克隆树所有子节点 |
| `page_list_` | `VmPageList` | 本节点拥有的页面 |
| `page_source_` | `fbl::RefPtr<PageSource>` | 可选，demand paging 数据源 |
| `paged_ref_` | `VmObjectPaged*` | 回指用户可见 VMO (hidden 为 null) |
| `parent_offset_` | `uint64_t` | 本节点在 parent 中的起始偏移 |
| `parent_limit_` | `uint64_t` | 超出此偏移停止向上查找 |
| `root_parent_offset_` | `uint64_t` | 投影到根节点的累积偏移 |
| `size_` | `uint64_t` | 本节点的页面大小 |

---

## 9. COW 克隆与写缺页完整流程

### 9.1 Clone 创建 (CreateCloneLocked)

```mermaid
sequenceDiagram
    participant A as A (原 VMO)
    participant H as Hidden
    participant B as B (新 Clone)

    Note over A: page_list_ = [Page X, share_count=0]

    Note over A: ---- Step 1: share_count++ 对所有可见页面 ----
    A->>A: ForEveryOwnedMutableHierarchyPage<br/>Page X share_count: 0 → 1

    Note over A: ---- Step 2: CloneNewHiddenParentLocked ----
    A->>H: 创建 Hidden 节点 (kHidden)
    A->>H: page_list_ 从 A 搬到 Hidden
    Note over H: Page X 现在在 Hidden 中
    A->>H: AddChildLocked(A, offset=0)
    A->>H: AddChildLocked(B, offset=clone_range)

    Note over H: page_list_ = [Page X, share_count=1]
    Note over A: page_list_ = [空/ParentContent]
    Note over B: page_list_ = [空]
```

> **关键**：share_count 在创建 clone 树结构**之前**就递增了。rollback 机制保证失败时回滚。

### 9.2 share_count 的含义

```
share_count = 还有几个子节点（兄弟姐妹）能通过 Hidden 看见这个页面

  share_count == 0  →  只有一个子节点还能看到，或没有共享
  share_count == 1  →  还有一个兄弟能看到（刚 clone 完的典型值）
  share_count == N  →  还有 N 个兄弟能看到
```

### 9.3 写缺页 COW：Fork vs Migrate

```mermaid
flowchart TD
    WF["A 写 Page X<br/>RequireOwnedPage"] --> FIND["FindPageContentLocked<br/>在 Hidden 中找到 Page X<br/>TargetIsOwner = false"]
    FIND --> CHECK{"page->object.share_count > 0 ?"}

    CHECK -->|"Yes (share_count=1)<br/>还有其他兄弟能看到"| FORK["FORK: 分配新物理页, 拷贝内容<br/>share_count--<br/>新页面插入 A 的 page_list<br/>━━━━━━━━━━━━━━<br/>Hidden: [Page X, sc=0]<br/>A: [Page X' (副本)]"]
    CHECK -->|"No (share_count=0)<br/>没有兄弟能看到了"| MIGRATE["MIGRATE: Hidden.page_list.RemoveContent<br/>取出 Page X → 插入 A 的 page_list<br/>零拷贝, 不分配新物理页<br/>━━━━━━━━━━━━━━<br/>Hidden: [空]<br/>A: [Page X (原页)]"]

    FORK --> RESULT["A 有独立副本 ✓"]
    MIGRATE --> RESULT
```

### 9.4 两种场景演示

```
初始: Hidden [Page X, share_count=1], A [空], B [空]

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
场景 1: A 先写, B 后写
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

A 写 → share_count=1>0 → FORK
       Hidden [Page X, sc=0], A [Page X'], B [空]

B 写 → share_count=0 → MIGRATE
       Hidden [空], A [Page X'], B [Page X]

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
场景 2: B 先写, A 后写
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

B 写 → share_count=1>0 → FORK
       Hidden [Page X, sc=0], A [空], B [Page X']

A 写 → share_count=0 → MIGRATE
       Hidden [空], A [Page X], B [Page X']
```

> **结论**：不管谁先写，第一个写的做 Fork（分配新页、memcpy），最后一个写的做 Migrate（直接搬原页，零开销）。share_count 精准保证了这一点。

### 9.5 PageSource 的角色

```mermaid
flowchart LR
    subgraph Tree["COW 克隆树"]
        R["Root<br/>page_source_ = ✓ or null<br/>(唯一可能有 PageSource 的节点)"]
        H["Hidden<br/>page_source_ = ✗"]
        A["A (原 VMO)<br/>page_source_ = ✗"]
        B["B (Clone)<br/>page_source_ = ✗"]
        R --> H --> A
        H --> B
    end

    subgraph PS["PageSource"]
        SRC["UserPager / null (Anonymous)"]
    end

    R -.->|"缺页到 Root 还是空"| SRC
    SRC -.->|"提供页面内容"| R

```

- **只有 Root 持有 `page_source_`**（可空，null 表示 Anonymous 零页）
- Clone 创建时不复制 PageSource——B 只能通过 Hidden → Root 间接用到它
- 缺页走到 Root 还是空 → 问 PageSource 要数据（可能阻塞等 UserPager）

### 9.6 为什么需要 Hidden 节点？

最直接的做法是让 B 直接挂到 A 下面：

```
简单方案:                     Fuchsia 方案:

  A [Page X]                   Hidden [Page X]
    └── B [空]                   ├── A [空]
                                 └── B [空]
```

简单方案的问题是 **A 自己要写 Page X 时**：

> A 既是"共享页面的持有者"又是"可以修改的参与者"。
> A 写 Page X → 页面在 A 自己的 page_list 里 → B 也在看 → 不能直接改
> → A 需要"对自己 COW"——把自己的页面复制一份替换自己？
> → 新页面放哪？怎么让 B 不受影响？

Fuchsia 的做法是把这两个角色拆开：

```
  Hidden [Page X]     ← 只负责持有共享页面，只读，永不变
    ├── A [空]        ← 只负责按需修改
    └── B [空]        ← 跟 A 完全对等
```

A 写和 B 写走**完全相同的 COW 路径**（CloneCowPageLocked），没有特殊情况。A 不需要对自己 COW，因为共享页面根本不在 A 的 page_list 里。

### 9.7 Hidden 何时消失？

触发条件（[vm_cow_pages.h:1012](zircon/kernel/vm/include/vm/vm_cow_pages.h#L1012)）：

```cpp
bool should_dead_transition_locked() const {
    return !paged_ref_ && children_list_len_ == 0 && life_cycle_ == LifeCycle::Alive;
}
```

Hidden 的 `paged_ref_` 永远为 null，所以就是等 **所有孩子都走了**（`children_list_len_ == 0`）。

```
场景 A: 还剩 2 孩子，走 1 个（合并成功）
  Hidden [Page X]            Sibling close
    ├── Child                   → RemoveChildLocked: children_list_len_==2
    └── Sibling(close)          → DropChildLocked(Sibling)
                                → MergeContentWithChildLocked:
                                  把 Hidden 的页面塞进剩下的 Child
                                  塞成功 → DropChildLocked(Child)
                                  children_list_len_ = 0 → Hidden 消失 ✅

场景 B: 还剩 2 孩子，走 1 个（合并失败）
  同上, 但 Child 的相同 offset 已经有自己的页面 → 塞不进去
  Hidden 留着, 剩 1 个孩子

场景 C: 只剩 1 孩子（合并失败留下），走掉
  Hidden [空/剩页]            Child close
    └── Child(close)             → DropChildLocked
                                 children_list_len_ = 0 → Hidden 消失 ✅
```

**合并失败**的唯一原因：剩下的 Child 在同一 offset 已经有自己的页面了（之前 COW Fork 过）。

---

## 10. 关键函数索引

| 函数 | 类 | 位置 | 功能 |
|------|------|------|------|
| `Create` | `VmObjectPaged` | [vm_object_paged.h:61](zircon/kernel/vm/include/vm/vm_object_paged.h#L61) | 创建普通 VMO |
| `CreateExternal` | `VmObjectPaged` | [vm_object_paged.h:82](zircon/kernel/vm/include/vm/vm_object_paged.h#L82) | 创建 pager-backed VMO |
| `GetPage` | `VmObjectPaged` | [vm_object_paged.cc:1995](zircon/kernel/vm/vm_object_paged.cc#L1995) | 缺页入口，创建 LookupCursor |
| `Create` | `VmCowPages` | [vm_cow_pages.h:162](zircon/kernel/vm/include/vm/vm_cow_pages.h#L162) | 创建 VmCowPages 节点 |
| `CreateCloneLocked` | `VmCowPages` | [vm_cow_pages.cc:1831](zircon/kernel/vm/vm_cow_pages.cc#L1831) | 创建 clone |
| `CloneNewHiddenParentLocked` | `VmCowPages` | [vm_cow_pages.cc:1582](zircon/kernel/vm/vm_cow_pages.cc#L1582) | 创建 hidden 父节点，页面迁移 |
| `FindPageContentLocked` | `VmCowPages` | [vm_cow_pages.cc:2958](zircon/kernel/vm/vm_cow_pages.cc#L2958) | 沿克隆链向上查找页面 |
| `RequireOwnedPage` | `LookupCursor` | [vm_cow_pages.cc:3747](zircon/kernel/vm/vm_cow_pages.cc#L3747) | 写缺页处理 |
| `RequireReadPage` | `LookupCursor` | [vm_cow_pages.cc:3888](zircon/kernel/vm/vm_cow_pages.cc#L3888) | 读缺页处理 |
| `MakePageFromReference` | `VmCowPages` | [vm_cow_pages.cc:951](zircon/kernel/vm/vm_cow_pages.cc#L951) | 解压 Reference → Page |
| `CloneCowPageLocked` | `VmCowPages` | [vm_cow_pages.cc:2671](zircon/kernel/vm/vm_cow_pages.cc#L2671) | Hidden 节点 COW (迁移/复制) |
| `TargetAllocateCopyPageAsResult` | `LookupCursor` | [vm_cow_pages.cc:3444](zircon/kernel/vm/vm_cow_pages.cc#L3444) | 直接 COW 复制 |
| `ReclaimPageForCompression` | `VmCowPages` | [vm_cow_pages.cc:7387](zircon/kernel/vm/vm_cow_pages.cc#L7387) | 压缩 Page → Reference |
