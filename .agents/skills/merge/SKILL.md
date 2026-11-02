---
name: merge
description: Integrating branches with the baseline (baseline/20260824), both directions. Use whenever a branch is to be merged, integrated or 合并 into the baseline or 主线, or a baseline is to be taken into a branch, before running any git merge; while conflicts are being resolved; and for the shared-checkout dance whenever the baseline checkout is dirty. A merge done out of order (branch first, baseline later) turns every conflict into a double resolution and can land a boot-broken baseline.
---

# merge

The baseline branch (`baseline/20260824`, checked out at the project root) is
the line every topic branch grows from and lands on. Integration is two-way
and ordered, and the order is the whole skill: the baseline goes into the
branch first, the branch is verified whole on the target, and only then does
the branch go back.

## The order

1. **Baseline into the branch, in the branch's worktree.** `git merge
   baseline/20260824`. Every conflict is resolved here, on the branch, where
   the branch's tests and the skill `kernel-design`'s settled decisions are at
   hand. Read both sides' change records (`docs/changes/<YYMMDD>/<module>.md`
   on both branches) before deciding a conflict: they say what each side
   intended, and several conflict classes have a standing answer —
   append-only change records concatenate; a deleted file whose job a newer
   mechanism took stays deleted; both sides' enums union.
2. **Verify the branch whole.** `python3
   .agents/skills/verify/scripts/boot_check.py` -- both images and the host
   tests, with the disk if the branch carries disk work (`DISK=<file>`) -- and
   boot the shape each new suite needs. A conflict resolution that compiles
   but changes a walk's semantics passes nothing; run the suites that name the
   resolved feature.
3. **The branch into the baseline, in the baseline checkout.** `git merge
   --no-ff <branch>`. `--no-ff` on purpose: the baseline keeps one commit that
   says the branch landed, and the branch stays free to continue past the
   integration point. Check `git merge-base --is-ancestor` first; when the
   baseline is an ancestor, `--ff-only` is the shape for a hotfix-shaped move
   the branch does not continue past.

## The baseline checkout is shared

The project-root checkout carries other sessions' work in progress -- at any
hour, hundreds of dirty paths, some of them files the merge must update. The
merge takes the index; their staged work would ride along, and their unstaged
files would block the merge outright. The dance:

1. Count what actually collides: the merge diff `git diff --name-only
   baseline/20260824 <branch>` against `git status --short`'s dirty set.
   Staged entries (`M `/`A ` first letter) anywhere means stop -- staged work
   rides a merge commit into history, and unstaging another session's index
   is their call, not yours.
2. Nothing collides: merge straight away.
3. A few collide, and their mtimes say the session has gone quiet (hours, not
   minutes): `git stash push -- <the colliding files>`, merge, then
   `git stash show -p | git apply --check -` before `git stash pop` -- the
   check refuses a pop that would strand conflict markers in a live session's
   tree. Say in the report what was stashed and popped, and in which order.
4. Much collides, or the session is live (mtimes minutes old): stop and ask
   the user. Their WIP is not yours to park.

After the merge, say whether the working tree returned to the dirty count it
had (`git status --short | wc -l`).

## Verifying an integration

The merge commit's tree is what needs to boot, and it is often a tree that
was already verified: when `git diff <branch> HEAD --stat` is empty after the
merge, the integration landed the verified tip byte-identical, and re-running
the boot adds nothing. Say that check and its empty result in the report
instead. Any non-empty diff (a resolution that leaned either way, a
conflicted file's splice) is a new tree: boot it, in the target worktree's
own `--out`, never a shared one.

## The commit

`merge(baseline): <branch> into baseline/20260824` for the way back; the
reverse direction names the branch it lands on. The body says what landed,
the resolutions a reader could not reconstruct from the diff, and the
verification with its counts. Never `git add` before a merge commit: the
index is the merge result, and pre-staged files sneak in.
