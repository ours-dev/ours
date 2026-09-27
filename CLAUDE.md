# CLAUDE.md

## Skills

Skills live in `.agents/skills/` so every agent can use them. Claude Code only
discovers the ones linked from `.claude/skills/`
(`ln -s ../../.agents/skills/<name> .claude/skills/<name>`). The ones nearly
every task needs:

- `ours-verify`: done means it builds and **both** images boot. Run it before
  every commit.
- `ours-kernel-design`: settled design decisions. Load it before reading code
  for any design, placement or review question about `kernel/` or `system/`.
- `cpp-write-and-verify`: every C++ edit, with `modern-cpp-development` for
  style.

## Repo conventions

- Tooling scripts are Python, never shell.
- Toolchains come from official upstream releases into `prebuilt/`. Never
  build the dev container or install anything into system directories.
- Lists describing build products are generated from GN metadata, not kept by
  hand. GN templates are named for their artifact (`rust_library`), not the
  tool.
- Benchmarks go in `benches/`; files that only register kshell console
  commands go in `kernel/commands`; new C++ libraries are scaffolded with
  copier from `tools/templates/cpp_library`.

## Commit after every change

One change, one commit, made as soon as that change builds and boots. Do not
let several fixes pile up in the working tree: once they blur together the
commit messages stop being writable, which is how this tree reached ~280
uncommitted paths.

The rhythm is: diagnose -> fix -> build and boot it (`ours-verify`) -> commit
-> next thing.
An update to the running repair document belongs in the same commit as the
change it describes.

This worktree often holds work from other concurrent sessions. Check
`git diff <file>` before staging: a file can carry both your hunks and someone
else's in-flight feature, and sweeping theirs into your commit is worse than
leaving yours uncommitted. Commit with `git commit -- <paths>`, never
`git add` followed by a bare `git commit`: the sessions share one index, and a
bare commit takes whatever the others have staged.

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
