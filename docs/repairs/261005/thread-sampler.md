# thread-sampler outstanding repairs

## 1. sampler_create allocates every buffer before checking the claim, so concurrent creates defeat the 4 MiB-per-CPU bound and can hang the kernel

**Severity**
Medium: The mechanism is real, but only as a latent defect.

**Symptom**
On dev.dbg_1 (512 MiB, 4 CPUs), a program holding SAMPLING (bin/profile gets a duplicate from the shell) creates one session. It then starts about 30 threads, each looping sampler_create(sampling, 0, {period = 1 ms, buffer_size = 4 MiB, targets = Kernel}). Each call commits 16 MiB of slots before it learns AlreadyExists, and the queue on vmalloc's lock keeps those blocks alive together. About 30 x 16 MiB (fewer, given what is already in use) exhausts memory. alloc_frames_slowpath then spins for ever while holding vmalloc's LOCK, and every page fault and large allocation in the system hangs. That is exactly what the change record "One sampler_create can no longer ask for most of the kernel's memory" set out to prevent. On a 16-CPU build (64 MiB per create) about 8 threads are enough.

**Cause**
Session::create() allocates first and checks the claim second. It builds the session, its slot block (possible CPUs x buffer_size, one vmalloc) and, for the Kernel target, the FDE index (1475 FDEs x 32 B, about 47 KB, a second vmalloc). Only then does it take s_lock and look at s_claimed. Nothing limits how many creates are in that phase at once, and that holds even while a session already exists; each loser frees only after building everything. The 4 MiB cap (ed09eb11) exists because the frame allocator retries for ever instead of failing (docs/repairs/260908/heap.md item 3), and its comments promise a per-create bound. N concurrent creates take N times that bound. vmalloc makes it worse: it holds one global Mutex across commit and populate (vmalloc.cpp:119). A create's FDE-index vmalloc therefore queues behind the other creates' slot vmallocs, so all N slot blocks are live at the same time. Upstream ThreadSampler::SetUp refuses ALREADY_EXISTS under its lock before allocating; that pre-check was dropped here. Two side effects: a create while a session exists can answer NoMemory instead of the documented AlreadyExists, and thread_sampler.hpp:171-175 still says the control lock is held by "create ... across allocations", which has been false since 89b0d6e4 and hides the problem. Fix: refuse when s_claimed is set before allocating, as upstream does. To close the race in which no session exists yet, reserve the claim (a Claiming state) before allocating and give it back if allocation fails.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (syscall-security-1, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler). Also reported as claims-3.
