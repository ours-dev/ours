# build 改动记录（2026-09-27）

## 加 `init_startup()`：由构建从 metadata 生成 init 的启动清单 `config/init/startup`（2026-09-27）

**文件**
- `system/service/init/init_startup.gni` — 新增：`init_startup()` 模板和能力词汇 `init_startup_capabilities`
- `kernel/kernel_package.gni` — 新参数 `init_startup`：把 deps 里所有条目的 `init_startup` metadata 汇成 BootFS 的 `config/init/startup`，并把它标成用户可见
- `kernel/products/image/dev/BUILD.gn` — shell 从"写死的程序列表"改为一个 `init_startup("shell")` 条目，带 `init` 能力；package 打开 `init_startup`

**症状**
无运行时症状，是缺一项能力。dev 镜像开机后 init 起什么是写死的（`init.cpp` 里 `constexpr ou::Str SHELL{"shell"}`），
外加最多一条 `ours.autorun.boot`。想让 init 开机起更多服务，只能改 init 的源码。

**原因**
Fuchsia 里"开机起哪些东西"写在清单里：component_manager 读 `root.cml`，`bootstrap` 的 children 里
`startup: "eager"` 的开机就起，每个 child 用 `use` 声明它要的能力。清单是系统的一部分、随系统打包，
而不是写在 component_manager 的代码里。ours 的 init 相当于 component_manager 的位置，却没有清单可读。

清单要由构建生成而不是手写：它描述的是构建产物（镜像里有哪些程序、以什么名字），手写就会和实际打进去的东西脱节。
仓库里已经有同类机制——`kernel_package` 的用户可见白名单就是顺着 deps 收集 `bootfs_user_visible`
metadata 生成的——这里用同一套。

清单归镜像而不归程序：同一个 shell，在 dev 里由 init 按清单起，在 dbg_1 里由 userboot 直接起。
所以条目是镜像目录下的 GN target，镜像在 package 的 deps 里列出它要的条目；这也和用户程序的
visibility 一致（它们只对 `//kernel/products/image/*` 可见）。

改前（dev）：

```gn
_user_programs = [
  "//examples/shell",
  "//system/service/init",
]
...
kernel_package("ours") {
  user_bootfs = true
  ...
  deps = [
    "..:common_package_items",
    "//kernel/products/bin:vmours.with-tests(...)",
  ]
```

**改法**
改后（dev）：

```gn
_user_programs = [ "//system/service/init" ]

init_startup("shell") {
  visibility = [ ":*" ]
  program = "//examples/shell"
  capabilities = [ "init" ]
}

kernel_package("ours") {
  user_bootfs = true
  init_startup = true
  ...
  deps = [
    ":shell",
    "..:common_package_items",
    ...
```

模板把条目写成一行 `<名字>[+参数...] [能力,...]`，放进 `init_startup` metadata：

```gn
  _line = string_join("+", _words)            # 名字 + 参数，和 ours.autorun.boot 同一种格式
  if (defined(invoker.capabilities) && invoker.capabilities != []) {
    _line += " " + string_join(",", invoker.capabilities)
  }
  group(target_name) {
    deps = [ "$_program(//build/toolchain/ours:user-${target_cpu})" ]
    metadata = {
      init_startup = [ _line ]
    }
  }
```

`kernel_package` 的新 `generated_resource` 以 `init_startup` 为 data_key 收集这些行，输出到
`config/init/startup`，并在自己的 metadata 里带上 `bootfs_user_visible = [ "config/init/startup" ]`，
再作为白名单 generated_resource 的 dep——否则清单会进 BootFS，却不在内核交给用户态的那份 BootFS 里，init 读不到。

取舍：

- **命令部分沿用 autorun 的格式**（`+` 分词，第一个词是 BootFS 名和 argv[0]）：init 已经有
  `init::AutorunCommand::parse()`，同一种写法不必再造第二个解析器。能力放在一个空格之后，用逗号分隔；
  参数里出现 `+`、空格、逗号会被读成分隔符，所以模板直接 assert 拒绝。
- **能力词汇只有三个**，各对应一个 processargs 句柄：`init`（`User0`，与 init 相连的 channel，init 在另一端做存活检查）、
  `bootfs`（`VmoBootfs`）、`job`（`JobDefault`）。词汇表 `init_startup_capabilities` 和模板放在一起，
  未知的词 assert 拒绝。这是 Fuchsia `use` 声明的最小对应：没有能力路由，只有 init 手里现成的三样东西。
- **条目依赖它的程序**：写了条目就会编译并打包那个程序，不需要再单独写进程序列表或 `--with`。
- **opt-in**：只有 `init_startup = true` 的 package 才生成清单，而且要求同时开 `user_bootfs`（assert），
  因为没有白名单时用户态什么都看不到。dbg_1 没有 init，不开。

**参照**
Fuchsia `src/sys/root/root.cml`、`src/sys/bootstrap/meta/bootstrap.cml` 的 `children`/`startup: "eager"`/`use`；
清单生成方式对照本仓库 `kernel/kernel_package.gni` 的用户可见白名单（Fuchsia `//build/dist/generated_resource.gni`）。

**验证**
- 生成的 `ours.init_startup.txt` 是 `shell init`；白名单变为 `shell userboot vdso init config/init/startup`；
  `config/init/startup` 出现在 package 的 distribution entries 里。
- 在 `kernel/products/image/` 下临时建探针（用 `gn gen --root-target` 只加载它）：
  `capabilities = [ "root" ]`、`args = [ "a+b" ]`、`args = [ "a b" ]`、带工具链后缀的 `program` 都在对应的 assert 处失败；
  `args = [ "one", "two" ]` + `capabilities = [ "bootfs", "job" ]` 生成 `fault_probe+one+two bootfs,job`，
  `name = "renamed"` 生成 `renamed`。探针已删除。
- QEMU 同时跑两个镜像 60 秒，零 panic：dev 的 kernel package 从 8 个文件变成 9 个
  （`config/init/startup (11 bytes)`），init 看到的用户 BootFS 从 4 项变成 5 项，开机流程不变
  （`init: spawned shell` → `init: shell says "pong"` → `shell: libc checks passed`）；dbg_1 不变（8 个文件）。

**故意没做**
- init 还不读这份清单：读清单、按能力发句柄、删掉写死的 `SHELL` 是 init 的活，交给用户态会话。
  在那之前 dev 仍按写死的方式起 shell，这一步只保证清单内容正确、init 读得到。
- `job` 能力现在只能是 init 自己的 job（根 job）的副本，不是每个条目一个子 job：用户态库还没有 `job_create`。
  已记入 `docs/repairs/260927/build.md`。
- 没做清单条目名字与白名单的交叉检查（条目的 `name` 如果和程序实际的 install_name 不一致，要到开机时才发现）：
  名字默认取 label 的目标名，和 `user_executable()` 的默认 install_name 相同，只有改过 install_name 的程序才需要显式写 `name`。
