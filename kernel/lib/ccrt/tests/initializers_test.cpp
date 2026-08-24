#include <ccrt/runtime.hpp>

namespace {
int sequence;
}

extern "C" auto ccrt_test_first() -> void {
    sequence = sequence * 10 + 1;
}

extern "C" auto ccrt_test_second() -> void {
    sequence = sequence * 10 + 2;
}

asm(".section __DATA,__const\n"
    ".p2align 3\n"
    ".globl __ccrt_init_start\n"
    "__ccrt_init_start:\n"
    ".quad _ccrt_test_first\n"
    ".quad _ccrt_test_second\n"
    ".globl __ccrt_init_end\n"
    "__ccrt_init_end:\n");

auto main() -> int {
    if (sequence != 0) {
        return 1;
    }
    ccrt::init_global_static_objects();
    if (sequence != 12) {
        return 2;
    }
    ccrt::init_global_static_objects();
    if (sequence != 12) {
        return 3;
    }
    return 0;
}
