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

## Working with the user

- **Ship features at "basically usable," not more.** Stop once it works and
  passes `ours-verify` — don't chase completeness, and don't add scope beyond
  what was asked.
- **Performance work comes after a feature is basically complete, and goes
  through the user in depth, item by item.** This is what the user values
  most here — they're building it into interview-ready experience. Before
  starting any performance work, lay out the measurement method, where the
  bottleneck is, and each candidate's mechanism and cost, and let the user
  decide; never optimize quietly, and never hand over a bare conclusion. A
  performance problem noticed while building a feature goes into
  `docs/repairs/`, not into an opportunistic fix. While building something to
  "basically usable," if a choice would cap its performance ceiling later
  (lock granularity, data structure, copy count), treat it as a decision to
  put to the user.
- **Concurrency is the teaching focus.** When a locking or concurrency
  question comes up (which lock, what granularity, lock order, how much the
  lock covers, lock-free structures), use it to teach the user how to think
  about it: which invariant is being protected, who accesses it concurrently,
  what the worst interleaving looks like, and what would change with a
  different lock.
- **Let data decide between locking options.** Write a benchmark in
  `benches/`, measure time held, contention rate, and latency distribution,
  and compare options with numbers, not just theory. Correctness is the
  exception: measurement can't prove "no race" — correctness rests on an
  invariant and lock-order argument, backed by lockdep and stress tests.
- **Close the learning loop: get output from the user, not just approval.**
  Picking a recommended option and saying "sounds good" exercises
  recognition, not the ability itself — an interview is a whiteboard with no
  AI. So:
  - For an architecture or concurrency decision, have the user write a
    one-line prediction first (which option, why, what will go wrong),
    before giving the analysis. When it's something to measure, put the
    prediction in that change's change record and check it against the
    result once it's in. Skip this step for renames and mechanical changes.
  - For a concurrency decision (which lock, lock order, how much it covers),
    have the user write the argument first — which invariant it protects,
    who accesses it concurrently, what the worst interleaving is — then have
    the AI grade it. Don't skip this.
  - At each milestone, pick one core function for the user to write by hand:
    the AI gives the spec and a failing test, the user writes it under a
    time limit, and the AI reviews it as a strict interviewer would. Record
    functions the user wrote in `docs/authored.md`.
- **Strategy and decisions belong to the user.** When a strategy needs to be
  set, a trade-off made between approaches, or a design doc or Zircon needs
  to be departed from, stop first: lay out the background and details, give
  a few options you think are good with your recommendation marked, and wait
  for the user to decide.
- **Push back, don't flatter.** When the user's idea is wrong or one-sided,
  say so directly and explain why; if you have a better idea, raise it and
  argue for it.
- **Never do something quietly when unsure.** When something is hard or
  impossible to decide on your own, write it up as a question for the user
  instead of deciding on your own and implementing it silently.
- **Keep answers short.** Lead with the conclusion, minimize setup, don't
  pile on.

## Workflow

Whenever a user presents a request, the first step should always be to ask for additional information to clarify and refine the requirements. Next, break the request down into a set of separate subtasks.

For each subtask, we may need to explicitly create a plan, track progress, validate the results, and review the completed work. Therefore, create a dedicated workspace for each subtask under `.agents/tasks` in the project root to record and manage these materials.


## Extension

### Creating a New Skill