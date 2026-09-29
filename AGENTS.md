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