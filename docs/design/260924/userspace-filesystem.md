# ours 用户空间文件系统：设计（2026-09-24，2026-09-25 修订 D1–D5）

> 输入：protocol、generator、boot、permissions、kernel 五个方向的设计，加一致性、安全、闭环三份评审。主人已定四条：fuchsia.io 子集；/boot 与 /svc 先内置在 init；协议代码用生成器；v1 纯能力、形状为 DAC 预留。评审的每一条要么落进正文，要么在第 9 节写明否决理由。代码引用在 HEAD（70b7a4da）上复核过，Fuchsia 引用在本地 checkout 上复核过，没核到的标“未核实”。本文建立在 `docs/design/260923/userspace-interface.md`（下称“接口设计”）之上。它 §1 的“现状”一栏已经过时：HandleTable 和 Channel 已实现。
>
> **2026-09-25 修订**（四次，按先后）：
> 1. D3 改为“可执行是一项能力”：`vmo_create` 永不带 Execute，Execute 只能经 `vmo_replace_as_executable(vmo, vmex)` 取得，vmex 是内核只交给第一个进程的资源句柄（Zircon 的做法）。初稿按 VMO 的来源（内核铸造的 BootFS VMO）授予 Execute，那是特例而不是能力，已否决（§9）。
> 2. D4 定为直接上 Port：init 的事件循环建在 port + object_wait_async 上（设计见 §7.4），object_wait_many 推迟到 P2；D6–D10 按推荐定下。新增的代码引用在 HEAD（93b05422）上复核过。
> 3. 主人要求依赖先行、文件系统不再建在自己依赖的替身上：D1、D2 定为加载器、userboot 和删除 spawn_boot 排在文件系统之前；D5 定为哈希 ordinal 加 selector；§7 改为四个阶段共 45 个提交。新增的代码引用在 HEAD（353c875f）上复核过。
> 4. 步骤按阶段顺序统一重新编号：K、G、U 各自从 1 起连续编号，编号的先后就是做的先后。此前各版的编号作废。

## 1 总览

**文件句柄与协议。** 每次 Open 都产生一条新 channel，即一条连接。协议取 fuchsia.io 的子集：Open、Close、GetAttributes、ReadDirents、Rewind、Read/ReadAt/Seek，外加事件 OnRepresentation。GetBackingMemory 在 M1 实现 Read，给出文件的只读 Slice；Execute 到 M2。

线格式沿用 FIDL 的消息头形状，但消息体只有定长部分加至多一段尾部，不做 out-of-line 编码。消息类型、编解码、客户端桩和服务端分派都由生成器从 GN scope 描述生成，与系统调用生成器共用前端。

**启动与托管。** 链路是 kernel → userboot → init（D2）。内核给 userboot 发一条 processargs，里面带根 job、只读的 BootFS VMO 和 vmex；userboot 用用户态 launcher 从 BootFS 起 init，把这三样转交后退出。init 在进程内托管 /boot 和 /svc，读 `/boot/config/services`，为每个服务准备一对 outgoing channel 和一组 NsDir，经 launcher 从 /boot 启动它，并在进程句柄上观察它退出。

**服务访问。** 进程能访问的目录，只有 processargs 交给它的那些 NsDir。init 为每个子进程铸一条专属的 /svc 连接，连接的节点是一个合成目录，子节点就是清单允许它使用的协议。客户端 Open(name) 时，init 把它的 channel 原样转交给提供者的 outgoing 目录，此后两端直接通信，init 不留任何状态。

**权限。** v1 是纯能力模型：
- Open 请求的 rights 必须同时是连接 rights 的子集和节点能力的子集，只缩不扩；
- 没有环境根，服务端永远看不到绝对路径；
- 连接记录现在就带 principal（v1 恒为 0）和 grant，rights 拆成 cap_rights 与 effective 两个字段，为 DAC 留好形状；
- 永不外发可写 VMO；
- 可执行是一项能力：只有持 vmex 资源的进程能把 VMO 换成可执行，换的同时去掉 Write（D3）。

**第一个里程碑 M1**：两个镜像都能启动，dev 镜像的日志依次出现下面几行。

```
userboot: started bin/init
init: bootfs has <n> entries; serving /boot
init: launched echo from /boot/bin/echo
init: launched shell from /boot/bin/shell
shell: /boot/config/services: 2 services
shell: ls /boot: bin config userboot vdso
shell: open /boot/vdso with WriteBytes = AccessDenied
shell: raw Open("..") on /boot = BadPath
shell: /boot/vdso backing memory: read-only, starts \x7fELF
shell: /svc/ours.echo.Echo says "hello"
shell: /svc/ours.nothing = NotFound
init: shell exited with 0
```

- 内核日志还要在 `userboot: started bin/init` 之后出现 `object: process 'userboot' … destroyed`，在最后一行附近出现 shell 的 destroyed。
- echo 一直在运行。
- dbg_1 镜像要出现 `userboot: started bin/shell`；12 行 `shell: probe <p> killed (retcode <r>)`，每个探针各带一行 `mm: aspace 'fault_probe' destroyed`；最后是 `shell: probes done (12 killed, 0 survived)`。

到 M1 共 45 个提交，分四个阶段（§7）：进程生命周期 7 个，对象、内存与加载 29 个，多路复用 3 个，文件系统 6 个。依赖先行（D1、D2）：文件系统开工之前，加载器和 userboot 已经落地，spawn_boot 已经删除。以下内容不在 M1 里，推到 M2 或 P2：GetBackingMemory 的 Execute、restart 和 lazy 启动、handle_replace、vma 的其余调用（unmap、protect、destroy，以及自动放置或带 CanMap 位的 allocate）、job_create、port_cancel、object_wait_many、撤销。

## 2 文件协议

### 2.1 消息格式

```cpp
// <ours/wire.hpp>; system/lib/wire is the only encoder/decoder of this header.
struct MessageHeader {           // 16 bytes, the shape of fidl_message_header_t
    u32 txid;                    // 0 for one_way/event/epitaph; two_way: 1..0x7FFF'FFFF
    MessageDirection direction;  // u8: ToServer 0, ToClient 1 (replies, events, epitaphs)
    u8 reserved[2];              // must be zero
    u8 magic;                    // WIRE_MAGIC = 1
    u64 ordinal;
};
```

- **头部形状**同 `fidl_message_header_t`（`zircon/system/public/zircon/fidl.h:430-438`）。txid 放在前 4 字节，与接口设计 §2.4 的约定一致。
- **direction 字段是安全上必需的。** 去掉它，线上就分不清请求和回复。以 Seek 为例：请求体和回复体都是 16 字节，`Status::Ok` 和 `SeekOrigin::Start` 又都等于 0，于是一条 Seek 回复本身就是一条合法的 Seek 请求。客户端只要把同一条 channel 的两端都交给 init，就能让 init 在单线程循环里自己跟自己无限对话。因此服务端解码器拒绝 ToClient 的消息，客户端解码器拒绝 ToServer 的消息。
- **消息体**由定长部分加至多一段尾部数组组成，尾部元素只能是 u8 或 char，长度由它前面的一个 u32 字段给出。不允许隐式填充，需要填充时显式写 reserved 字段。
- **句柄**走 channel 的句柄数组。体里每个句柄字段占一个 u32，值为 `HANDLE_PRESENT = 0xFFFF_FFFF`（同 `fidl.h:326`），句柄数组按字段顺序与之对应。请求里的句柄都是必选的；回复里的句柄只在 status 为 Ok 时出现。
- **回复**的 txid 和 ordinal 都与请求相同。
  - Ok 时，第一个字段是 u32 status，其后的字段按无隐式填充规则排列。
  - 非 Ok 时，整条回复固定为 24 字节 `{header, status, u32 0}`，不带句柄。
- **epitaph** 的 ordinal 全为 1（`fidl.h:494`），体是 `{Status status; u32 0}`，发完立即关闭 channel。Open 没有回复，失败只能以 epitaph 的形式报告在新 channel 上。
- **尺寸上限**：
  - `MAX_NAME = 255`，同 fuchsia.io（`sdk/fidl/fuchsia.io/io.fidl:25`）。
  - `MAX_PATH = 1024`。fuchsia.io 是 4095（`io.fidl:50`），装不进 4 KiB 的消息。
  - `MAX_TRANSFER = CHANNEL_MAX_MSG_BYTES − 24 = 4072`。fuchsia.io 是 8192（`file.fidl:12`）。生成的头文件用 static_assert 从 `ours::CHANNEL_MAX_MSG_BYTES`（`system/public/ours/syscall.hpp:41`）推出这个值。
  - 消息上限升到 64 KiB 后再放大这几个值，放大属于兼容变更。

### 2.2 首批消息

体内偏移从 16 起算。

| 方法 | 种类 | 请求体 | Ok 回复 | 需要的 IoRights | 阶段 |
|---|---|---|---|---|---|
| Directory.Open | one_way | `IoRights rights@16; OpenOptions options@24; u32 object@28`（server_end）`; u32 path_size@32; char path[]@36`，最长 1060 字节 | 无；带 Describe 时先发 OnRepresentation | 见 §2.4 | M1 |
| Node.OnRepresentation | event | — | `NodeKind kind@16; u32 0; IoRights rights@24`（实际授予的） | — | M1 |
| Node.Close | two_way | — | status，随后服务端关闭连接 | 无 | M1 |
| Node.GetAttributes | two_way | — | `u32 0; NodeAttributes attributes@24` | GetAttributes | M1 |
| Directory.ReadDirents | two_way | `u64 max_bytes` | `u32 size; u8 dirents[]@24` | Enumerate | M1 |
| Directory.Rewind | two_way | — | status | Enumerate | M1 |
| File.Read | two_way | `u64 count` | `u32 actual; u8 data[]@24` | ReadBytes | M1 |
| File.ReadAt | two_way | `u64 offset; u64 count` | 同 Read | ReadBytes | M1 |
| File.Seek | two_way | `SeekOrigin origin(u32); u32 0; i64 offset` | `u32 0; u64 offset@24` | 无 | M1 |
| File.GetBackingMemory | two_way | `BackingMemoryOptions options(u32); u32 0` | `u32 vmo@20`（句柄） | Read 需 ReadBytes，Execute 另需 Execute | Read 在 M1，Execute 在 M2 |
| Write、WriteAt、Resize、UpdateAttributes、Sync、Unlink、Rename、Watch、Reauthenticate | — | — | — | — | M2 起 |

- **`OpenOptions : u32`**
  - M1 定义 ProtocolDirectory、ProtocolFile、ProtocolService、Describe 四位。
  - 描述里另声明保留位 MaybeCreate、MustCreate、Truncate、Append、NewGrant（§6.3）。它们在 M1 回 NotSupported。
  - 三个 Protocol 位都不置时，接受节点本来的协议；置了若干位时，节点的种类必须在其中，否则回 NotDir 或 NotFile（`system/public/ours/status.hpp:82-83`）。
- **`NodeKind : u32`**：Directory 1、File 2、Service 3。
- **dirent** 沿用 Fuchsia 的紧凑布局 `{u64 ino; u8 name_size; u8 type; char name[]}`，不填充（`directory.fidl:459-469`）。type 取 DirentType 的值：目录 4、文件 8、服务 16（`directory.fidl:297-311`）。服务端不产生 "." 和 ".."。
- **`NodeAttributes`**：用有效位掩码代替 FIDL 的 table。

```cpp
struct NodeAttributes {  // 80 bytes; offsets pinned with static_assert
    u64 valid;           // which of the fields below mean anything
    u64 id, content_size, storage_size, link_count;
    u64 creation_time, modification_time;  // UTC ns, 0 = unknown
    u32 mode, uid, gid;  // POSIX meaning, reported for real from M1 (§6.2)
    u32 protocols;       // NodeKind bits
    u64 abilities;       // IoRights this node itself supports
};
```

- **边界规则**：
  - Seek 的结果必须落在 [0, INT64_MAX] 内，否则回 InvArgs，不回绕；可以越过 content_size。
  - Read/ReadAt 在 offset ≥ content_size 时返回 0 字节。0 字节是 EOF 的唯一表示。
  - 实际读取量 = min(count, content_size − offset, MAX_TRANSFER)。count 超过 MAX_TRANSFER 不算错，服务端短读即可。
  - BootServer 读的是 `entry.data_offset + offset`，先检查这个加法不溢出、结果不越过条目末尾，再去读。

### 2.3 路径规则（服务端）

- **语法**：照现行 fuchsia.io 的 Path 不变式（`io.fidl:38-42`）。
  - 路径非空，开头和结尾都不能是 '/'；
  - 要么整条路径恰好是 "."，要么每一段都是合法的 Name：非空、不超过 255 字节、不含 NUL 和 '/'、不是 "." 也不是 ".."。
  - 违反任何一条都回 BadPath（`openable.fidl:16` 同样规定回 ZX_ERR_BAD_PATH）。
- **开头的 '/'**：拒绝它不算偏离。旧的 Open1 文档允许开头的 '/'（`directory.fidl:379-380`），现行的不变式已经禁止（`io.fidl:40`）。命名空间解析是客户端的事，服务端见到绝对路径就说明客户端有 bug。
- **"."** 即 clone：得到同一节点的一条新连接，同样要过 §2.4 的检查。
- **编码**：不要求 UTF-8，路径是字节串（D9）。
- **解析方式**：从连接所在的节点出发，逐段在节点树上查找；不拼接字符串，也不用父指针。子树约束因此由结构本身保证。

### 2.4 rights

- **类型**：`enum class IoRights : u64`，位值照 fuchsia.io 的 Operations（`rights-abilities.fidl:9-44`）。
  - 各位：Connect 0x1、ReadBytes 0x2、WriteBytes 0x4、Execute 0x8、GetAttributes 0x10、UpdateAttributes 0x20、Enumerate 0x40、Traverse 0x80、ModifyDirectory 0x100。
  - 写类的位现在就定下编号，v1 不授予。
  - IoRights 与句柄上的 `ours::Rights` 无关。
- **常量由 ours 自己定义，不含 Connect。** fuchsia.io 的 R_STAR_DIR 和 RX_STAR_DIR 都含 CONNECT（`rights-abilities.fidl:47-56`），照搬的话，下面第 5 条节点能力检查会让所有 /boot 连接都得到 AccessDenied。
  - `R_STAR_DIR = Enumerate | Traverse | ReadBytes | GetAttributes`
  - `RX_STAR_DIR = R_STAR_DIR | Execute`
  - `SVC_DIR_RIGHTS = Connect | Traverse | Enumerate`
- **Open 按以下顺序检查**，照 Rust VFS 的 `handle_open`（`src/storage/lib/vfs/rust/src/directory/connection.rs:393-410`）和 `new_connection_validate_options`（`src/storage/lib/vfs/rust/src/file/common.rs:25-33`）：
  1. 路径语法不合法，回 BadPath。
  2. 路径不是 "."，而连接没有 Traverse，回 AccessDenied。
  3. `rights & ~conn.cap_rights` 非零，回 AccessDenied；不会悄悄把请求缩小。
  4. 带 ProtocolService，而连接没有 Connect，回 AccessDenied。
  5. 解析出节点；`rights` 超出节点的能力（abilities），回 AccessDenied。
  6. 新连接的 cap_rights = effective = rights；principal 和 grant 照抄父连接（§6）。
- **v1 没有 INHERIT 位。** 客户端从 OnRepresentation 得知自己实际拿到了哪些 rights，自己求交集后再发请求。
- **节点能力**：
  - /boot 目录：R_STAR_DIR | Execute。目录上的 Execute 只作为子节点的上限。
  - /boot 文件：ReadBytes | GetAttributes。M2 起，带 Executable 标志的条目再加 Execute。
  - /svc 合成目录：SVC_DIR_RIGHTS；其下的服务节点：Connect。

### 2.5 错误与严格性

| 情况 | 处理 |
|---|---|
| 畸形消息：长度、magic、reserved、direction、txid 规则不对，尾长 > max，enum 越界，含未知位，句柄数与 PRESENT 槽数不符 | 发 epitaph InvArgs，关闭连接 |
| 未知 ordinal | 发 epitaph NotSupported，关闭连接 |
| 描述里声明为保留、但还没实现的位或方法 | handler 正常回复 NotSupported |
| 路径语法错误 | BadPath，经 Open 的 epitaph 报告 |
| 业务错误 | 放进回复的 status；Open 的业务错误写成 epitaph |
| 回复时遇到 NoResource（客户端不读） | 关掉这条连接；init 从不为任何一个客户端阻塞 |
| 客户端收到畸形回复 | 关闭 channel，返回 BadState |

- **句柄类型 v1 查不到**，因为还没有 channel_read_etc，解码器只核对句柄个数。
- 类型不对的句柄，第一次使用时会得到 BadType，服务端随即关掉连接，不 assert。
- read_etc 落地后再补类型和 rights 检查，做法照 `FidlEnsureHandleRights`（`sdk/lib/fidl_base/internal.c:61`）。

### 2.6 连接模型

```cpp
struct Connection {          // the one definition; BootServer and later filesystems share it
    ou::Channel channel;
    NodeRef node;            // pins the node, not the name
    IoRights cap_rights;     // only for Open's subset check and for deriving
    IoRights effective;      // only for operations on this node; == cap_rights in v1
    OpenOptions options;
    u64 cursor;              // byte offset for a file, dirent cursor for a directory
    PrincipalId principal;   // u64, 0 = no principal (pure capability)
    GrantId grant;           // revocation and quotas key on this
};
```

- **dup 出来的 fd** 共享同一条连接，因而也共享偏移，符合 POSIX 语义。
- **clone 是共享连接的唯一途径。** Channel 的默认 rights 不含 Duplicate（`kernel/object/include/ours/object/channel_dispatcher.hpp:16-17`），所以要共享只能 clone，服务端因此能看见每一个持有者。
- **节点**由 `NodeKind` 加 variant 组合而成，不引入基类。
- **分派**由生成的 `serve_<protocol><Handler>` 静态绑定，不用虚函数。

## 3 协议描述与生成器

### 3.1 描述

```gn
# //system/public/ours/io/file.gni
ours_io_file = {
  name = "File"
  compose = [ "Node" ]
  methods = [
    {
      name = "ReadAt"
      kind = "two_way"           # one_way | two_way | event
      brief = "Reads up to |count| bytes at |offset|; the seek offset does not move."
      request = [
        { name = "offset"  type = "u64" },
        { name = "count"  type = "u64" },
      ]
      response = [               # an implicit leading Status status
        { name = "actual"  type = "u32" },
        { name = "data"  type = "u8"  count = "actual"  max = "MAX_TRANSFER" },
      ]
    },
  ]
}
```

- **组织**：每个库有一个汇总 scope（`library = "ours.io"`），里面列 consts、bits、enums、structs、protocols。
- **整数写法**：GN 只认十进制整数，用 `prebuilt/gn/gn` 试过，`0x10` 会报 "This is not a valid number"。所以位写成 `bit = n`。
- **ordinal**（D5 已定）：
  - 取 SHA-256("ours.io/File.ReadAt") 的前 8 字节按小端解释，再与 0x7FFF_FFFF_FFFF_FFFF 相与，同 fidlc 的 `Sha256MethodHasher`（`tools/fidl/fidlc/src/compiler.cc:28-45`）。
  - 最高位恒为 0，所以不会与 epitaph 的全 1 相撞。
  - 方法改名或挪进被 compose 的协议时，写 `selector = "旧名"` 把编号钉住，相当于 fidlc 的 @selector（例：`openable.fidl:18`）。
- **句柄字段**写成 `type = "handle"  object = "Channel"  rights = [ ... ]`。rights 是所需的最低权限；在 read_etc 落地之前只记录，不校验。

### 3.2 前端复用与目录

- **目录改名**：`build/syscall` 改为 `build/abi`，作为一次单独的纯移动提交。
- **抽出 `description.py`**，内容包括：
  - DescriptionError、load_json、check_keys；
  - IDENTIFIER、TYPE_NAME、PRIMITIVES；
  - LINE_WIDTH 与声明折行；
  - `banner(generator, source)`：今天的 banner 写死在 `build/syscall/gen_syscalls.py:59-62`；
  - write_if_changed、run_main。
- **不共用**：Arg/Call 的寄存器模型，以及五个寄存器 ABI 渲染器。
- **新写 `gen_protocols.py`**：
  - 类型模型：Library、Const、Bits、Enum、Struct、Field、Protocol、Method；
  - 布局：偏移、对齐、隐式填充检测；
  - compose 展平，检查 ordinal 是否撞号；
  - 尺寸检查：16 + 定长部分 + max ≤ CHANNEL_MAX_MSG_BYTES，句柄数 ≤ CHANNEL_MAX_MSG_HANDLES，两个上限由 GN 传入；
  - 各个渲染器。
- **别漏 inputs**：两个 action 都要加 `inputs += [ "//build/abi/description.py" ]`，否则改了共享模块不会触发重新生成。
- **syscall 生成器只需为 K10 放宽一处**：`release` 放宽到 HandleValue 数组。今天 `gen_syscalls.py:97-98` 只收按值传递的句柄；release 只是供校验的标注，渲染器不读它，也只有 K10 的 channel_write 用它，所以并进 K10，不单独成步。
  - 不再需要 `dir = "inout"`（`gen_syscalls.py:41` 只认 in/out）：它原本给 wait_many 的 items 用，wait_many 已推迟到 P2（§7.4）。
  - 结构体指针参数已经支持（`gen_syscalls.py:247-260`），port 的 PortPacket 直接可用。
  - 7 参数的桩（`:35-38`）在阶段二的 G2 加，给 vma_map 用。

### 3.3 产物与运行库

GN 模板叫 `protocol_library("io")`，放在 `//build/abi/protocol_library.gni`。与 syscalls 一样，只在默认工具链里生成一次。

| 产物 | 内容 |
|---|---|
| `<ours/io/io.hpp>` | 只有类型：常量；bits（`USTL_ENABLE_ENUM_BITMASK`，同 `system/public/ours/rights.hpp:61`）；enum、struct；每个方法的 Request/Response，尾部按 max 定长；ordinal 常量；逐字段的 offsetof/sizeof static_assert |
| `<ours/io/io_client.hpp>` | 自由函数。one_way：编码后 channel_write。two_way：走 `wire::call()`，今天的实现是 write + wait_one + read，校验 txid、ordinal、direction；遇到 PEER_CLOSED 时读出残留的 epitaph 作为 status。event：`wait_<event>()` |
| `<ours/io/io_server.hpp>` | `template <class Handler> auto serve_file(Handler &, typename Handler::Connection &, wire::IncomingMessage &) -> wire::DispatchResult`。按 ordinal switch（含 compose 进来的方法），完整校验后调用 `handler.read_at(conn, offset, count, response)`。Handler 缺方法或签名不符时编译失败 |

- **回复缓冲不能留残数据。** 骨架在调用 handler 之前把回复缓冲清零；尾部的长度由 handler 返回实际写入的字节数，骨架只发 `offsetof(尾部) + 实际长度`；status 不是 Ok 时，只发 24 字节的错误形态。否则单线程的 init 反复使用同一块缓冲，会把上一个客户端读到的数据漏给下一个客户端。
- **手写运行库 `system/lib/wire`**（include/src/test），提供：
  - MessageHeader 的编解码；
  - `IncomingMessage`：4096 字节加 64 个句柄，析构时关掉没被取走的句柄；
  - `call`、`reply`、`send_epitaph`；
  - `ClientEnd<P>` 与 `ServerEnd<P>`。
- **可见性**：`:io.client` 和 `:io.server` 断言 `toolchain_environment == "user"`，visibility 不包含 `//kernel/*`。内核不使用这些协议。processargs 不是请求/回复式的协议，仍按接口设计 §3.3 手写。
- **测试**：
  - `build/abi/test/gen_protocols_test.py`：喂畸形的描述。
  - `system/lib/wire/test`（宿主端）：用畸形消息测试生成的 io 解码器，覆盖截断、超长、reserved 非零、未知位、句柄数不符、direction 错、count 大于 max。
  - init 开始服务 /svc 之前，这组测试必须通过。
- **ABI 稳定性**：M1 不检入 golden，因为所有使用者都出自同一次构建。等第一次出现跨构建互通（持久的 /data，或树外程序）时，再照 Fuchsia 的做法（检入的 `sdk/fidl/fuchsia.io/fuchsia.io.api`）加检入摘要和比对。

## 4 启动链与 init 内置的 /boot、/svc

### 4.1 M1 时序

| # | 谁 | 做什么 | 之后谁持有什么 |
|---|---|---|---|
| K | 内核 | 解压内核包，建 BootFS VmoDispatcher：整体提交，rights = Basic\|Read\|Map\|GetProperty；铸 vmex 槽位。建 bootstrap channel，写一条 processargs：JobDefault（根 job）、VmoBootfs、SystemResource（vmex），environ 带命令行里的 `userboot.*`。用内核 elfload 装载 userboot 并启动，rdi 为 bootstrap 端 | userboot：根 job、BootFS VMO、vmex |
| U | userboot | runtime 解析 processargs。复制 BootFS，用 vmex 把其中一份换成 bootfs_x；用 launcher 从 bootfs_x 起 `userboot.next`（默认 `bin/init`），转交根 job、不可执行的那份 BootFS 和 vmex；关掉 bootfs_x，process_exit(0) | init：根 job、BootFS VMO、vmex |
| I1 | init | runtime 解析 processargs。用 vmo_read 把 Header、Entry[] 和名字表读进 bump 缓冲，建 `bootfs::Tree`。VMO 里本来就只有白名单放行的条目（K12），不需要再隐藏什么。在进程内直接读 `config/services`。复制 BootFS，再用 vmex 换出可执行的一份 bootfs_x 给 launcher 用；不可执行的那份留给 GetBackingMemory 的 Slice | init：Tree、清单、两份 BootFS |
| I2 | init | 按清单顺序处理每个服务：建 (o_c, o_s)；为每个 uses 铸一条根连接，各带一个新 grant；调 launch（§4.6），从 bootfs_x 装 binary，processargs 带 NsDir、DirectoryRequest = o_s 和 VmaRoot，bootstrap channel 由 launcher 自己建；保留返回的进程句柄，以 TERMINATED 挂进 Loop | 子进程：NsDir、o_s。init：o_c、进程句柄、各连接的服务端 |
| S1 | shell | fsio 按最长前缀选中 /boot，写 `Open("config/services", ProtocolFile\|Describe, R_STAR_DIR 的子集, s)`，等 OnRepresentation | shell：c。init：s 的连接记录 |
| S2 | shell | `fsio::connect("/svc/ours.echo.Echo")`，过程见 §4.4 | c 在 shell，s 在 echo，init 不留状态 |
| S3 | shell | 在 `vdso` 的连接上调 GetBackingMemory(Read)，得到只读 Slice，vma_map(R) 进自己的根 VMA，读出 ELF 头（U12） | shell：Slice。init：这个 grant 的 backing 计数加一 |
| S4 | shell | main 返回，runtime 调 process_exit(0)。内核关掉它的句柄表，拆掉它的地址空间，置 TERMINATED | init：在进程句柄上看到 TERMINATED，用 get_info 取退出码；在它的各条连接上看到 PEER_CLOSED，回收记录 |

- **清单的 binary 字段由生成器填写**，取 `user_executable` 的实际安装名（`system/user_executable.gni:41` 的 install_name）。清单在 U13 才有，那时 U9 已经把程序挪进 `bin/`，所以它从一开始就是 `bin/echo`、`bin/shell`。
- **内核私有文件**：
  - dev 镜像的包里有 `vmours/image.elf`（`kernel/products/image/dev/BUILD.gn:21-22`），而内核带 KASLR（`kernel/phys/physboot.cpp:144-148`），所以内核映像不能暴露给普通进程。
  - **黑白名单，在内核里过滤**（2026-09-25 定，见 `docs/repairs/260925/userabi.md` 第 1 条）：只在 init 里隐藏是不够的——持有原始 BootFS VMO 的 userboot 和 init 仍能按偏移读出整个内核 ELF。所以 K12 建 VMO 时就只拷**白名单**里的条目：名单由构建从 GN metadata 生成（标在产出文件的 target 上，如 `user_executable()`；机制见 `build/dist/distribution_manifest.gni`），随内核包打入，默认不放行。**黑名单**兜底：内核自己的目录（phys 的启动选项 `kernel.name`，`kernel/phys/include/ours/phys/boot_options.hpp:66`；vmours 的 `BootOptions` 要补上这一项）与补丁表，即使误入白名单也拒绝，并在启动时报错。
  - 不用"跳过内核目录"这一条特判：内核包以后会装什么说不准，写死今天已知的那一项，新加的私有文件就会悄悄漏出去。
  - 长期把用户 BootFS 拆成独立的 OBI 条目（D7）。Fuchsia 的内核包和用户 bootfs 就是两类条目：STORAGE_KERNEL 与 STORAGE_BOOTFS（`zircon/kernel/phys/kernel-package.cc:65`）。

### 4.2 init 的组成

```cpp
class Init {              // composition, no base class
    loop::Loop loop;      // one port fed by object_wait_async (system/lib/loop)
    BootServer boot;      // /boot
    SvcRouter svc;        // /svc
    Supervisor services;  // manifest; launch and observe exit; no restart
};
```

- **Loop**（`system/lib/loop`，U10 新建，目录包含 include/、src/、test/、.gitignore、.clang-format）
  - 做成库，是因为 echo 也要同时等多个句柄：U13 的 `svc::Outgoing` 把 o_s 和每条接入的客户端 channel 都 add 到同一个 Loop 上（§4.4）。M1 没有 object_wait_many，不共用 Loop 就只能各写一套 port 循环。
  - 接口：`add(handle, signals, fn, ctx) -> Key`、`run()`。`fn(ctx, Signals observed) -> LoopAction`，返回 Rearm 或 Close。回调是函数指针加上下文，不用虚函数。
  - 内部是一个 port 加一张定长槽表，槽表由使用者提供。槽为 `{handle, signals, fn, ctx, generation}`，`key = generation << 32 | index`。
  - run：`port_wait(INFINITE)` 取一个包，按 index 找槽；generation 对不上或槽已空就丢弃，否则调 fn。
  - Rearm 由 Loop 发 wait_async，handler 从不自己挂等待。遇到 Close 或 wait_async 失败时：handle_close，generation 加一，归还槽位。
  - **只有槽自己的回调能关它，M1 不提供 remove(key)。** 回调运行时，这个槽的 observer 已经触发、包也已取走，port 上没有它的任何东西。所以关掉它既不需要 port_cancel，也不会留下过期包，generation 检查只作防御。要从别处关一个在挂的槽（撤销 grant），得等 port_cancel（§7.8）。
  - **每处理一条消息就重挂一次。** observer 只能是一次性的；电平触发让积压的消息立即再出包，并排到队尾，所以灌消息的客户端只能按轮次得到服务。
- **预算**
  - `LOOP_SLOTS = CONTROL_SLOTS + CONNECTION_POOL = 8 + 248 = 256`：CONTROL_SLOTS 给各服务，每个服务两个槽，一个等 o_c，一个在进程句柄上等 TERMINATED，所以 `CONTROL_SLOTS = 2 * SERVICES_MAX`，SERVICES_MAX = 4；CONNECTION_POOL 给连接。槽表由 bump 分配器一次分好，不再有 wait_many 的 64 项上限。
  - `static_assert(LOOP_SLOTS <= PORT_MAX_OBSERVERS)`：每个槽至多一个活跃 observer，要么在挂，要么它的包在队里。两者不会同时存在，因为包被取走之后才会重挂。万一遇到 NoResource，只关那一条连接。
  - **句柄配额按 K11 的记法算。** K11 把句柄记在创建它的进程名下（接口设计 §4.8），所以要核对的是 init 自己创建的句柄：port 1 个；BootFS 的可执行副本 1 个（init 自己复制再换，§4.6）；每个服务 5 个，即 o_c、o_s、b_s、进程句柄和子进程的 VmaRoot（后三个由 init 里的 launcher 建出；o_s、b_s、VmaRoot 住在子进程里，仍记在 init 名下）；每条根连接的两端 2 个；GetBackingMemory 发出的 Slice，每个 grant 终身至多 `GRANT_MAX_BACKING_VMOS` 个（init 看不到客户端何时关掉它们，只能按终身计，U12）；再加正在写的 b_c、U10 自测连接、launcher 的线程句柄和段 VMO 这类临时句柄 `INIT_TRANSIENT_HANDLES`。于是 `ROOT_CONNECTIONS_MAX = CONNECTION_POOL / GRANT_MAX_CONNECTIONS`，`static_assert(2 + 5 * SERVICES_MAX + ROOT_CONNECTIONS_MAX * (2 + GRANT_MAX_BACKING_VMOS) + INIT_TRANSIENT_HANDLES <= HANDLE_MAX_PER_PROCESS)`。客户端经 Open 交来的 server end 记在客户端名下，由连接池和每 grant 配额限住；userboot 转来的根 job、BootFS 和 vmex 也不由 init 创建。这两类都不在式内。
  - 每个 grant 最多 `GRANT_MAX_CONNECTIONS = 16` 条连接。清单生成器检查根连接数 × 16 ≤ CONNECTION_POOL、服务数 ≤ SERVICES_MAX，否则拒绝生成。
  - 池满或配额用尽时，在新的 server end 上发 epitaph NoResource，然后关闭。
- **o_c**
  - init 把 o_c add 进 Loop，等 READABLE | PEER_CLOSED。
  - 提供者不应往 o_c 写任何东西。一旦收到入站消息，回调就返回 Close 关掉 o_c，防止提供者往里停 64 条消息、每条带满句柄。
  - PEER_CLOSED 表示提供者已经死了。Supervisor 同时在它的进程句柄上等 TERMINATED（U13），两个信号先到哪个都一样处理。
  - 两种情况下，回调都先在服务记录上标记该提供者不可用，再返回 Close；§4.4 第 4 步据此回 ConnectionRefused。
- **BootServer**
  - 连接池是定长的，因为 runtime 只有 bump 分配器。
  - Read/ReadAt 直接 vmo_read 进已清零的回复缓冲。
  - NoResource 只算单个请求失败，从不 assert。
- **只写不等**：init 对子进程只写消息、从不等回复。否则一个卡住的服务就能把 init 卡死。
- **子进程退出**：内核关掉死进程的整张句柄表（K4）、拆掉它的地址空间（K6），然后置 TERMINATED（K7）。init 在它的每条连接上看到 PEER_CLOSED，据此回收记录；在进程句柄上看到 TERMINATED，用 get_info 取退出码记进日志，不重启（§4.3）。退出不触发按 grant 撤销（§6.3）。

### 4.3 服务清单

`/boot/config/services` 是文本，一行一个服务，由 `//build/service/gen_service_manifest.py` 从 GN metadata 生成：

```
# generated by //build/service/gen_service_manifest.py; do not edit
service echo  binary=bin/echo  start=eager restart=never provides=ours.echo.Echo uses=/boot:r
service shell binary=bin/shell start=eager restart=never uses=/boot:rx,/svc/ours.echo.Echo
```

- **怎么生成**：`system_service("echo") { binary = ":echo_bin"  provides = [...]  uses = [...] }` 设置 metadata；镜像里的 `service_manifest("services")` 用 generated_file 收集，再交给 Python 生成。
- **uses 词表**，映射在生成器里写死：
  - `/boot:r` → R_STAR_DIR；
  - `/boot:rx` → RX_STAR_DIR；
  - `/svc/<proto>` → 把该协议加进这个服务的 /svc 合成目录，连接 rights 为 SVC_DIR_RIGHTS。
- **生成器的校验**：
  - 服务名唯一；
  - 每个被 uses 的协议恰好有一个提供者；
  - binary 在同一个包里；
  - 词表合法；
  - 不存在的目录（例如 /data）直接拒绝；
  - 服务数不超过 SERVICES_MAX（§4.2）；
  - **拒绝 start=lazy 和 restart≠never。** 理由：M1 的清单没有使用者。它们的前提 TERMINATED 和 VmAspace::destroy 已在阶段一落地（K6、K7），Supervisor 也已能观察退出，缺的只是重启策略本身（§7.8）。

### 4.4 /svc 转发

1. **shell 发起连接。** 调 `channel_create(c, s)`，在自己的 /svc 连接上写 `Open("ours.echo.Echo", ProtocolService, Connect, s)`。之后可以立刻在 c 上发请求，它们会在通道里排队。
2. **init 查找。** 在这条连接的合成目录里做普通查找。协议不在允许表里，就回普通的 NotFound epitaph。ReadDirents 同样只列出允许的协议。
3. **init 转发。** init 自己拼出 `svc/ours.echo.Echo`，带固定的 ProtocolService 和 Connect，写进提供者的 o_c；s 随这条消息转走。客户端给的 path 和 options 不透传。这与 component_manager 的 `OutgoingConnector::send` 相同：它对 SubNode 只打开 "."，也只带 PROTOCOL_SERVICE（`src/sys/component_manager/src/model/component/instance.rs:801-820`）。
4. **提供者不可用。**
   - init 已知提供者不可用（o_c 已经 PEER_CLOSED，或者因为收到入站消息被 init 关掉了，§4.2）：在 s 上发 epitaph ConnectionRefused（`status.hpp:87`）。
   - 转发时写 o_c 失败：s 已经随那条失败的消息被关掉了，因为句柄一进 channel_write 就被消耗（接口设计 §2.4）。客户端只会看到一个没有 epitaph 的 PEER_CLOSED。
   - fsio 在 /svc 连接上把这两种情况都报告为 ConnectionRefused，具体原因写进 init 的日志。
5. **提供者一侧。** `system/lib/svc` 的 `svc::Outgoing` 取出 DirectoryRequest 句柄，只接受 `Open("svc/<name>", ProtocolService, ...)`，然后按名字调用注册好的 `on_connect(ou::Channel)`。对照 `component::OutgoingDirectory::ServeFromStartupInfo`（`sdk/lib/component/outgoing/cpp/outgoing_directory.h:127`）。
   - Outgoing 跑在 `system/lib/loop` 的 Loop 上（§4.2）：o_s 和 on_connect 收下的每条 channel 都 add 到同一个 Loop，所以 echo 能同时服务多个客户端，on_connect 也不会阻塞 o_s。

### 4.5 BootFS 库

> **2026-09-26 修订（主人）**：文件系统走 Linux 那种多文件系统结构——一个公共的 VFS（"宏文件系统"），每个具体文件系统只把自己和它的
> 文件操作集适配上去，就像 Linux 的文件系统向 VFS 提供 super/inode/file 的操作集。所以**不再给 bootfs 单独建 `Tree`**，也不让 init
> 拿 bootfs 自己的目录树去服务 `/boot`。bootfs 库保持格式本身需要的东西（格式、View：解析、查找、读取）；它通向用户的路径是一个适配层，
> 把它表现为 VFS 的文件操作集。VFS 放在哪一层、接口什么样，另行设计；本节下面关于 `Tree` 的内容、I1 里"建 `bootfs::Tree`"、以及第四阶段
> 里依赖它的部分（BootServer 等）都待按 VFS 重新设计。

- **挪到 system/lib**：`kernel/lib/bootfs` 的 format.hpp 和 view.hpp 挪到 `system/lib/bootfs`，目录包含 include/、src/、test/、.gitignore、.clang-format。
  - builder.hpp 用了 std::string 和 std::vector（`builder.hpp:8-9`），拆成只在宿主工具链可见的 `:builder`。
  - kernel/lib/bootfs 挪空之后删除（D10），原来依赖它的目标改为依赖 system/lib/bootfs。
- **View 支持只读元数据**：今天的 `View::from(Bytes)` 要求整个 BootFS 在一块连续内存里，并核对 `total_size == data.size()`（`view.hpp:53`）。新增一个只看元数据的入口：输入 Header、Entry[]、名字表和 total_size，文件数据另外按偏移读。init 用 vmo_read 喂这个入口，内核照旧交出整块。
- **页对齐**（U4）：文件数据按页对齐、尾部补零；View 拒绝数据不页对齐的条目，以及与别的条目共享一页的条目。GetBackingMemory 按页取整的 Slice 靠这一条才不会越出本文件（U12）。
- **`Tree`**：把扁平的文件名按 '/' 切成目录节点，拒绝空段、"." 和 ".."；节点 id 取条目下标加 1。
- **宿主单测**：View、Tree，以及 Builder 生成再由 View 解析的往返测试。

### 4.6 阶段二：launcher 与 userboot

- **launcher**（`system/lib/launch`，K23）
  - 接口：`launch(Spec &&) -> Result<ou::Handle, Status>`。
  - Spec 包括：name、程序所在的 VMO 及偏移、vdso、job、argv、`NamespaceEntry[]`（path 加目录连接，按 names 下标发成 `NsDir | i << 16`），以及可选的 directory_request。process_create 返回的根 VMA 总是作为 VmaRoot 交给子进程。processargs 由 U2 的构造器写。
  - 句柄类型沿用 Zircon 的数值：PA_NS_DIR 0x20、PA_DIRECTORY_REQUEST 0x3B（`zircon/system/public/zircon/processargs.h:140,155`）。对照 `ProcessBuilder::add_namespace_entries`（`src/lib/process_builder/src/process_builder.rs:377`）。
  - 不支持共享库：程序只链接 vDSO。
- **可执行的 BootFS**（D3）
  - 先复制，再换：`handle_duplicate(bootfs, SameRights, &copy)`，然后 `vmo_replace_as_executable(copy, vmex, &bootfs_x)`。replace 无论成败都消耗原句柄（K19），不先复制，就没有不可执行的那一份留给 GetBackingMemory 的 Slice（U12）。
  - BootFS VMO 本来就不带 Write，换出来的句柄是 Basic\|Read\|Map\|GetProperty\|Execute。做这件事的只有 userboot、init 和 dbg_1 的 shell，三者本身就是 vmex 的持有者，所以这不是权限放大。对照 Fuchsia BootfsSvc 用 `bootfs_vmex` 对 bin/、lib/ 下的文件调 `replace_as_executable`（`src/sys/component_manager/src/bootfs.rs:179-183、294-305`）。
- **装段方式**（D3）
  - 按 eftl 的 `make_plan`（`system/lib/eftl/include/eftl/elf/load_policy.hpp:185`）逐段规划。只读段和可执行段直接从 `bootfs_x` 映射，偏移为 `entry.data_offset + p_offset`；只有 PF_X 的段按 X 映射。
  - 可写段用 vmo_create + vmo_write 复制，这些 VMO 永远不带 Execute；Binder 也用 vmo_write 写 GOT。
  - launcher 自己不映射任何东西，所以用不到 vma_unmap。
  - 这改动了接口设计 §3.2 的第 2、4、5 步。
- **放置**
  - 所有 PT_LOAD 和栈都按 Specific 映射在一个固定基址上，各段保持链接时的相对偏移。程序都是 PIE（`system/user_executable.gni:22`），自动放置只保证第一段有空间，后面几段的区间可能已被占用，所以不用它。
  - 先用 `get_property(ProcessVdsoBaseAddress)` 读出 vDSO 的位置并避开它。还没有 ASLR，固定基址与今天内核装载时相同（`kernel/lib/userabi/src/userboot.cpp:205` 的 USER_ASPACE_BASE）。
  - 栈下方的 guard 页用 K22 的 vma_allocate 按 Specific 占住。它不带 CanMap 位，什么也映射不进去，之后自动放置的映射也不会落进去（接口设计 §3.4）。
- **进程句柄**原样返回。降到 Wait|Inspect|Destroy 要等 handle_replace（M2）。
- **vDSO 一致性**：launcher 映射的共享 vDSO VMO（内核装载一次、把句柄交给第一个进程，照 Zircon；见 K16，2026-09-28 修订：vDSO 不再由内核在 process_create 时映射，所以上面“用 get_property 避开 vDSO”和 K23 里“映射了 vDSO 的 aspace”都要在 K23 按此重看），与 launcher 的 Binder 读取的，是 BootFS 里同一个 `vdso` 文件（`kernel/lib/userabi/src/userboot.cpp:43`），一致性由构造保证。因此接口设计 D13 的 build-id 核对不再需要，D13 要同步修改。
- **userboot**（U8）
  - 今天的 `kernel/lib/userabi/userboot` 只是一个 ring 3 的 hello 程序。
  - **部分落地（2026-09-26，`a53c5ec5`、`a2a5b2b5`、`ccc7847a`）**：userboot 已跑在 runtime 上。内核总是先起它，environ 带命令行里的 `userboot.*`（前缀白名单，内核不解析）；它读 `userboot.next`，把根 job 和 BootFS 原样转交给 next，再用 `process_spawn_boot` 起它；两个镜像的 cmdline 已删掉 `kernel.select.userboot`，改写 `userboot.next`。**还没做**：vmex（K18）、复制 BootFS 并换出 bootfs_x、用 launcher 起 next（今天用临时的 `process_spawn_boot`，K25 删它时要改这里）。起完 next 后从 main 返回 0、由 runtime 调 process_exit 已做（`object: process 'userboot' … destroyed` 已出现在两个镜像的日志里）。
  - U8 把它换成真正的 userboot。内核总是先起它，processargs 带根 job、BootFS 和 vmex（SystemResource），environ 带命令行里的 `userboot.*`。
  - 顺序：复制 BootFS，用 vmex 把其中一份换成 bootfs_x；用 launcher 从 bootfs_x 起 `userboot.next`；交出的 VmoBootfs 是不可执行的那一份，连同根 job 和 vmex；然后关掉 bootfs_x，process_exit(0)。要是交出 bootfs_x，next 手里“不可执行的那份”其实带 Execute，而 Slice 的 rights 等于父句柄的 rights（K20）。U12 另外用 handle_duplicate 钉住 rights，两道防线各自成立。
  - `userboot.next` 默认是 init，U9 之后是 `bin/init`；dbg_1 设为 shell，U9 之后是 `bin/shell`。选项名同 Fuchsia（`zircon/kernel/lib/userabi/userboot/main.rs:14、27`），不支持 `userboot.root`。
  - init 收到的 processargs 格式不变，所以 init 不用为 userboot 改。
  - 对照：Fuchsia userboot 默认的下一站是 `bin/component_manager+--boot`（`main.rs:14`）。

## 5 客户端库 fsio

`system/lib/fsio` 的目录包含 include/、src/、test/、.gitignore、.clang-format，依赖 ou、wire、生成的 io 客户端桩和 runtime。

- **状态**，对照 `fdio_state_t`（`sdk/lib/fdio/fdio_state.h:27-34`）：
  - `fd_table[FSIO_MAX_FD = 256]`，每个槽放一个引用计数的 `FileObject{channel, kind, rights}`。
  - `Namespace`：扁平表，每项是（规范化后的绝对前缀 → 目录 channel, rights），按路径分量边界做最长前缀匹配。
  - `cwd`：只存规范化后的绝对路径。chdir 时用 Open(ProtocolDirectory | Describe) 验证目标存在。
  - 每个进程目前只有一个线程，暂不加锁；futex 落地后补上。
- **路径规范化**照 fdio 的 `CleanPath`（`sdk/lib/fdio/cleanpath.cc:21`）。它的注释引用了 Plan 9 的 lexnames 一文，该文未在本地核对（未核实）。步骤：
  1. 相对路径先拼上 cwd；
  2. 合并 "//"，去掉 "."，按字面折叠 ".."，根上的 ".." 直接丢弃；
  3. 末尾的 '/' 剥掉，改为置 ProtocolDirectory；
  4. 超过 MAX_PATH 时在本地回 BadPath；
  5. 匹配到前缀后，剩余部分为空就发 "."。

  fuchsia.io 同样要求客户端先规范化（`io.fidl:44-46`）。现在没有符号链接，所以字面折叠与 POSIX 语义等价。以后加了符号链接也保留 Plan 9 语义：链接由客户端解析，服务端只把目标当作数据返回（`symlink.fidl:15-17`）。
- **API**：Status 加出参风格，不做 POSIX 兼容层。

```cpp
namespace fsio {
auto open(StringView path, IoRights rights, OpenOptions options, Fd *out) -> Status;  // with Describe: errors come back synchronously
auto open_channel(StringView path, IoRights rights, OpenOptions options, HandleValue server_end) -> Status;  // pipelined
auto read(Fd fd, void *buffer, usize size, usize *actual) -> Status;  // also read_at, seek, read_dirents, get_attributes
auto connect(StringView path, HandleValue server_end) -> Status;      // /svc, ProtocolService
auto close(Fd fd) -> Status;
auto install_startup_namespace() -> Status;
} // namespace fsio
```

- **同步调用**：`wire::call` 的 txid 递增，保持在 0x8000_0000 以下（接口设计 §2.4）。遇到 PEER_CLOSED 时读出残留的 epitaph，作为 status 返回；没有 epitaph 时按连接种类决定：/svc 连接报 ConnectionRefused，其余报 PeerClosed。
- **启动**：`install_startup_namespace()` 从 runtime 取出所有 NsDir 句柄，与 names[arg] 一一绑定，照 fdio 的 `__libc_extensions_init`（`sdk/lib/fdio/unistd.cc:586-631`）。DirectoryRequest 由 svc 库处理，fsio 不碰。
- **推迟到 M2**：`export_namespace`（给子进程用 Open(".") 派生缩权连接）和 Fd（0x30）句柄，等 shell 能起子进程时再做。M1 的 shell 不持有 vmex，要起子进程只能等 GetBackingMemory 的 Execute（M2）。

## 6 权限

### 6.1 v1 规则

- **纯能力**，检查规则见 §2.4。进程没有 "/" 连接，只有 processargs 给的 NsDir；服务端永远看不到绝对路径。
- **身份由铸造者决定。** 每个子进程的 /svc 连接由 init 铸造，允许表固定在它的合成目录上。能用什么服务，取决于这条连接是谁铸的，而不是客户端自称是谁。
- **服务替客户端开文件**时，只能在客户端交来的目录连接上按相对路径打开，即 openat 的做法；绝不用服务自己的命名空间去解析客户端给的字符串。
- **可执行是一项能力**（D3）：
  - `vmo_create` 永不带 Execute，内核铸造的 BootFS VMO 也不带。
  - Execute 只能经 `vmo_replace_as_executable(vmo, vmex)` 取得：它消耗原句柄，新句柄在原 rights 上加 Execute、去掉 Write。对照 Zircon `sys_vmo_replace_as_executable`（`zircon/kernel/lib/syscalls/vmo.rs:360-387`）；Zircon 保留 Write（给 JIT 用），ours 去掉，真有 JIT 时再议。
  - vmex 是 System 种类的资源槽位 [VMEX, VMEX+1)，同 `ZX_RSRC_SYSTEM_VMEX_BASE = 1`（`zircon/system/public/zircon/syscalls/resource.h:36`）。内核把它交给 userboot，userboot 转交给 next：dev 是 init，dbg_1 是 shell。init 不再外发它，只给自己的 launcher 用。
  - 去掉 Write 只保证之后不能再用这个句柄建可写映射；换之前已经建好的可写映射、别处留着的带 Write 的句柄仍然有效。所以 vmex 的持有者属于可信基，这与 Zircon 相同。能执行任意代码的，永远只是被授予 vmex 的那几个进程，而不是任何能调 vmo_create 的进程。
  - M1 用到 vmex：userboot、init 和 dbg_1 的 shell 用它换出 bootfs_x 来装程序。外发的只有 GetBackingMemory 的只读 Slice，不外发任何可执行 VMO。

### 6.2 为 DAC 预留的形状

- **principal 不可篡改。** 连接记录见 §2.6。principal 只能在认证握手新建连接时设置，Open 和 clone 一律照抄。任何 Open 选项都不能设置或清除它：空主体意味着不做 DAC 过滤，允许清除就等于允许任何受 DAC 限制的连接给自己提权。
- **以后的规则**：`effective = cap_rights ∩ DAC(principal, uid, gid, mode)`，只用于对本节点本身的操作；Open 的子集检查仍然只看 cap_rights。如果把 DAC 的结果也折进 cap_rights，0555 目录就会截掉其下 0644 文件的写权限，这与 POSIX 不符。
- **身份只能减权**：没有 uid 0 的绕过；管理权就是持有根连接这个能力本身。
- **属性**
  - NodeAttributes 里的 mode/uid/gid 从 M1 起真实上报：/boot 的目录报 0555，文件报 0444，uid 和 gid 都为 0。这些值由 init 合成。
  - M2 升级 BootFS 格式（D6）：
    - Entry.flags 的 bit0 定为 Executable；
    - 新增 u32 mode、uid、gid 和一个 u32 reserved，Entry 从 32 字节（`format.hpp:49`）变为 48 字节；
    - 格式版本号放进 Header.reserved；
    - 这些字段由 GN metadata 填写：user_executable 标 Executable、mode 0555，其余文件 0444。
  - 可执行与否只看条目标志，不按路径前缀判断。Fuchsia 的 BootfsSvc 是按 bin/、lib/ 等目录名判断的（`src/sys/component_manager/src/bootfs.rs:44-53, 294-305`）。
  - v1 只存储、只上报，不据此裁决。Fuchsia 规定 POSIX 属性只存不解释（`node.fidl:151-154`）；ours 以后要解释它们，所以现在就必须存对。
- **主体证明**（以后做，Hurd 式会合；本地没有 Hurd 源码，未核实）：
  1. 客户端建一个会合令牌，经现有连接向 fs 发 `Reauthenticate(token, server_end)`，同时把令牌交给 auth 服务。
  2. fs 向 auth 出示令牌，auth 按 koid 配对，告诉 fs 对方是哪个主体。
  3. fs 为 server_end 新建一条带 principal 的连接，沿用原来的 grant，旧连接不变。

  v1 只在描述里写上这个方法名；ordinal 用哈希时不需要另外预留号段。

### 6.3 撤销与配额

- **GrantId 的来源**：不经 Open 产生的根连接各铸一个新 grant，也就是 init、以后的 login、launcher 交出去的那些根连接；经 Open 和 clone 产生的连接继承父连接的 grant。M1 只用 grant 计配额（§4.2）。
- **子进程退出不撤销。** 进程死后内核关掉它的句柄（K4），连接靠 PEER_CLOSED 回收。按 grant 撤销只用于显式的策略事件，比如注销和吊销。原因：经 Open(".") 派生出去的连接继承同一个 grant，若退出即撤销，shell 一重启，它派生给子程序的 /boot 和 /svc 就会一起失效。
- **子 grant**：OpenOptions 预留 `NewGrant` 位，让服务端为新连接铸一个子 grant。grant 因此成树，撤销父级时级联撤销。
- **`revoke(grant)`**（P1-B 之后）：服务端维护 grant → 连接的侵入式链表，撤销时关掉这些连接的服务端端点。撤销按派生关系走，不看谁持有，所以转手出去的 clone 也一起失效。
- **撤销够不着的两处**：
  - 已经外发的 VMO：内核收不回句柄，映射也还在；
  - 已经转发出去的 /svc 连接：归提供者管，只能靠杀掉提供者来切断。
- **永不外发可写 VMO**，理由有三：
  - 它逃过撤销；
  - 它绕过服务端对以后每一次访问的中介：DAC、配额、mtime、写入顺序；
  - 它能在 launcher 校验完 ELF 头之后改掉共享的可执行映射，构成 TOCTOU 式的代码注入。

### 6.4 多用户方向

- **会话怎么建**：login 服务持有根连接。用户认证后，它为这次会话铸几个 grant：/home/alice（读写，带 principal）、/boot（rx）、过滤后的 /svc，作为 NsDir 交给 shell。注销就是撤销这几个 grant。
- **v1 已经就位的前提**：
  - 没有环境根；
  - 服务端不见绝对路径；
  - 每个进程有专属的 /svc 连接；
  - grant 从第一天就有；
  - Open 改不动 principal。

### 6.5 v1 威胁模型

| 威胁 | v1 的处理 |
|---|---|
| 糊涂代理：init 转发 /svc | 名字必须是单段且在允许表里；路径由 init 自己拼，选项固定；目标只能是提供者的 DirectoryRequest（§4.4） |
| 糊涂代理：服务替客户端开路径 | 只在客户端交来的目录连接上做 openat |
| 权限放大 | 子集检查、节点能力上限、没有 INHERIT 位；M1 只发只读 Slice，rights 由 handle_duplicate 钉成 Basic\|Read\|Map\|GetProperty（U12）；vmo_create 与 BootFS VMO 都不带 Execute，Execute 只能凭 vmex 换得且换时去掉 Write；BootFS VMO 不带 Write |
| 路径穿越 | 服务端做语法检查，逐段按节点解析；没有符号链接和挂载；客户端先做文本规范化，".." 越过根时停在 "/" |
| 自环：让 init 自己跟自己对话 | 消息头的 direction 字段。init 同时持有一条 channel 两端时仍会泄漏两条连接记录，由 grant 配额兜底；有了 get_info(HandleBasic) 之后，拒绝 related_koid 指向 init 自己服务端的 object |
| 句柄耗尽、针对 init 的 DoS | 每进程句柄上限，在途句柄也计入（K11），init 自己创建的句柄数静态核对（§4.2）；连接池加每 grant 配额；GetBackingMemory 每个 grant 终身至多 GRANT_MAX_BACKING_VMOS 个；每处理一条消息就重挂，按轮次服务；port 的活跃 observer 上限含已入队的包，并与 LOOP_SLOTS 静态核对；回复遇到 NoResource 即断开该连接；NoResource 只让单个请求失败；o_c 一收到入站消息就关掉 |
| 进程死后资源不释放 | K4：所有进入 Dead 的路径都清空句柄表，句柄在锁外销毁；K23 补上 Initial 进程关掉最后一个句柄这条路；K6 拆掉地址空间和页表 |
| channel 层层嵌套撑爆内核栈 | K8：对象析构改走每线程的延迟删除链表 |
| 畸形消息 | 生成的解码器校验长度、magic、direction、txid、reserved、未知位、句柄个数；失败就关连接，不 assert；有宿主测试覆盖 |
| 句柄类型不符 | v1 查不到类型：第一次使用时得到 BadType，服务端关连接；read_etc 落地后补上 |
| 回复残留泄漏给下一个客户端 | 回复缓冲先清零，只发 handler 报告的长度 |
| 读越界 | 按 §2.2 的 Seek/Read 边界规则，做加法前先查溢出 |
| 内核映像泄露 | K12 只把白名单里的条目拷进 BootFS VMO，内核目录与补丁表另有黑名单兜底（§4.1）；长期拆成独立的 OBI 条目 |
| 路径 TOCTOU | BootFS 不可变，init 单线程，连接钉住节点而不是名字；可变文件系统上的 rename/unlink 竞争推迟处理 |

## 7 内核前置工作（及夹在其间的生成器、用户侧提交）

**每一步都要满足：**
- 一次提交；
- 构建通过；
- 两个镜像都能启动，没有 panic 或 ASSERT：`python3 scripts/qemu_boot.py`，以及 `IMAGE=out/bringup.x86/kernel.dev.dbg_1.obi python3 scripts/qemu_boot.py`；
- 同一次提交里写 `docs/changes/<YYMMDD>/<MODULE>.md`。

下表“验证”一列只写这之外还要看到的东西。编号前缀：K 为内核，G 为生成器，U 为用户侧，X 为外部前置（不在本计划里做，但有步骤依赖它）。

**依赖先行**（D1、D2）。文件系统里程碑不建在它自己依赖的替身上。主人原话：“你应该先把依赖项给做完再做这些事情…否则不就变成本末倒置了，至少你得保证接口的存在和基本功能实现，只是不需要你全量的实现接口”。所以进程生命周期、加载器和 userboot 都排在文件系统之前，spawn_boot 也在那之前删掉。每个依赖都做到“基本但真实”：接口以真实的签名和 rights 检查存在，基本功能经启动验证；只有本计划里没有上层依赖的选项和边角，才回 NotSupported（§7.6）。

**总数**：到 M1 共 45 个提交，阶段一 7 个，阶段二 29 个，阶段三 3 个，阶段四 6 个。K、G、U 各自按做的先后从 1 起连续编号。这是按步骤计的计划数：实际做的时候一步常拆成几个提交（K12、U1、U2、K14 都是），步骤的编号不变，拆分写在各步的行里。

**外部前置 X1**（已修，2026-09-25，`3bcabef9`：叶子只带请求的权限，Global 只给内核页）：原 `docs/repairs/260924/x86-kernel.md` 第 1 条。今天每个叶子 PTE 都被强加 Writable 和 Global；用户页是全局页，CR3 切换后 TLB 项仍在（同文件 :16-17）。所以 K6 在 X1 之前释放用户帧，会被别的进程经陈旧翻译读写；K21 的只读映射、U6 的 pf_write_ro 探针也验证不出来。

### 7.1 阶段一 进程生命周期

| # | 提交标题 | 文件 | 改动 | 验证 | 依赖 |
|---|---|---|---|---|---|
| K1 | `fix(mm): propagate commit errors and commit a VMO range once` | `kernel/mm/vm_object_paged.cpp:40、:65-72` | 创建时不再丢掉 commit_range 的返回值；commit 循环原来每一页都重新提交整个区间（平方复杂度），改为只提交一次。从原 M1 挪到这里，因为 K3 要用它 | 日志不变 | — |
| K2 | `feat(mm): kernel-buffer VmObjectPaged::read/write and VmObject::size()` | `vm_object_paged.cpp:103-113`；`kernel/mm/include/ours/mm/vm_object.hpp` | 经 cursor 按页拷贝；越界或溢出回 OutOfRange；未提交的页读出为零；不在自旋锁下拷贝 | 日志不变，功能在 K3 和 K12 验证 | K1 |
| K3 | `fix(elfload): back a loaded image with a paged VMO, so its frames go with it` | `kernel/lib/elfload/src/loader.cpp:102-151` | 今天 Loader::map 用 get_frames_n 取帧（:109），再包成 physical VMO（:146-147）。physical VMO 不拥有这些帧，映射拆掉后帧照样泄漏，K6 就释放不了程序映像。改为整体提交的 VmObjectPaged，用 K2 的 write 逐段写入，这正是 :140-143 的注释说的去向。每进程一份的 vDSO 拷贝走同一个 Loader（`kernel/lib/userabi/src/userboot.cpp:246-268`），一起改掉。按段映射仍不做（`docs/repairs/260919/elfload.md` 第 1 条） | 两个镜像的日志不变；帧随进程归还，在 K6 核对 | K2 |
| K4 | `feat(object): a process reaches Dead when its last thread leaves, and closes its handles` | `kernel/object/process_dispatcher.cpp:154-196` | 今天最后一个线程 detach 时状态不变（:189-196），只有“kill 时没有线程”才进 Dead（:163-168）。本步补上 Dying→Dead。所有进入 Dead 的路径都在锁外调 finish_dead_transition，照 Zircon 的 FinishDeadTransition（`zircon/kernel/object/process_dispatcher.cc:547`）。这一步只实现其中一段：在锁内摘下句柄，在锁外销毁 | dbg_1 的每个 fault_probe：调试日志 `object: 'fault_probe' dead, <n> handles closed` 早于 `process … destroyed` | — |
| K5 | `feat(syscall): process_exit` | `system/public/ours/syscalls.gni`、lib/syscall、process_dispatcher、ou；`examples/shell/shell.cpp` 末尾；`system/service/init/init.cpp:98-100` | 语义照接口设计 §2.2。生成器已支持 noreturn 桩（`build/syscall/gen_syscalls.py:132-133`）。shell 末尾的 pause 循环换成 `process_exit(0)`。init 读完 pong 后不再直接 halt（:100），先 wait_one(PEER_CLOSED)，再打一行日志 | dev：出现 `object: process 'shell' … destroyed` 和 `init: shell closed its channel` | K4 |
| K6 | `feat(mm): VmAspace::destroy, run as a process reaches Dead` | `kernel/mm/vm_aspace.cpp`、`kernel/mm/include/ours/mm/vm_aspace.hpp`；`kernel/arch/x86/mm/arch_vm_aspace.cpp`、`kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`；finish_dead_transition | ① `VmAspace::destroy()`：拆掉根 VMA 和所有映射，打断 aspace 与根 VMA 之间的 Arc 环（`docs/repairs/260924/object.md` 第 2 条，同一提交删掉它）；清掉 m_last_fault，它持有一个 Arc<VmMapping>（`vm_aspace.hpp:128`）。映射拆掉后，只被它们引用的 VMO 随之释放自己的页。② `~VmAspace`（今天是空的，`vm_aspace.cpp:43`）从这一步起才真正运行。它先在 ALL_ASPACE_LIST_LOCK 下把自己摘下：create 时挂上了（:83-87），而 m_managed_hook 用默认的 SafeLink（`vm_aspace.hpp:132`），析构时还挂着就 require 失败（`list_hook.hpp:133-135`）。然后调新增的 `ArchVmAspace::destroy()`（今天只有 `~ArchVmAspace() = default`，`arch_vm_aspace.hpp:24`），只释放用户半区的页表页，跳过 alias 过来的内核表项（`arch_vm_aspace.cpp:82-84`）。顺序照 Zircon 的 ~VmAspace：先 erase，再 `arch_aspace_.Destroy()`（`zircon/kernel/vm/vm_aspace.cc:277-290`）。③ 页表留给 ~VmAspace，因为 destroy 在进程最后一个线程上执行，CR3 还指着它；Zircon 也是这样拆成两半的（`vm_aspace.cc:285-290` 的注释写明原因，Destroy 在 `:303-336`）。④ 不需要 TLB shootdown：进程只有一个线程，换到别的 aspace 时必然改写 CR3（`kernel/kernel/task/sched/scheduler.cpp:600-602`），PCID 又从未分配（`arch_vm_aspace.cpp:44`），改写 CR3 就清掉全部非全局项。前提是 X1。⑤ VmAspace 加一个名字成员：构造函数今天丢掉了 name 参数（`vm_aspace.cpp:34-41`）。⑥ 调试构建在每个进程 destroyed 时打出 PMM 空闲帧总数，由各 buddy 的 free_count 汇总（`kernel/mm/include/ours/mm/priv/frame_buddy.hpp:29`） | dbg_1：10 个探针都出现 `mm: aspace 'fault_probe' destroyed, <n> pages`，且 n 相同。从第二个探针起，相邻两次的 PMM 空闲帧数之差恰好是一个内核栈（内核栈的泄漏另案处理，§9）；多出任何一帧，都是 destroy 漏了东西 | K4、K3、X1 |
| K7 | `feat(object): TERMINATED, raised as a process finishes dying` | process_dispatcher | finish_dead_transition 的顺序照 Zircon：aspace、句柄表、TERMINATED（`process_dispatcher.cc:555-576`）。信号位见 `system/public/ours/signals.hpp:37`。读退出码的 get_info 另成一步（K24）：K23 之前用户态拿不到进程句柄（spawn_boot 不返回它，接口设计 §2.8），在这里加就要空放二十来个提交 | dbg_1 调试日志：每个探针一行 `object: process 'fault_probe' terminated, retcode <r>`，在它的 aspace destroyed 之后 | K6 |

### 7.2 阶段二 对象、内存与加载

| # | 提交标题 | 文件 | 改动 | 验证 | 依赖 |
|---|---|---|---|---|---|
| G1 | `refactor(build): move the syscall generator to build/abi` | `build/syscall/*` → `build/abi/*`；`system/public/BUILD.gn:55`；`kernel/lib/userabi/vdso/BUILD.gn:108` | 纯移动 | gen_syscalls 的五个输出（`system/public/BUILD.gn:63-69`）除 banner 两行外字节不变；libours.ifs 不变 | — |
| K8 | `feat(object): defer dispatcher destruction through a per-thread list` | `kernel/object/dispatcher.cpp`；`handle.cpp`（`~Handle`，:211-221）；task::Thread 加一个字段 | 最后一个引用释放时，如果本线程已经在删除对象，就把它挂到每线程链表上，由最外层逐个删除。照 Zircon 的 SafeDeleter（`zircon/kernel/object/dispatcher.cc:30-76`） | 日志不变；回归测试在 K10 | — |
| K9 | `feat(object): let a MessagePacket carry handles` | `kernel/object/include/ours/object/message_packet.hpp`；`channel_dispatcher.cpp:87` | MessagePacket 加一个 `ustl::Box<Handle>` 数组。在途句柄的 table_koid 为 0，正好满足 `handle_table.cpp:41` 的断言；析构时关掉没被取走的句柄。`read(max_handles)`：字节或句柄任一放不下就回 BufferTooSmall，消息留在队首 | 日志不变 | K8 |
| K10 | `feat(syscall): move handles through channel_write and channel_read` | `kernel/lib/syscall/src/channel.cpp:83、:133-137`；`handle_table.cpp`；`build/abi/gen_syscalls.py:97-98`、`syscalls.gni` | 生成器的 release 放宽到 HandleValue 数组，channel_write 的 handles 参数标 release（§3.2；初稿里它是单独的一步）。新增 `HandleTable::remove_for_transfer`：锁外拷入句柄值，在一次表锁内全部摘下；先摘后查：缺 Transfer 回 AccessDenied，句柄正是被写的 channel 回 NotSupported，同一值出现两次时第二次摘除得 BadHandle；无论成败句柄都被消耗。读端先拷字节，再写出句柄值，全部成功后才发布（沿用 `channel.cpp:43-58` 的顺序）。对照 Zircon 的 msg_put_handles / msg_get_handles（`zircon/kernel/lib/syscalls/channel.cc:314、:146`）。顺手改正 `handle_table.cpp:64-66` 的注释：Zircon 的 RemoveHandleLocked 并不取消等待（`handle_table.cc:124-135`），取消发生在接收端（`channel.cc:166`） | dbg_1 shell：经 channel 传过去一个 channel 端，再经它 ping 通；传 channel 自己回 NotSupported；数组含坏值回 BadHandle，且其余句柄已被消耗；嵌套 1000 层 channel 再关闭，内核照常运行；syscall 描述测试覆盖 HandleValue 数组上的 release | K9、G1 |
| K11 | `feat(object): charge every live handle to the process that created it` | `handle.cpp`、`process_dispatcher`、`system/public/ours/syscall.hpp` | 按接口设计 §4.8 实现 `HANDLE_MAX_PER_PROCESS`：在途句柄也计入，关闭时归还，超额回 NoResource | dbg_1：循环 channel_create 直到得到 NoResource；关掉一个之后能再建 | K10 |
| K12 | `feat(userabi): hand the kernel package out as a read-only BootFS VMO` | `kernel/lib/userabi/src/userboot.cpp`、`kernel/object/vmo_dispatcher.cpp`；生成白名单的 GN 模板；`kernel/include/ours/boot/options.hpp`（补 `kernel.name`） | 建一个 Pinned、整体提交的 VmObjectPaged，用 K2 的 write 写入一份只含白名单条目的 BootFS（重建头部、条目表和名字表）；内核目录与补丁表按黑名单拒绝，误入白名单即报错（§4.1）。rights 为 Basic\|Read\|Map\|GetProperty，不给 Write，也不给 Execute（可执行权经 vmex 换得，D3）。解压缓冲暂不释放：spawn_boot 还在读它，vfree 也没有 TLB shootdown；K25 删掉 spawn_boot 之后，只剩后一条理由 | 出现 `userabi: bootfs vmo <N> bytes`；回读的头部 magic 一致 | K2 |
| K13 | `feat(syscall): vmo_read` | `kernel/lib/syscall/src/vmo.cpp`（今天是空壳）、`syscalls.gni` | `ours_vmo_read(HandleValue vmo, void *buffer, u64 offset, usize length)`，要求 Read 权限。在 VMO 锁下先拷进一页中转缓冲，放锁后再拷给用户（接口设计 §4.6） | 功能在 K14 验证 | K2 |
| U1 | `feat(libc): give user programs mem* and str*` | `kernel/lib/libc/string.cpp` → `system/lib/libc/src`；`system/BUILD.gn:80-82` | 内核与用户共用一份实现；user_executable 默认链接 | shell 里一次 memcpy 的结果正确 | — |
| U2 | `feat(runtime): parse processargs into a startup-handle table` | `system/public/ours/process_args.hpp`（新）、`system/lib/runtime`（新） | 实现接口设计 §3.3 的格式；runtime 负责解析，提供 `take_startup_handle(HandleType, u16)` 和 names 访问；main 返回后，runtime 调 process_exit。process_args.hpp 里另有一个 header-only 的构造器，内核和用户态共用：K14 的内核一侧和 shell 一侧、K23 的 launcher 都用它写 processargs，全系统只有这一份编码 | 宿主单测：构造器写出的消息由解析器原样读回 | U1、K5 |
| K14 | `feat(userabi): send the first process a processargs message`（2026-09-26 拆为四个提交，每一对启动方与被启动方一起切换：① fault_probe 先迁到 runtime，只有 dbg_1 的 shell 启动它，shell 给它写 processargs、另给一个回答用的 channel，并加一个不计入统计的对照探针 `none`，让"main 返回、runtime 调 process_exit"真正跑一次；② U3 的搬移部分提前：bootfs 头文件挪到 system/lib，免得 init、shell 依赖 kernel/lib；③ 内核发 processargs，init、shell 同时迁移；④ dbg_1 的 shell 列出 BootFS 条目并补测 vmo_read） | `kernel/lib/userabi/src/userboot.cpp`、`system/service/init/init.cpp`、`examples/shell`、`kernel/lib/syscall/test/fault_probe` | 内核建 bootstrap channel，写 processargs（JobDefault = 根 job、VmoBootfs），rdi 为 bootstrap 端。init 和 shell 改由 runtime 取句柄。这两件放在同一个提交里，因为启动协议要同时切换。fault_probe 也改走 runtime：探针名改为经 processargs 的 argv[1] 传递（今天是 channel 上的第一条消息，`fault_probe.cpp:164-172`，runtime 解析不了）；answer_and_stay（`:146-155`）改为写回答案后从 main 返回，由 runtime 调 process_exit，不再原地自旋。**runtime 读完 processargs 就关掉 bootstrap channel**（照 Zircon，U2），所以对端在 main 运行之前就会看到 PEER_CLOSED。受影响的有：shell 与 fault_probe 今天在 bootstrap 上一问一答（ping/pong、探针名与回答），要改为另给一个 channel，作为 User0 之类的启动句柄交过去；init 在 bootstrap 上等 PEER_CLOSED、打 `init: shell closed its channel`（`system/service/init/init.cpp:107-115`）——注意这一行今天其实是 shell 自己 `check_handle_close(bootstrap)` 时触发的，并不表示 shell 已经退出，要表示退出得等进程句柄上的 TERMINATED（K23 之后）。另外 **init 今天是先 spawn、后写消息**（`init.cpp:75-81`），runtime 因此在读之前先等 READABLE 或 PEER_CLOSED | dev 出现 `init: bootfs has <n> entries`（头部由 vmo_read 读出）；dbg_1 的 shell 打出同样的行，并照旧驱动 fault_probe。dbg_1 的 shell 再按条目表逐个打出文件名：只有白名单里的 4 个，没有 `vmours/` 下的任何条目（原 `docs/repairs/260925/userabi.md` 第 1 条的验收，K12 修掉时挪到这里）。dbg_1 的 shell 还要在 BootFS VMO 上补齐 K13 时测不到的 vmo_read 路径（K13 时用户态还没有 VMO 句柄，只测得到句柄解析）：跨页读 [PAGE_SIZE-8, PAGE_SIZE+8) 与 [PAGE_SIZE-8, PAGE_SIZE)、[PAGE_SIZE, PAGE_SIZE+8) 两次页内读拼起来一致（BootFS 的文件数据按页对齐，VMO 必然大于一页）；offset = size、长度 0 回 Ok；offset = size、长度 1 回 OutOfRange；长度 size+1 回 OutOfRange；缓冲区在内核半区、未映射、只读（vDSO）都回 InvArgs | K10、K12、K13、U2 |
| K15 | `feat(syscall): handle_duplicate` | `kernel/lib/syscall/src/handle.cpp`、handle_table、syscalls.gni、ou | 签名照接口设计 §2.1。句柄要有 Duplicate 权限；rights 只能是 SameRights 或源权限的子集，否则回 InvArgs；出参按“预留、写出、发布”的顺序（接口设计 §4.1）；新句柄记在调用者名下（K11）。使用者：userboot（U8）、init（U7）和 dbg_1 的 shell 在把 BootFS 换成可执行之前先复制一份，不可执行的那份留给 Slice（K20、U12）；U12 还用它把 Slice 的 rights 钉住 | dbg_1 shell：复制 BootFS 后两份都读出同一个头部；复制 channel 回 AccessDenied（channel 默认不带 Duplicate，`kernel/object/include/ours/object/channel_dispatcher.hpp:16-17`）；rights 超出源句柄回 InvArgs；去掉 Read 的副本做 vmo_read 回 AccessDenied | K11、K14 |
| U3 | ~~`refactor(bootfs): move format and view to system/lib/bootfs; add Tree`~~（2026-09-26 取消：搬移已在 K14 的第 ② 个提交做完；`Tree` 不做，bootfs 改为适配到 VFS，见 §4.5 的修订） | `kernel/lib/bootfs` → `system/lib/bootfs` | 按 §4.5。搬移部分提前到 K14 的第 ② 个提交（init、shell 在 K14 要读 BootFS 的格式，不该依赖 kernel/lib），这一步只剩 Tree | 宿主单测；内核照旧解包 | U1 |
| U4 | `feat(bootfs): page-align file data and zero the tail` | `builder.hpp:17`；system/lib/bootfs 的 View | ALIGNMENT 从 8 改为页大小，文件数据尾部补零。View 拒绝数据不页对齐的条目，以及与别的条目共享一页的条目（原列在 M2）。GetBackingMemory 的 Slice 按页取整，靠这一条才不会越出本文件（U12） | 宿主往返测试；喂给 View 不对齐、共享一页的条目，都被拒绝；phys 照常解出内核 | U3 |
| K16 | `feat(userabi): lay the vDSO out once and map that one VMO into every process`（**已落地，2026-09-28**；2026-09-28 按主人要求改为 Zircon 的形状，原标题为 "mapped by ProcessDispatcher::create"） | `kernel/lib/elfload`（`Loader::load()` 与 `Loader::map(ImageVmo &)` 分开）、`kernel/lib/userabi/src/userboot.cpp` | 内核只在 `userboot_init()` 里把 vDSO 装进一个 VMO，照 Zircon `VDso::Create()` 的 `instance_`；**映射交给启动进程的一方**，`ProcessDispatcher::create` 不碰 vDSO：今天是内核里的 `userabi::launch()`，K23 之后是用户态 launcher，用 vma_map 映射内核交给它的 vDSO VMO 句柄（Zircon 的 userboot 在 bootstrap 消息里收到 vDSO VMO，process_builder 再逐个映射）。取代原先每个进程各拷一份 | 日志不变，外加一行 `userabi: vdso laid out once`；dbg_1 起的 14 个进程映射的是同一个 VMO；`pf_write_vdso` 照旧被杀 | K12 |
| K17 | `feat(syscall): vmo_create and vmo_write`（**已落地，2026-09-28，两个提交**：先 vmo_write，后 vmo_create） | `kernel/object/vmo_dispatcher.cpp`、`kernel/lib/syscall/src/vmo.cpp`、syscalls.gni、ou | 新 VMO 由新的 `VmoDispatcher::create_user()` 建，Lazy、不带 Execute、大小上限为 `mm::VmObjectPaged::MAX_SIZE`（照 Zircon 的 `VmObject::MAX_SIZE`）；内核自用的 `create_paged` 仍是 Pinned，不再像原计划那样改它（userboot 的栈、镜像和 BootFS 都要在 ring 3 之前驻留）；options 只收 None，Resizable 回 NotSupported，其它位回 InvArgs | 写入后读回一致；越界回 OutOfRange；未写过的页读出 0；无 Write 回 AccessDenied | K13 |
| K18 | `feat(object): a System resource slot for vmex, handed to the first process`（**已落地，2026-09-28**） | `kernel/object/resource_dispatcher.cpp`、`kernel/lib/userabi/src/resource.cpp`、`kernel/lib/userabi/src/userboot.cpp` | 按接口设计 §2.6 重写 `ResourceDispatcher::create`（今天自相矛盾，也不构造对象），先只支持 System 种类、size = 1 的槽位；补完 `userabi::make_resource_handle`；内核铸一个 [VMEX, VMEX+1) 的槽位，在第一个进程的 processargs 里作为 `SystemResource`（0x54）交出，handle_info 的 arg 写槽位的 base（Vmex = 1）。U8 之后第一个进程是 userboot，由它转交。校验照 Zircon `validate_ranged_resource`（`zircon/kernel/object/resource.rs:159`） | dev 的 init 和 dbg_1 的 shell 都能按 arg 取出 vmex 句柄，打出 `vmex resource ok`。反向检查放到 K19：这时还没有 vmo_replace_as_executable，全系统也铸不出别的种类的资源 | K14 |
| K19 | `feat(syscall): vmo_replace_as_executable`（**已落地，2026-09-28**） | `kernel/lib/syscall/src/vmo.cpp`、syscalls.gni、ou；接口设计 §4.2、§9 | `ours_vmo_replace_as_executable(HandleValue vmo, HandleValue vmex, HandleValue *out)`：vmex 必须是覆盖 VMEX 槽位的 System 资源；无论成败原句柄都被消耗；新句柄 rights = 原 rights − Write + Execute（D3）。vmex 句柄本身不要求任何 right，同 Zircon：它只经 validate_ranged_resource 查种类和槽位（`zircon/kernel/lib/syscalls/vmo.rs:367-368`）。因为原句柄被消耗，使用者先用 K15 复制（§4.6）。同一提交修订接口设计 §4.2 的 Vmo 行与 §9：删去“临时带 Execute”的偏离 | 没有 vmex（HANDLE_INVALID）回 AccessDenied；vmex 的位置传 channel 或 BootFS VMO 回 BadType，传无效值回 BadHandle；原句柄带 Write 时新句柄不带 Write；换出的 VMO 能按 X 映射（K21 验证） | K18、K17、K15 |
| K20 | `feat(mm): vmo_create_child(Slice)`（**已落地，2026-09-28**；锁先挪进 VmCowPages，`42d8f41f`） | `kernel/mm/vm_object_paged.cpp`、`kernel/mm/include/ours/mm/vm_object_paged.hpp`、vmo_dispatcher、`kernel/lib/syscall/src/vmo.cpp`、syscalls.gni、ou | 签名 `ours_vmo_create_child(HandleValue vmo, VmoChildOptions options, u64 offset, u64 size, HandleValue *out)`。父句柄要有 Duplicate\|Read，同 Zircon（`zircon/kernel/lib/syscalls/vmo.rs:293-295`），否则 AccessDenied。size 先向上取整到页（同 `vmo.rs:273`），offset 不对齐回 InvArgs，范围必须完全落在父对象内，否则回 InvArgs（`CreateChildSlice`，`zircon/kernel/vm/vm_object_paged.cc:567-610`）。子对象共享父对象的 m_cow_pages（hpp:54），另带 {offset, size}，make_cursor 和 read 都加上这个偏移（hpp:43-45）。锁先挪进 VmCowPages（Zircon 的位置，`zircon/kernel/vm/include/vm/vm_object_paged.h:49-54`），父子共用同一把锁，因此 Lazy 的父对象也能切片。子句柄 rights 等于父句柄 rights，永远不加 Write，也不照 Zircon 加 SetProperty（`vmo.rs:329-332`，§10）。Snapshot、Reference 回 NotSupported | dbg_1 shell：取 `vdso` 条目的 Slice，读出 `\x7fELF`；offset 不对齐回 InvArgs；Snapshot 回 NotSupported；对 vmo_create 建的 Lazy VMO 取 Slice，经父对象写入的字节从 Slice 读出，反之亦然；去掉 Read 的父句柄回 AccessDenied | K13、K14、K15、U4 |
| G2 | `feat(abi): seven-argument vDSO stubs`（**已落地，2026-09-28**） | `build/abi/gen_syscalls.py:35-38`、`kernel/lib/userabi/vdso/vdso.S` | 7 参数的桩只保存并装入 %r12（接口设计 §4.7），给 vma_map 用 | 已有桩的 .text 字节不变 | G1 |
| K21 | `feat(syscall): vma_map with a page-aligned vmo_offset`（**已落地，2026-09-28**，分两步：K21a 对象层与 mm，K21b 系统调用本身。VmaRoot 暂由 `process_spawn_boot` 把子进程的根 VMA 返回给启动者，启动者在 spawn 之后写进唯一那条 processargs；内核启动 userboot 同样先建进程、后写消息） | `kernel/object/vma_dispatcher.cpp`、`kernel/mm/vm_area.cpp`、`kernel/mm/vm_mapping.cpp`、syscalls.gni、ou | 签名不再暴露 mm 类型（`vma_dispatcher.cpp` 的 `map()`）；root VMA 补上 R/W/X（`:38-44`）；vmo_offset 透传：`vm_area.cpp:267` 今天写死为 0，paged 路径已经会加 m_vmo_ofs（`vm_mapping.cpp:191、485`），`map_physical` 还要补（`:235`）；保存每个映射的权限上限；processargs 加 VmaRoot；同一提交按 D3 修订接口设计 §2.3、§3.2 | W+X 被拒；不带 Execute 的 VMO 按 X 映射被拒；经 K19 换出的 BootFS VMO 能按 X 映射；K20 的 Slice 能按 R 映射。“写只读映射会杀掉进程”挪到 U6 的 pf_write_ro 探针：这时能写 shell 只读映射的只有 shell 自己，会把驱动检查的程序一起杀掉 | G2、K19、K20、U4、X1 |
| K22 | `feat(syscall): vma_allocate, Specific and without CanMap bits`（**已落地，2026-09-28**） | `kernel/object/vma_dispatcher.cpp`、lib/syscall、syscalls.gni、ou | 签名照接口设计 §2.3：`ours_vma_allocate(HandleValue parent, VmOptions options, usize offset, usize size, HandleValue *child, VirtAddr *child_addr)`，建在 mm 的 `VmArea::create_subvma`（`kernel/mm/include/ours/mm/vm_area.hpp:82`）上。只收 Specific；不带 Specific、或带任何 CanMap 位都回 NotSupported，于是子区域里什么也映射不进去，这正是 guard 要的。关掉子句柄不拆区域：父区域的 VmaSet 持有它（`kernel/mm/include/ours/mm/vm_area_or_mapping.hpp:441-449`），随 aspace 在 K6 拆掉。用途只有一个：launcher 占住栈下方的 guard 页（接口设计 §3.4），否则后来自动放置的映射可能正好落进去 | dbg_1 shell：在自己的根 VMA 里按 Specific 占一页，再对同一地址 vma_map(Specific) 被拒；带 CanMapRead 回 NotSupported | K21 |
| U5 | `refactor(elfload): move the Binder to system/lib/elfload`（**已落地，2026-09-28**；内核留下的 Loader 改名 `kernel/lib/kelfload`，eftl 先挪到 `system/lib/eftl`） | `kernel/lib/elfload` 拆成 `system/lib/elfload`（框架）和内核特化两部分 | 纯移动，外加去掉对内核 logz4 胶水的依赖（`kernel/lib/elfload/BUILD.gn:26`，接口设计 §6 末行）。按段规划不在这一步：它的第一个使用者是 K23 的用户态 launcher；内核加载器保持整段映射，`docs/repairs/260919/elfload.md` 第 1 条仍留在 repairs 里 | 宿主单测；内核照常装载第一个进程 | U1 |
| K23 | `feat(launch): process_create, thread_create, process_start, get_property and system/lib/launch`（拆分中：K23a 的 process_create、Initial 进程的 `on_zero_handles`、thread_create、process_start、vDSO 由 launcher 映射（VmoVdso 句柄、`vma_map` 登记范围）已落地 2026-09-28/29；余下 get_property、launch 库） | syscalls.gni、lib/syscall、`kernel/object/process_dispatcher`、`kernel/object/thread_dispatcher.cpp:52`、`system/lib/launch`（新）、ou | 签名和 rights 照接口设计 §2.2；options 只收 None，property 只有 ProcessVdsoBaseAddress。装段和放置见 §4.6：按 eftl 的 make_plan 逐段、按 p_flags 映射；所有段和栈按 Specific 放在固定基址，先用 get_property 避开 vDSO；guard 页用 K22 占住；processargs 由 U2 的构造器写，带 VmaRoot。thread_create 只建 ThreadDispatcher，task::Thread 延到 process_start 才建（接口设计 §2.2），这样顺带关掉 `docs/repairs/260924/object.md` 第 1 条。加上 `ProcessDispatcher::on_zero_handles`：Initial 的进程关掉最后一个句柄时转 Dead，走同一个 finish_dead_transition，同 Zircon（`process_dispatcher.cc:221-236`）；否则 launcher 在 process_create 之后的任何失败，都会留下一个映射了 vDSO、永远不 destroy 的 aspace。同一提交修订接口设计 D13（§4.6 的 vDSO 一致性） | dbg_1 shell 经 launcher 起 fault_probe，argv[1] 为 `none`：出现 `fault_probe: unknown probe none`，并写回 `unknown`；shell 做一次 process_create 后直接关掉句柄，出现 `mm: aspace … destroyed` | K16、K21、K6、K22、U5 |
| K24 | `feat(syscall): object_get_info for the Process topic` | `kernel/lib/syscall/src/object.cpp`、syscalls.gni、`system/public/ours/object_info.hpp`（新）、process_dispatcher、ou | 签名照接口设计 §2.1：`ours_object_get_info(HandleValue handle, InfoTopic topic, void *buffer, usize buffer_size, usize *actual, usize *avail)`。只收 Process 一个 topic：句柄要有 Inspect，否则 AccessDenied；对象不是进程回 BadType；别的 topic 回 NotSupported；缓冲不够回 BufferTooSmall；写回 actual 和 avail。InfoProcess 的布局照 `zx_info_process_t`（`zircon/system/public/zircon/syscalls/object.h:168-182`），用 static_assert 钉住 | dbg_1 shell 经 launcher 起 `none` 探针，在进程句柄上等 TERMINATED，get_info 读回它 main 的返回值；缓冲少一字节回 BufferTooSmall；HandleBasic 回 NotSupported；去掉 Inspect 的副本（K15）回 AccessDenied；对 channel 回 BadType | K23、K7、K15 |
| U6 | `test(syscall): launch fault_probe with the launcher and read its retcode` | `examples/shell/shell.cpp:181-231`、`kernel/lib/syscall/test/fault_probe` | :202 的 spawn_boot 换成 launch，argv[1] 为探针名；在返回的进程句柄上等 TERMINATED，再用 get_info 取退出码。新增探针 `pf_write_ro`：写自己只读段里的一个常量，那一段由 launcher 按 R 映射（K23）。K21 的“写只读映射会杀掉进程”在这里验证 | 12 行 `shell: probe <p> killed (retcode <r>)`，外加 `shell: probes done (12 killed, 0 survived)`；PMM 空闲帧数照 K6 的办法核对，这时映像、栈、可写段都由 VMO 承载 | K23、K15、K24 |
| U7 | `feat(init): launch the shell from /boot with the launcher` | `system/service/init/init.cpp:69` | 复制 BootFS，用 vmex 换成可执行的一份，从它 launch shell（§4.6），不再用 spawn_boot；init 保留进程句柄，wait_one(TERMINATED) 后用 get_info 取退出码 | dev：`init: launched shell from /boot/shell`、`init: shell exited with 0` | K23、K15、K24 |
| U8 | `feat(userboot): kernel -> userboot -> init`（部分落地 2026-09-26，已做与未做见 §4.6 的 userboot 一节） | `kernel/lib/userabi/userboot/userboot.cpp`（今天是 hello 程序）、`kernel/lib/userabi/src/userboot.cpp:457-473`、两个镜像的 cmdline（`kernel/products/image/dev/BUILD.gn:73`、`kernel/products/image/dev.dbg_1/BUILD.gn:69`） | 内核总是先起 userboot，两个镜像的 cmdline 删掉 `kernel.select.userboot`。processargs 带 JobDefault、VmoBootfs、SystemResource(vmex)，environ 带命令行里的 `userboot.*`（接口设计 §3.2）。userboot 的顺序见 §4.6：复制 BootFS，用 vmex 把其中一份换成 bootfs_x，用 launcher 从它起 `userboot.next`（默认 init；dbg_1 设 `userboot.next=shell`；选项名同 Fuchsia `zircon/kernel/lib/userabi/userboot/main.rs:14、27`）；交出的是不可执行的那份 BootFS，连同根 job 和 vmex；然后关掉 bootfs_x，process_exit(0) | dev：出现 `userboot: started init`，接着 `object: process 'userboot' … destroyed`；dbg_1：出现 `userboot: started shell`，探针日志不变 | U7、U6 |
| K25 | `refactor(syscall): remove process_spawn_boot` | `docs/repairs/260923/syscall.md` 第 1 条的清单，其中有 `kernel/lib/syscall/BUILD.gn:50-53` 到 userabi 的边和 `system/public/ours/syscall.hpp:62` 的 PROCESS_BOOT_NAME_MAX；另改 `kernel/lib/userabi/src/userboot.cpp`、`kernel/lib/userabi/include/userabi/userboot.hpp:36` | 删除清单上的全部。内核里不再留“按名字从包里装载任意程序”的路径：`userabi::launch` 从公开头文件撤下，改为 userboot_init 的文件内函数；常驻的 `resident_package`（`userboot.cpp:167、449`）删掉，包视图回到 userboot_init 的局部。在 docs/repairs 登记一条：等 vfree 有了 TLB shootdown，在 userboot 启动后释放解压缓冲（K12） | lib/syscall 不再依赖 userabi；`kernel/`、`system/`、`examples/`、`build/` 下 grep 不到 spawn_boot（docs/changes 只增不改，不在此列） | U8 |
| U9 | `build(system): install user programs under bin/` | `system/user_executable.gni`、两个镜像的 BUILD.gn、userboot、init、`examples/shell` | 用户程序装到 `bin/` 下；userboot 的 install_name 钉在顶层，因为内核按名字起它。同一提交改掉四处写死的名字，否则两个镜像都会因为 NotFound 起不来：userboot.next 的默认值改为 `bin/init`（Fuchsia 的默认值本来就带目录，`main.rs:14`），dbg_1 改为 `userboot.next=bin/shell`，init 起 shell 用的名字（今天 `init.cpp:49` 的 SHELL，U7 沿用），shell 起 fault_probe 用的名字（`shell.cpp:202`，U6 沿用）。后两个由 GN 从各 user_executable 的 install_name 生成成一个头文件，源码里不写字面量 | 内核启动时的包清单日志（`userboot.cpp:442-446` 逐个打出文件名）出现 `bin/…`；dev：`userboot: started bin/init`、`init: launched shell from /boot/bin/shell`；dbg_1：`userboot: started bin/shell`，12 行探针日志不变 | K25 |

### 7.3 阶段三 多路复用

K26、K27、K28 的设计见 §7.4，依赖与原先相同（K26 没有前置）。放在阶段二之后，是按使用者排：它们的第一个使用者是阶段四的 Loop（U10）。K15 已在阶段二，port 句柄因此能被复制，K26 不能假定同一个 port 只有一个等待者（§7.4 锁序一节）。

| # | 提交标题 | 文件 | 改动 | 验证 | 依赖 |
|---|---|---|---|---|---|
| K26 | `feat(object): a port -- port_create, port_queue, port_wait` | `kernel/object/port_dispatcher.cpp`、`kernel/object/include/ours/object/port_dispatcher.hpp`、`system/public/ours/port.hpp`（均为新文件）；`system/public/ours/types.hpp:87-97`；`system/public/ours/vdso.hpp:23-26` 与 `kernel/lib/syscall/include/syscall/handlers.hpp:6-10` 加 include；`syscalls.gni`；`kernel/lib/syscall/src/port.cpp`（新）；ou；`kernel/kernel/sync/event.cpp:77-80` | 按 §7.4，只有 User 包。先修 Event::unsignal，让它和 signal 一样关着中断取锁：port_wait 是第一个开着中断调用它的地方（§7.4 锁序）。等待者睡在 m_ready 上，m_ready 只在 port 锁内 signal/unsignal。m_zero_handles 从这一步起就有：port_queue 在锁内先查它，已置位回 BadHandle；on_zero_handles 在锁内置位、清队、unsignal m_ready，包在锁外释放。~PortDispatcher 不取锁，只断言队列为空、两个计数为 0。调试构建在析构时打出 `object: port <koid> destroyed`。同一提交修订接口设计（§7.4 末条） | dbg_1 shell：queue 一个 type=SignalOne、status=InvArgs 的包，wait(0) 读回同一个 key，type 为 User，status 仍是 InvArgs；空队 wait(0) 回 TimeOut，deadline 5 回 NotSupported；灌满 256 个后回 ShouldWait；对 channel 句柄 port_wait 回 BadType；带着未读的包关 port，日志出现 destroyed | — |
| K27 | `feat(object): object_wait_async through a one-shot PortObserver` | port_dispatcher；`kernel/object/dispatcher.cpp`、`dispatcher.hpp`（attach_port_observer，~Dispatcher 的断言）；`handle.cpp:211-221`（~Handle）；`handle_table.{hpp,cpp}`（在一次表锁内解析两个句柄）；`wait.hpp`、`wait.cpp`；`kernel/lib/syscall/src/object.cpp`；syscalls.gni；ou | on_match 把包入队，observer 随包归队列；on_cancel 走 maybe_reap。两者进门先复制一份 Arc<PortDispatcher>，port 的方法把该释放的 observer 交回调用者（§7.4）。on_match 入队前在 port 锁内查 m_zero_handles，已置位就丢包并释放 observer。options 在解析完两个句柄之后、挂 observer 之前检查，不先挂后撤。兜底：~Handle 对 dispatcher 调 cancel(this)，~Dispatcher DEBUG_ASSERT 观察者链表为空。在这一步，关 port 时仍挂着的 observer 要等对象触发或句柄关闭才释放 | (a,b)：wait_async(b, port, 7, READABLE) 后 wait(0) 回 TimeOut；写 a 之后得到 {7, SignalOne, READABLE, READABLE\|WRITABLE}，再 wait(0) 回 TimeOut；b 已可读时一挂上就出包；b 已可读时带未知 options 挂，回 InvArgs，随后 wait(0) 仍回 TimeOut；关掉 b 后不再出包，关之前入队的包照常读到；对 port 句柄 wait_async 回 AccessDenied；port 参数给 channel 回 BadType | K26 |
| K28 | `feat(object): tear a port's observers down with it, and bound them` | port_dispatcher.cpp、`port.hpp` | on_zero_handles 拆除在挂的 observer（§7.4 生命期表“port 关闭”一行）；活跃计数含已入队的包，超限回 NoResource | ① b 空闲时在 b 上挂 3 个 READABLE 等待后关 port：destroyed 必须紧接着出现，早于 shell 随后打的一行标记（在 K27 的代码上它要等写 a 才出现，这一条才分辨得出拆除做没做）；之后写 a，没有别的日志，也没有断言。② b 空闲时循环 wait_async 直到 NoResource；关掉 b 后能在 a 上再挂。③ b 可读时循环 wait_async 直到 NoResource，每次都立即入队，验证已入队的包也计入；关掉 b 后再挂仍回 NoResource，port_wait 读掉一个包后才能再挂一个 | K27 |

### 7.4 M1 的 Port（K26、K27、K28）

D4 定为 M1 直接上 Port（§8）：init 和 echo 的 Loop 都建在 port + object_wait_async 上（§4.2）。

**object_wait_many 推迟到 P2。** Zircon 两者都有。wait_many 的使用者是 poll/select：fdio 的 ppoll 和 select 用的就是它（`sdk/lib/fdio/unistd.cc:1980、:2084`）。等 fsio 要做 poll 时再加。

**对象**

```cpp
// <ours/port.hpp> (ABI): the layout of zx_port_packet_t, pinned with static_assert
enum class PacketType : u32 { User = 0, SignalOne = 1 };
struct PacketSignal { Signals trigger; Signals observed; u64 count; Instant timestamp; u64 reserved; };  // 32
struct PortPacket { u64 key; PacketType type; Status status; union { u8 user[32]; PacketSignal signal; }; };  // 48
enum class PortCreateOptions : u32 { None = 0 };
enum class WaitAsyncOptions : u32 { None = 0 };    // Timestamp/Edge in P2
inline constexpr u32 PORT_MAX_OBSERVERS = 1024;    // live: armed, or its packet queued
inline constexpr u32 PORT_MAX_USER_PACKETS = 256;
```

- **布局**照 `zircon/system/public/zircon/syscalls/port.h:59-74、162-176`。count 照 Zircon 置 1（`zircon/kernel/object/port_dispatcher.cc:356-360`）。
- **PortDispatcher**：`SoloDispatcher<PortDispatcher, RIGHTS_DEFAULT_PORT>`，`RIGHTS_DEFAULT_PORT = (Basic & ~Wait) | Io`，与 `zircon/system/public/zircon/rights.h:84` 相同。port 不可等待，所以 port 挂不到 port 上。ObjectType 加 Port = 6（`system/public/ours/types.hpp:87-97`，同 ZX_OBJ_TYPE_PORT）。成员：
  - `m_port_lock`：一把独立的 sync::Mutex，不复用 Dispatcher 的 m_lock。这样“对象锁 → port 锁”是两把能区分的锁（接口设计 §4.8 的总序）；
  - `m_packets`、`m_armed`、`m_live_observers`、`m_user_packets`、`m_zero_handles`，都受 port 锁保护；
  - `m_ready`：手动复位的 sync::Event，等待者睡在上面（见“锁序”）。
- **~PortDispatcher 不取任何锁，也不做 detach**，因为它可能在任意被观察对象的 m_lock 下运行（见下面 PortObserver 的最后一条）。它只 DEBUG_ASSERT 队列与 m_armed 为空、m_user_packets 与 m_live_observers 为 0，照 Zircon 的析构（`port_dispatcher.cc:196-198`）。m_armed 此时必然为空：每个在挂的 observer 都持有 Arc<Port>。
- **QueuedPacket**（内核内部）：`{PortPacket packet; hook; PortObserver *owner}`。owner 为空的是 port_queue 包，单独 kmalloc。Zircon 包里的 handle 字段只供 port_cancel 用，随 port_cancel 一起加。
- **PortObserver** final : public SignalObserver。它和 WaitSignalObserver（`kernel/object/wait.cpp:22`）一样派生自已有的基类，没有新增基类。包照 Zircon 嵌在 observer 里（`zircon/kernel/object/include/object/port_dispatcher.h:211`）：
  - `QueuedPacket m_packet`：key 和 trigger 在构造时写好；
  - `Arc<PortDispatcher> const m_port`；
  - `Dispatcher *m_object`：受 port 锁保护，非空表示仍登记在对象上。**谁把它置空，谁负责释放 observer**，对应 Zircon 的 dispatcher_（`port_dispatcher.h:215-220`）。
  - **on_match 和 on_cancel 进门先复制一份 `ustl::Arc<PortDispatcher> port = m_port;`，之后一律经它调用。** 理由有二。其一，ustl::Rc 传递 const：const Arc 的 operator-> 返回 `T const *`（`system/lib/ustl/include/ustl/rc.hpp:439-445`），经 m_port 调不到入队、maybe_reap 这些非 const 方法；Zircon 的 `fbl::RefPtr<PortDispatcher> const port_` 不传递 const，所以没有这个问题。其二，释放 observer 会放掉它持有的 port 引用，这可能是最后一个，而 port 自己的方法这时也许还没返回。Zircon 的 OnMatch 为此先取 port_ref（`port_dispatcher.cc:142-150`），换成 Semaphore、放锁后 Post 时这份引用更是必需。
  - **port 的方法不自己释放 observer**，而是放掉 port 锁之后把该释放的那个交回（一个 Box）。回调释放 observer，最后才丢掉本地 Arc。

  包入队后 observer 归队列所有，出队或清队时释放。

**生命期**

| 情形 | 处理 |
|---|---|
| 在挂时句柄被关（K10 转移、K4 同理） | `HandleTable::remove` 在表锁内调 `cancel`（`kernel/object/handle_table.cpp:61-67`）→ on_cancel → maybe_reap：m_object 非空就置空并摘出 m_armed，放锁后释放。不出包（`port_dispatcher.cc:446-474`） |
| 不经表销毁句柄 | m_object 是裸指针，前提是“句柄销毁前一定先 cancel”；有了 port，这条前提从活性问题变成了内存安全问题。今天只有 `~HandleTable` 和 `HandleTable::remove` 会 cancel（`handle_table.cpp:28、:67`），`~Handle` 不会（`kernel/object/handle.cpp:211-221`），而 K10、K4 都在新增销毁路径。所以 K27 让 `~Handle` 对 dispatcher 调 `cancel(this)`，重复调用无害，对应 Zircon 的 HandleTableArena::Delete（`zircon/kernel/object/handle.cc:185-187`）；~Dispatcher 再 DEBUG_ASSERT 观察者链表为空 |
| 包已入队后句柄才被关 | 包保留，照常送达，同 Zircon：Cancel 只处理对象上的 observer（`zircon/kernel/object/dispatcher.cc:166-187`）。M1 的 Loop 不会遇到：它只在槽自己的回调里关句柄（§4.2） |
| port 关闭 | on_zero_handles 在 port 锁内置 m_zero_handles、清队并 unsignal m_ready，包和它们的 observer 在锁外释放（K26）。然后逐个弹出 m_armed：锁内取 `Arc(m_object)`（`rc.hpp:283` 会加引用）并置空，**放锁**后 detach_observer，然后释放（K28，`port_dispatcher.cc:202-252`）。并发的 on_match 看到 m_zero_handles 就丢包，看到 m_object 为空就不释放 |
| port 关闭后还有人入队 | port_queue（K26）和 on_match（K27）都在 port 锁内先查 m_zero_handles，已置位就回 BadHandle，包在锁外释放，同 Zircon 的 QueuePacketLocked（`port_dispatcher.cc:336-338`）。不查的话，包里的 observer 持有 Arc<Port>，与没有句柄的 port 成环，两者都永久泄漏 |
| port_wait 后重挂 | 旧 observer 出队时已释放，重挂会新分配一个；条件仍成立时立即入队，排到队尾。回 Ok |
| observer 超限 | 活跃数 ≥ PORT_MAX_OBSERVERS 时回 NoResource，**已入队的 signal 包也计入**（与 Zircon 不同，见 §10） |
| 用户包超限 | 回 ShouldWait（接口设计 §2.4）。Zircon 的上限是每个 port 的 `kMaxAllocatedPacketCountPerPort = 4096`（`port_dispatcher.cc:61、:345-351`） |
| 等待中 port 被关 | 不唤醒等待者，同 Zircon。on_zero_handles 已经 unsignal，刚醒的等待者看到队空后再去 wait 会睡下，不会空转 |

**锁序**

| 路径 | 依次取的锁 |
|---|---|
| wait_async | HandleTable → 对象 m_lock → port 锁（查上限、挂进 m_armed）；条件已满足时 on_match → port 锁 → Event 的 WaitQueue 锁 |
| update_state | [PeerHolder →] 对象 m_lock → on_match → port 锁 → WaitQueue 锁，放锁后 unpark（PI、runqueue） |
| port_wait | port 锁内出队后放锁；队空时 Event::wait（PI → WaitQueue）。copy_to_user 和释放 observer 都在锁外 |
| 句柄关闭 | HandleTable → 对象 m_lock → port 锁。port 的 on_zero_handles 取 port 锁、放掉，再取对象锁 |

- **只有“对象锁 → port 锁”一个方向。** wait_one 只走“对象锁 → WaitQueue 锁”，不碰 port 锁，两条路径之间没有倒置。
- **登记必须在对象锁内。** 新增 `Dispatcher::attach_port_observer`，照 MakePortObserver（`zircon/kernel/object/dispatcher.cc:108-144`）先向 port 登记，再做电平检查。否则 on_zero_handles 可能在 observer 挂上之前就把它释放。
- **等待者睡在 sync::Event 上**，不新增原语：
  - wait_one 已经在对象锁下调 signal（`wait.cpp:26-28`），而且 Event 会锁存信号（`kernel/kernel/sync/event.cpp:126-128`）。
  - m_ready 只在 port 锁内 signal/unsignal，保持“置位 ⇔ 队列非空”：入队时 signal；出队取空、on_zero_handles 清队时 unsignal。如果像 Zircon 那样放锁后才 Post（`port_dispatcher.cc:327-329`），手动复位的 Event 会出现“队空却已置位”，等待者会空转：Event::wait 见到锁存值就直接返回（`event.cpp:56-62`）。
  - **Event::unsignal 要先修（K26）。** 它用裸 Guard 取 WaitQueue 的自旋锁（`event.cpp:77-80`），而这把锁本身什么也不关（`kernel/include/ours/sync/spin_lock.hpp:56-60`）。现有唯一的调用点在 IrqSave 锁里（`kernel/kernel/task/dpc.cpp:84-88`）；port_wait 是第一个开着中断调它的地方。持锁时被抢占，同一 CPU 上另一个 port_wait 进了 wait_etc 就会关着中断空转；P1-E 放开 deadline 之后，tick 里的 handle_timeout 也要取这把锁（`kernel/kernel/task/wait_queue.cpp:328-338`），会自锁。M1 就碰得到：K15 之后 port 句柄能复制（RIGHTS_DEFAULT_PORT 含 Basic，Basic 含 Duplicate，`system/public/ours/rights.hpp:56`），复制一份转给另一个进程，同一个 port 就有了两个等待者，各在自己的进程里。同一进程里还不会有第二个：K23 的 thread_create 只建首线程，thread_start 不在本计划里。所以 K26 不能假定只有一个等待者：Event 一次唤醒全部等待者，port_wait 醒来见队空就回去再睡（生命期表“等待中 port 被关”一行）。修法只有一行，所以在第一个线程上下文调用者出现时就修：改成和 signal 一样先关中断（`event.cpp:99-108`）。
  - 代价是每次唤醒全部等待者。出现第二个等待线程时，补完空壳 Semaphore（`kernel/kernel/sync/semaphore.cpp:4-8`）替换它（§7.8）。

**系统调用**

| 签名 | rights | 语义 |
|---|---|---|
| `Status ours_port_create(PortCreateOptions options, HandleValue *out)` | 无 | options 不是 None 回 InvArgs |
| `Status ours_port_queue(HandleValue port, PortPacket const *packet)` | Write | 只强制 type=User，status 等其余字段原样保留，同 Zircon 的 QueueUser（`port_dispatcher.cc:261-262`） |
| `Status ours_port_wait(HandleValue port, Instant deadline, PortPacket *packet)` | Read | blocking。deadline 只收 0 和无限，其余回 NotSupported（同 `wait.cpp:63-67`），到 P1-E 放开；队空时 poll 回 TimeOut |
| `Status ours_object_wait_async(HandleValue handle, HandleValue port, u64 key, Signals signals, WaitAsyncOptions options)` | handle: Wait；port: Write | 一次性、电平触发。在一次表锁内先解析 port，再解析 handle，然后查 options，最后才挂 observer |

- **wait_async 不得先挂后撤。** wait_one 是先挂上、再查 deadline、不对再摘下（`wait.cpp:55-67`）。wait_async 照做的话，条件已满足时包当场就入队了，而 M1 没有 port_cancel 能撤回它。Zircon 在取表锁之前就查 options（`zircon/kernel/lib/syscalls/object_wait.cc:34-42`）；ours 放在解析完两个句柄之后、挂之前，保留接口设计 §4.5 的错误优先级（BadHandle > BadType > AccessDenied > InvArgs）。
- **port_queue：M1 要。** init 不用它，但 K26 靠它单独验证队列，免得一个 bug 盖住另一个。
- **port_cancel：M1 不要。** M1 的 Loop 只在槽自己的回调里关它，那时 port 上既没有它的 observer，也没有它的包（§4.2）。
- **K26 同一提交修订接口设计**：§2.1 的 wait_many 改为 P2；§2.4 的等待者改为 Event，port_cancel 推迟，observer 上限含已入队的包，queue 保留 status；§4.8 去掉 WAIT_MANY_MAX_ITEMS；§5 的 P1-D 把 port 和 wait_async 挪到 P1-C（M1），wait_many 标为 P2；§6 的 Semaphore 一行不再是 port 的前置条件。

### 7.5 阶段四 文件系统 M1

| # | 提交标题 | 文件 | 改动 | 验证 | 依赖 |
|---|---|---|---|---|---|
| G3 | `refactor(abi): share the description front end as description.py` | `build/abi/description.py`、`gen_syscalls.py` | 抽出 §3.2 所列的共享部分；两个 action 都加 inputs | 五个输出与 G3 前一个提交的产物字节相同（阶段二的 G2 和十来个新系统调用已经改过它们，不能再拿 G1 之后的比）；两个 action 都已加上 description.py 的 inputs | G1 |
| G4 | `feat(abi): generate protocol code; add system/lib/wire and ours.io` | `build/abi/gen_protocols.py`、`protocol_library.gni`、`system/lib/wire`、`system/public/ours/io/*.gni` | 实现 §3。G 阶段支持：consts、bits、enum、定长 struct、一段尾部数组、必选句柄、one_way、two_way、event；回复句柄仅在 Ok 时出现 | gen_protocols_test.py；wire 的宿主测试喂畸形消息 | G3 |
| U10 | `feat(init): serve /boot` | `system/lib/loop`（新）、`system/service/init`；接口设计 | 新建 Loop 库（§4.2，port + wait_async），init 用它加上 BootServer、Connection 和 §2.4 的检查（不再隐藏任何条目：K12 交来的 VMO 里只有白名单条目）；§4.2 的两条 static_assert 落在这里。Supervisor 把 U7 的 wait_one(TERMINATED) 换成 Loop 上的一个槽。同一提交修订接口设计：§1 的现状列、§3.6 的 /svc 手写消息改为引用生成的 io 协议、§5 与 §8 把 IDL 从 P2 提前 | init 对自己的 /boot 连接自测：Open 并 Read `vdso` 的前 4 字节，得到 `\x7fELF` | K28、K14、G4、U3、U7 |
| U11 | `feat(fsio): a namespace, an fd table and /boot for the shell` | `system/lib/fsio`（新）、init、`examples/shell` | init 用 launcher 起 shell，processargs 带 NsDir /boot 和 VmaRoot；shell 用 fsio 读、列 /boot | `init: launched shell from /boot/bin/shell`、`shell: ls /boot: bin config userboot vdso`、`shell: open /boot/vdso with WriteBytes = AccessDenied`、`shell: raw Open("..") on /boot = BadPath`。services 那一行要到 U13 才有 | U10 |
| U12 | `feat(init): GetBackingMemory for /boot files, read-only` | init 的 BootServer、fsio | 从不可执行的那份 BootFS 为条目建 Slice（K20），再用 handle_duplicate（K15）把 rights 钉成 Basic\|Read\|Map\|GetProperty 后发出，关掉 Slice 本身：不依赖父句柄恰好不带 Execute。连接要有 ReadBytes（同 Rust VFS，`file/common.rs:73-77`）；options 只收 Read，Execute 按 §2.5 的保留位规则回 NotSupported。每个 grant 终身至多 `GRANT_MAX_BACKING_VMOS` 个，超了回 NoResource：发出的句柄记在 init 名下（K11），init 又看不到客户端何时关掉它，不设上限的话，任何持有 /boot 连接的客户端都能耗尽 init 的句柄配额（§4.2） | shell：对 `vdso` 取 backing memory，vma_map(R) 进自己的根 VMA，读出 `\x7fELF`；vmo_write 回 AccessDenied；带 Execute 回 NotSupported；第 GRANT_MAX_BACKING_VMOS + 1 次回 NoResource | U11、K20、K21、K15 |
| U13 | `feat(init): route /svc from a generated service manifest; add echo` | `build/service/gen_service_manifest.py`、system_service 与 service_manifest 模板、`system/lib/svc`、`examples/echo`、init 的 SvcRouter 与 Supervisor | 按 §4.3–4.4；svc::Outgoing 用 U10 的 Loop 同时等 o_s 和各条客户端 channel。Supervisor 按清单用 launcher 起每个服务，把进程句柄以 TERMINATED 挂进 Loop，记下退出码并把提供者标为不可用；o_c 的 PEER_CLOSED 是第二个信号，先到哪个都一样处理。生成器检查服务数 ≤ SERVICES_MAX（§4.2） | M1 判据全部行（§1） | U12 |

### 7.6 拉进来的接口：基本范围与推迟部分

原则见本节开头：接口以真实的签名和 rights 检查存在，基本功能经启动验证。下面每一项“推迟”都核对过，本计划里没有上层依赖它。

- **Dead 迁移、句柄关闭、VmAspace::destroy**（K4、K23、K6）：所有进入 Dead 的路径——kill 时没有线程、最后一个线程离开、Initial 的进程关掉最后一个句柄——都走同一个 finish_dead_transition，全量实现。
- **process_exit**（K5）
  - 基本：记退出码，进程走到 Dead。
  - 推迟：无。“杀其余线程”的代码写上，但没有多线程进程，跑不到。
- **TERMINATED 与 get_info**（K7、K24）
  - 基本：只有 Process 一个 topic，要 Inspect。
  - 推迟：其余 topic 回 NotSupported。
  - 核对：本计划只读退出码。§6.5 里的 HandleBasic 仍标为“以后”。
- **job_create、task_kill、thread_start、thread_exit**
  - 不拉进来。
  - 核对：没有步骤建 job，launcher 用传进来的根 job；没有步骤按句柄杀进程；每个进程只有一个线程。
- **handle_duplicate**（K15）：全量。
- **handle_replace**
  - 推迟。
  - 核对：GetBackingMemory 的 Slice 用 handle_duplicate 钉住 rights，再关掉原句柄（U12）；父进程持有子进程的全权句柄；launcher 不发 ProcSelf。
- **vmo_create、vmo_write**（K17）
  - 基本：Lazy 提交，options 只收 None。
  - 推迟：Resizable 回 NotSupported。
  - 核对：launcher 按段的固定大小建 VMO。
- **vmo_create_child**（K20）
  - 基本：只有 Slice；父对象可以是 Lazy，父子共用 VmCowPages 和它的锁。
  - 推迟：Snapshot、Reference、物理 VMO 的 Slice 回 NotSupported。
  - 核对：launcher 的可写段用 vmo_create 加 vmo_write（§4.6），不用 Snapshot；只对 BootFS 切片，而 BootFS 是整体提交的。
- **vmex 与 vmo_replace_as_executable**（K18、K19）
  - 基本：只有 System 种类的 VMEX 槽位。
  - 推迟：resource_create（P2）。
  - 核对：vmex 由内核直接铸造。
- **vma_map 与 vma_allocate**（K21、K22）
  - 基本：vma_map 收页对齐的 vmo_offset，Specific 或自动放置；vma_allocate 只收 Specific，不带 CanMap 位。
  - 推迟：unmap、protect、destroy，以及自动放置或带 CanMap 位的 allocate。
  - 核对：launcher 不做 unmap（§4.6）；所有段和栈按 Specific 放在固定基址，guard 页由 vma_allocate 占住；shell 的映射随进程退出由 K6 拆除。
- **process_create、thread_create、process_start、get_property**（K23）
  - 基本：options 只收 None；property 只有 ProcessVdsoBaseAddress。
  - 核对：只有 launcher（它的 Binder）读 property。
- **processargs、runtime、libc、launcher、userboot**：都是基本实现。
  - launcher 不支持共享库，因为程序只链接 vDSO。
  - userboot 不支持 `userboot.root`。
- **Port**：同 §7.4。
- **GetBackingMemory**（U12）
  - 基本：Read，每个 grant 有终身上限。
  - 推迟：Execute，要随 D6 的 Executable 标志在 M2 做。
  - 核对：本计划里能起程序的只有 userboot、init 和 dbg_1 的 shell，三者都直接持有 vmex 和 BootFS，不经 /boot 协议取可执行 VMO。
- **监督**（U7、U10、U13）
  - 基本：观察退出、记日志、标记提供者不可用。
  - 推迟：restart 和 lazy，生成器继续拒绝。
  - 核对：清单里两项都是 `start=eager restart=never`。

### 7.7 fault_probe 的迁移

fault_probe 只打进 dbg_1（`kernel/products/image/dev.dbg_1/BUILD.gn:44`），由 shell 驱动。它跟着依赖一路迁移，中间从不停用：

1. K14：探针名改走 processargs 的 argv[1]；答完不再原地自旋，而是从 main 返回，由 runtime 调 process_exit。
2. K23：用 `none` 探针做一次 launcher 冒烟；K24 也用它读回退出码。
3. U6：run_probes 改用 launcher，在进程句柄上等 TERMINATED，再用 get_info 读退出码；新增 pf_write_ro，共 12 个探针（写 vDSO 的 pf_write_vdso 已随 X1 的修复加入）。
4. U8：dbg_1 的 cmdline 从 `kernel.select.userboot=shell` 改为 `userboot.next=shell`。shell 从 userboot 手里拿到根 job、BootFS 和 vmex，地位与今天由内核直接起、持有根 job 时相同。（cmdline 与根 job、BootFS 的转交已做，2026-09-26，`ccc7847a`；vmex 待 K18。）
5. K25 排在 U6 之后，删除时已没有任何调用者。
6. U9：cmdline 改为 `userboot.next=bin/shell`，shell 起的是 `bin/fault_probe`，名字由构建生成。

初稿里另一个方案否决了（§9）：dbg_1 改为起 init，由一个测试程序驱动 fault_probe。测试程序拿不到可执行 VMO：GetBackingMemory 的 Execute 在 M2，而把 vmex 交给它会扩大可信基。代价是 dbg_1 在 M2 之前不在 DEBUG_ASSERT 下跑 init。

### 7.8 M2（依赖以上四个阶段）

- **port 的补充**：port_cancel（从别处关一个在挂的槽要用它，例如撤销 grant）、Timestamp/Edge 选项；出现第二个等待线程时，补完 Semaphore，换掉 m_ready 上的 Event（§7.4）。
- **进程监督**：reaper、restart 和 lazy 启动。它们的前提 TERMINATED、get_info(Process)、VmAspace::destroy 已在阶段一、二落地。
- **handle_replace**。
- **vmo_create_child 的其余种类**：
  - Reference 子 VMO 的 rights 等于父句柄的 rights，永远不加 Write。
  - Snapshot 类的子 VMO 按 Zircon 加 Write、去 Execute（`zircon/kernel/lib/syscalls/vmo.rs:329-349`）。
  - 加一条内核测试：对带 Execute 的 VMO 做 create_child(Snapshot)，结果不带 Execute。
- **vma 的其余调用**：unmap、protect、destroy；allocate 的自动放置与 CanMap 位。其中 unmap、protect、destroy 落地时要接上 mm 已有的封印检查（`VmArea::unmap()`、`protect()` 遇到 `VmaFlags::Sealed` 的映射回 AccessDenied；`vma_destroy` 要先检查区域里有没有封印的映射），否则用户程序可以拆掉封印的 vDSO、在原地址映射自己带 `syscall` 的代码，绕过“只有 vDSO 能发系统调用”的检查（接口设计 D2）。
- **BootFS 格式升级**（§6.2）。
- **GetBackingMemory 的 Execute**：
  - 连接要有 Execute，条目也要带 Executable 标志，才授予 Execute（参照 `file/common.rs:72-87`）；
  - 返回的 VMO rights 在 Basic\|Read\|Map\|GetProperty 之上，按上一条再加 Execute；
  - 永不给 Write、SetProperty、Resize；
  - 不允许任何“发出整个 BootFS VMO”的过渡方案。
- **channel_read_etc**：补上句柄类型和 rights 的校验。
- **grant 撤销**、export_namespace、64 KiB 消息。

## 8 需要主人拍板的决策

| # | 问题 | 选项 | 推荐 |
|---|---|---|---|
| D1 | M1 的边界 | **已定（2026-09-25）：依赖先行**（加载器、userboot、删除 spawn_boot 排在文件系统之前）。M1 跑在 kernel → userboot → init 上，init 经 launcher 从 /boot 起服务并观察退出，GetBackingMemory 的 Read 有真实实现；每个依赖做到“基本但真实”（§7） | — 。否决原推荐 A（只做原 M1 的 22 步，spawn_boot 起程序，GetBackingMemory 回 NotSupported）：它让文件系统建在自己依赖的替身上。主人原话：“你应该先把依赖项给做完再做这些事情…否则不就变成本末倒置了，至少你得保证接口的存在和基本功能实现，只是不需要你全量的实现接口” |
| D2 | userboot 的时机 | **已定（2026-09-25）：依赖先行**（加载器、userboot、删除 spawn_boot 排在文件系统之前）。U8 在阶段二落地，M1 跑在 kernel → userboot → init 上（原选项 B） | — 。否决原推荐 A（M1 由内核直接起 init，U8 再插入 userboot）：理由同 D1。C（永远不做 userboot）早已否决（§9） |
| D3 | 可执行权从哪来 | **已定（2026-09-25）**：可执行是一项能力。vmo_create 与 BootFS VMO 都不带 Execute；Execute 只能经 `vmo_replace_as_executable(vmo, vmex)` 取得，换时去掉 Write；vmex 是 System 资源槽位，内核交给 userboot，由它转交给 next（U8）。vma_map 接受页对齐的 vmo_offset，launcher 从换出的 BootFS VMO 直接映射只读段和可执行段。撤回接口设计 §4.2、§9“vmo_create 临时带 Execute”的偏离 | — 。否决的两种：vmo_create 带 Execute（任何进程都能把同一个 VMO 先按 W、再按 X 各映射一次，单个映射上的 W+X 检查挡不住）；按来源授予（只有内核铸造的 BootFS VMO 带 Execute，是特例而不是能力，见 §9） |
| D4 | M1 的多路复用 | **已定（2026-09-25）**：M1 直接上 Port（原选项 B）。init 和 echo 共用的 Loop 建在 port + object_wait_async 上（§4.2），内核步骤为 K26、K27、K28（§7.4）；等待者先用 sync::Event，Semaphore 不再是前置条件；object_wait_many 推迟到 P2 | — 。否决的原推荐 A（object_wait_many 加 64 项预算）：连接池只有 56 槽，出现 shell 和 echo 以外的程序之前还得再换一次 |
| D5 | ordinal 方案 | **已定（2026-09-25）：哈希 ordinal 加 selector，不做显式编号**。SHA-256 哈希加可选的 selector，照 fidlc（§3.1）；M1 不用分配库号，也不检入 golden，改名时写 selector 钉住编号 | — 。否决 B（显式编号 `ordinal_base << 32 \| method`，由 GN metadata 查全构建唯一，另检入 golden）：主人认为“直接编号太危险”。它唯一的长处是十六进制转储里看得懂 |
| D6 | BootFS 的 mode、uid、gid 什么时候落盘 | A：M1 由 init 合成；M2 随 GetBackingMemory 的 Execute 升级 BootFS 格式，从 GN metadata 填入真实值和 Executable 标志。B：M1 就升级格式 | **已定（2026-09-25，按推荐）：A**。M1 不外发任何可执行物，而格式改动会牵动 phys、KernelPackage 和打包器。“盘上 mode 从第一天就存”这一条，从第一个可写文件系统起严格执行 |
| D7 | /boot 与内核包是否分开 | A：M1 由 init 隐藏构建生成的私有条目；长期在 OBI 里加一个独立的用户 BootFS 条目。B：现在就分开 | **已定（2026-09-25，按推荐）：A**；**同日修订**：隐藏改为内核在 K12 按黑白名单过滤，只拷白名单条目（init 隐藏挡不住持有原始 VMO 的程序，见 §4.1） |
| D8 | 连接权限类型的名字 | IoRights 或 FsRights | **已定（2026-09-25，按推荐）：ours::io::IoRights**。/svc 这类非文件系统节点也用它；与句柄的 `ours::Rights` 在 GetBackingMemory 这类方法里会同时出现，名字不同才不会混淆 |
| D9 | 路径的编码 | A：字节串，只禁 NUL 和 '/'。B：照 fuchsia.io 要求 UTF-8 | **已定（2026-09-25，按推荐）：A**。服务端逐段按字节比较，用不着解释编码 |
| D10 | kernel/lib/bootfs 挪空之后是否删除 | 删除，或者保留为转发 | **已定（2026-09-25，按推荐）：删除**。它只剩转发，没有独立的职责（§4.5） |

## 9 被否掉或推迟的方案

| 方案 | 出处 | 处置 | 理由 |
|---|---|---|---|
| 消息头用 `{u16 flags; u16 wire_version}` | protocol | 否决 | 统一为 FIDL 的形状加 direction 字段；版本由 magic 承担 |
| 句柄不在消息体里占槽，接收方要求句柄数恰好相等 | protocol | 否决 | 表达不了“只在 Ok 时带句柄”，以后也做不了可选句柄 |
| 显式 ordinal 加 golden | generator、一致性评审 | 否决（2026-09-25） | D5 定为哈希加 selector；主人认为直接编号太危险 |
| OpenOptions 的低 16 位放 rights，另加 INHERIT_WRITE/EXECUTE | permissions | 否决 | rights 单独成一个字段；继承会让实际授予的权限随父连接悄悄变化 |
| 照搬 fuchsia.io 的 R_STAR_DIR、RX_STAR_DIR | protocol | 否决 | 它们含 Connect，与节点能力检查冲突 |
| M1 只做子集检查、不查节点能力 | 闭环评审 | 否决 | 只多两行，与 Rust VFS `file/common.rs:25-33` 一致；常量不含 Connect 之后，它不会误伤 /boot |
| 路径上限 4095、传输上限 8192、按 CHANNEL_MAX − 32 截断 | protocol、permissions | 否决 | 装不进 4 KiB 消息；消息升到 64 KiB 后再放大 |
| Read 上限 2048 | kernel | 否决 | MAX_TRANSFER 由消息上限推出 |
| 所有回复都以 `{status; u32 reserved}` 开头 | 闭环评审 | 部分采纳 | 只用作错误形态；Ok 回复按无隐式填充排列 |
| M1 就做 GetBackingMemory | protocol、generator | 部分采纳（2026-09-25） | Read 进 M1（U12）：Slice（K20）加 handle_duplicate（K15）钉住 rights，用不着 handle_replace；Execute 要新的 BootFS 格式（D6），留在 M2 |
| 用整个 BootFS VMO 充当过渡的 GetBackingMemory | — | 否决 | 越出单个文件的范围 |
| init 用 vma_map 映射 BootFS | boot、protocol | 推迟 | vmo_read 已经够用：BootServer 按请求拷贝，用不着整块映射 |
| M1 的 Loop 用 port + wait_async | protocol、boot | 采纳（2026-09-25） | 见 D4、§7.4 |
| 连接池 256 槽、每 grant 上限 256 | boot、permissions | 部分采纳（2026-09-25） | 池扩到 256 槽（§4.2），port 没有 wait_many 那样的 64 项上限；每 grant 上限仍是 16，否则一个 grant 就能占满整个池 |
| object_wait_many 进 M1 | 初稿 | 推迟到 P2 | M1 的多路复用改用 port；wait_many 的使用者是 poll/select（`sdk/lib/fdio/unistd.cc:1980、:2084`），等 fsio 做 poll 时再加 |
| syscall 描述里 release 的放宽单独成一个提交 | Port 设计、Port 评审 | 并入 K10 | 删去 inout 之后只剩这一处，它只是供校验的标注，渲染器不读，也只有 K10 的 channel_write 用它 |
| M1 删去 port_queue，K26 只做 create 和 wait | Port 评审 | 否决 | K26 靠它单独验证入队、出队和清队，免得 K27 的 observer 和队列的 bug 互相掩盖。它带来的偏离已消除：status 改为照 Zircon 原样保留（§7.4） |
| port_cancel 进 M1，Loop 提供 remove(key) | Port 评审 | 推迟到 M2（§7.8） | M1 的 Loop 只在槽自己的回调里关它，那时 port 上没有它的 observer，也没有它的包，不会留下过期包；要从别处关在挂的槽，只有撤销 grant，M1 没有 |
| static_assert(2 × LOOP_SLOTS ≤ PORT_MAX_OBSERVERS)，按“每槽一个在挂 observer 加一个过期包”留余量 | Port 设计 | 更正 | 没有机制保证“至多一个过期包”；改为从结构上排除过期包，断言收紧为 LOOP_SLOTS ≤ PORT_MAX_OBSERVERS（§4.2） |
| 转移句柄时把配额迁给接收方 | Port 评审 | 否决 | 与接口设计 §4.8“记在创建它的进程名下”相反；改为在 §4.2 静态核对 init 自己创建的句柄数 |
| Event::unsignal 的修复单独成一个提交 | — | 并入 K26 | 修法只有一行，而且 port_wait 是它第一个开着中断的调用者；K26 的 changes 记录里单列一节 |
| Reaper、restart=on-failure、lazy 启动 | boot | 部分采纳（2026-09-25） | 观察退出进 M1：TERMINATED、get_info 和 destroy 在阶段一、二落地，Supervisor 记下退出码、标记提供者不可用（U7、U10、U13）；restart 和 lazy 仍推迟，M1 的清单用不到，生成器继续拒绝 |
| 子进程退出即撤销它的 grant | permissions | 否决 | 会误伤它派生出去的后代连接；靠 PEER_CLOSED 回收已经足够 |
| 每端点 `CHANNEL_MAX_PENDING_HANDLES` | kernel | 否决 | 被 K11 的每进程记账取代；只限单个端点挡不住 arena 被耗尽 |
| 把按进程记账推迟 | permissions | 否决 | 接口设计 §4.8 规定它与句柄转移同批落地，K11 照做 |
| handle_duplicate/replace、vmo_create_child 进 M1 | permissions、generator | 部分采纳（2026-09-25） | duplicate（K15）和 Slice（K20）进 M1：replace_as_executable 消耗原句柄，要先复制才留得下不可执行的一份，GetBackingMemory 的 Read 要 Slice；handle_replace 和 Snapshot 仍推迟，本计划里没有使用者 |
| channel_read_etc 提前到 P1-C | generator | 推迟到 GetBackingMemory 之前 | M1 只核对句柄个数 |
| vmo_create 带 Execute；vma_map 只收 vmo_offset=0 | kernel | 否决 | 见 D3 |
| Execute 只来自内核铸造的 BootFS VMO（初稿的 D3-A） | 综合稿 | 否决（2026-09-25） | 按 VMO 的来源授予，而不是按持有的能力；换成 vmex，谁能执行由谁持有 vmex 决定 |
| vmo_replace_as_executable 保留 Write（Zircon 的语义） | — | 推迟 | 只有 JIT 需要；没有 JIT 之前去掉 Write，让可执行句柄不能再建可写映射 |
| 加载器用“映射进 launcher → memcpy → unmap” | boot、接口设计 §3.2 | 否决 | 改为 vmo_write 加直接映射，省掉依赖 TLB shootdown 的 vma_unmap |
| 不做 userboot | boot | 否决 | 见 D2 |
| 按 bin/、lib/ 路径前缀判断是否可执行，并据此合成 mode | protocol、boot | 否决 | 判据来自构建生成的条目标志（M2）；M1 合成的 mode 不区分可执行 |
| 过渡期把一条裸的 /boot 连接当作 rdi | boot | 否决 | 违反接口设计 §3.3 |
| /svc 失败时写 Unavailable | boot | 否决 | Status 里没有这一项（`status.hpp` 止于 `:109` 的 BufferTooSmall = 33），而且 s 此时已被消耗 |
| 路径错误回 InvArgs | permissions | 否决 | 统一回 BadPath（`openable.fidl:16`） |
| 结尾的 '/' 视为 ProtocolDirectory | protocol | 否决 | `io.fidl:41` 禁止结尾的 '/'；由 fsio 剥掉并置 ProtocolDirectory |
| 把“拒绝开头的 '/'”列为偏离、请主人拍板 | protocol、permissions、一致性评审 | 撤销 | 这不是偏离，`io.fidl:40` 本来就禁止 |
| 保留 build/syscall 这个目录名 | 闭环评审 | 否决 | 协议生成器放在 syscall 目录下名不副实；改名只是一次纯移动的提交 |
| G1 的验收标准写成“七样产物字节不变” | 一致性评审 | 更正 | gen_syscalls.py 只写五个文件（`system/public/BUILD.gn:63-69`），而且 banner 行必然会变 |
| 预留 Reauthenticate 的序号 | permissions | 撤销 | D5 定为哈希 ordinal，不需要预留号段 |
| M1 就做 export_namespace 和 Fd 0x30 | protocol | 推迟到 M2 | M1 的 shell 不起子进程；它要起子进程得有可执行 VMO，那要等 GetBackingMemory 的 Execute |
| 解码器在 v1 校验句柄的对象类型 | permissions | 更正 | v1 做不到，按 §2.5 的表述处理 |
| M1 就做 revoke(grant) | permissions | 推迟到 P1-B 之后 | 触发事件（注销）在 M1 不存在 |
| 建 Tree 时跳过写死的 "vmours" 常量 | 闭环评审 | 改用别的做法 | 目录名是 phys 的启动选项，init 无从得知；而且 init 里隐藏挡不住持有原始 VMO 的程序。定为内核在 K12 按黑白名单过滤（2026-09-25，§4.1） |
| M1 借 process_spawn_boot 起 shell 和 echo（原 D1-A） | 初稿 | 否决（2026-09-25） | 依赖先行（D1）：文件系统不建在替身上；spawn_boot 在 K25 删除，排在文件系统之前 |
| M1 由内核直接起 init，U8 再插入 userboot（原 D2-A） | 初稿 | 否决（2026-09-25） | 依赖先行（D2）：M1 跑在 kernel → userboot → init 上 |
| GetBackingMemory 在 M1 回 NotSupported | 初稿 | 否决（2026-09-25） | 依赖先行：Read 有基本的真实实现（U12），只有 Execute 留到 M2 |
| dbg_1 改为起 init，由一个测试程序驱动 fault_probe | 初稿 | 否决 | 测试程序拿不到可执行 VMO：GetBackingMemory 的 Execute 在 M2，而把 vmex 交给它会扩大可信基。代价是 dbg_1 在 M2 之前不在 DEBUG_ASSERT 下跑 init（§7.7） |
| K19 用去掉权限的 vmex 副本做反向检查，期待 AccessDenied | 依赖先行评审 | 否决 | vmex 句柄本身不要求任何 right：Zircon 只经 validate_ranged_resource 查它是否覆盖 VMEX 槽位的 System 资源（`zircon/kernel/lib/syscalls/vmo.rs:367-368`、`zircon/kernel/object/resource.rs:159`）。K19 的反向检查改用 channel 和 BootFS VMO（BadType）以及无效值（BadHandle） |
| 线程退出时释放它的内核栈 | K6 的帧数核对 | 推迟（K6 同一提交在 docs/repairs 登记） | 内核栈是内核 aspace 里的映射（`kernel/mm/stack.cpp:23-55`）；~Stack 只丢掉自己那份引用，映射仍挂在内核根 VMA 上，拆它要 TLB shootdown。K6、U6 把每个进程一个内核栈当作已知的差值 |

## 10 与 Fuchsia/Zircon 的偏离汇总

| 条目 | Fuchsia/Zircon | ours | 理由 |
|---|---|---|---|
| 线格式 | 完整的 FIDL 编码：out-of-line、envelope、table | 定长部分加至多一段尾部；NodeAttributes 用有效位掩码代替 table | 编解码只需要做边界检查 |
| 消息头 | at_rest_flags[2] + dynamic_flags | direction 字段 + reserved | 让请求和回复在线上可以区分，防自环 |
| 畸形消息与未知 ordinal | LLCPP 服务端直接解绑关闭，epitaph 只在显式 Close 时发（`sdk/lib/fidl/cpp/wire/async_binding.cc:451-455`） | 先发 epitaph（InvArgs 或 NotSupported）再关 | 客户端能分辨是哪一种违规 |
| 尺寸上限 | 路径 4095，单次传输 8192 | 路径 1024，单次传输 4072 | 消息只有 4 KiB；升到 64 KiB 后放大 |
| Open 的 rights | 与 Flags 同在一个 u64 里，带 PERM_INHERIT_* | 单独的 IoRights 字段，没有继承位 | 实际授予的权限不随父连接悄悄变化 |
| rights 别名 | R_STAR_DIR 等都含 CONNECT | 不含 Connect，另有 SVC_DIR_RIGHTS | 与节点能力检查一致 |
| 路径编码 | FIDL string，必须是 UTF-8 | 字节串 | D9 |
| mode/uid/gid | 只存不解释（`node.fidl:151-154`） | v1 只存；以后按 DAC 解释，而且只会减权 | 多用户方向 |
| 可执行判据 | BootfsSvc 按目录名判断（`bootfs.rs:44-53`），再经 vmex 换成可执行 | 可执行权同样经 vmex 换得；launcher 按 PF_X 决定哪些段按 X 映射，GetBackingMemory 按构建生成的条目标志（M2） | 判据来自构建，而不是路径约定 |
| vmo_replace_as_executable | 在原 rights 上加 EXECUTE，保留 WRITE（`vmo.rs:382-385`） | 加 Execute、去 Write | 没有 JIT 之前，可执行句柄不应再能建可写映射 |
| vmex 的来源 | 内核把整个 system resource 交给 userboot，userboot 用 resource_create 派生 vmex（`src/bringup/lib/userboot/rust/src/program.rs:102-112`） | 内核直接铸 Vmex 槽位交给 userboot，按 arg 区分；userboot 转交给 next（dev 是 init，dbg_1 是 shell） | 还没有 resource_create（P2）；有了之后改为交整个 system resource、由持有者派生 |
| 每文件 VMO | `Vmo::create` + `transfer_data`（`bootfs.rs:204-220`） | M1 用 Slice 子 VMO（U12），rights 由 handle_duplicate 钉住 | ours 没有 transfer_data；BootFS 已整体提交且不可写 |
| Slice 子 VMO | 子句柄 rights 是父句柄 rights 去掉 Resize，再加 GET_PROPERTY 和 SET_PROPERTY（`vmo.rs:329-332`）；锁在 VmCowPages 里，父子共用（`zircon/kernel/vm/include/vm/vm_object_paged.h:49-54`） | rights 等于父句柄 rights，不加 SetProperty；锁同样在 VmCowPages 里，父子共用 | 外发的 VMO 永不带 SetProperty（K20） |
| GetBackingMemory 返回的 VMO | 按 VmoFlags 可以带 WRITE（`file/common.rs:40` 起的 `vmo_flags_to_rights`） | 永远不带 Write、SetProperty、Resize | 不外发可写 VMO |
| /boot 由谁托管 | component_manager 的 BootfsSvc | init 内置 | 主人的决定 |
| /svc 路由 | 组件拓扑加 capability routing | 清单里的静态允许表，按客户端合成目录 | 不做组件框架 |
| 多路复用 | port + wait_async，另有 wait_many | 同样用 port + wait_async；M1 没有 port_cancel，wait_many 推迟到 P2 | D4；§7.4 |
| port 的 observer 上限 | 只数在挂的（`port_dispatcher.cc:460、:491`），超限抛策略异常（`:490-495`）；已入队的 signal 包不计，反复 wait_async 可以无限堆包 | 在挂的与已入队的一起计，超限回 NoResource | 包嵌在 observer 里，只数在挂的就等于不限包数 |
| port 的用户包上限 | 每个 port 4096（`port_dispatcher.cc:61、:345-351`），超限抛异常 | 256，回 ShouldWait | 同接口设计 §2.4 |
| port 的等待者 | Semaphore，放锁后 Post（`port_dispatcher.cc:327-329`） | 手动复位的 sync::Event，只在 port 锁内 signal/unsignal | Semaphore 还是空壳；Event 放锁后才 signal 会出现“队空却已置位”。出现第二个等待线程时换回 Semaphore（§7.8） |
| userboot | 在用户态解压 ZBI，然后起 component_manager | 有 userboot（U8），next 由 `userboot.next` 选；BootFS 由内核解压 | D2 |
| 句柄转移时取消等待 | 在接收端取消（`channel.cc:166`） | 在发送端摘除句柄时取消（`handle_table.cpp:61-67`） | 效果等价，只是更早 |
| 句柄数量限制 | 靠 job policy 和内存压力兜底 | 每进程上限，在途句柄也计入 | 同接口设计 §9 |
| 内核包与用户 BootFS | 两类 ZBI 条目 | 共用一个包，内核按黑白名单只把用户条目拷进 BootFS VMO | D7 |
