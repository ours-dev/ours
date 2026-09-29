# Agents

This file provides guidance to AI coding agents (Claude Code, Cursor, Codex, Copilot, Antigravity, etc.) when working with code in this repository.

## Repository Overview

A microkernel **modeled after** these proven architectures, designed to explore the next generation of OS design. It embraces a **bleeding-edge** stack, adopting state-of-the-art technologies and solutions **to the fullest extent possible**

## Preset Knowledges

### Requirements

### Skill Rules

- If a task matches a skill, you MUST invoke it
- Skills are located in .agents/skills/<skill-name>/SKILL.md
- Never implement directly if a skill applies
- Always follow the skill instructions exactly (do not partially apply them)

### Automatic Evolution And Explicitly Recruiting

- 

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

## Workflow

Whenever a user presents a request, the first step should always be to ask for additional information to clarify and refine the requirements. Next, break the request down into a set of separate subtasks.

For each subtask, we may need to explicitly create a plan, track progress, validate the results, and review the completed work. Therefore, create a dedicated workspace for each subtask under `.agents/tasks` in the project root to record and manage these materials.


## Extension

### Creating a New Skill