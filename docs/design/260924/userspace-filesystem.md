# ours 用户空间文件系统：设计（2026-09-24）

> 输入：protocol、generator、boot、permissions、kernel 五个方向的设计，加一致性、安全、闭环三份评审。主人已定四条：fuchsia.io 子集；/boot 与 /svc 先内置在 init；协议代码用生成器；v1 纯能力、形状为 DAC 预留。评审的每一条要么落进正文，要么在第 9 节写明否决理由。代码引用在 HEAD（70b7a4da）上复核过，Fuchsia 引用在本地 checkout 上复核过，没核到的标“未核实”。本文建立在 `docs/design/260923/userspace-interface.md`（下称“接口设计”）之上。它 §1 的“现状”一栏已经过时：HandleTable 和 Channel 已实现。

## 1 总览

**文件句柄与协议。** 每次 Open 都产生一条新 channel，即一条连接。协议取 fuchsia.io 的子集：Open、Close、GetAttributes、ReadDirents、Rewind、Read/ReadAt/Seek，外加事件 OnRepresentation。GetBackingMemory 已写进协议描述，到 M2 才实现。

线格式沿用 FIDL 的消息头形状，但消息体只有定长部分加至多一段尾部，不做 out-of-line 编码。消息类型、编解码、客户端桩和服务端分派都由生成器从 GN scope 描述生成，与系统调用生成器共用前端。

**启动与托管。** 目标链路是 kernel → userboot → init。M1 由内核直接起 init（D2）：内核给 init 发一条 processargs，里面带根 job 和只读的 BootFS VMO。init 在进程内托管 /boot 和 /svc，读 `/boot/config/services`，为每个服务准备一对 outgoing channel 和一组 NsDir，然后启动它。M1 起程序暂时借用 `process_spawn_boot`，到 B 段换成用户态 launcher，并删掉它。

**服务访问。** 进程能访问的目录，只有 processargs 交给它的那些 NsDir。init 为每个子进程铸一条专属的 /svc 连接，连接的节点是一个合成目录，子节点就是清单允许它使用的协议。客户端 Open(name) 时，init 把它的 channel 原样转交给提供者的 outgoing 目录，此后两端直接通信，init 不留任何状态。

**权限。** v1 是纯能力模型：
- Open 请求的 rights 必须同时是连接 rights 的子集和节点能力的子集，只缩不扩；
- 没有环境根，服务端永远看不到绝对路径；
- 连接记录现在就带 principal（v1 恒为 0）和 grant，rights 拆成 cap_rights 与 effective 两个字段，为 DAC 留好形状；
- 永不外发可写 VMO。

**第一个里程碑 M1**：两个镜像都能启动，dev 镜像的日志依次出现下面几行。

```
init: bootfs has <n> entries; serving /boot
shell: /boot/config/services: 2 services
shell: ls /boot: config echo init shell userboot vdso
shell: open /boot/vdso with WriteBytes = AccessDenied
shell: raw Open("..") on /boot = BadPath
shell: /svc/ours.echo.Echo says "hello"
shell: /svc/ours.nothing = NotFound
```

M1 共 21 个提交：11 步内核、4 步生成器、6 步用户侧（§7）。以下内容不在 M1 里，分别推到 B 段或 M2：用户态加载器、GetBackingMemory、子进程监督与重启、port、撤销。

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
| File.GetBackingMemory | two_way | `BackingMemoryOptions options(u32); u32 0` | `u32 vmo@20`（句柄） | Read 需 ReadBytes，Execute 另需 Execute | 描述在 M1，实现在 M2 |
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
- **ordinal**（推荐方案，见 D5）：
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
- **syscall 生成器还要为内核步骤补两样**（G2）：
  - `dir = "inout"`，给 wait_many 的 items 用。今天 `gen_syscalls.py:41` 只认 in/out；`UserInOutPtr` 已经有了（`kernel/lib/user_copy/include/user_copy/user_copy.hpp:58`）。
  - `release` 放宽到 HandleValue 数组。今天 `gen_syscalls.py:97-98` 只收按值传递的句柄；release 只是供校验的标注，渲染器不读它。
  - 结构体指针参数已经支持（`gen_syscalls.py:247-262`），不用改。
  - 7 参数的桩（`:35-38`）留到 B 段 vmar_map 时再加。

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
- **ABI 稳定性**：M1 不检入 golden，因为所有使用者都出自同一次构建。等第一次出现跨构建互通（持久的 /data，或树外程序）时，再照 Fuchsia 的做法（检入的 `sdk/fidl/fuchsia.io/fuchsia.io.api`）加检入摘要和比对。如果 D5 选了显式编号，golden 必须从第一天就有。

## 4 启动链与 init 内置的 /boot、/svc

### 4.1 M1 时序

| # | 谁 | 做什么 | 之后谁持有什么 |
|---|---|---|---|
| K | 内核 | 常驻内核包，建 BootFS VmoDispatcher：整体提交，rights = Basic\|Read\|Map\|GetProperty。建 bootstrap channel，写一条 processargs：JobDefault（根 job）、VmoBootfs，argv = `init`。然后 `userabi::launch("init")`，rdi 为 bootstrap 端 | init：根 job、BootFS VMO |
| I1 | init | runtime 解析 processargs。用 vmo_read 把 Header、Entry[] 和名字表读进 bump 缓冲，建 `bootfs::Tree`，同时隐藏内核私有条目。在进程内直接读 `config/services` | init：Tree、清单 |
| I2 | init | 按清单顺序处理每个服务：建 (o_c, o_s)；为每个 uses 铸一条根连接，各带一个新 grant；建 (b_c, b_s)，在 b_c 上写 processargs（NsDir、DirectoryRequest = o_s）；调 `process_spawn_boot(job, binary, b_s)`；关掉 b_c | 子进程：NsDir、o_s。init：o_c、各连接的服务端 |
| S1 | shell | fsio 按最长前缀选中 /boot，写 `Open("config/services", ProtocolFile\|Describe, R_STAR_DIR 的子集, s)`，等 OnRepresentation | shell：c。init：s 的连接记录 |
| S2 | shell | `fsio::connect("/svc/ours.echo.Echo")`，过程见 §4.4 | c 在 shell，s 在 echo，init 不留状态 |

- **spawn_boot 在 M1 延期删除。** 它的删除条件仍按接口设计 §2.8。它的 bootstrap 参数本身是 channel，第一条消息就是 processargs，与接口设计 §3.3 一致。
- **清单的 binary 字段由生成器填写**，取 `user_executable` 的实际安装名（`system/user_executable.gni:41` 的 install_name）：M1 是 `echo`，B 段挪到 `bin/` 之后自动变成 `bin/echo`。spawn_boot 拒绝含 '/' 的名字（`kernel/lib/syscall/src/process.cpp:67-73`），所以挪目录必须排在删除 spawn_boot 之后。
- **内核私有文件**：
  - dev 镜像的包里有 `vmours/image.elf`（`kernel/products/image/dev/BUILD.gn:21-22`），而内核带 KASLR（`kernel/phys/physboot.cpp:144-148`），所以内核映像不能暴露给普通进程。
  - M1 由 init 在建 Tree 时隐藏所有不是 user 工具链产出的条目。
  - 这份隐藏清单由构建生成为 `config/boot_private`：从 kernel_package 汇总的 distribution 条目里挑（条目带 label，label 含工具链；机制见 `build/dist/distribution_manifest.gni`）。不能写死 "vmours"，因为内核目录名是 phys 的启动选项 `kernel.name`（`kernel/phys/include/ours/phys/boot_options.hpp:66`），init 无从得知。
  - 长期把用户 BootFS 拆成独立的 OBI 条目（D7）。Fuchsia 的内核包和用户 bootfs 就是两类条目：STORAGE_KERNEL 与 STORAGE_BOOTFS（`zircon/kernel/phys/kernel-package.cc:65`）。

### 4.2 init 的组成

```cpp
class Init {             // composition, no base class
    Loop loop;           // driven by object_wait_many; its interface has the shape of a port
    BootServer boot;     // /boot
    SvcRouter svc;       // /svc
    Supervisor services; // manifest and start-up; no supervision in M1
};
```

- **Loop**
  - 接口：`add(handle, signals, fn, ctx) -> Key`、`remove(key)`、`run()`。回调是函数指针加上下文，不用虚函数。
  - 每一轮从登记表重建 WaitItem 数组，调用 `object_wait_many`，上限 64 项（同 `zircon/system/public/zircon/types.h:199`）。
  - 以后换成 port + wait_async 时，只改 Loop 内部，对外接口不变。
- **预算**
  - `CONTROL_SLOTS + CONNECTION_POOL = 64`：CONTROL_SLOTS 为 8，给各服务的 o_c；CONNECTION_POOL 为 56。两式用 static_assert 钉住。
  - 每个 grant 最多 `GRANT_MAX_CONNECTIONS = 16` 条连接。
  - 清单生成器做静态检查：根连接数 × 16 必须 ≤ 56，否则拒绝生成，并提示先换 port。
  - 池满或配额用尽时，在新的 server end 上发 epitaph NoResource，然后关闭。
- **o_c**
  - init 在 o_c 上等 READABLE | PEER_CLOSED。
  - 提供者不应往 o_c 写任何东西。一旦收到入站消息就关掉 o_c，防止提供者往里停 64 条消息、每条带满句柄。
  - PEER_CLOSED 表示提供者已经死了；这一点要等 K10 之后才成立。
- **BootServer**
  - 连接池是定长的，因为 runtime 只有 bump 分配器。
  - Read/ReadAt 直接 vmo_read 进已清零的回复缓冲。
  - NoResource 只算单个请求失败，从不 assert。
- **只写不等**：init 对子进程只写消息、从不等回复。否则一个卡住的服务就能把 init 卡死。
- **子进程退出**：K10 之后，内核会关掉死进程的整张句柄表。init 在它的每条连接上看到 PEER_CLOSED，据此回收记录。退出不触发按 grant 撤销（§6.3）。

### 4.3 服务清单

`/boot/config/services` 是文本，一行一个服务，由 `//build/service/gen_service_manifest.py` 从 GN metadata 生成：

```
# generated by //build/service/gen_service_manifest.py; do not edit
service echo  binary=echo  start=eager restart=never provides=ours.echo.Echo uses=/boot:r
service shell binary=shell start=eager restart=never uses=/boot:rx,/svc/ours.echo.Echo
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
  - **在 TERMINATED 和 VmAspace::destroy 落地之前，拒绝 start=lazy 和 restart≠never。** 理由：M1 起程序用的 spawn_boot 不返回进程句柄，init 无从监督；lazy 的提供者每被连一次就要重起一次，而今天进程死后地址空间不回收，每次都泄漏一个。

### 4.4 /svc 转发

1. **shell 发起连接。** 调 `channel_create(c, s)`，在自己的 /svc 连接上写 `Open("ours.echo.Echo", ProtocolService, Connect, s)`。之后可以立刻在 c 上发请求，它们会在通道里排队。
2. **init 查找。** 在这条连接的合成目录里做普通查找。协议不在允许表里，就回普通的 NotFound epitaph。ReadDirents 同样只列出允许的协议。
3. **init 转发。** init 自己拼出 `svc/ours.echo.Echo`，带固定的 ProtocolService 和 Connect，写进提供者的 o_c；s 随这条消息转走。客户端给的 path 和 options 不透传。这与 component_manager 的 `OutgoingConnector::send` 相同：它对 SubNode 只打开 "."，也只带 PROTOCOL_SERVICE（`src/sys/component_manager/src/model/component/instance.rs:801-820`）。
4. **提供者不可用。**
   - init 已知提供者死了（o_c 已经 PEER_CLOSED）：在 s 上发 epitaph ConnectionRefused（`status.hpp:87`）。
   - 转发时写 o_c 失败：s 已经随那条失败的消息被关掉了，因为句柄一进 channel_write 就被消耗（接口设计 §2.4）。客户端只会看到一个没有 epitaph 的 PEER_CLOSED。
   - fsio 在 /svc 连接上把这两种情况都报告为 ConnectionRefused，具体原因写进 init 的日志。
5. **提供者一侧。** `system/lib/svc` 的 `svc::Outgoing` 取出 DirectoryRequest 句柄，只接受 `Open("svc/<name>", ProtocolService, ...)`，然后按名字调用注册好的 `on_connect(ou::Channel)`。对照 `component::OutgoingDirectory::ServeFromStartupInfo`（`sdk/lib/component/outgoing/cpp/outgoing_directory.h:127`）。

### 4.5 BootFS 库

- **挪到 system/lib**：`kernel/lib/bootfs` 的 format.hpp 和 view.hpp 挪到 `system/lib/bootfs`，目录包含 include/、src/、test/、.gitignore、.clang-format。
  - builder.hpp 用了 std::string 和 std::vector（`builder.hpp:8-9`），拆成只在宿主工具链可见的 `:builder`。
  - kernel/lib/bootfs 暂时保留，只做依赖转发。删不删由主人定（D10）。
- **View 支持只读元数据**：今天的 `View::from(Bytes)` 要求整个 BootFS 在一块连续内存里，并核对 `total_size == data.size()`（`view.hpp:53`）。新增一个只看元数据的入口：输入 Header、Entry[]、名字表和 total_size，文件数据另外按偏移读。init 用 vmo_read 喂这个入口，内核照旧交出整块。
- **`Tree`**：把扁平的文件名按 '/' 切成目录节点，拒绝空段、"." 和 ".."；节点 id 取条目下标加 1。
- **宿主单测**：View、Tree，以及 Builder 生成再由 View 解析的往返测试。

### 4.6 B 段：launcher 与 userboot

- **launcher**（`system/lib/launch`）
  - 接口：`launch(Spec &&) -> Result<ou::Handle, Status>`。
  - Spec 包括：name、程序所在的 VMO 及偏移、vdso、job、argv、`NamespaceEntry[]`（path 加目录连接，按 names 下标发成 `NsDir | i << 16`），以及可选的 directory_request。
  - 句柄类型沿用 Zircon 的数值：PA_NS_DIR 0x20、PA_DIRECTORY_REQUEST 0x3B（`zircon/system/public/zircon/processargs.h:140,155`）。对照 `ProcessBuilder::add_namespace_entries`（`src/lib/process_builder/src/process_builder.rs:377`）。
- **装段方式**（D3 的推荐）
  - 只读段和可执行段直接从 BootFS VMO 映射，偏移为 `entry.data_offset + p_offset`。
  - 可写段用 vmo_create + vmo_write 复制；Binder 也用 vmo_write 写 GOT。
  - launcher 自己不映射任何东西，所以用不到 vmar_unmap。
  - 这改动了接口设计 §3.2 的第 2、4、5 步。
- **进程句柄**原样返回。降到 Wait|Inspect|Destroy 要等 handle_replace（M2）。
- **vDSO 一致性**：内核映射的共享 vDSO VMO，与 launcher 的 Binder 读取的，是 BootFS 里同一个 `vdso` 文件（`kernel/lib/userabi/src/userboot.cpp:43`），一致性由构造保证。因此接口设计 D13 的 build-id 核对不再需要，D13 要同步修改。
- **userboot**
  - 今天的 `kernel/lib/userabi/userboot` 只是一个 ring 3 的 hello 程序。
  - B 段把它换成真正的 userboot：接收内核的 processargs，用 launcher 起 init 并把句柄整体转交，然后 process_exit。
  - init 收到的 processargs 格式不变，所以 init 不用改。
  - 对照：Fuchsia userboot 默认的下一站是 `bin/component_manager+--boot`（`zircon/kernel/lib/userabi/userboot/main.rs:14`）。

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
- **推迟到 B 段**：`export_namespace`（给子进程用 Open(".") 派生缩权连接）和 Fd（0x30）句柄，等 shell 能起子进程时再做。

## 6 权限

### 6.1 v1 规则

- **纯能力**，检查规则见 §2.4。进程没有 "/" 连接，只有 processargs 给的 NsDir；服务端永远看不到绝对路径。
- **身份由铸造者决定。** 每个子进程的 /svc 连接由 init 铸造，允许表固定在它的合成目录上。能用什么服务，取决于这条连接是谁铸的，而不是客户端自称是谁。
- **服务替客户端开文件**时，只能在客户端交来的目录连接上按相对路径打开，即 openat 的做法；绝不用服务自己的命名空间去解析客户端给的字符串。
- **可执行**：M1 不外发任何可执行 VMO。B 段起，Execute 只来自内核铸造的 BootFS VMO（D3）。

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
- **子进程退出不撤销。** 进程死后内核关掉它的句柄（K10），连接靠 PEER_CLOSED 回收。按 grant 撤销只用于显式的策略事件，比如注销和吊销。原因：经 Open(".") 派生出去的连接继承同一个 grant，若退出即撤销，shell 一重启，它派生给子程序的 /boot 和 /svc 就会一起失效。
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
| 权限放大 | 子集检查、节点能力上限、没有 INHERIT 位；M1 不发 VMO；B 段起 vmo_create 不带 Execute，BootFS VMO 不带 Write |
| 路径穿越 | 服务端做语法检查，逐段按节点解析；没有符号链接和挂载；客户端先做文本规范化，".." 越过根时停在 "/" |
| 自环：让 init 自己跟自己对话 | 消息头的 direction 字段。init 同时持有一条 channel 两端时仍会泄漏两条连接记录，由 grant 配额兜底；有了 get_info(HandleBasic) 之后，拒绝 related_koid 指向 init 自己服务端的 object |
| 句柄耗尽、针对 init 的 DoS | 每进程句柄上限，在途句柄也计入（K4）；wait_many 预算加每 grant 配额；回复遇到 NoResource 即断开该连接；NoResource 只让单个请求失败；o_c 一收到入站消息就关掉 |
| 进程死后句柄不释放 | K10：所有进入 Dead 的路径都清空句柄表，句柄在锁外销毁 |
| channel 层层嵌套撑爆内核栈 | K1：对象析构改走每线程的延迟删除链表 |
| 畸形消息 | 生成的解码器校验长度、magic、direction、txid、reserved、未知位、句柄个数；失败就关连接，不 assert；有宿主测试覆盖 |
| 句柄类型不符 | v1 查不到类型：第一次使用时得到 BadType，服务端关连接；read_etc 落地后补上 |
| 回复残留泄漏给下一个客户端 | 回复缓冲先清零，只发 handler 报告的长度 |
| 读越界 | 按 §2.2 的 Seek/Read 边界规则，做加法前先查溢出 |
| 内核映像泄露 | init 隐藏构建生成的私有条目（§4.1）；长期拆成独立的 OBI 条目 |
| 路径 TOCTOU | BootFS 不可变，init 单线程，连接钉住节点而不是名字；可变文件系统上的 rename/unlink 竞争推迟处理 |

## 7 内核前置工作（及夹在其间的生成器、用户侧提交）

**每一步都要满足：**
- 一次提交；
- 构建通过；
- 两个镜像都能启动，没有 panic 或 ASSERT：`python3 scripts/qemu_boot.py`，以及 `IMAGE=out/bringup.x86/kernel.dev.dbg_1.obi python3 scripts/qemu_boot.py`；
- 同一次提交里写 `docs/changes/<YYMMDD>/<MODULE>.md`。

下表“验证”一列只写这之外还要看到的东西。编号前缀：K 为内核，G 为生成器，U 为用户侧。

### 7.1 M1

| # | 提交标题 | 文件 | 改动 | 验证 | 依赖 |
|---|---|---|---|---|---|
| K1 | `feat(object): defer dispatcher destruction through a per-thread list` | `kernel/object/dispatcher.cpp`；`handle.cpp`（`~Handle`，:211-221）；task::Thread 加一个字段 | 最后一个引用释放时，如果本线程已经在删除对象，就把它挂到每线程链表上，由最外层逐个删除。照 Zircon 的 SafeDeleter（`zircon/kernel/object/dispatcher.cc:30-76`） | 日志不变；回归测试在 K3 | — |
| G0 | `refactor(build): move the syscall generator to build/abi` | `build/syscall/*` → `build/abi/*`；`system/public/BUILD.gn:55`；`kernel/lib/userabi/vdso/BUILD.gn:108` | 纯移动 | gen_syscalls 的五个输出（`system/public/BUILD.gn:63-69`）除 banner 两行外字节不变；libours.ifs 不变 | — |
| G1 | `refactor(abi): share the description front end as description.py` | `build/abi/description.py`、`gen_syscalls.py` | 抽出 §3.2 所列的共享部分；两个 action 都加 inputs | 五个输出与 G0 之后字节相同 | G0 |
| G2 | `feat(abi): inout pointers and consumed handle arrays in syscall descriptions` | `gen_syscalls.py:41、:97-98`；`syscalls.gni` | 支持 `dir = "inout"`；release 允许用在 HandleValue 数组上；channel_write 的 handles 参数标 release | 现有输出不变；描述测试覆盖新增的键 | G1 |
| K2 | `feat(object): let a MessagePacket carry handles` | `kernel/object/include/ours/object/message_packet.hpp`；`channel_dispatcher.cpp:87` | MessagePacket 加一个 `ustl::Box<Handle>` 数组。在途句柄的 table_koid 为 0，正好满足 `handle_table.cpp:41` 的断言；析构时关掉没被取走的句柄。`read(max_handles)`：字节或句柄任一放不下就回 BufferTooSmall，消息留在队首 | 日志不变 | K1 |
| K3 | `feat(syscall): move handles through channel_write and channel_read` | `kernel/lib/syscall/src/channel.cpp:83、:133-137`；`handle_table.cpp` | 新增 `HandleTable::remove_for_transfer`：锁外拷入句柄值，在一次表锁内全部摘下；先摘后查：缺 Transfer 回 AccessDenied，句柄正是被写的 channel 回 NotSupported，同一值出现两次时第二次摘除得 BadHandle；无论成败句柄都被消耗。读端先拷字节，再写出句柄值，全部成功后才发布（沿用 `channel.cpp:43-58` 的顺序）。对照 Zircon 的 msg_put_handles / msg_get_handles（`zircon/kernel/lib/syscalls/channel.cc:314、:146`）。顺手改正 `handle_table.cpp:64-66` 的注释：Zircon 的 RemoveHandleLocked 并不取消等待（`handle_table.cc:124-135`），取消发生在接收端（`channel.cc:166`） | dbg_1 shell：经 channel 传过去一个 channel 端，再经它 ping 通；传 channel 自己回 NotSupported；数组含坏值回 BadHandle，且其余句柄已被消耗；嵌套 1000 层 channel 再关闭，内核照常运行 | K2、G2 |
| K4 | `feat(object): charge every live handle to the process that created it` | `handle.cpp`、`process_dispatcher`、`system/public/ours/syscall.hpp` | 按接口设计 §4.8 实现 `HANDLE_MAX_PER_PROCESS`：在途句柄也计入，关闭时归还，超额回 NoResource | dbg_1：循环 channel_create 直到得到 NoResource；关掉一个之后能再建 | K3 |
| K5 | `feat(syscall): object_wait_many` | `kernel/object/wait.cpp:46-98`、`syscalls.gni`、`system/public/ours`（WaitItem、`WAIT_MANY_MAX_ITEMS = 64`） | 泛化 wait_one：N 个 WaitSignalObserver 共用一个 sync::Event；每一项都要 Wait 权限；count > 64 回 InvArgs；deadline 仍只接受 0 和无限 | dbg_1：写第二个 channel 后 pending[1] 为 READABLE；deadline 0 回 TimeOut；count 65 回 InvArgs | G2 |
| K6 | `fix(mm): propagate commit errors and commit a VMO range once` | `kernel/mm/vm_object_paged.cpp:40、:65-72` | 创建时不再丢掉 commit_range 的返回值；commit 循环原来每一页都重新提交整个区间（平方复杂度），改为只提交一次 | 日志不变 | — |
| K7 | `feat(mm): kernel-buffer VmObjectPaged::read/write and VmObject::size()` | `vm_object_paged.cpp:103-113`；`kernel/mm/include/ours/mm/vm_object.hpp` | 经 cursor 按页拷贝；越界或溢出回 OutOfRange；未提交的页读出为零；不在自旋锁下拷贝 | 日志不变，功能在 K8 验证 | K6 |
| K8 | `feat(userabi): hand the kernel package out as a read-only BootFS VMO` | `kernel/lib/userabi/src/userboot.cpp`、`kernel/object/vmo_dispatcher.cpp` | 建一个 Pinned、整体提交的 VmObjectPaged，用 K7 的 write 从解压缓冲拷入。rights 为 Basic\|Read\|Map\|GetProperty，不给 Write，也不给 Execute（按 D3 到 B5 再加）。解压缓冲暂不释放：spawn_boot 还在读它，vfree 也没有 TLB shootdown | 出现 `userabi: bootfs vmo <N> bytes`；回读的头部 magic 一致 | K7 |
| K9 | `feat(syscall): vmo_read` | `kernel/lib/syscall/src/vmo.cpp`（今天是空壳）、`syscalls.gni` | `ours_vmo_read(HandleValue vmo, void *buffer, u64 offset, usize length)`，要求 Read 权限。在 VMO 锁下先拷进一页中转缓冲，放锁后再拷给用户（接口设计 §4.6） | 功能在 K11 验证 | K7 |
| K10 | `feat(object): close a process's handles when it reaches Dead` | `kernel/object/process_dispatcher.cpp:163-168、:189-196` | 进入 Dead 有三条路：kill 时没有线程、最后一个线程 detach、以后的 process_exit。三条路都在锁内把表里的句柄摘出来，在锁外销毁。这是接口设计 §2.2 finish_dead_transition 的前半段，不含 VmAspace::destroy 和 TERMINATED。顺序照 Zircon 的 FinishDeadTransition（`zircon/kernel/object/process_dispatcher.cc:547-576`） | dbg_1：fault_probe 被杀后，它持有的 channel 的对端看到 PEER_CLOSED | K3 |
| U1 | `feat(libc): give user programs mem* and str*` | `kernel/lib/libc/string.cpp` → `system/lib/libc/src`；`system/BUILD.gn:80-82` | 内核与用户共用一份实现；user_executable 默认链接 | shell 里一次 memcpy 的结果正确 | — |
| U2 | `feat(runtime): parse processargs into a startup-handle table` | `system/public/ours/process_args.hpp`（新）、`system/lib/runtime`（新） | 实现接口设计 §3.3 的格式；runtime 负责解析，提供 `take_startup_handle(HandleType, u16)` 和 names 访问 | 宿主单测 | U1 |
| K11 | `feat(userabi): send the first process a processargs message` | `kernel/lib/userabi/src/userboot.cpp`、`system/service/init/init.cpp`、`examples/shell` | 内核建 bootstrap channel，写 processargs（JobDefault = 根 job、VmoBootfs），rdi 为 bootstrap 端。init 和 shell 改由 runtime 取句柄。这两件放在同一个提交里，因为启动协议要同时切换 | dev 出现 `init: bootfs has <n> entries`（头部由 vmo_read 读出）；dbg_1 的 shell 打出同样的行，并照旧驱动 fault_probe | K3、K8、K9、U2 |
| G3 | `feat(abi): generate protocol code; add system/lib/wire and ours.io` | `build/abi/gen_protocols.py`、`protocol_library.gni`、`system/lib/wire`、`system/public/ours/io/*.gni` | 实现 §3。G 阶段支持：consts、bits、enum、定长 struct、一段尾部数组、必选句柄、one_way、two_way、event；回复句柄仅在 Ok 时出现 | gen_protocols_test.py；wire 的宿主测试喂畸形消息 | G1 |
| U3 | `refactor(bootfs): move format and view to system/lib/bootfs; add Tree` | `kernel/lib/bootfs` → `system/lib/bootfs` | 按 §4.5 | 宿主单测；内核照旧解包 | U1 |
| U4 | `feat(init): serve /boot` | `system/service/init`；接口设计 | 加入 Loop（wait_many）、BootServer、Connection 和 §2.4 的检查，隐藏构建生成的私有条目。同一提交修订接口设计：§1 的现状列、§3.6 的 /svc 手写消息改为引用生成的 io 协议、§5 与 §8 把 IDL 从 P2 提前、D9（按 D2） | init 对自己的 /boot 连接自测：Open 并 Read `vdso` 的前 4 字节，得到 `\x7fELF` | K5、K11、G3、U3 |
| U5 | `feat(fsio): a namespace, an fd table and /boot for the shell` | `system/lib/fsio`（新）、init、`examples/shell` | init 用带 NsDir /boot 的 processargs、经 spawn_boot 起 shell；shell 用 fsio 读、列 /boot | M1 判据的第 2–5 行 | U4 |
| U6 | `feat(init): route /svc from a generated service manifest; add echo` | `build/service/gen_service_manifest.py`、system_service 与 service_manifest 模板、`system/lib/svc`、`examples/echo`、init 的 SvcRouter | 按 §4.3–4.4 | M1 判据全部 7 行 | U5 |

### 7.2 B 段（P1-C 的子集：从 /boot 起程序，删除 spawn_boot）

| # | 提交标题 | 要点 | 验证 | 依赖 |
|---|---|---|---|---|
| G4 | `feat(abi): seven-argument vDSO stubs` | `gen_syscalls.py:35-38`、`kernel/lib/userabi/vdso/vdso.S` | 已有桩的 .text 字节不变 | G1 |
| K12 | `feat(object): one shared vDSO VMO mapped by ProcessDispatcher::create` | 取代 `userboot.cpp:246-268` 为每个进程拷一份的做法（接口设计 D2-A） | 日志不变 | K8 |
| K13 | `feat(syscall): vmo_create and vmo_write` | `create_paged` 的 Pinned（`vmo_dispatcher.cpp:48`）改为 Lazy 并设上限；新 VMO 不带 Execute | 写入后读回一致；越界回 OutOfRange | K9 |
| U7 | `feat(bootfs): page-align file data and zero the tail` | `builder.hpp:17` 的 ALIGNMENT 从 8 改为页大小 | 宿主往返测试；phys 照常解出内核 | U3 |
| K14 | `feat(syscall): vmar_map with a page-aligned vmo_offset` | 签名不再暴露 mm 类型（`vma_dispatcher.cpp:49-67`）；root VMAR 补上 R/W/X（`:38-44`）；vmo_offset 透传：`vm_area.cpp:267` 今天写死为 0，paged 路径已经会加 m_vmo_ofs（`vm_mapping.cpp:191、485`），`map_physical` 还要补（`:235`）；保存每个映射的权限上限；processargs 加 VmarRoot；BootFS VMO 加 Execute（D3）；同一提交按 D3 修订接口设计 §2.3、§3.2、§4.2、§9 | W+X 被拒；不带 Execute 的 VMO 按 X 映射被拒；写只读映射会杀掉进程，内核继续运行 | G4、K13、U7 |
| K15 | `feat(launch): process_create, thread_create, process_start, get_property and system/lib/launch` | 装段方式见 §4.6；同一提交修订接口设计 D13（§4.6 的 vDSO 一致性） | dbg_1 出现 `hello: bootstrap ok` | K12、K14 |
| K16 | `feat(syscall): process_exit` | 复用 K10 的 Dead 迁移 | 进程退出后出现 `process 'hello' … destroyed` | K15 |
| U8 | `feat(userboot): kernel -> userboot -> init` | 按 D2 | 出现 `userboot: started init`，M1 判据不变 | K15、K16 |
| U9 | `feat(init): launch services from /boot with the launcher` | — | 出现 `init: launched echo from /boot` | U8 |
| U10 | `refactor(tests): drive fault_probe from a test program, not the shell` | shell 今天经 spawn_boot 起它（`examples/shell/shell.cpp:202`）；dev.dbg_1 的 cmdline 改为 `init` | dbg_1 的探针日志不变 | U9 |
| K17 | `refactor(syscall): remove process_spawn_boot` | 按 `docs/repairs/260923/syscall.md` 第 1 条列出的清单删除 | lib/syscall 不再依赖 userabi | U10 |
| U11 | `build(system): install user programs under bin/` | 清单里的 binary 自动变成 `bin/echo` | `ls /boot` 出现 `bin` | K17 |

### 7.3 M2（依赖 B 段）

- **port + object_wait_async**：在出现 shell 和 echo 以外的程序之前完成。
- **进程监督**：TERMINATED、get_info(Process)、reaper、restart 和 lazy 启动。前提是 VmAspace::destroy。
- **handle_duplicate / handle_replace**。
- **`vmo_create_child(Slice)`**：
  - Slice 和 Reference 子 VMO 的 rights 等于父句柄的 rights，永远不加 Write。
  - Snapshot 类的子 VMO 按 Zircon 加 Write、去 Execute（`zircon/kernel/lib/syscalls/vmo.rs:329-349`）。
  - 加一条内核测试：对带 Execute 的 VMO 做 create_child(Snapshot)，结果不带 Execute。
- **BootFS 格式升级**（§6.2），同时 View 拒绝数据区间重叠或共享同一页的条目。
- **GetBackingMemory**：
  - 连接要有 Execute，条目也要带 Executable 标志，才授予 Execute（参照 `file/common.rs:72-87`）；
  - 返回的 VMO rights 为 Basic\|Read\|Map\|GetProperty，按上一条再加 Execute；
  - 永不给 Write、SetProperty、Resize；
  - 不允许任何“发出整个 BootFS VMO”的过渡方案。
- **channel_read_etc**：补上句柄类型和 rights 的校验。
- **grant 撤销**、export_namespace、64 KiB 消息。

## 8 需要主人拍板的决策

| # | 问题 | 选项 | 推荐 |
|---|---|---|---|
| D1 | M1 的边界 | A：只做 §7.1 的 21 步，起程序由 spawn_boot 做后端，GetBackingMemory 先回 NotSupported。B：连同用户态加载器一起做 | **A**。代价是 spawn_boot 要活到 K17 |
| D2 | userboot 的时机 | A：M1 由内核直接起 init，U8 再插入 userboot。B：现在就做 userboot，等于把 B 段整体提前。C：永远不做 userboot | **A**。这是对已批准的 kernel → userboot → init 的暂时偏离；init 收到的 processargs 格式不变，以后插入 userboot 时 init 不用改 |
| D3 | 可执行权从哪来 | A：Execute 只来自内核铸造的 BootFS VMO；vmo_create 永不带 Execute；vmar_map 接受页对齐的 vmo_offset；launcher 直接从 BootFS VMO 映射只读段和可执行段；撤回接口设计 §2.3、§4.2、§9 的偏离。B：照接口设计 §4.2，vmo_create 带 Execute，launcher 复制所有段 | **A**。B 让任何进程都能把同一个 VMO 先按 W、再按 X 各映射一次，从而执行任意代码；单个映射上的 W+X 检查挡不住这种双重映射。A 在内核里只多了 vmo_offset 透传这一件事，代码现状也已照 Zircon 不带 Execute（`vmo_dispatcher.hpp:19-20`） |
| D4 | M1 的多路复用 | A：object_wait_many，加 §4.2 的预算和配额。B：先做 port + wait_async，需要把 P1-D 的 PortDispatcher、锁序、Semaphore 一起提前 | **A**。出现 shell 和 echo 以外的程序之前换成 B |
| D5 | ordinal 方案 | A：SHA-256 哈希加可选的 selector，照 fidlc。B：显式编号 `ordinal_base << 32 \| method`，全构建唯一由 GN metadata 检查，另检入 golden | **A**。M1 不用分配库号，也不用检入 golden；ours 只有一棵源码树，改名时写 selector 钉住编号即可。B 的长处是十六进制转储里看得懂 |
| D6 | BootFS 的 mode、uid、gid 什么时候落盘 | A：M1 由 init 合成；M2 随 GetBackingMemory 升级 BootFS 格式，从 GN metadata 填入真实值和 Executable 标志。B：M1 就升级格式 | **A**。M1 不外发任何可执行物，而格式改动会牵动 phys、KernelPackage 和打包器。“盘上 mode 从第一天就存”这一条，从第一个可写文件系统起严格执行 |
| D7 | /boot 与内核包是否分开 | A：M1 由 init 隐藏构建生成的私有条目；长期在 OBI 里加一个独立的用户 BootFS 条目。B：现在就分开 | **A** |
| D8 | 连接权限类型的名字 | IoRights 或 FsRights | **ours::io::IoRights**。/svc 这类非文件系统节点也用它；与句柄的 `ours::Rights` 在 GetBackingMemory 这类方法里会同时出现，名字不同才不会混淆 |
| D9 | 路径的编码 | A：字节串，只禁 NUL 和 '/'。B：照 fuchsia.io 要求 UTF-8 | **A**。服务端逐段按字节比较，用不着解释编码 |
| D10 | kernel/lib/bootfs 挪空之后是否删除 | 删除，或者保留为转发 | **删除**。它只剩转发，没有独立的职责；按规矩先问 |

## 9 被否掉或推迟的方案

| 方案 | 出处 | 处置 | 理由 |
|---|---|---|---|
| 消息头用 `{u16 flags; u16 wire_version}` | protocol | 否决 | 统一为 FIDL 的形状加 direction 字段；版本由 magic 承担 |
| 句柄不在消息体里占槽，接收方要求句柄数恰好相等 | protocol | 否决 | 表达不了“只在 Ok 时带句柄”，以后也做不了可选句柄 |
| 显式 ordinal 加 golden | generator、一致性评审 | 见 D5 | 推荐哈希；若主人选显式编号，golden 必须同批落地 |
| OpenOptions 的低 16 位放 rights，另加 INHERIT_WRITE/EXECUTE | permissions | 否决 | rights 单独成一个字段；继承会让实际授予的权限随父连接悄悄变化 |
| 照搬 fuchsia.io 的 R_STAR_DIR、RX_STAR_DIR | protocol | 否决 | 它们含 Connect，与节点能力检查冲突 |
| M1 只做子集检查、不查节点能力 | 闭环评审 | 否决 | 只多两行，与 Rust VFS `file/common.rs:25-33` 一致；常量不含 Connect 之后，它不会误伤 /boot |
| 路径上限 4095、传输上限 8192、按 CHANNEL_MAX − 32 截断 | protocol、permissions | 否决 | 装不进 4 KiB 消息；消息升到 64 KiB 后再放大 |
| Read 上限 2048 | kernel | 否决 | MAX_TRANSFER 由消息上限推出 |
| 所有回复都以 `{status; u32 reserved}` 开头 | 闭环评审 | 部分采纳 | 只用作错误形态；Ok 回复按无隐式填充排列 |
| M1 就做 GetBackingMemory | protocol、generator | 推迟到 M2 | 需要 Slice、handle_replace 和新的 BootFS 格式，而 M1 没有进程需要文件 VMO |
| 用整个 BootFS VMO 充当过渡的 GetBackingMemory | — | 否决 | 越出单个文件的范围 |
| init 用 vmar_map 映射 BootFS | boot、protocol | 推迟 | A 段没有 vmar_map；vmo_read 的接口以后也不必变 |
| M1 的 Loop 用 port + wait_async | protocol、boot | 推迟 | 见 D4 |
| 连接池 256 槽、每 grant 上限 256 | boot、permissions | 否决 | 超过 wait_many 的 64 项；一个 grant 就能占满整个池 |
| Reaper、restart=on-failure、lazy 启动 | boot | 推迟到 TERMINATED 与 VmAspace::destroy 之后 | M1 拿不到进程句柄；lazy 每重起一次就泄漏一个地址空间 |
| 子进程退出即撤销它的 grant | permissions | 否决 | 会误伤它派生出去的后代连接；靠 PEER_CLOSED 回收已经足够 |
| 每端点 `CHANNEL_MAX_PENDING_HANDLES` | kernel | 否决 | 被 K4 的每进程记账取代；只限单个端点挡不住 arena 被耗尽 |
| 把按进程记账推迟 | permissions | 否决 | 接口设计 §4.8 规定它与句柄转移同批落地，K4 照做 |
| handle_duplicate/replace、vmo_create_child 进 M1 | permissions、generator | 推迟到 M2 | M1 没有消费者 |
| channel_read_etc 提前到 P1-C | generator | 推迟到 GetBackingMemory 之前 | M1 只核对句柄个数 |
| vmo_create 带 Execute；vmar_map 只收 vmo_offset=0 | kernel | 否决 | 见 D3 |
| 加载器用“映射进 launcher → memcpy → unmap” | boot、接口设计 §3.2 | 否决 | 改为 vmo_write 加直接映射，省掉依赖 TLB shootdown 的 vmar_unmap |
| 不做 userboot | boot | 否决 | 见 D2 |
| 按 bin/、lib/ 路径前缀判断是否可执行，并据此合成 mode | protocol、boot | 否决 | 判据来自构建生成的条目标志（M2）；M1 合成的 mode 不区分可执行 |
| 过渡期把一条裸的 /boot 连接当作 rdi | boot | 否决 | 违反接口设计 §3.3 |
| /svc 失败时写 Unavailable | boot | 否决 | Status 里没有这一项（`status.hpp` 止于 `:109` 的 BufferTooSmall = 33），而且 s 此时已被消耗 |
| 路径错误回 InvArgs | permissions | 否决 | 统一回 BadPath（`openable.fidl:16`） |
| 结尾的 '/' 视为 ProtocolDirectory | protocol | 否决 | `io.fidl:41` 禁止结尾的 '/'；由 fsio 剥掉并置 ProtocolDirectory |
| 把“拒绝开头的 '/'”列为偏离、请主人拍板 | protocol、permissions、一致性评审 | 撤销 | 这不是偏离，`io.fidl:40` 本来就禁止 |
| 保留 build/syscall 这个目录名 | 闭环评审 | 否决 | 协议生成器放在 syscall 目录下名不副实；改名只是一次纯移动的提交 |
| G0 的验收标准写成“七样产物字节不变” | 一致性评审 | 更正 | gen_syscalls.py 只写五个文件（`system/public/BUILD.gn:63-69`），而且 banner 行必然会变 |
| 预留 Reauthenticate 的序号 | permissions | 撤销 | 用哈希 ordinal 不需要预留号段；若 D5 选显式编号，这一条恢复 |
| M1 就做 export_namespace 和 Fd 0x30 | protocol | 推迟到 B 段 | M1 的 shell 不起子进程 |
| 解码器在 v1 校验句柄的对象类型 | permissions | 更正 | v1 做不到，按 §2.5 的表述处理 |
| M1 就做 revoke(grant) | permissions | 推迟到 P1-B 之后 | 触发事件（注销）在 M1 不存在 |
| 建 Tree 时跳过写死的 "vmours" 常量 | 闭环评审 | 改用别的做法 | 目录名是 phys 的启动选项，init 无从得知；改用构建生成的私有条目表 |

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
| 可执行判据 | BootfsSvc 按目录名判断（`bootfs.rs:44-53`），再经 vmex 换成可执行 | 构建生成的条目标志；Execute 只从 BootFS 根往下继承，没有 vmex | 判据来自构建，而不是路径约定 |
| 每文件 VMO | `Vmo::create` + `transfer_data`（`bootfs.rs:204-220`） | M2 用 Slice 子 VMO | ours 没有 transfer_data；BootFS 已整体提交且不可写 |
| GetBackingMemory 返回的 VMO | 按 VmoFlags 可以带 WRITE（`file/common.rs:40` 起的 `vmo_flags_to_rights`） | 永远不带 Write、SetProperty、Resize | 不外发可写 VMO |
| /boot 由谁托管 | component_manager 的 BootfsSvc | init 内置 | 主人的决定 |
| /svc 路由 | 组件拓扑加 capability routing | 清单里的静态允许表，按客户端合成目录 | 不做组件框架 |
| 多路复用 | port | M1 用 wait_many | D4 |
| userboot | 在用户态解压 ZBI，然后起 component_manager | M1 没有 userboot，B 段加回；BootFS 由内核解压 | D2 |
| 句柄转移时取消等待 | 在接收端取消（`channel.cc:166`） | 在发送端摘除句柄时取消（`handle_table.cpp:61-67`） | 效果等价，只是更早 |
| 句柄数量限制 | 靠 job policy 和内存压力兜底 | 每进程上限，在途句柄也计入 | 同接口设计 §9 |
| 内核包与用户 BootFS | 两类 ZBI 条目 | M1 共用一个包，init 隐藏私有条目 | D7 |
