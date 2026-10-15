# idle_states

The x86 idle states a CPU may enter with MWAIT, and which of them it has

## Layout

    include/idle_states/   public headers, reached as <idle_states/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//kernel/arch/x86/lib/idle_states`; its headers alone are
`//kernel/arch/x86/lib/idle_states:headers`. Tests are executables under
`//kernel/arch/x86/lib/idle_states/test` and run on the host, not in the kernel.
