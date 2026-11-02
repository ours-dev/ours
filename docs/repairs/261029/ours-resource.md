# ours-resource outstanding repairs

## 1. The README, the build file and the header comment describe three protocols, and the header declares five

**Severity**
Low: documentation only. A reader of the README learns that `ours.resource` has `Debug`, `Irq` and `Ioport` and does not learn that `Sampling` and `ours.kernel.RootJob` are served by the same codec.

**Symptom**
`system/lib/ours.resource/README.md:3` ("The three Gets share one shape"), `system/lib/ours.resource/BUILD.gn:4-6` ("Debug, Irq and Ioport") and the header comment of `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:14-35` (lines 20-23 list three, line 32 says "The three Gets") all stop at three. The same header declares `SAMPLING_PROTOCOL` (line 100) and `ROOT_JOB_PROTOCOL` (line 116). The second has the library name `ours.kernel`, not `ours.resource`, which the comment at lines 110-115 explains but the README does not.

**Cause**
`Sampling` and `RootJob` were added after the three, with their own comments, and the library-level texts were not revisited.

**Change**
Say "five protocols" and list them in the three places, and name `ours.kernel/RootJob.Get` as the one whose library name is not this library's.

**Reference**
None, this project's own design.

**Acceptance**
The three texts agree with the declarations of the header.
