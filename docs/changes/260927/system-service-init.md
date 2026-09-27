## 启动清单的一行拆成命令加能力：`init::StartupEntry`，加主机单测（2026-09-27）

**文件**
- `system/service/init/include/init/startup.hpp` — 新增 `STARTUP_PATH`、`Capability`、`Capabilities`、`parse_capability()`、`StartupEntry`、`StartupLines`
- `system/service/init/BUILD.gn` — 新增 `cpp_interface("startup")`
- `system/service/init/test/startup_test.cpp`、`test/BUILD.gn` — 新增主机 gtest `startup`
- `BUILD.gn` — 把 `//system/service/init/test:startup($host_toolchain)` 挂进 `//:default`

**症状**
无，这一步是准备。构建侧已经把 `config/init/startup` 放进 init 能看到的 BootFS（`docs/changes/260927/build.md`），但 init 还不会读它；shell 仍由 `init.cpp` 里写死的 `SHELL` 和 `start_shell()` 启动。

**原因**
清单格式是 init 和构建之间的契约（`system/service/init/init_startup.gni`）：每行 `<命令> [<能力>,...]`。命令部分和 `ours.autorun.boot` 同一个格式。能力部分每个词对应一个 processargs 启动句柄。先把「读一行」单独做成纯字符串处理，理由和 `AutorunCommand` 一样：init 只在 user 工具链里存在，要在主机上测，拆行和解析就不能依赖内核。

「拆行」和「解析一行」分成两个类型，是为了让坏行只坏它自己：`StartupLines` 只负责按 `\n` 切，不做解析，所以某一行解析失败时，调用方可以报错、引用原文，再继续读下一行。如果做成一个「解析整份清单」的函数，要么第一行错就整份放弃，要么得在函数里再造一套错误收集。

**改法**
改前：无，init 不认识清单。改后的核心：
```cpp
    static constexpr auto parse(std::string_view const line, StartupEntry *const out)
        -> ours::Status {
        auto const space = line.find(' ');
        StartupEntry parsed;
        if (auto const status = AutorunCommand::parse(line.substr(0, space), &parsed.m_command);
            status != ours::Status::Ok) {
            return status;
        }
        // ... 其后按 ',' 拆能力词，空词跳过，未知词 NotSupported
```
- 命令直接复用 `AutorunCommand::parse()`，所以 `InvArgs`/`OutOfRange` 的含义和 autorun 完全一致。
- 能力按第一个空格切开。第二个空格会落进某个能力词里，那个词就不认识，整行被拒——构建侧（`init_startup.gni`）本来就不允许参数里有分隔符，所以这种行只可能是有人手写的。
- 未知能力拒绝整行（`NotSupported`），不是忽略：一个条目要了某个句柄却没拿到就被启动，会在离原因很远的地方出错。
- 空能力词跳过，和命令里空的 `+` 词一样处理。
- `Capabilities` 是个小类型，不是裸 `u8`：只能通过写出某个 `Capability` 来往里加，所以混不进没有对应词的位；init 那边问的是「有没有这个能力」，而不是自己去测位。
- `StartupLines` 跳过空行，这样构建在文件末尾写的换行不会被读成一个「什么也没指名」的条目。

**参照**
- Fuchsia `src/sys/bootstrap/meta/bootstrap.cml` 里 `children` 的 `startup: "eager"`，以及各 child 的 `use`：一行清单对应一个 eager child 加上它 `use` 的能力。component_manager 读的是编译过的 manifest，这里读的是一行文本，但「一个条目 = 程序 + 它拿到的能力」这层意思相同。
- 命令拆分同 Fuchsia console-launcher `LaunchAutorun()`（见 `260926/system-service-init.md`）。

**验证**
- `out/init-startup`（`tools/set.py bringup.x86`）：`host/obj/system/service/init/test/startup` 8 项全过，`autorun` 5 项照旧全过。
- clang-format 已跑；clang-tidy（`-p out/init-startup`）在两个新文件上没有诊断。
- 这一步还没有改 init，所以没有 QEMU 实测，镜像行为不变。

**故意没做**
- 行号：错误报告直接引用出错的那一行，清单一般只有几行，行号帮不上什么。
- `\r` 容错：文件由构建生成，不会有。
- init 接上清单，放下一步单独提交。
