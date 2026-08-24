# Fuchsia 内核 VM：COW 页面管理与压缩流程图

> 基于 `zircon/kernel/vm/vm_cow_pages.cc` 源码分析。
>
> 文件索引：
> - [vm_cow_pages.cc](/home/developer/fuchsia/zircon/kernel/vm/vm_cow_pages.cc) — COW 页面管理实现
> - [vm_cow_pages.h](/home/developer/fuchsia/zircon/kernel/vm/include/vm/vm_cow_pages.h) — `LookupCursor`、`VmCowPagesOptions` 等定义
> - [vm_page_list.h](/home/developer/fuchsia/zircon/kernel/vm/include/vm/vm_page_list.h) — `VmPageOrMarker` 三态表示
> - [compression.h](/home/developer/fuchsia/zircon/kernel/vm/include/vm/compression.h) — `VmCompression` 压缩子系统接口
> - [physmap.h](/home/developer/fuchsia/zircon/kernel/vm/include/vm/physmap.h) — `paddr_to_physmap()` 物理地址到内核虚拟地址映射

---

## 1. VmPageOrMarker：一个槽位的三种状态

VMO page list 中每个槽位是 `VmPageOrMarker`（内部编码为一个 `uint32_t`，低 3 bit 为类型标记）。

```mermaid
stateDiagram-v2
    state "槽位状态" as slot
    state "Empty\n(编码: Page(0))" as Empty
    state "Page\n(kPageType = 0b000)\n真实物理页 → vm_page_t*\n占用物理内存" as Page
    state "Reference\n(kReferenceType = 0b010)\n压缩引用 → VmCompression 索引\n不占物理内存" as Reference
    state "Marker\n(kZeroMarkerType = 0b001)\n零页面标记\n不占物理内存，存储 share_count" as Marker

    slot --> Empty : 初始状态
    slot --> Page : 分配/映射
    slot --> Reference : 压缩
    slot --> Marker : 零页标记

    Page --> Reference : SwapPageForReference()
    Reference --> Page : SwapReferenceForPage()
    Page --> Empty : 释放
    Reference --> Reference : SwapReferenceForReference()
    Marker --> Page : ForkMarkerLocked()
```

### 1.1 三种状态对比

| 状态 | 编码 | 存储内容 | 占用物理内存 | 典型场景 |
|------|------|----------|:--:|----------|
| **Page** | `0b000` | `vm_page_t*` 指针（PMM 页索引） | ✅ | 正常分配、COW 复制后 |
| **Reference** | `0b010` | 压缩数据句柄（`VmCompression` 内部索引） | ❌ | 内存压力下被压缩的冷页 |
| **Marker** | `0b001` | 共享计数（高 29 bit） | ❌ | 已提交但内容全零的页 |

---

## 2. 页面压缩与解压的完整生命周期

```mermaid
flowchart TD
    subgraph "正常使用"
        A[物理页 Page<br/>在 VMO page list 中<br/>share_count = N]
    end

    subgraph "后台压缩 (ReclaimPage)"
        B[PMM 回收器选中冷页]
        C[ReclaimPageForCompression<br/>line 7387]
        D["compressor->Compress<br/>锁外压缩，可能耗时"]
        E[SwapPageForReference<br/>line 7448: 临时 Reference]
        F{压缩成功?}
        G[SwapReferenceForReference<br/>line 7484: 永久 Reference]
        H[SwapReferenceForPage<br/>line 7496: 恢复 Page]
    end

    subgraph "按需解压 (Page Fault)"
        I[缺页中断 / LookupCursor]
        J[FindPageContentLocked<br/>line 2958: 发现槽位是 Reference]
        K[MakePageFromReference<br/>line 951]
        L[AllocPage: 分配新物理页 p]
        M[SwapReferenceForPage: 取出 ref, 装入 p]
        N[paddr_to_physmap: 获取 p 的内核虚拟地址]
        O["compression->Decompress<br/>ref → page_data, 输出 metadata"]
        P["p->object.share_count = page_metadata"]
    end

    A -->|内存压力| B
    B --> C
    C --> D
    D --> E
    E --> F
    F -->|Yes| G
    F -->|No| H
    G -->|后续访问缺页| I
    H --> A
    I --> J
    J --> K
    K --> L --> M --> N --> O --> P
    P -->|回到正常使用| A

```

### 2.1 MakePageFromReference 详细流程（你选中的代码）

```mermaid
flowchart LR
    subgraph "1. 前置检查"
        A1["page_or_mark->IsReference<br/>DEBUG_ASSERT 断言"]
        A2[Pmm::Node.GetPageCompression<br/>获取压缩子系统单例]
    end

    subgraph "2. 分配新页面"
        B1["AllocPage(&p, page_request)<br/>可能返回 ZX_ERR_SHOULD_WAIT"]
    end

    subgraph "3. 原子替换"
        C1["ref = SwapReferenceForPage(p)<br/>↓↓↓<br/>槽位: Reference → Page<br/>返回: 压缩数据句柄 ref"]
    end

    subgraph "4. 解压"
        D1["page_data = paddr_to_physmap p->paddr<br/>物理地址 → 内核虚拟地址"]
        D2["compression->Decompress(ref, page_data, &page_metadata)<br/>ref 指向的压缩数据 → 写入 page_data"]
    end

    subgraph "5. 恢复元数据"
        E1["p->object.share_count = page_metadata<br/>恢复共享计数，保证 COW 语义"]
    end

    A1 --> A2 --> B1 --> C1 --> D1 --> D2 --> E1
```

---

## 3. 写缺页（Write Fault）完整决策流程

入口：`VmCowPages::LookupCursor::RequireOwnedPage`（[line 3747](zircon/kernel/vm/vm_cow_pages.cc#L3747)）

```mermaid
flowchart TD
    Start([RequireOwnedPage<br/>will_write = true]) --> Establish[EstablishCursor<br/>沿克隆链查找页面所在节点]
    Establish --> CheckRef{CursorIsReference?}
    CheckRef -->|Yes| Decompress[CursorReferenceToPage<br/>→ MakePageFromReference 解压]
    Decompress --> CheckOwner
    CheckRef -->|No| CheckOwner{TargetIsOwner<br/>and CursorIsPage?}
    CheckOwner -->|"Yes: 页面是我的"| DirectWrite1{userpager-backed?}
    DirectWrite1 -->|Yes| LoanCheck{页面是 loaned?}
    LoanCheck -->|Yes| ReplaceLoan[ReplacePageLocked<br/>替换 loaned 页面]
    LoanCheck -->|No| DirtyCheck{页面 clean?}
    ReplaceLoan --> DirtyCheck
    DirtyCheck -->|Yes| DirtyReq[DirtyRequest<br/>标记 Dirty，通知 pager]
    DirtyCheck -->|No| ReturnDirect
    DirtyReq --> ReturnDirect[return CursorAsResult<br/>writable = true<br/>无需 COW]
    DirectWrite1 -->|No| ReturnDirect
    CheckOwner -->|No| CheckPage{CursorIsPage?<br/>必然 !TargetIsOwner}
    CheckPage -->|"Yes: 页面在祖先节点"| MarkAccess[MarkAccessed<br/>记录页面被访问]
    MarkAccess --> CheckHidden{owner 是 hidden 节点?}
    CheckHidden -->|"No: 直接 COW 复制"| DirectCopy[TargetAllocateCopyPageAsResult<br/>line 3818<br/>从 owner 分配新页面复制内容<br/>插入到 target page_list<br/>owner 页面不变]
    DirectCopy --> ReturnCow[return writable page]
    CheckHidden -->|"Yes: Hidden 节点 COW"| CloneCow[CloneCowPageLocked<br/>line 3821]
    CloneCow --> ShareCheck{share_count == 0?}
    ShareCheck -->|"Yes: 无其他子节点共享"| Migrate[迁移: 直接从 hidden 移到 target<br/>零拷贝！]
    ShareCheck -->|"No: 有其他子节点"| CopyPage[复制: share_count 减 1<br/>分配新页面拷贝内容]
    Migrate --> InvalCursor[InvalidateCursor<br/>hidden 的 page_list 被修改了]
    CopyPage --> InvalCursor
    InvalCursor --> ReturnCow
    CheckPage -->|No| CheckZero{CursorIsContentZero?}
    CheckZero -->|Yes| ZeroDetail{具体情形}
    ZeroDetail -->|"Marker + hidden owner + will_write"| ForkMarker[ForkMarkerLocked<br/>line 3864]
    ZeroDetail -->|"其他"| AllocZero[TargetAllocateCopyPageAsResult<br/>vm_get_zero_page<br/>分配零页]
    ForkMarker --> ReturnZero[return writable page]
    AllocZero --> ReturnZero
    CheckZero -->|No| CheckEmpty{CursorIsEmpty?}
    CheckEmpty -->|Yes| ReadReq[ReadRequest<br/>向 UserPager 请求内容<br/>阻塞等待]
    ReadReq --> Retry[等待完成后重试]
```

---

## 4. TargetIsOwner() 与克隆层级

### 4.1 核心概念

```mermaid
flowchart TD
    subgraph "VMO 克隆树"
        Root[Root VMO<br/>可见, 可能是 userpager-backed<br/>有自己的 page_list]
        Hidden[Child1 hidden 节点<br/>不可见, 无 VmObjectPaged<br/>持有共享页面<br/>有自己的 page_list]
        Child1[Child2 可见 VMO<br/>用户直接操作<br/>有自己的 page_list]
        Child2[Child3 可见 VMO<br/>用户的兄弟克隆<br/>有自己的 page_list]
    end

    Root -->|"parent_"| Hidden
    Hidden -->|"parent_"| Child1
    Hidden -->|"parent_"| Child2

    subgraph "FindPageContentLocked 查找过程"
        direction LR
        S1["1. 查 target (Child2) page_list"] -->|空| S2["2. 查 parent (Hidden) page_list"]
        S2 -->|"找到 Page A"| S3["3. 停止: owner = Hidden<br/>TargetIsOwner = false"]
    end

    Root --- S1
```

### 4.2 `TargetIsOwner()` 判定逻辑

```
TargetIsOwner() := (owner_info_.owner == null)

  owner == null  →  页面在 target 自己的 page_list  →  直接写, 无 COW
  owner != null  →  页面在祖先的 page_list          →  COW
```

### 4.3 Hidden 节点的创建（CloneNewHiddenParentLocked）

```mermaid
sequenceDiagram
    participant R as Root VMO (可见)
    participant H as Hidden Node (新)
    participant C as Clone VMO (可见)

    Note over R: 初始状态: page_list 有若干页面<br/>parent = null

    R->>H: 1. 创建 hidden 节点 (kHidden flag)
    R->>H: 2. 所有页面从 Root 迁移到 Hidden<br/>Root.page_list → Hidden.page_list
    R->>H: 3. Root.parent 改为 Hidden<br/>Root 变成 Hidden 的子节点
    H->>C: 4. Clone 创建, 挂在 Hidden 下

    Note over R,C: 最终拓扑: Hidden → {Root, Clone}<br/>共享页面在 Hidden 的 page_list 中
```

---

## 5. FindPageContentLocked：沿克隆树向上查找

```mermaid
flowchart TD
    Start([FindPageContentLocked<br/>offset, max_length]) --> Init[curr = target<br/>cur_offset = offset]

    Init --> Loop{curr 还有父节点?}

    Loop -->|Yes| Lookup["在 curr.page_list 中查找<br/>LookupNearestMutableCursor(cur_offset)"]
    Lookup --> Found{"找到非空内容?"}

    Found -->|"Yes: Page/Reference/Marker"| ReturnOwner["return {<br/>  cursor: 槽位游标,<br/>  owner: curr,<br/>  owner_offset: cur_offset<br/>}"]
    Found -->|No: Empty/ParentContent| CheckMarker{curr 有 parent_content markers?}

    CheckMarker -->|Yes| CheckPCM{"有 marker 在 cur_offset?"}
    CheckPCM -->|No: 向上被 marker 阻断| ReturnEmpty["return empty<br/>向上肯定没内容"]
    CheckPCM -->|Yes| Continue

    CheckMarker -->|No: hidden 节点无 marker| Continue["cur_offset += parent_offset_<br/>curr = curr.parent"]
    Continue --> Loop

    Loop -->|No: 到达 Root| LookupRoot["在 Root.page_list 中查找"]
    LookupRoot --> ReturnRoot["return {cursor, owner=Root}"]

```

---

## 6. 两种 COW 路径对比

### 6.1 路径 A：直接复制（owner 为非 hidden，如 Root）

```
  Before:                    After (write fault):
  ┌─────────────┐            ┌─────────────┐
  │ Root (owner)│            │ Root        │
  │ page_list:  │            │ page_list:  │
  │  [Page A]   │            │  [Page A]   │  ← 不变！share_count 不变
  └──────┬──────┘            └──────┬──────┘
         │                          │
  ┌──────┴──────┐            ┌──────┴──────┐
  │ Child (target)│           │ Child        │
  │ page_list:   │            │ page_list:   │
  │  [空]        │            │  [Page A']   │  ← 新分配的复制页
  └─────────────┘            └─────────────┘
```

### 6.2 路径 B：Hidden 节点 COW（CloneCowPageLocked）

```
  Before: share_count > 0           After (write fault on Child2):

  ┌──────────────┐                  ┌──────────────┐
  │ Hidden       │                  │ Hidden       │
  │ page_list:   │                  │ page_list:   │
  │  [Page A]    │                  │  [Page A]    │  ← 还在
  │ share_count=2│                  │ share_count=1│  ← 减 1
  └──┬────────┬──┘                  └──┬────────┬──┘
     │        │                        │        │
  ┌──┴──┐  ┌──┴──┐                 ┌──┴──┐  ┌──┴──┐
  │Child1│  │Child2│                │Child1│  │Child2│
  │ [空] │  │ [空] │                │ [空] │  │[Pg A']│ ← Child2 新副本
  └─────┘  └─────┘                 └─────┘  └─────┘


  Before: share_count == 0           After (write fault on Child1):

  ┌──────────────┐                  ┌──────────────┐
  │ Hidden       │                  │ Hidden       │
  │ page_list:   │                  │ page_list:   │
  │  [Page A]    │                  │  [空]        │  ← Page A 被移走
  │ share_count=0│                  └──────────────┘
  └──┬────────┬──┘                        │
     │        │                     ┌──────┴──────┐
  ┌──┴──┐  ┌──┴──┐                 │ Child1      │
  │Child1│  │Child2│                │ page_list:  │
  │ [空] │  │ [空] │                │  [Page A]   │  ← 直接迁移，零拷贝！
  └─────┘  └─────┘                 └─────────────┘
```

---

## 7. 读缺页（Read Fault）决策流程

相对简单——读操作不需要 COW，直接返回已有页面（只读）。

```mermaid
flowchart TD
    Start([RequireReadPage]) --> Establish[EstablishCursor]
    Establish --> Check{IsPage or IsReference?}
    Check -->|Yes| CheckRef{IsReference?}
    CheckRef -->|Yes| Decompress[CursorReferenceToPage<br/>解压]
    CheckRef -->|No| ReturnRO
    Decompress --> ReturnRO["return CursorAsResult<br/>writable = false<br/>如果是祖先的页面 → 映射为只读"]
    Check -->|No| CheckZero{IsContentZero?}
    CheckZero -->|Yes| ReturnZero[return vm_get_zero_page<br/>不分配新页面]
    CheckZero -->|No| ReadReq[ReadRequest<br/>向 UserPager 请求内容<br/>阻塞等待]
```

---

## 8. 完整流程总览

```mermaid
flowchart TD
    subgraph "触发路径"
        PF[MMU 缺页中断] --> VOP[VmObjectPaged::GetPage]
        VOP --> Cursor[GetLookupCursorLocked<br/>创建 LookupCursor]
        Cursor --> Dispatch{will_write?}
        Dispatch -->|true| ROP[RequireOwnedPage]
        Dispatch -->|false| RRP[RequireReadPage]
    end

    subgraph "压缩子系统"
        Reclaim[PMM 回收 ↓] --> RPC[ReclaimPageForCompression]
        RPC --> Compress[VmCompression::Compress]
        Compress --> SwapRef[SwapPageForReference<br/>Page → Reference]
    end

    subgraph "解压 (Reference → Page)"
        ROP --> IsRef1{CursorIsReference?}
        RRP --> IsRef2{CursorIsReference?}
        IsRef1 -->|Yes| Decomp[MakePageFromReference]
        IsRef2 -->|Yes| Decomp
    end

    subgraph "COW 判断"
        IsRef1 -->|No| OwnCheck{TargetIsOwner?}
        OwnCheck -->|Yes| Direct[直接写]
        OwnCheck -->|No| CowType{owner is_hidden?}
        CowType -->|No| DirectCopy[TargetAllocateCopyPageAsResult]
        CowType -->|Yes| CloneCow[CloneCowPageLocked<br/>share_count==0? 迁移 : 复制]
    end

```

---

## 9. 关键函数索引

| 函数 | 位置 | 功能 |
|------|------|------|
| `VmCowPages::MakePageFromReference` | [vm_cow_pages.cc:951](zircon/kernel/vm/vm_cow_pages.cc#L951) | 将压缩 Reference 解压为物理 Page |
| `VmCowPages::ReplaceReferenceWithPageLocked` | [vm_cow_pages.cc:977](zircon/kernel/vm/vm_cow_pages.cc#L977) | 解压 + 加入 page queues |
| `VmCowPages::ReclaimPageForCompression` | [vm_cow_pages.cc:7387](zircon/kernel/vm/vm_cow_pages.cc#L7387) | 压缩一个页面 Page → Reference |
| `VmCowPages::FindPageContentLocked` | [vm_cow_pages.cc:2958](zircon/kernel/vm/vm_cow_pages.cc#L2958) | 沿克隆树向上查找页面内容 |
| `LookupCursor::RequireOwnedPage` | [vm_cow_pages.cc:3747](zircon/kernel/vm/vm_cow_pages.cc#L3747) | 写缺页核心处理 |
| `LookupCursor::RequireReadPage` | [vm_cow_pages.cc:3888](zircon/kernel/vm/vm_cow_pages.cc#L3888) | 读缺页核心处理 |
| `VmCowPages::CloneCowPageLocked` | [vm_cow_pages.cc:2671](zircon/kernel/vm/vm_cow_pages.cc#L2671) | Hidden 节点 COW 分叉（迁移/复制） |
| `LookupCursor::TargetAllocateCopyPageAsResult` | [vm_cow_pages.cc:3444](zircon/kernel/vm/vm_cow_pages.cc#L3444) | 直接 COW 复制（非 hidden owner） |
| `VmCowPages::CloneNewHiddenParentLocked` | [vm_cow_pages.cc:1582](zircon/kernel/vm/vm_cow_pages.cc#L1582) | 创建 hidden 节点，页面从源迁移到 hidden |
| `VmCowPages::ForkMarkerLocked` | [vm_cow_pages.cc:2755](zircon/kernel/vm/vm_cow_pages.cc#L2755) | Hidden 节点中 Marker 的 COW 分叉 |
| `LookupCursor::CursorReferenceToPage` | [vm_cow_pages.cc:3558](zircon/kernel/vm/vm_cow_pages.cc#L3558) | 游标层解压封装 |
| `VmPageOrMarker::SwapReferenceForPage` | [vm_page_list.h:127](zircon/kernel/vm/include/vm/vm_page_list.h#L127) | 原子替换：槽位 Reference → Page |
| `VmPageOrMarker::SwapPageForReference` | [vm_page_list.h:137](zircon/kernel/vm/include/vm/vm_page_list.h#L137) | 原子替换：槽位 Page → Reference |
| `VmCompression::Decompress` | [compression.cc:139](zircon/kernel/vm/compression.cc#L139) | 压缩子系统解压入口 |
| `paddr_to_physmap` | [physmap.h](zircon/kernel/vm/include/vm/physmap.h) | 物理地址 → 内核虚拟地址映射 |
