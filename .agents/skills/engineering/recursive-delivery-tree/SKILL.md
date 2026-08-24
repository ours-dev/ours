---
name: recursive-delivery-tree
description: Orchestrates parallel feature development across multiple modules using a recursive tree of Claude Code sub-agents (via the Agent tool) — each node analyzes requirements, designs a breakdown, dispatches parallel implementers in isolated git worktrees (or recurses into child nodes for work that's still too big), verifies each deliverable the moment it lands, integrates, and gets an independent acceptance review before reporting status upward. Use this whenever the user wants to parallelize implementation across several modules or features, asks for a "总控"/orchestrator/master agent to manage other agents, wants a tree or hierarchy of agents with built-in review gates, or describes needing requirements analysis → design → implementation → verification → acceptance as separate steps for delivering multiple features at once.
---

# Recursive delivery tree

## Overview

A pattern for turning a breakdown of modules and features into a tree of Claude Code
agents that plan, implement, verify, and accept work — then report a synthesized
status upward, one level at a time, until it reaches whoever asked for the work.

The key idea: there is no separate "root template" and "module template" and "leaf
template". There is exactly **one recursive template**. The human's request is just
depth 0 of it. This matters because real work rarely decomposes into a clean fixed
number of layers — a module might need to split into sub-modules, which split into
sub-features, and you don't want to have designed a rigid 3-tier org chart when the
work actually needs 5 tiers in one branch and 1 in another.

## When to use this

- The user has several modules or features that can be built in parallel and wants
  each one to go through its own requirements → design → build → verify → accept
  cycle, not just get implemented and hoped-for.
- The user asks for a "master" or "orchestrator" agent that coordinates other agents,
  especially when they describe wanting review/QA gates between the workers and the
  final report.
- The work is large enough that a flat "spawn N agents, review the diffs yourself"
  approach would make you the bottleneck for every verification and every integration
  decision.

**Don't use this for a single, already-well-scoped task** — spawn one agent and let it
work. The overhead of requirements analysis + independent design + independent
acceptance only pays for itself when there's real decomposition and parallelism to
manage.

## The recursive node template

Every node in the tree — including the human's own top-level request — runs the same
six steps. A "node" is not necessarily a spawned agent; at depth 0 the node *is* the
current conversation. At depth 1+, the node is an agent spawned with this same set of
instructions, scoped to whatever slice of work its parent handed it.

```
 scope handed down from parent (or the human, at depth 0)
            │
            ▼
   1. requirements analysis  (independent agent)
            │
            ▼
   2. design                 (independent agent)
            │  produces: child work-items, each with
            │  acceptance criteria + an atomic/non-atomic call
            ▼
   2.5 resume the requirements-analyst to sanity-check the
       design's criteria against the original requirements
       — cheap, reads documents only, no implementation yet
            ▼
   3. dispatch, per child item, in parallel
            │
   ┌────────┴─────────┐
   ▼                   ▼
 atomic              non-atomic
   │                   │
 leaf agent          recurse: spawn another
 (isolation:          instance of THIS WHOLE
  worktree)            TEMPLATE, scoped to
   │                   that item
   │                   │
   └────────┬──────────┘
            ▼
   4. verify THIS item, immediately on completion
      (independent agent — fail → send feedback back
       to the SAME implementer/child, don't respawn)
            │  (repeat 3-4 per item; items don't wait on each other)
            ▼
   5. once every item is individually verified: integrate
      (the node does this itself — merge branches, check
       for cross-item conflicts — no sub-agent needed)
            ▼
   6. acceptance review        (independent agent, saw
      none of the above steps)
            │
      reject──┴──accept
      (rework loop      (this node's own final answer IS
       back to 2 or      the "done" report — see the async
       to specific       gotcha below for why that matters)
       children)
            │
            ▼
   report synthesized status to parent (or to the human,
   at depth 0)
```

### Where acceptance criteria actually come from

Acceptance criteria are not something the design step invents from scratch — they're
carved out of the requirements document that came before it. The requirements
analysis step should already state "what needs to exist when this scope is done" as
checkable claims, not vague feature descriptions. When the design step splits the
scope into child items, each item's acceptance criteria should be the slice of the
parent's requirements that item is responsible for satisfying — traceable back to a
specific line in the requirements doc, not freshly authored by whoever happens to be
doing the design.

Make criteria concrete enough that someone who never saw the implementation can judge
pass/fail without a judgment call. Prefer automatable checks (a build that succeeds,
a specific test that passes, a documented behavior for a given input) over prose like
"works correctly". Where the project can't yet build or test a given area (true for
parts of this kernel today), fall back to explicit, checkable structural criteria
instead — "exposes interface X", "does not modify file Y", "follows the conventions
in `modern-cpp-development`" — rather than leaving the criterion vague because
automation isn't available.

**This has a self-grading problem worth naming.** The same design step that splits the
work also writes the criteria those splits will be judged against. If a criterion is
too lax, or misses part of what the requirements actually asked for, every later
step — the per-item verifier, even the independent acceptance reviewer — is checking
against a wrong yardstick and can end up rubber-stamping something that never
satisfied the original ask. The acceptance-review step at the end is the real
backstop for this, since it judges the integrated result against the original
requirements directly rather than against the design's stated criteria — but that
backstop only fires after every item has already been implemented, so a bad criterion
is expensive to discover that late.

Catch it earlier and cheaper: after the design step produces its breakdown, **resume
the requirements-analyst agent** (the same one, not a new one — it already knows the
requirements it wrote) and have it sanity-check the breakdown before anything gets
implemented:

```
Here is the design/breakdown produced for the requirements you wrote:
[paste the design doc, including every child item's acceptance criteria]

If every child item's acceptance criteria were fully satisfied, would that actually
satisfy everything you required? Call out anything missing, anything a criterion
states too loosely to be checkable, and anything a criterion covers that isn't
actually required. This costs nothing to fix now and gets expensive to fix after
implementation starts.
```

This check only reads documents — no implementation has happened yet, so it's nearly
free compared to discovering the same gap at final acceptance after every item has
already been built.

### Why each step is independent

- **Requirements analysis and design are separate, independently-spawned agents** —
  not something the node just thinks through inline. A fresh context forces the
  design to actually be checked against the stated requirements instead of drifting
  along with whatever the node was already assuming.
- **Verification happens per item, the instant that item is reported done** — not
  batched at the end. This costs more verification calls than checking everything
  once at the end, but it surfaces a broken implementer immediately instead of after
  every sibling has also finished, and it keeps each item's rework loop from blocking
  the others.
- **Verification is a fresh agent given only the diff/output and the acceptance
  criteria** — not the implementer's own account of what it did. An agent grading its
  own work tends to agree with itself.
- **Acceptance is a fresh agent that participated in none of the prior steps** — not
  the designer, not an implementer, not a verifier. It only sees: the original
  requirements, the design, the integrated result, and the verification reports. This
  is the same reason a real org doesn't let the implementer sign their own QA sheet.
- **Integration is NOT a separate agent** — merging worktree branches and checking for
  cross-item conflicts is mechanical (git + reading diffs) and the node doing it
  itself is both cheaper and fine, since integration isn't a judgment call the way
  acceptance is.

## The depth safety valve

Nothing stops a design step from concluding "this child item is still too big, recurse
again" indefinitely. Two things need to bound that in practice, and they bound
different things:

**A hard cap on real agent nesting.** Pick a depth (default: 3 levels of spawned
manager-agents below the human's own request) beyond which no new manager identity
gets created. Once a node is at the cap and its design step still finds non-atomic
child items, it does **not** spawn a child running this template — it keeps doing the
requirements/design decomposition itself, in its own reasoning, recursively, until
everything remaining is atomic, and then flat-dispatches all of those atomic items
directly as leaf implementers. The tree of *identities* stops growing; the tree of
*work* doesn't have to.

Why cap it here rather than trust "recurse until atomic" alone: every additional layer
of spawned agents multiplies both cost (each layer is a fresh context that has to
re-derive its scope from scratch) and the number of hops a completion notification has
to travel before it reaches the human. Neither failure mode announces itself loudly —
you just end up with a much more expensive run than expected, or a branch of the tree
that goes quiet.

**Validate shallow before trusting deep.** Real nested Agent-tool spawning — a
spawned agent itself spawning agents, whose completions need to propagate back up
through the first agent to whoever asked — has not been exercised at depth beyond 1-2
in this environment as of writing this skill. A pilot run of just one level of nesting
(a manager spawning two trivially small dummy leaf tasks) did not return a final
synthesized result within a reasonably long conversation window. That doesn't mean
deeper nesting doesn't work — it means it hasn't been confirmed to work, which is a
different thing. **The first time you use this skill for real work, start with a
depth cap of 1** (the human spawns manager-agents directly; those managers do not
recurse into further manager-agents, they flatten straight to leaves once they hit any
non-atomic item). Only raise the cap once you've watched a depth-1 run actually
complete and report back cleanly. Treat "the cap is set to 3" as a target to grow into,
not a default to trust blindly on day one.

## The async gotcha every manager-role prompt must handle

Spawning a child via the Agent tool is fire-and-forget: the call returns immediately
with "launched in the background", and the spawning agent's own turn simply ends
after that — it does not block waiting for children to finish. The spawning agent
gets a fresh turn later, automatically, when a child's completion notification
arrives.

This means a node that just launched five child items and then, in the same turn,
writes "all five are done, here's the integrated report" **is fabricating that
report** — it cannot possibly know that yet. Left to its own devices, an agent asked
to "coordinate children and report back" will often do exactly this, because ending
a turn with a confident-sounding summary is the path of least resistance.

Every prompt for a manager-role node (any node running this template above depth 0)
must say, explicitly, something like:

> After you spawn your children for this step, end your turn — do not fabricate or
> guess at their results. You will be resumed automatically, in a fresh turn, each
> time one of them reports completion. Keep track of how many you are still waiting
> on; this may take several turns to resolve. Only write your real synthesized report
> once every child you spawned for this step has actually reported back to you.

Skipping this instruction is the single most likely way this pattern silently produces
false "done" reports.

## Isolation

Every **leaf implementer** agent — the ones that actually write code for an atomic
item — must be spawned with `isolation: "worktree"` (an `Agent` tool parameter) so
that implementers running in parallel never collide on the same files. Requirements
analysts, designers, verifiers, and acceptance reviewers don't write code and don't
need worktree isolation.

## Self-contained prompts, always

Every spawned agent, at every role and every depth, starts with zero memory of
whatever conversation led to it being spawned. Whoever writes the spawn prompt — the
human at depth 0, or a manager node at depth 1+ — has to include the full scope for
that piece of work and its acceptance criteria explicitly. "See the discussion above"
means nothing to a fresh agent.

## Prompt templates

Fill in the bracketed parts. Keep the async-gotcha paragraph verbatim in every
manager-role prompt — don't paraphrase it away.

**Requirements analyst** (independent agent, no worktree needed):
```
You are doing requirements analysis for this scope: [scope description / the parent
node's ask]. This is a standalone task — you have no access to any prior discussion.

Read whatever is relevant in the repo to understand the current state of this scope.
Produce a structured requirements document: what needs to exist when this scope is
done, any constraints (interfaces it must honor, things it must not break), and open
questions the design step will need to resolve. Do not design a solution — only state
what's required.
```

**Designer** (independent agent, no worktree needed):
```
You are designing the breakdown for this scope: [scope description]. Here is the
requirements document produced for it: [paste requirements doc].

Break this scope into child work-items. For each item, state:
- what it covers
- its acceptance criteria (specific enough that someone who never saw your design
  could check a delivered result against it)
- whether it is ATOMIC (small enough for one implementer to finish end-to-end in one
  focused pass) or NON-ATOMIC (still needs to be broken down further by recursing
  into this same process)

Flag interfaces or contracts between items that implementers need to honor so parallel
work doesn't collide.
```

**Leaf implementer** (independent agent, spawn with `isolation: "worktree"`):
```
Implement this item: [item description from the design step]. Acceptance criteria:
[paste this item's acceptance criteria]. Any interface contracts with sibling items:
[paste relevant contracts from the design step].

This is a standalone task in an isolated worktree — implement, and where the project
has a test/build step, run it before reporting done.
```

**Verifier** (independent agent, spawned the moment its target item reports done, no
worktree needed):
```
Verify this delivered item against its acceptance criteria. You are not told anything
about how it was implemented — judge only the result.

Acceptance criteria: [paste this item's acceptance criteria]
Delivered diff/output: [paste or point at the item's diff/output]

Run the project's tests/build against this if the project has them. Report pass/fail
per criterion, and if it fails, describe exactly what's wrong — this feedback goes
straight back to whoever implemented it.
```

**Acceptance reviewer** (independent agent, spawned once after integration, saw none
of the prior steps, no worktree needed):
```
Review this completed scope for final acceptance. You did not participate in
designing, implementing, or verifying any of it — judge it fresh against what was
asked.

Original requirements: [paste requirements doc]
Design / breakdown: [paste design doc]
Integrated result: [paste the merged diff / final state]
Per-item verification reports: [paste all verification reports]

Render an accept/reject verdict for the scope as a whole. On reject, say specifically
what's missing or wrong and whether it needs a design change or just rework on
specific items.
```

**Manager** (this node's own orchestration logic — this is what depth 1+ nodes are
spawned with; depth 0 just follows it directly, since depth 0 is the human's own
conversation, not a spawned agent):
```
You are managing this scope: [scope description, self-contained — the full ask, not
a reference to prior discussion].

Run the recursive-delivery-tree pipeline for this scope:
1. Spawn a requirements-analyst agent for this scope.
2. Once you have the requirements, spawn a designer agent with them.
2.5. Resume the requirements-analyst agent (the same one — not a new one) with the
   design's breakdown and ask it to sanity-check every child item's acceptance
   criteria against what it actually required. Fix any gaps it flags before moving
   on — this step only reads documents, so it's cheap compared to discovering a bad
   criterion after implementation.
3. For each child item the design produces: if atomic, spawn a leaf implementer
   (isolation: worktree). If non-atomic AND you are below the depth cap
   ([N] levels below the human's original request), recurse — spawn another
   manager agent (this same template) scoped to that item. If non-atomic and AT the
   depth cap, do the decomposition yourself in your own reasoning instead of
   spawning another manager, until everything under this item is atomic, then
   dispatch those as leaf implementers directly.
4. The moment any single item reports done, spawn a verifier for just that item.
   On failure, send the verifier's findings back to that SAME implementer/child (by
   name/id — do not spawn a replacement) and ask it to fix, then re-verify. This
   loop is per-item; don't let it block progress on sibling items.
5. Once every item is individually verified, integrate them yourself (merge worktree
   branches, check for cross-item conflicts) — you don't need another agent for this.
6. Spawn an acceptance-reviewer agent with the original requirements, the design, the
   integrated result, and all verification reports. On reject, loop back to design or
   to specific items for rework. On accept, your own final answer for this task IS the
   report that reaches whoever spawned you — make it a synthesized status of this
   whole scope, not a placeholder.

Important: spawning a child does not block you. Your turn ends right after you launch
one; you get resumed automatically, in a fresh turn, each time a child reports back.
Do not fabricate or guess at results in the meantime. Track how many you're still
waiting on — this may take several turns. Only produce your real final report once
every child you're responsible for at each step has actually reported back to you.
```

## Starting a run

Give the human's breakdown directly, e.g. "module A: feature 1, feature 2; module B:
feature 3" (or point at a requirements doc). Treat the whole thing as depth 0's scope:
run requirements analysis and design on it yourself (spawning those two agents as
usual), let the design step decide whether each named module is atomic enough to hand
straight to a leaf or still needs its own manager-agent, and proceed from step 3 of
the template above. Remember the depth-cap-of-1 recommendation above for the first
real run.

## Common rationalizations

| Rationalization | Reality |
|---|---|
| "I'll just skip the independent acceptance step, the verifiers already checked everything" | Verifiers check items in isolation; acceptance is the only step that looks at whether the pieces actually fit together against the original ask. |
| "I'll wait in this turn until all children are done" | You can't — spawning is async. Writing a result before the completion notifications arrive is fabrication, not patience. |
| "Depth cap of 3 is the design, so I'll use 3 from the start" | The cap is a target once nesting is proven to work, not a starting point. Nobody has confirmed multi-hop notification propagation past 1 level here yet. |
| "This item is basically atomic, I'll skip the non-atomic recursion check" | That judgment call belongs to the design step, explicitly, so it's checkable — not to whichever step happens to be impatient. |
