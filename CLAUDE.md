# CLAUDE.md

Load skills from .agents/skills

Write C++ code through skill @cpp-write-and-verify

## Commit after every change

One change, one commit, made as soon as that change builds and boots. Do not
let several fixes pile up in the working tree: once they blur together the
commit messages stop being writable, which is how this tree reached ~280
uncommitted paths.

The rhythm is: diagnose -> fix -> build -> boot it -> commit -> next thing.
An update to the running repair document belongs in the same commit as the
change it describes.

This worktree often holds work from other concurrent sessions. Check
`git diff <file>` before staging: a file can carry both your hunks and someone
else's in-flight feature, and sweeping theirs into your commit is worse than
leaving yours uncommitted.
