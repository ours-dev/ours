# 模板

用 [copier](https://copier.readthedocs.io/) 生成，模板本身就是仓库里的普通文件。

先装依赖（只需一次）：

```
python3 -m pip install -r scripts/requirements.txt
```

## `cpp_library` — 一个 C++ 库的骨架

```
python3 -m copier copy tools/templates/cpp_library kernel/lib/<name>
```

生成的结构就是本仓库的库该有的样子：

```
include/<name>/   公开头文件，以 <name>/... 引用
src/              源文件
test/             宿主侧单测，用 googletest
.gitignore
.clang-format
BUILD.gn
README.md
```

**为什么值得用模板而不是手抄一个现成库**：`.clang-format` 必须写上这个库自己的
include 前缀（见 `docs/changes/260910/include-order.md` 第 2 条）。抄错前缀不会报错，
只会让这个库的 include 排序悄悄不对，直到有人觉得看着别扭为止。生成时填一次名字，
这一处就不可能错。

生成之后还要手工做的两件事，模板管不了：

1. 把新库挂进依赖它的目标的 `deps`；
2. 如果它要参与单测汇总，把 `test/` 里的目标挂进对应的 test group。
