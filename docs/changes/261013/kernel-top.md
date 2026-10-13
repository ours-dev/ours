# kernel-top changes

## The two sample threads that kept two CPUs busy for ever are gone (2026-10-13)

**Files**
- `kernel/top/main.cpp` — `labour_routine()` no longer starts the "gibberish" threads `Sample1` and `Sample2`
- `scripts/kshell_run.py` — after the boot's end marker, console silence counts only while `lockup spin all` runs (`stops_every_cpu()`); the `BUSY` table is gone
- `.agents/skills/ours-verify/SKILL.md`, `.agents/skills/ours-verify/scripts/boot_check.py` — the notes that assumed the dbg image logs for ever

**Symptom**
Two CPUs never halted on any image: `idle.test` read 0 halts and 0.0% halted on CPUs 2 and 3. The cause was two threads started at boot that spun for ever logging "I'm Thread N" at debug level.

**Cause**
Bring-up scaffolding for watching the scheduler run two busy threads, left running on every image. It defeats idle power on two of the four CPUs, and buries the dbg log. The user chose to delete it (option ii, 2026-10-13; recommended was a boot option, off by default).

**Change**
- **The threads.** Deleted.
- **The script.** `kshell_run.py` had leaned on them: it took a dbg console that went quiet for 15 s as every CPU stopped, and sent QEMU an NMI for a lockup report. Without the threads, a dbg machine that has finished booting is quiet because it is idle.
  - Silence now counts during the boot, which logs as it goes.
  - After the end marker it counts only for `lockup spin all`, the one command that stops every CPU and so is reported only through that NMI.

**Reference**
None, this project's own scaffolding.

**Verification**
- Built and booted with `boot_check.py`: dbg and rel PASS.
- `kshell_run.py idle.test hotplug.test "lockup spin all"` on dbg: all ok, `lockup spin all` reported through the script's NMI after 15 s of silence.
- `kshell_run.py "lockup status" "lockup spin 2"`: ok, cpu2 found by cpu0's check after 10 s.
- `idle.test` now reads every CPU 99.8% to 99.9% halted, each still woken about 100 times a second by its tick.

**Deliberately not done**
The example session in `docs/development/debugging.md` still lists `Sample1` and `Sample2`; it is a record of one session, left as it was.

**How it was found**
`idle.test`'s baseline (step 1 of `.agents/tasks/idle-power`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did `kshell_run.py` treat a quiet dbg console as a hang, and why is that wrong now?
2. Why does `lockup spin all` still need the silence-triggered NMI, while `lockup spin 2` does not?
3. What did the two threads cost each CPU they ran on, in halts per second?
