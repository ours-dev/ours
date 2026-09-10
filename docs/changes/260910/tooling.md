# tooling —— 改动记录（2026-09-10）

## `tools/set.py` 在 macOS 自带的 Python 3.9 上根本跑不起来（2026-09-10）

**文件**
- `tools/set.py` —— 加 `from __future__ import annotations`

**症状**
配置构建目录的那条文档化入口，一行自己的代码都没执行就崩了：

```
$ python3 tools/set.py --list
Traceback (most recent call last):
  File ".../tools/set.py", line 68, in <module>
    def _clang_tool_dir() -> str | None:
TypeError: unsupported operand type(s) for |: 'type' and 'NoneType'
```

`--list`、`bringup.x86`、任何参数，全都一样——错误发生在 import 期。

**原因**
`str | None` 是 PEP 604，**3.10 才有**。函数注解在没有
`from __future__ import annotations` 时是**定义期求值**的，所以模块一被加载，
`str.__or__(None)` 就抛 TypeError；函数体是否被调用无关紧要。

而 macOS 至今把 3.9 作为 `python3`（本机 `Python 3.9.6`）。文件里同时用到的
`list[str]` / `tuple[str, str]` 反而没事——PEP 585 在 3.9 就落地了——所以只有 `| None`
那三处触发，也因此更容易被忽略：文件看起来"用了新语法但没那么新"。

这不是理论问题：`scripts/qemu-debug.sh` 在构建目录不存在时给出的下一步就是
`tools/set.py <产品>.<架构>`，照着敲会直接吃到这个 traceback。

**改法**
把注解变成惰性求值，一行：

```python
# 改前
import argparse

# 改后
# PEP 604 annotations (`str | None`) are evaluated at definition time without
# this, and macOS still ships Python 3.9 as `python3` ...
from __future__ import annotations

import argparse
```

注解本身一个字没改。被否掉的方案是把 `str | None` 改写成 `Optional[str]`：那要改三处、
引入 `typing` 依赖，并且把代码往回写；`from __future__ import annotations` 是同一件事的
一行版本，而且对以后新写的注解同样有效。

**参照**
CPython PEP 563（`from __future__ import annotations`）与 PEP 604（`X | Y`）。

**验证**
```
$ python3 --version
Python 3.9.6
$ python3 tools/set.py --list
Products:  bringup
Arches:    x86 arm riscv
$ python3 tools/set.py bringup.x86
Generated .../out/bringup.x86/args.gn  (product=bringup, cpu=x86, triple=x86_64-elf, ...)
Running: .../prebuilt/gn/gn gen .../out/bringup.x86 --export-rust-project
Build:   ninja -C .../out/bringup.x86 //products/bringup:bringup
```
`ast.parse` 也过。

**故意没做**
- **没有给仓库定一个最低 Python 版本**。该定，但那要连 `tools/` 下其它脚本一起看，
  是另一件事。这里只让这个文件在实际会遇到的解释器上能跑。
