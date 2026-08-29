# ustl::Box 实施结果

日期：2026-08-25　分支：`feat/baseline/20260824`　任务区：`.agents/tasks/ustl-box`

## 交付物

| 文件 | 状态 | 说明 |
|---|---|---|
| `system/lib/ustl/include/ustl/box.hpp` | 新增 | `ustl::Box<T>`：仿 Rust `Box` 的独占所有权指针 |
| `system/lib/ustl/include/ustl/detail/deallocate.hpp` | 新增 | 从 rc.hpp 抽取的共享 Disposer 回收契约（`HasDisposer` / `default_deallocate`） |
| `system/lib/ustl/include/ustl/rc.hpp` | 修改 | 改为包含共享 detail 头，删除本地副本；公共 API 与行为不变 |
| `system/lib/ustl/test/box_test.cpp` | 新增 | 宿主冒烟测试（11 个用例 + 8 条 static_assert） |
| `system/lib/ustl/test/box_doc_examples_test.cpp` | 新增 | docs/box.md 示例可执行化 |
| `system/lib/ustl/docs/box.md` | 新增 | 使用指南（对齐 rc.md 结构） |
| `system/lib/ustl/BUILD.gn` | 修改 | headers 列表 + `box-test` / `box-doc-examples-test`，并入 `tests` 组 |

## 验证记录

- `./prebuilt/gn/gn gen out/bringup.x86`：通过（289 targets），新目标已生成。
- 宿主 `clang++ -std=c++23 -Wall -Wextra -Wconversion -Werror
  -fsanitize=address,undefined`（llvm@22）编译并运行 5 个测试，全部 PASS：
  `box_test`、`box_doc_examples_test`、`rc_test`、`rc_doc_examples_test`、
  `rc_race_test`（rc 三个是回归，证明 rc.hpp 重构行为不变）。
- clang-format（llvm@22）：全部改动文件已 format，二次运行无变化（幂等）。
- clang-tidy（llvm@22，`-std=c++23` 按 rc-test.ninja 实际 flags 手工传递）：
  两个新测试文件 0 告警；box.hpp / detail/deallocate.hpp / rc.hpp 无新告警；
  rc_test.cpp 上 7 条 `modernize-use-override` 为未改动文件的既有告警，未处理。
- 3 处 `bugprone-use-after-move` 以带理由的 `// NOLINT(...)` 窄标注
  （移动后判空是 Box 规定行为，属测试意图本身）。

## 环境备注（重要）

- 本会话 `dev-common` 容器未运行（Docker daemon 停止），clang-format/tidy
  使用宿主 `/opt/homebrew/opt/llvm@22/bin/`（与容器 `/usr/lib/llvm-22` 同主版本 22.1.x）。
- vendored `prebuilt/clang/llvm-23` 以 `--target=x86_64-unknown-none-elf` 编译
  ustl 测试在**本机本来就失败**（HEAD 版 rc_test.cpp 同样报 `__config: "No
  thread API"`、rc-race-test 报 `<thread>` 错误），与本次改动无关——已用
  `git show HEAD:rc.hpp` 遮蔽编译复现确认。仓库内未提交的
  `build/toolchain/get_macos_sdk_path.py` 显示宿主构建适配正在进行。
  因此功能验证走宿主 clang 原生编译；GN 图本身已验证正确。
- 曾在 detail 头中加过 `#include <new>`，因 freestanding libc++ 下会引入
  thread-API 依赖而已移除，保持与原 rc.hpp 完全一致的包含集。

## 设计要点回顾

1. API 以 Rust `Box` 为蓝本：`make`（= `Box::new`，原地构造）、`from_raw`、
   `into_raw`、`leak`、`swap`、`clone`（= derived `Clone`，要求 T 可拷贝构造）。
2. 刻意差异：可为空（同 `Rc` 形态）；多态销毁需虚析构。
3. 存储回收与 `RefCounted` 共享 Disposer 契约（析构 → Disposer 或
   `operator delete`），对象缓存类型与 Rc 行为一致。
4. 拷贝构造/赋值 `= delete`；自移动、自收养（`reset(p)` 传己方指针）安全。

## 评审补充（2026-08-25，第二轮）

- 增加 `Box<T>::adopt(p)`：`Rc::adopt` 的独占版叫法（= `from_raw`，无计数可加），
  保持 ustl 收养词汇统一；`from_raw`/`into_raw` 保留贴 Rust 文档的叫法。
  有意**不**加 `ustl::adopt<T>(p)` 免费函数——rc.hpp 已有同名 Rc 版本模板，
  再加 Box 版会重载歧义（`adopt<T>(p)` 无法在 `Rc<T>`/`Box<T>` 间裁决）。
- 测试加 `test_adopt`；docs/box.md API 表补一行；format/tidy/ASan+UBSan 全过。
