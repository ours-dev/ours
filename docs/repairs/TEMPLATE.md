# Repair item template

`docs/repairs/<YYMMDD>/<MODULE>.md` lists only what is **not fixed yet**: one numbered item per problem, most severe first. A new file starts with the H1 `# <MODULE> outstanding repairs`, where `<MODULE>` is the file name without `.md`.

Each item must be claimable on its own: someone should be able to take item 3 without doing item 1 first. If an item does depend on another, say so in its Cause.

When an item is fixed, delete it from this file and append an entry to `docs/changes/<YYMMDD>/<MODULE>.md` (template: `docs/changes/TEMPLATE.md`). The fields below match that template on purpose, so the text moves over with little rewriting.

A field with nothing to say gets the word "None", not a deleted heading.

Items written before this template was written are in Chinese and use labels such as 现象, 严重程度, 原因, 改法, 参照, 验收. They are not rewritten; new items use the labels below, even when they are added to an older file.

---

```markdown
# <MODULE> outstanding repairs

## 1. One sentence saying what is wrong

**Severity**
High, Medium or Low, as the first word, then a short reason. High: wrong results, crashes or data loss on a path the kernel runs today. Medium: wrong, but not reached today or only under a condition you can state. Low: naming, style, a missing test, cleanup.

**Symptom**
What can be observed. For a latent problem write "Latent, cannot be triggered today" and say under what condition it would be.

**Cause**
Why this is a mistake, in enough depth that the fix is obvious to someone who has not read the code. Include any dependency on another item.

**Change**
The intended fix, and the options ruled out with reasons. For a performance item, the measurement to take first (see "Working with the user" in CLAUDE.md).

**Reference**
The counterpart in Linux or Zircon, down to the function name, or "None, this project's own design".

**Acceptance**
How to tell it is fixed: the command to run, the boot line to see, the test that must pass.
```
