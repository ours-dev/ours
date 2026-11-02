# Trigger evals

`skill_triggers.json` holds the cases; there are two ways to run them.

## The claude-CLI runner (`skill_trigger_eval.py`)

Spawns each case as a fresh `claude -p` session in the repository root and
watches the event stream: a Skill call, or a read that touches a file inside a skill folder, is the verdict; a non-read-only tool is stopped before the CLI runs it, and twelve read-only steps without a skill load count as "no skill". Objective, and it observes the session live. `CLAUDE_BIN` names the binary when `claude` is not on PATH.

Its failure mode is silent: without a login on the machine, every case "answers" in under a second with `Not logged in · Please run /login`, the harness counts the error result as `answered`, and the run prints a plausible-looking score of nothing. If every `secs` is under two and every `trail` is empty, the runner had no credentials, not that the skills failed.

## The ZCode-mode procedure

A ZCode session runs the suite itself; no script can spawn its subagents, so this is a procedure and not a program.

1. For each case, spawn a fresh `general-purpose` subagent whose prompt frames the query:

   ```text
   You are part of a skill-selection evaluation for this repository.

   Treat the following as a user's message:

   "<the query from skill_triggers.json>"

   Decide whether this request calls for loading one of the project skills available to you before doing the work. If it does, invoke the Skill tool with that skill now. Then STOP immediately — do not follow the skill's instructions, do not execute the task: no builds, no boots, no git commands, no file edits.

   Your final report must be exactly two lines:
   SKILL: <the skill name you invoked, or the word none>
   REASON: <one sentence, why>
   ```

2. Grade any-of: the loaded skill is one of the case's `expect`, and a `none` case passes on `none`. A nested skill (`engineering/*`, `language/*`) is never discoverable, so an `expect` that names one passes on its discoverable alternative.
3. About three subagents run concurrently (the plan's parallel limit); batch the cases by three.

The oracle is weaker than the CLI runner's: a self-report after an explicit stop instruction. It does not catch a session that loads its skill mid-execution instead of up front. What it preserves is the part that matters: a fresh context, the real skill listing, a real Skill-tool call, and the model's own choice of which skill.

## Results

- 2026-11-03, ZCode mode, all 26 cases: 26/26. The renamed `verify` and `kernel-design` triggered on 11/11 of their cases; `merge` triggered on its three positive cases and stayed closed on the `--no-ff`-versus-fast-forward concept question; the six `none` cases loaded nothing. The review cases loaded `kernel-design` (the discoverable member of their `expect` pairs) and cited AGENTS.md's Skills section as their reason.
