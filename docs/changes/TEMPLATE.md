# Change record entry template

One entry per change, appended to the end of `docs/changes/<YYMMDD>/<MODULE>.md`. A new file starts with the H1 `# <MODULE> changes`, where `<MODULE>` is the file name without `.md`.

The fields are fixed because the reader is here to learn: they want to know "why was this a mistake" and "what should it have been modeled on", not just "which line changed".

A field with nothing to say gets the word "None", not a deleted heading. A visible "None" reads better than a missing field.

Entries written before this template was translated are in Chinese and use the labels 文件, 症状, 原因, 改法, 参照, 验证, 故意没做. They are not rewritten; new entries use the labels below, even when they are appended to an older file.

---

```markdown
## Title: one sentence saying what changed (YYYY-MM-DD)

**Files**
- `path/to/file.cpp` — one line on what changed in this file

**Symptom**
What can be observed: an error, a hang, a wrong number. For a latent problem write "Latent, cannot be triggered today" and say under what condition it would be.

**Cause**
Why this was a mistake. **This section is the main learning value, so write it thick**: not just "one write was missing", but why missing that one write means it does not work at all (for example, the HPET comparator is an equality comparison, so being one tick late is not late, it is never).

**Change**
What was done. Quote the key code as it was before and as it is after; prose alone is not enough. Where there was a trade-off, also write down the rejected options and why they were rejected.

**Reference**
The counterpart in Linux or Zircon, **down to the function name**, so the reader can go compare. For example: `Linux clockevents_program_min_delta()`, `Zircon Timer::Cancel()`. If there is no counterpart write "None, this project's own design" and give the design reason.

**Verification**
Build, clang-tidy, QEMU runs. Put numbers in when there are numbers, including comparison tables. Say "not measured" for anything that was not verified.

**Deliberately not done**
And why. This often says more than what was done: it tells later readers where the boundary is and which things were considered rather than missed.

**How it was found**
For a bug: which commands were run and which outputs were read (logs, registers, lldb) to get from the symptom to the root cause. This is teaching material for debugging craft. For a new feature write "None".

**The user's prediction**
When there was a decision card or a measurement: the user's prediction made in advance, compared with the actual result, and where they differ. Otherwise write "None".

**Three questions you should be able to answer**
Three questions the user should be able to answer from memory after closing this record, for review and self-testing. Ask about reasons and consequences, not "which file was changed".
```

---

## A well-written example

See the section "频率改用比值法，窗口拉长到 200 ms" (frequency by the ratio method, window lengthened to 200 ms) in `260903/x86-clock.md`.

## Relation to `docs/repairs/`

- `docs/changes/<YYMMDD>/<MODULE>.md` — what changed and why. **Append-only**; it is history.
- `docs/repairs/<YYMMDD>/<MODULE>.md` — what is not fixed yet (template: `docs/repairs/TEMPLATE.md`). When an item is fixed, delete it from there and append an entry here.
