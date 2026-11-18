# ktest outstanding repairs

## 1. `ktest sel=list` and `ktest sel=death` reject their own selection word

**Severity**
Medium, `ktest sel=list` and `ktest sel=death` always fail today, but `ktest all` and filter runs are unaffected.

**Symptom**
On the dbg image, `ktest sel=list` prints `ktest: ERROR ktest sel=list takes no options` and lists nothing. `ktest sel=death` prints the `sel=death` usage line.

**Cause**
`ktest_command` (`kernel/lib/ktest/src/runner.cpp`) counts `invocation.words().size()`, and `words()` holds every word after the command name, including the `sel=` word itself. The list branch requires a count of 0 and the death branch a count of 0 or 1 (with `filter=`), so the selection word alone breaks the rule. The checks were written for the positional grammar (`ktest list`), where the selection was not one of the option words, and were not updated when `sel=` was introduced (d84981e3).

**Change**
Count the words other than `sel=`: subtract one when `values.has("sel")`. Once the command is an object (a later step of `.agents/tasks/ktest-hsien-oop/PLAN.md`), this becomes a method that asks the parsed options how many words the selection did not claim.

**Reference**
None, this project's own command grammar.

**Acceptance**
`python3 scripts/kshell_run.py --out out/<dir> 'ktest sel=list'` passes and prints every `ktest: TEST` and `ktest: DEATH` line. `ktest sel=death filter=<name>` reaches the `ktest: DEATH <name>` line.
