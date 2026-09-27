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

## Workflow

Whenever a user presents a request, the first step should always be to ask for additional information to clarify and refine the requirements. Next, break the request down into a set of separate subtasks.

For each subtask, we may need to explicitly create a plan, track progress, validate the results, and review the completed work. Therefore, create a dedicated workspace for each subtask under `.agents/tasks` in the project root to record and manage these materials.


## Extension

### Creating a New Skill