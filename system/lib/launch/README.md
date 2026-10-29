# launch

The user-space launcher: how a program from BootFS becomes a running process.

- `launch/plan.hpp`: `Plan::build()`, which decides how the segments of a program are mapped from numbers alone: which pages come straight from the file, which from a copy-on-write snapshot of it, which from a zero-fill VMO with the file bytes of one partial page copied in, and with what protection each is mapped. It does no I/O, so it is tested on the host.
- `launch/process_launcher.hpp`: `ProcessLauncherState`, one ours.process.Launcher connection as its server keeps it: what the client's AddArgs, AddHandles and AddNames have given, judged at Launch against what one processargs message can carry, then handed to `launch::LauncherOps::start()`, an abstract class the server's connection class derives from. It makes no system call itself, so it is tested on the host too.
- `launch/launch.hpp`: `start()`, which carries a plan out through system calls. It takes the program as a VMO alone and reads it in place, through a mapping of the whole VMO into the caller's root region made with read and execute permission, then unmapped. A VMO without the Execute right is refused at that mapping, before a byte of it is read. Only a holder of vmex can give a VMO that right. On the dev image that is one process, which gives it only to BootFS files, and nobody can write those, so no file can change while it is parsed (component-framework design, D14 and I6).
- `launch/bootfs.hpp`: a launcher made once from a BootFS, which finds a program by name and hands `start()` a slice of the executable BootFS.

## Layout

    include/launch/       public headers, reached as <launch/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/launch`; its headers alone are
`//system/lib/launch:headers`. Tests are executables under
`//system/lib/launch/test` and run on the host, not in the kernel.
