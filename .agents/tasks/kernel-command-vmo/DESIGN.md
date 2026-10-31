# kernel-command-vmo: give the user shell the kernel's command list as a read-only VMO it maps

**Date**: 2026-10-31
**Status**: Proposed

## Problem
The user shell lists the kernel's console commands by calling `ours::command_list` into a fixed `char m_text[2048]` (`system/app/shell/shell.cpp:87`, `:100`). The registry text is 4002 bytes, so half of it is cut: this is item 1 of `docs/repairs/261031/shell.md`. Measured at HEAD `ec94fc21`, from the built dbg.1 `vmours` (`out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours`) and the sources:
- `__commands_start` is `0x116000` and `__commands_end` is `0x1163d8`, so 984 bytes, which is 41 records of 24 bytes. The sources hold 41 `GKTL_COMMAND` invocations in 40 files, the same number.
- Written as `command_list` writes them (`<cmd>`, two spaces and the help if there is one, a newline), the 41 records are 4002 bytes. The buffer holds 2047 bytes and a NUL, so 20 commands are shown and the 20th has its help cut. Which 20 follows link order.
- The longest line is 203 bytes (`lockup`, help 194), the longest command name is 21 bytes, and one command has an empty help (`dump-frame`, `kernel/mm/pm_frame.cpp:51-54`).

The user chose, over an offset parameter on `command_list` and over a bigger buffer, to expose the registry to the shell as a read-only memory object (a VMO) that the shell maps and reads, as Zircon does for kernel counters (2026-10-31). That choice is the only thing in this document that is decided. Every other piece below is a proposal with options, for the user to decide.

The raw records cannot be shown as they are. `gktl::Command {char const *cmd; char const *help; Callback callback}` (`kernel/lib/gktl/include/gktl/command.hpp:9-15`) holds three kernel addresses, which mean nothing in another address space, and the section that holds the records shares its pages with other read-only kernel data (`kernel/kernel.ld:161-188`). So the kernel has to build a self-contained table, with the names and help strings inline, in pages of its own, and hand out a read-only VMO of it.

## Goals and non-goals
Goals:
- The shell lists every registered command (41 today) with its whole help text, in an order that D2 settles, and the shell holds no buffer that a longer registry can outgrow.
- No handle that exists can write the table.
- Each implementation item lands on its own and leaves both x86 images booting.

Non-goals:
- Running kernel commands from user space. The registry is listed and never run, as `kernel/lib/syscall/BUILD.gn:57-60` already says of `command_list`.
- Kernel counters or any other kernel-made VMO, as a working assumption that D9 puts to the user.
- riscv. It builds no user ABI yet (`kernel/kernel_params.gni:92`, `kernel_no_userabi = ours_cpu == "riscv"`), so the kernel half has to compile out there and nothing else is asked of it.

## Design
### The current mechanism
- A `GKTL_COMMAND` puts one `gktl::Command` in section `.gktl.commands` (`command.hpp:21-27`). The linker script keeps the section between `__commands_start` and `__commands_end`, inside `.data.rel.ro` (`kernel/kernel.ld:162-171`).
- Two code paths read that range. `gktl::run_command` builds a radix tree from it on the first dispatch (`kernel/lib/gktl/src/command.cpp:12-13`, `:64-85`, `:89`). `ours::syscall::command_list` (`kernel/lib/syscall/src/command.cpp:20-21`, `:85-110`) copies `name`, `  help` and a newline per record into the user buffer through `UserTextWriter` (`:35-82`), and answers `NoSpace` when the text does not fit.
- The syscall's entry is `system/public/ours/syscalls.gni:125-149`, with `attrs = ["debug"]` ("a debug facility with no counterpart in a finished system", `:55`). It takes no handle, so any process may call it.
- Its only caller is `Registry::fetch` (`shell.cpp:87`), used by the `commands` built-in (`:522-527`, which ignores `NoSpace`) and by the boot listing (`:715-725`, which prints "(truncated"). Each call site has its own `static Registry`, so the shell carries 4 KiB of `.bss` for the list.
- Nothing checks the list. `boot_check.py` looks only for `--- end ---` (`.agents/skills/ours-verify/scripts/boot_check.py:95`) and the lines in `REQUIRED` (`:110-120`), and `scripts/kshell_run.py:112` only waits for the same marker.

### Zircon's mechanism
Zircon never lists console commands to user space. Its console registry is arrays of `cmd` records, each holding a name, a help string, a function pointer and an availability mask, in `.data.rel.ro.commands` (`zircon/kernel/lib/console/include/lib/console.h:41-46`, `:55`; `zircon/kernel/kernel.ld:172-175`), `zx_debug_send_command` runs one command and returns only a status (`zircon/kernel/lib/syscalls/debug.rs:99-133`), and `help` prints to the kernel's own console (`zircon/kernel/lib/console/mod.rs:420`). The model the user named is the kernel counters pipeline, which exports an immutable name table plus live counts as read-only VMOs:

| Step | What Zircon does | Where |
|---|---|---|
| Make | The linker builds the counter descriptor table: one input section per counter, sorted by name, page aligned and padded "so it can be exported as a VMO without exposing any other .rodata contents". | `zircon/kernel/kernel.ld:75-94` |
| Wrap | At the end of boot the kernel adopts those pages into a VMO (`CreateFromWiredPages`, exclusive, so the kernel's own mapping is removed), names it `counters/desc`, and makes a handle with Write taken off. | `zircon/kernel/lib/userabi/userboot.cc:380-391`, `:252-271`; `zircon/kernel/vm/vm_object_paged.cc:428`, `zircon/kernel/vm/include/vm/vm_object_paged.h:70-80` |
| Keep | The live arena VMO also gets a global reference, so its memory outlives user space closing the last handle. The descriptor VMO gets none. | `userboot.cc:247-249`, `:399` |
| Deliver | The kernel pushes the handles into userboot's bootstrap message. userboot tags every VMO that is not `zbi` or `vdso/*` as `PA_VMO_KERNEL_FILE`, and component_manager publishes each at `/boot/kernel/<vmo-name>`. | `src/bringup/lib/userboot/rust/src/program.rs:155-161`, `zircon/system/public/zircon/processargs.h:129`, `src/sys/component_manager/src/bootfs.rs:319-355` |
| Read | Open the file, `fdio_get_vmo_exact`, map `ZX_VM_PERM_READ`, check the magic and the sizes, index. | `zircon/system/ulib/kcounter/kcounter.cc:50-96` |
| Layout | A 24-byte header `{magic, max_cpus, descriptor_table_size}`, then 64-byte `{char name[56]; type}` records. A private, unstable ABI, "always built from source" with the kernel. | `zircon/kernel/lib/counters/include/lib/counter-vmo-abi.h:13-23`, `:34-37`, `:47-69` |

(`src/...`, `zircon/...` and `sdk/...` are all under `~/Workspaces/projects/fuchsia`; `zircon/kernel/kernel.ld` is the Zircon checkout's `kernel/kernel.ld`.)

### Objects and events
Objects, in the order a reader meets them (the recommended option of each decision below is assumed, so there is something concrete to point at):
- **The registry.** The 41 `gktl::Command` records the linker collected. Their strings are pointers into the kernel image, so they are no use outside the kernel.
- **The table.** A block of bytes the kernel builds from the registry: a header, then one entry per command with its name and help inline, and no pointer in it.
- **The table VMO.** A kernel object that is a run of memory pages holding the table. A program cannot make one like it, and it holds nothing but the table.
- **A handle and its rights.** A program does not hold the VMO, it holds a handle to it, and the rights live on the handle. The kernel decides the rights when it makes the handle, and a copy of a handle can only lose rights, never gain them (`handle_duplicate`, `syscalls.gni:1221`).
- **A startup handle.** A handle placed in a new process's first message (the processargs message). The process takes it once, by type (`ort::take_startup_handle`, `system/lib/ort/include/ort/startup.hpp:29-37`).
- **userboot, init, the shell.** userboot is the first user process, and it starts the next program: init on the dev image (the root of the component tree, which then starts the shell as a component), or the shell itself on dev.dbg_1 (which has no root). The shell runs in both images and has no allocator.
- **A mapping.** `vma_map` makes a VMO's pages readable at an address in the shell's address space, so the shell reads them like an array and needs no buffer of its own (`syscalls.gni:971`).

Events, with the numbers of today's build:
1. At the `Platform` init level, on the laborer thread, a hook reads the 41 records, writes a 4204-byte table (two pages) and gives it to a VMO the kernel makes. This runs before `userboot_init()` (`kernel/top/main.cpp:56-57`, `:76`).
2. `userboot_init()` puts a handle to the VMO, with the rights Basic, Read, Map and GetProperty, in userboot's startup message, beside the up to 11 it already sends (`kernel/lib/userabi/src/userboot.cpp:382-526`).
3. userboot takes the handle and sends it on with the rest it forwards (`kernel/lib/userabi/userboot/userboot.cpp:315-349`). On dev.dbg_1 the next program is the shell. On dev it is init.
4. On dev.dbg_1 the shell takes the handle from its startup message. On dev init takes it, publishes it as the file `/boot/kernel/commands` in the tree it already serves, and the shell opens that file and asks for its backing memory, which is a handle to the same VMO.
5. The shell maps the VMO read-only, checks the header, and prints one line per entry with `ort::debug_line`.

One worked entry. `dump-frame` has the name of 10 bytes and no help. Its entry is `0a 00` (name size), `00 00` (help size), `dump-frame` and a NUL, then a NUL for the empty help: 2 + 2 + 11 + 1 = 16 bytes, no padding. `lockup` has the longest line: name 6, help 194, so 4 + 7 + 195 = 206 bytes, padded to 208. All 41 entries add up to 4188 bytes, and with a 16-byte header that is 4204 bytes: 108 bytes over one page, so a table sized to fit one page would already be too small.

### D1. How the handle reaches the shell
Two facts constrain it. First, the shell starts in two ways. On dev.dbg_1 userboot starts it, so every handle it has is a startup handle (`shell.cpp:628-652`). On dev init starts it as a component, with a namespace holding `/boot` and `/svc` (`system/app/shell/BUILD.gn:81-99`), a job duplicate, the vDSO, the root VMA and its own process handle (`system/service/init/realm.cpp:462`, `:567`, `:582-583`, `:614`, `:633`), and no manifest key adds a startup handle (D19 chose a fixed set of startup handles and refused a program key for the Debug resource, its option (c), `docs/design/261004/component-framework.md:386`). Second, the project's rule is that programs reach capabilities by path, with only rootless programs taking startup handles (`.claude/skills/ours-kernel-design/references/boot.md:104-108`).

The kernel already sends userboot three VMOs: the vDSO (`kernel/lib/userabi/src/userboot.cpp:424-431`), the boot image (`:509`) and the crash record (`:470-478`). Only the crash record is made from run-time kernel state, and userboot closes it (`kernel/lib/userabi/userboot/userboot.cpp:398-411`), where it forwards the vDSO and the BootFS it unpacked itself (`:334-337`). So every hop between the kernel and the shell is new work under any option except the syscall. Two arrays are already full: the kernel's `handles[11]` and `infos[11]` (`userabi/src/userboot.cpp:388-389`), whose count reaches 11 when both the debug resource and the crash record are present, and userboot's `handles[10]` and `infos[10]` (`userboot/userboot.cpp:323-324`). Each needs one more slot, and the message size follows `sizeof(infos)` (`:357`).

| Option | What it is | What it costs | What it gives |
|---|---|---|---|
| (a) Startup chain, then a file on dev | Kernel to userboot to (dbg_1 shell or init) as a startup handle of a new type. On dev init publishes it as `/boot/kernel/commands` and the shell opens the file and asks for its backing memory. | Kernel `write_bootstrap` plus a handle type; userboot plus one field; init takes it and needs a `BootServer` method that adds one file from a VMO and a mapping (`ingest()` knows only the BootFS view, `system/service/init/boot_server.cpp:54`); the shell reads from two sources. No manifest change: the shell already uses `/boot` with rx* (`system/app/shell/BUILD.gn:86-90`). | The same chain as Zircon's. A second kernel VMO later is one more handle type and one more file. |
| (b) Startup chain, then a built-in protocol on dev | Same chain, but init serves the VMO from `/svc` the way it serves `ours.kernel.RootJob` (commit `a319b06b`: 9 files, 139 lines). | Everything in (a) upstream; plus a protocol constant (`system/lib/ours.resource/include/ours_resource/ours_resource.hpp:116`), an init server and storage (`init.cpp:114`, `:311`), `RESOURCE_SERVERS` 5 to 6 (`pools.hpp:39`, which feeds `LOOP_SLOTS`), the `BUILTINS` table and `connect` in `realm.cpp`, an offer in `kernel/products/image/dev/BUILD.gn:156` and a `use` in `shell.cm`. The method's ordinal is a literal written beside its selector and hashed from it (`ours_resource.hpp:116-124`, `system/lib/wire/include/wire/wire.hpp:111-120`), checked by `system/lib/wire/test/ordinals_test.py`. | By protocol, with the router as the only gate. Each new VMO costs a protocol. |
| (c) A syscall that returns the handle | A new call that makes a handle to a VMO the kernel keeps. It replaces `command_list`. What gates it is open. `debug_read` is gated by the Debug resource and by `kernel.debug_read=true` (`kernel/lib/syscall/src/debug.cpp:67-74`), but only dev holds that resource: dev.dbg_1's command line has no `kernel.debug_read` (`kernel/products/image/dev.dbg_1/BUILD.gn:85-100`; the only one is `kernel/products/image/dev/BUILD.gn:266`), so userboot mints no Debug resource there (`kernel/lib/userabi/src/userboot.cpp:433`) and its shell returns right after the boot listing (`system/app/shell/shell.cpp:727-735`). A Debug-gated call would leave dbg_1, the log `boot_check.py` reads, with no list. The gate is a choice of its own: none (as `command_list` has today), another resource both shells hold, or `kernel.debug_read=true` on dbg_1. | A `syscalls.gni` entry and a handler; a kernel-wide owner of the VMO in `kernel/object` (`JobDispatcher::root_job()`, `kernel/object/include/ours/object/job_dispatcher.hpp:40`, is the precedent for a singleton there), because the kernel must keep a reference (D3). Nothing in userboot, init or any manifest. | The fewest files and one read path in the shell. But it is a new way for a program to get a handle to a kernel object from nothing, which is an authority decision, and it is not how Zircon does it. |
| (d) A startup handle on dev as well, through a manifest key | Same chain as (a) up to init, then init's `Realm` adds the handle to the shell's first message through a manifest key, so the shell takes the same startup handle on both images. | A manifest feature in `Realm`, which hands out no such handle by a key today (`realm.cpp:462`, `:567`, `:582-583`, `:614`, `:633`); it breaks the "by path" rule (`boot.md:104-108`). D19 refused this device for the Debug resource (2026-10-04); that settled one resource, not this one. | One read path in the shell on both images, and no file or protocol on dev. Listed without a recommendation. |

Zircon: the chain of (a). The kernel pushes the handles (`userboot.cc:380-405`), userboot forwards them as `PA_VMO_KERNEL_FILE` (`program.rs:155-161`), component_manager publishes them under `/boot/kernel/<vmo-name>` (`bootfs.rs:319-355`), and the reader opens the file (`kcounter.cc:50-65`). Zircon tells the VMOs apart by their names. Ours has none (`system/public/ours/process_args.hpp:105-109`), so each kernel VMO would get a handle type of its own in the 0xe0-0xef range (`:80`), as the crash record has (`VmoCrashlog = 0xe0`, `:110`).

**Recommendation: (a).** It is the user's stated model, it keeps the "by path" rule on dev, and it is the one a second kernel VMO reuses. Its price is about six landable items against about four for (c). If shipping the least matters more than the Zircon shape, (c) is a sound choice, with the authority question above to answer first.

### D2. The table's layout and versioning
The kernel writes the table and the shell reads it, both built from one small library (as `system/lib/bootfs` is one library whose `Builder` only the host packer runs (`kernel/lib/obi/cli/pack.cpp:57`) and whose `View` the kernel (`kernel/phys/include/ours/phys/kernel_package.hpp:58`) and user code both read; here the kernel runs the builder at boot, as `kernel/top/crash_record.cpp:117` runs `crashlog::Writer`). The VMO has no content length, only whole pages (`vmo_get_size`, `syscalls.gni:1193`), so the header has to say where the table ends, and the rest of the last page is zero. The sizes in the table are computed from the 41 name and help strings of the dbg.1 build by a throwaway script, since no table exists yet.

| Option | Layout | Size for today's 41 | What the reader can do |
|---|---|---|---|
| (a) Text | A header, then today's text unchanged. | 4018 bytes (one page, 78 bytes of room). | Print it. Nothing else, and the order of the output is the kernel's format. |
| (b) Fixed records | A header, then `{char name[24]; char help[200]}` per command. | 9200 bytes (three pages). | Index by position. The help of `lockup` is 194 of 200, so the next long help is a cliff of the same kind as the 2048-byte buffer. |
| (c) Index plus pool | A header, `{name offset, help offset}` per command, then the strings. | 4307 bytes (two pages). | Index by position, but every offset is a pointer to validate. |
| (d) Self-delimiting entries | A header, then per command `{u16 name size, u16 help size, name, NUL, help, NUL}` padded to 4 bytes (as Fuchsia's BOOTFS directory entry is: an inline name and a 4-byte pad, `sdk/lib/zbi-format/include/lib/zbi-format/internal/bootfs.h:45-69`; ours's own BootFS keeps its names in a table, `system/lib/bootfs/include/bootfs/format.hpp`). | 4204 bytes (two pages). | Walk forward, which is all the shell does. The only offset is the running cursor. |

Two smaller choices go with it. Order: sort by name in the builder, or keep link order. Link order moved on 2026-10-31 when four tests changed target (`docs/repairs/261031/shell.md`), and a sort of 41 pointers costs nothing next to the boot. Promise: the table is private to one image, with a magic and a version that the reader compares for equality and refuses on mismatch, or a versioned format that old readers may still read. No one reads it but the shell of the same image, and the project has no legacy constraints.

Zircon: fixed 64-byte records, because the table's index is also the index into every per-CPU array of the live arena (`counter-vmo-abi.h:62-64`), sorted by the linker (`kernel.ld:90`), a header of three words, and one magic compared for equality with a comment "change it when changing this layout" (`counter-vmo-abi.h:54`; the reader refuses on mismatch and checks the sizes against the VMO, `kcounter.cc:89-96`). Nothing indexes commands, so the reason for fixed records does not carry over.

**Recommendation: (d), sorted by name, magic plus version compared for equality, private to one image.** A shared library with a builder and a view, laid out like `system/lib/bootfs` (`format.hpp`, `builder.hpp`, `view.hpp`, a host test). The working name is `cmdtable`.

### D3. When and where the table is built, and how long it lives
Terms. An init hook is a function registered for an init level (`GKTL_INIT_HOOK`); `set_init_level` runs the hooks of a level in turn (`kernel/lib/gktl/src/init_hook.cpp:17-35`), and hooks of the same level run in no stated order because the sort is not stable. The laborer thread runs `init_platform()`, then the `Platform` hooks, then `userboot_init()` (`kernel/top/main.cpp:52-76`). A paged VMO is ordinary memory pages that the VMO owns; `VmoDispatcher::create_paged` makes them resident (`kernel/object/vmo_dispatcher.cpp:60-74`). Wired pages are frames reserved at boot that `create_from_wired_pages` adopts in place; its only caller is the boot image (`userabi/src/userboot.cpp:305`, `kernel/object/vmo_dispatcher.cpp:76-90`).

| Question | Option | Notes |
|---|---|---|
| When | (a) At boot, in an init hook on the laborer thread, before `userboot_init()`. | What the rest of this document assumes: events 1 and 2, R2. |
| | (b) At compile and link time. | Zircon's `KCOUNTER` macro emits each descriptor with its name inline (`char name[56]`, `zircon/kernel/lib/counters/include/lib/counter-vmo-abi.h:34-35`) in a section named after the counter (`zircon/kernel/lib/counters/include/lib/counters.h:218-230`), and the linker script sorts and page-aligns the section (`zircon/kernel/kernel.ld:81-94`), so nothing builds a table at boot. `GKTL_COMMAND` (`kernel/lib/gktl/include/gktl/command.hpp:21-27`) could do the same: name and help copied at compile time into inline arrays of the literals' lengths, in a section named after the command, with the `Command` record pointing into it. Cost: a macro change; a sorted, page-aligned section in `kernel/kernel.ld`; adopting a range of the kernel image as wired pages, which the wired-pages option below says nothing shows is possible; the table exists in every build, so D6 is only about whether to hand it over. Gain: no builder, no hook, no run-time self-check, and the sort is the linker's. It changes D2 (the layout is whatever the macro emits, so layout (d) needs the linker to total its size) and R2. |
| Where | (a) A `Platform` hook in `kernel/top` that builds the bytes and calls a `userabi::stash_command_table(bytes)`, which makes the VMO and stores it. | The shape of `CrashRecordRecover` and `stash_crashlog` (`kernel/top/crash_record.cpp:129-132`, `userabi/src/userboot.cpp:885-904`), including the `KERNEL_NO_USERABI` guard (`crash_record.cpp:23-25`, `:102-106`). `userabi` stays a layer that touches dispatchers only (`.claude/skills/ours-kernel-design/references/layering.md:8-12`). |
| | (b) Inside `userboot_init()`. | Puts the registry in `userabi`, which has no `//kernel/lib/gktl` edge today (`kernel/lib/userabi/BUILD.gn:40-49`). |
| What pages | (a) Paged, with the bytes written by `VmoDispatcher::write`, as `stash_crashlog` does. | Size comes from the builder, so there is no capacity to outgrow. |
| | (b) Wired: a static page-aligned buffer in the kernel image, adopted by `create_from_wired_pages`. | Needs a fixed capacity, which is the bug being fixed, and nothing shows the kernel image's frames are adoptable (only the boot image has been). Not investigated. |
| Lifetime | (i) Hand over and forget: move the VMO into the handle (`CRASHLOG_VMO`, `userboot.cpp:162`, `:470-473`). | The pages are freed when the last handle closes. Enough when exactly one handle is ever made. |
| | (ii) The kernel keeps a reference for life, and each handle is made from it (`SHARED_VDSO`, `:141-156`, `:424-426`). | Eight KiB never freed. Required if more than one handle is ever made, which option (c) of D1 does on every call. |

Zircon: built by the linker, and wrapped in `bootstrap_vmos` (`zircon/kernel/lib/userabi/userboot.cc:315`), which `userboot_init` calls after the last init level before user space (`zircon/kernel/top/main.cc:250-260`). The arena has a kernel reference, the descriptor VMO has none.

**Recommendation: build at boot, (a), paged, and (i) unless D1 is (c).** The hook sits in `kernel/top` so it can be switched off with the console (D6). The link-time table is Zircon's shape and the one to weigh this against; which of the two is the user's choice.

### D4. The rights on the VMO
A VMO's default handle rights are Basic, Io (Read and Write), Property (Get and Set), Map and Signal (`kernel/object/include/ours/object/vmo_dispatcher.hpp:23-24`; `system/public/ours/rights.hpp:55-58`), so read-only has to be a mask applied by the kernel when it makes the handle. `vma_map` needs Map on the VMO and, for each permission bit asked, the matching right on both the VMO and the region, and `PermRead` alone is allowed (`syscalls.gni:980-981`). `vmo_write` needs Write (`:1156`). A snapshot child of the table gains Write only on its own copy (`:1070`).

| Option | Rights | Note |
|---|---|---|
| (a) Basic, Read, Map | As `BOOT_IMAGE_RIGHTS` (`userabi/src/userboot.cpp:262`) and `CRASHLOG_RIGHTS` (`:167`). | Enough to map and to `vmo_read`. Not enough to be served as a file. |
| (b) Basic, Read, Map and GetProperty | As userboot's BootFS handle (`BOOTFS_RIGHTS`, `userboot/userboot.cpp:152`). | Needed if init serves the VMO as a file: `GetBackingMemory` duplicates with Basic, Map and GetProperty, plus Read, and a duplicate cannot add what its source lacks (`system/lib/ioservice/src/server.cpp:444`, `system/lib/ioservice/include/ioservice/tree.hpp:49-56`). |
| (c) The default less Write | Zircon's way. | Keeps SetProperty and Signal, which a reader of a constant table has no use for. |

Recommended: the mask goes in one constant beside `CRASHLOG_RIGHTS`, applied at the one place the handle is made, and not a parameter. The alternative is Zircon's `get_vmo_handle(vmo, readonly, ...)`, which takes it as a flag; two sibling VMOs in the same file went out with `readonly` false (`userboot.cc:366`, `:419`, `:252-271`).

Zircon: (c), with the mask opt-in per call site. With no Write on the handle, `zx_vmar_map` refuses `PERM_WRITE`, which is what keeps the mapping read-only (`zircon/kernel/lib/syscalls/vmar.cc:132-142`).

**Recommendation: (b) if D1 is (a), because the file route needs GetProperty; (a) for the other D1 options.** One rights constant, one place.

### D5. What happens to `command_list`
Facts. It has one caller (`shell.cpp:87`). Its call number is private to the kernel and the vDSO, and reordering is allowed (`syscalls.gni:20-23`), so deleting an entry breaks no stable ABI. A program that imports a vDSO symbol the vDSO lacks fails to launch by name, before it runs (`kernel/lib/userabi/src/userboot.cpp:625-632`), so the shell must stop importing it before the entry goes. `docs/design/260923/userspace-interface.md:199` planned for it to be either renamed to `ours_debug_command_list` with the System/Debug resource, or deleted with the shell demo. Its traces beyond the entry and handler: the `//kernel/lib/gktl` dependency and its comment (`kernel/lib/syscall/BUILD.gn:57-61`), the `NoSpace` wording (`system/public/ours/status.hpp:48`, `:126`), and a comment at `kernel/top/BUILD.gn:61`.

| Option | What it means |
|---|---|
| (a) Delete it in the last item, after the shell stops calling it. | One way to list commands. |
| (b) Keep it unused. | A second path to keep true, with the buffer problem still in its contract. |
| (c) Keep it as the answer for a kernel with no table (D6). | Keeps the 2048-byte problem alive for exactly the case where the list is nearly empty. |

Zircon has no equivalent to keep or drop.

**Recommendation: (a).** There is no "release" here to keep it for: no program outside this tree calls it, the project carries no legacy constraints, and the entry is already marked `debug`.

### D6. What a kernel without a shell answers
Facts. `kernel_shell_enabled = false` removes `//kernel/commands`, kshell and the command benchmarks (`kernel/top/BUILD.gn:68-74`), but the registry stays on purpose, because a command in a subsystem's own file keeps its record: `dump-frame` in `//kernel/mm` (`top/BUILD.gn:58-67`). So that kernel's registry has at least one record, which nothing can run. The dev image links kshell but starts it only when asked, and its command line says `kernel.shell=false` (`kernel/products/image/dev/BUILD.gn:264-266`), so on dev the listed commands cannot be typed either. riscv has no user ABI at all.

| Option | What the shell sees |
|---|---|
| (a) Build the table from whatever the registry holds. | One command, `dump-frame`, which cannot be run. This is what `command_list` answers today. |
| (b) Build an empty table when the kernel has no console. | A header with a count of 0. |
| (c) Build nothing. No VMO, no handle, no file. | The shell finds no table and prints one line saying so. |

Zircon has no equivalent; with the console compiled out, `zx_debug_send_command` still validates its arguments and then does nothing (`zircon/kernel/lib/syscalls/debug.rs:121-132`).

**Recommendation: (c).** One answer covers a console-less kernel, riscv, and a `create_paged` that failed. The glue is then gated by what GN links, and not by an `#if`, as `kernel/top/BUILD.gn` gates the console (`:46-49`, `:68-74`): either a target of its own listed in `deps` under the same condition, or a conditional `sources +=` in `source_set("top")`, whose list every CPU shares today (`:5-12`). `kernel/top/crash_record.cpp` uses `#ifndef KERNEL_NO_USERABI` instead (`:23-25`, `:104-106`), because the rest of that file is needed on every CPU. Its cost is a "no table" path in the shell that only a separate build can exercise (see Testing and verification).

### D7. How the shell reads it
The shell has no allocator (`shell.cpp:80-83`). It already maps a read-only VMO and reads it in place on dev.dbg_1: `launch::map_bootfs` reads the header with `vmo_read`, rounds up and calls `vma_map` with `PermRead` (`system/lib/launch/src/bootfs.cpp:35-58`, used at `shell.cpp:742`). The root VMA that `vma_map` needs is a startup handle the shell holds on both images (`shell.cpp:652`; init puts it in a component's message at `realm.cpp:614`). The registry is fixed at link time (`kernel.ld:162-171`), so one mapping stays valid for the shell's life.

| Option | What it is |
|---|---|
| (a) Map once, keep it | A `CommandTable` object in the shell maps lazily on first use and keeps the mapping. Both call sites print from it. |
| (b) Map, print, unmap | Per call. `vma_unmap` needs no right (`syscalls.gni:1034`). Saves 8 KiB of address space for two uses. |
| (c) Read in pieces | `vmo_read` into a small stack buffer and parse the stream. Needs only Read, but a stream parser instead of a view. |

How lines are printed matters: `ort::debug_line({name, "  ", help})` goes out as one system call when the line is at most `MAX_DEBUG_WRITE`, 256 bytes (`system/lib/ort/src/debug.cpp:35-54`, `system/public/ours/syscall.hpp:69`), and the longest line is 203, so no line is split. The old path wrote the whole text in 256-byte chunks (`debug.cpp:11-33`), which can cut a line in the middle.

Parsing goes through the library's view, which checks the magic, the version, `total_size` against the mapping, and every entry against the bytes left, with its NUL, before any string is used, as `bootfs::View::from` does for the BootFS. A bad table prints one line and does not crash the shell.

Zircon: map `ZX_VM_PERM_READ`, check the magic and sizes, cast and index (`kcounter.cc:80-96`); it can use vectors, ours cannot.

**Recommendation: (a), with the view, and `ort::debug_line` per entry (the two spaces only when the help is not empty, as `command_list` writes it, `kernel/lib/syscall/src/command.cpp:100-103`).** The two 2 KiB `Registry` buffers go.

### D8. Tests and the check
What exists: no test or boot check reads the listing. `boot_check.py` and `kshell_run.py` wait for `--- end ---` (`boot_check.py:95`, `kshell_run.py:112`).

| Option | What is checked |
|---|---|
| (a) Host test of the library, a kernel self-check when it builds the table, and a boot-log count check | The host test round-trips and refuses bad tables. The kernel validates its own output with the same view and does not hand over a table that fails. `boot_check.py` compares the kernel's count, the shell's count and the number of lines listed. |
| (b) Host test only | Proves the format. Proves nothing about the VMO, the rights or the hand-over. |
| (c) (a) plus a shell self-check that the table is read-only | The shell tries `vmo_write` and a `PermWrite` map and requires both refused (`vma_map` answers AccessDenied for a missing right, `syscalls.gni:980-981`). |

Zircon tests the real thing on a target: `src/zircon/tests/kcounter/kcounter-test.cc` opens `/boot/kernel/` and binary-searches the sorted table (`:29`, `:69`).

**Recommendation: (c).** The count check is what catches the original bug, a list cut short, without the "(truncated" line that no longer exists. The read-only check is the only test that the rights of D4 are what the kernel set. The steps are in Testing and verification.

### D9. Whether the same mechanism is meant for kernel counters
Facts. Ours has per-CPU kernel counters (`gktl::Counter`, `kernel/lib/gktl/include/gktl/counter.hpp:29-36`, with its descriptors in section `.gktl.counter.descs`, `kernel/kernel.ld:173-175`), and only the kernel reads them (for example `kernel/kernel/sync/spin_lock.cpp:194`, `kernel/object/port_dispatcher.cpp:336`). None reaches user space. They are a different shape from the table: live numbers the kernel keeps writing after the VMO exists, which in Zircon is the arena VMO, whose pages are pinned and referenced by the kernel (`userboot.cc:393-405`), against an immutable table written once.

| Option | Meaning |
|---|---|
| (a) Out of scope | This design makes the commands table only. |
| (b) Generic now | A handle type that carries a file id, and init publishes any such VMO by id under `/boot/kernel/`. |
| (c) Counters first or together | Design the live arena alongside. |

Zircon builds both together from the same code (`userboot.cc:380-405`).

**Recommendation: (a), with D1 chosen so that counters can reuse it.** A handle type per VMO (the project's way, `process_args.hpp:105-109`) plus the `/boot/kernel/` path convention means counters add one type and one file, not a redo. (b) pays for a generality with one user.

### Implementation items
A dependency sketch, not an approved plan: the steps become a PLAN.md, introduced one at a time, once the user has decided D1 to D9. In this order, each one commit, each landable with the x86 `ours-verify` passing (both images booting, host tests, and the riscv phys check unchanged). They assume the recommended option of each decision; the decisions they depend on are named, and what changes if another is chosen is listed after them.

| Item | What it does | Depends on | Done when |
|---|---|---|---|
| R1. The table library | New `system/lib/cmdtable` (working name): `format.hpp` (header, entry layout, magic, version), `builder.hpp` (size of the table for a list of name and help pairs, then write it into a span, sorted by name), `view.hpp` (`View::from(bytes)` with every check of D7, forward iteration). A host test under `test/`, wired into the root test group beside `//system/lib/bootfs:tests` (`BUILD.gn:164`). Modelled on `system/lib/bootfs` and its test. | D2, D7 | The host test passes in the `host-tests` line of `ours-verify`: a round trip including an empty help and a table over a page; one refusal per check (bad magic, bad version, `total_size` over the span, a count that overruns, an entry past the end, a missing NUL); the output is sorted. No consumer yet, so both boots are unchanged. |
| R2. The kernel builds the table and hands it to userboot | `gktl` exposes the linker bounds as one `std::span<Command const>` accessor (replacing the extern at `kernel/lib/gktl/src/command.cpp:12-13`; the syscall keeps its own copy until R5). A `kernel/top` source with a `Platform` hook builds the bytes, checks them with the view, calls `userabi::stash_command_table`, and logs the count and size. `userabi` makes the paged VMO, keeps the rights constant, and sends a handle of a new type (0xe0-0xef) in `write_bootstrap`, growing its arrays to 12. The userboot program takes it, checks the header with the view, logs its count and closes it, as `report_crashlog` does. `kernel/top/BUILD.gn` adds the source, or a target holding it, under `kernel_shell_enabled` and `!kernel_no_userabi` (D6). | D1, D2, D3, D4, D6 | Dbg and rel boot logs carry a kernel line and a userboot line with the same count (41 at HEAD) and both images reach `--- end ---`. The riscv phys check is unchanged. A build with `kernel_shell_enabled=false` logs neither line (by hand). |
| R3. userboot forwards it and init serves it | userboot's `HandedHandles` gets a field, `start_next`'s arrays grow to 11, and `main` stops closing the handle. init takes it (`init.cpp:218-250`), maps it, and adds it to the `/boot` tree as `kernel/commands` through a new `BootServer` method (two of the 256 nodes, the new directory `kernel` and the file: `system/service/init/pools.hpp:48-51`, `system/service/init/boot_server.cpp:92`, `:134`). The dbg_1 shell does not use it yet. | D1, D4 | The rel boot log carries an init line with the same count. `ls /boot/kernel` in the dev shell shows the file (by hand). dbg_1 is unchanged. |
| R4. The shell reads the table | A `CommandTable` in the shell: the startup handle on dbg_1 and `/boot/kernel/commands` on dev, `vma_map`, the view, `ort::debug_line` per entry, a closing count line. Both call sites use it, and the `Registry` class, its two buffers and the "(truncated" line go. `boot_check.py` compares the kernel's count, the shell's count and the lines listed. A shell self-check tries `vmo_write` and a `PermWrite` map and requires them refused. The same commit deletes `docs/repairs/261031/shell.md`, whose only item is item 1 (a repair file goes when its last item is fixed, as `681b3a4d` removed `docs/repairs/261029/kernel-arch-seam.md`), and adds a change record. | D1, D2, D4, D7, D8 | Both boot logs list all 41 commands, ending with the last registered one, with no "(truncated" and the three counts equal. The shell's `.bss` is about 4 KiB smaller (`size` on `bin/shell` before and after). The `commands` built-in prints the same list. |
| R5. Delete `command_list` | Remove the entry (`syscalls.gni:125-149`), `kernel/lib/syscall/src/command.cpp` and its `BUILD.gn` source (`:31`), the `//kernel/lib/gktl` dependency and its comment (`:57-61`), and the stale mentions (`status.hpp:48`, `:126`, `kernel/top/BUILD.gn:61`, `userabi/src/userboot.cpp:631`, and the sentence in `docs/repairs/261004/kernel-panic.md:55`). | D5 | Both images boot; the vDSO exports no `ours_command_list`; `git grep command_list` finds nothing outside `docs/changes/` (append-only) and the older documents that are not rewritten (`docs/design/260923/userspace-interface.md`, `.agents/tasks/syscall-boundary/PLAN.md`). |
| R6. Record the decision | Move this document to `docs/design/` with its Status set to Accepted and the decided column filled in, and add the `/boot/kernel/` convention to `boot.md`. | all | The pre-commit document lint passes. |

If D1 is (b), R3 serves a protocol instead of a file, and R4's dev source is `ours_resource::get`. If D1 is (c), R3 does not exist, R2 adds the syscall and the kernel-held VMO, R4 calls it on both images (so its gate has to be one both shells pass, D1 (c)), and R4 and R5 can be one commit. If D6 is (a) or (b), R2's build condition drops the console gate. If D3 is (ii), R2 keeps the VMO's reference after the first handle.

## Alternatives considered
- **An offset argument on `command_list`, and the shell asks until a call returns less than it was given.** Rejected by the user (2026-10-31). It changes a `syscalls.gni` entry and the contract of a call that exists to be removed, and every caller loops.
- **A bigger buffer in the shell (8 KiB).** Rejected by the user (2026-10-31). It moves the cliff to about 80 commands, in a program that has no allocator to grow it (`docs/repairs/261031/shell.md`).
- **Sorting the text in the kernel and keeping the buffer.** Makes the cut predictable and shows no more commands.
- **Exporting the pages of `.gktl.commands` as they are.** The records are pointers, and the section shares pages with other read-only kernel data (`kernel.ld:161-188`).
- **Putting the table in the user BootFS at build time.** The kernel neither builds nor reads the user BootFS (`userabi/src/userboot.cpp:363-365`), the registry is link-time kernel state that differs by architecture and by `kernel_shell_enabled`, and a build-time copy could disagree with the kernel that is running.
- **Reusing the boot image handle (`make_boot_image_handle`, `:283`).** It carries the OBI container, which is fixed at build time and cannot be appended to (`boot.md:34-35`), so it cannot hold anything only the running kernel knows.
- **Printing the list from the kernel, as Zircon's `help` does (`zircon/kernel/lib/console/mod.rs:420`).** It needs no user-space piece, but on the dev image the kernel's console is not running (`kernel.shell=false`, `kernel/products/image/dev/BUILD.gn:265`), so nothing could ask for it, and none of today's 41 commands is `help`.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D0. The shape of the fix | Offset argument on `command_list`; a bigger buffer; a read-only VMO the shell maps | The VMO (the user's choice) | Not asked | A read-only VMO the shell maps and reads (2026-10-31) |
| D1. How the handle reaches the shell | (a) startup chain, then `/boot/kernel/commands` on dev; (b) startup chain, then a built-in protocol on dev; (c) a new syscall, with its gate open; (d) a startup handle on dev through a manifest key | (a) | Not asked | |
| D2. Layout and versioning | (a) text; (b) fixed records; (c) index plus pool; (d) self-delimiting entries; sorted or link order; private to one image or versioned | (d), sorted by name, magic and version compared for equality, private to one image | Not asked | |
| D3. When, where, and how long | Built at boot or at compile and link time; hook in `kernel/top` or inside `userboot_init`; paged or wired pages; hand over or keep a reference | Built at boot, hook in `kernel/top`, paged, hand over (keep a reference only if D1 is (c)) | Not asked | |
| D4. Rights on the VMO | (a) Basic, Read, Map; (b) plus GetProperty; (c) default less Write | (b) if D1 is (a), else (a); one constant at the one `Handle::make` | Not asked | |
| D5. `command_list` | (a) delete; (b) keep unused; (c) keep as the no-table answer | (a), in the last item | Not asked | |
| D6. A kernel without a shell | (a) table of whatever is registered; (b) empty table; (c) no table | (c) | Not asked | |
| D7. How the shell reads it | (a) map once and keep; (b) map and unmap each time; (c) read in pieces | (a), through the view, one `debug_line` per entry | Not asked | |
| D8. Tests and the check | (a) host test, kernel self-check and count check; (b) host test only; (c) (a) plus a read-only check in the shell | (c) | Not asked | |
| D9. Kernel counters | (a) out of scope; (b) generic mechanism now; (c) design with counters | (a), with D1 chosen so counters can reuse it | Not asked | |

## Testing and verification
- **Host test (R1).** The library's gtest runs on the build machine in the `host-tests` step of `ours-verify` (the way `//system/lib/bootfs/test:view` does, `system/lib/bootfs/test/BUILD.gn`). It covers a round trip, an empty help, a table over a page, the sort, and one refusal per check of the view.
- **Kernel self-check (R2).** The kernel builds the table, runs the view over its own bytes, and compares the entry count with the registry's length (`__commands_end - __commands_start`); on a mismatch it logs an error and hands over nothing. It logs `N commands, B bytes`.
- **Boot check (R2, R3, R4).** `ours-verify` boots both images. The log has the kernel's count, userboot's count, init's count (rel only), and the shell's closing count line, with one line per command between the shell's headings. `boot_check.py` requires them equal and at least 1, which is the check that a list was not cut short. At HEAD the shell lists 20 and prints the tail, so this check would fail it. The old acceptance in `docs/repairs/261031/shell.md` ("`boot_check.py` fails when the tail is printed") becomes moot, since the tail no longer exists.
- **Read-only check (R4).** A shell self-check in `system/app/shell/shell_tests.cpp` requires `vmo_write` and a `PermWrite` map of the table to be refused.
- **Not verifiable under `ours-verify`.** The "no table" path of D6 needs a separate build (`tools/set.py bringup.x86 --args kernel_shell_enabled=false`, from `kernel/kernel_params.gni:69`), run by hand once. riscv has no user ABI until step 9 of `.agents/tasks/riscv-kernel/PLAN.md`, so only the compile-out is checked there, by the riscv phys check staying green. Whether the table's VMO is read-only to every possible holder rests on the rights argument of D4, and the shell self-check covers only the shell's own handle.

## Open questions
- Which commands cross to user space. `command_list` hands over every record today and the table would too, including `crash`, `lockup` and `cov`. `.claude/skills/ours-kernel-design/references/layering.md` ("Trust boundaries", `:121-125`) asks for an explicit allow/deny list, default deny, generated from GN metadata on the producing targets. Is listing every name and help string acceptable because nothing can run them from user space, or should the table carry only commands whose target marks them listable?
- The library's name and namespace. `cmdtable` is a working name.
- The exact wording of the shell's closing count line, which `boot_check.py` parses.
- On the dev image the listed commands cannot be typed, because `kernel.shell=false` at run time (`kernel/products/image/dev/BUILD.gn:264-266`), and the table cannot say so unless it records whether the console is running. Today's list has the same property. Should the header carry a flag for it?
- `dump-frame` has an empty help and an empty body (`kernel/mm/pm_frame.cpp:51-54`). The table lists it as the registry does. Is that the intent?
- Whether the dev shell should read `/boot/kernel/commands` by `GetBackingMemory` and map it (what D7 assumes, and what needs GetProperty in D4) or stream it with `File.Read`, which needs no Map right and no GetProperty.
- Whether a restarted init or shell can ever need a second handle. Nothing restarts today. It matters only for D3's choice of lifetime (i) over (ii).
