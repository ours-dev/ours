# kshell-word-cap outstanding repairs

## 1. A line past the console's 16-word cap truncates silently

**Severity**

Medium: wrong, only reachable today by typing more than 15 options after a
command — but the failure is silent, so a long options line looks like the
command accepted it and ignored most of it.

**Symptom**

`kshell/kshell/shell.cpp`'s dispatch splits a line into `char *argv[16]` and
stops adding words when the table is full; whatever the line carried past
the cap is dropped without a word. A command told to carry more than 15
options sees 15 and answers as if the rest had never been typed — for the
options builder, that means `TooManyWords` can never fire from the console
(the builder's own cap is generous against this), and a long
`ktest sel=all a=1 b=2 ...` line would run with only the first 14 test
options set.

**Cause**

The console's bring-up scope states the cap ("16-argument cap, 128-byte
lines — bring-up scope by design", kernel/lib/kshell/README.md) but not the
silence. The splitter's loop condition `argc < 16` bounds the table and the
loop leaves the rest of the line unparsed; nothing reports what it dropped,
and the runner discards command exit codes, so the only evidence is the
command answering as if the tail did not exist.

**Change**

Report the truncation at error level — one line naming the command word and
the cap, the same way a rejected word is reported — and fail the dispatch
there (the command does not run, because its argument list is not the one
that was typed). The options builder's `TooManyWords` fault stays as the
host-side belt: a host test driving more words than the tables hold still
sees the rejection.

**Reference**

None, this project's own design — Zircon's console tokenizer
(`zircon/kernel/lib/console/mod.rs`, tokenize_command) has the same cap
(MAX_NUM_ARGS = 16) and the same silent behavior, so there is no upstream
shape to copy; the report-instead-of-drop rule is the repo's own
rejected-word convention applied to the console.

**Acceptance**

A line with a 17th word prints the error line and the command does not run;
kshell_run.py's grammar tests cover the line.
