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

## 与用户协作

- **功能做到基本可用即可。** 达到能用、能通过 `ours-verify` 验证就停，不追求完备，也不做没被要求的扩展。
- **性能优化放到功能基本完善之后，并且逐项和用户深入讨论。** 用户最看重这部分，要把它积累成求职经验。
  开展任何性能工作前，先讲清测量方法、瓶颈在哪、每个候选方案的原理和代价，由用户决定；
  不要悄悄优化，也不要只丢一个结论。功能开发中发现的性能问题记进 `docs/repairs/`，不顺手改。
  做“基本可用”时，若某个选择会锁死以后的性能上限（锁粒度、数据结构、拷贝次数），当作决策项问用户。
- **并发是教学重点。** 遇到锁和并发问题（选哪种锁、粒度、锁序、持锁范围、无锁结构），
  抓住机会教用户怎么想：保护的是哪条不变量、谁会并发访问、最坏的交错是什么、换一种锁会怎样。
- **锁的选择用数据说话。** 在 `benches/` 写基准，量出持锁时间、争用率、延迟分布，拿数字比较方案，
  不只靠理论。正确性例外：测量证明不了“没有竞争”，正确性靠不变量和锁序论证，再用 lockdep 和压力测试佐证。
- **学习闭环：要用户产出，不只是认可。** 用户挑推荐选项、说“可以”练的是识别，不是能力；
  面试是白板、没有 AI。所以：
  - 架构和并发的决策卡，先请用户写一行预测（选哪个、为什么、会出什么事），再给分析；
    要测量的，预测写进该提交的改动记录，结果出来后对照。改名和机械性改动不做这一步。
  - 并发决策（锁的选择、锁序、持锁范围）由用户先写论证：保护哪条不变量、谁并发访问、
    最坏交错是什么；AI 再批改。不跳过。
  - 每个里程碑挑一个核心函数由用户亲手写：AI 先给规格和一个会失败的测试，用户限时手写，
    AI 按严苛面试官的标准审。用户写的函数记在 `docs/authored.md`。
- **策略和决策交给用户。** 需要定策略、在方案间取舍、偏离设计文档或 Zircon 时，先停下：
  讲清背景和细节，给出几个你认为好的选项并标出推荐，等用户决定。
- **不奉承，要碰撞。** 用户的想法错误或片面时，直接指出并说明理由；有更好的思路就提出来争论。
- **拿不准的事绝不悄悄做。** 难以决策或无法决策的，整理成想法交给用户，不自行拍板后默默实现。
- **回答简洁。** 先给结论，少铺垫，不堆砌。

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
