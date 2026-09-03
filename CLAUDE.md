# CLAUDE.md

Load skills from .agents/skills

Write C++ code through skill @cpp-write-and-verify

## Commit after every change

One change, one commit, made as soon as that change builds and boots. Do not
let several fixes pile up in the working tree: once they blur together the
commit messages stop being writable, which is how this tree reached ~280
uncommitted paths.

The rhythm is: diagnose -> fix -> build -> boot it -> commit -> next thing.
An update to the running repair document belongs in the same commit as the
change it describes.

This worktree often holds work from other concurrent sessions. Check
`git diff <file>` before staging: a file can carry both your hunks and someone
else's in-flight feature, and sweeping theirs into your commit is worse than
leaving yours uncommitted.

## 文档

**改动记录** 写在 `docs/changes/<YYMMDD>/<MODULE>.md`，一条改动一节，格式固定，
模板和字段说明见 `docs/changes/TEMPLATE.md`。改完就写，和改动同一轮，不要事后补。
这份文档是用来学习的，所以「为什么这是个错」和「照着 Linux/Zircon 的哪个函数改」
要写透，不能只写改了哪一行。只增不改。

**待修项** 写在 `docs/repairs/<YYMMDD>/<MODULE>.md`，只写还没修的，按严重程度排，
每条要能被独立认领。修掉一条就从这里删掉，同时在 changes 里追加一条。

`<YYMMDD>` 六位日期，`<MODULE>` 用短横线的模块路径名（`kernel-time`、`x86-clock`）。
按模块拆是控制篇幅的手段——不要把两件事合回一份不断增长的文件。

`docs/repairs-20260830.md` 是这套结构之前的单一流水账，只读归档；**它有多个 agent
并发写入，不要整文件重写**。
