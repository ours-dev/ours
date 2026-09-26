# system/lib/runtime 待修项（2026-09-26）

## 1. 用户程序的全局对象不会被构造：没有人执行 `.init_array`

**现象**：潜伏。今天没有一个用户程序有 `.init_array`（`llvm-readelf -S` 看 shell、init、userboot、fault_probe、runtime_smoke 都没有）。
一旦某个程序或它链接的库里有一个需要动态初始化的全局对象（构造函数不是 constexpr、初始值要在运行时算），它的构造函数会被编进
`.init_array`，但永远不会被调用：那个对象从 `.bss` 的零开始，而且不会报任何错。

**原因**：执行 `.init_array` 是 crt 的活。内核加载器（`kernel/lib/elfload`）只映射段、做重定位，不跑构造函数；runtime 的
`runtime_start()`（`system/lib/runtime/src/start.cpp`）在调 main 之前也没有做。runtime 自己的全局变量都写成 `constinit`，
编译器会拒绝任何需要动态初始化的写法，所以 runtime 内部是安全的；程序的全局变量没有这层保护。

**改法**：`runtime_start()` 在读完 processargs、调 main 之前，按顺序调用 `[__init_array_start, __init_array_end)` 里的每个函数指针
（这两个边界符号由链接器提供；lld 在什么条件下定义它们、没有 `.init_array` 时给出什么，改的时候要先查清楚）。对照 Zircon `sdk/lib/c/startup` 的 `StartupCtors`、
musl `__libc_start_init()`。`.fini_array` 同理可以在 main 返回后、`process_exit` 之前执行，但我们的程序不会被别人"卸载"，
可以先不做。

**验收**：给 runtime_smoke 加一个带非 constexpr 构造函数的全局对象，`llvm-readelf -S` 能看到 `.init_array`；K14 之后在一个真正运行的
程序（shell）里加同样的对象，启动日志证明构造函数在 main 之前跑过。
