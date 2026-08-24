#include <ccrt/runtime.hpp>

#if defined(PHYS_ENVIRONMENT)
#include <ours/panic.hpp>

#include <new>

#include <cstddef>

#include <__verbose_abort>
#endif

namespace {
using ItaniumGuard = unsigned long long;

auto core_guard(ItaniumGuard *guard) -> ccrt::Guard * {
    return reinterpret_cast<ccrt::Guard *>(guard);
}
} // namespace

// These names are required by the compiler ABI and cannot follow local naming rules.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" {
void *__dso_handle;

auto __cxa_atexit(ccrt::Destructor destructor, void *argument, void *dso) -> int {
    return ccrt::Registry::instance().register_destructor(destructor, argument, {dso}) ? 0 : -1;
}

auto __cxa_finalize(void *dso) -> void {
    ccrt::Registry::instance().finalize({dso});
}

auto __cxa_guard_acquire(ItaniumGuard *guard) -> int {
    return core_guard(guard)->acquire() ? 1 : 0;
}

auto __cxa_guard_release(ItaniumGuard *guard) -> void {
    core_guard(guard)->release();
}

auto __cxa_guard_abort(ItaniumGuard *guard) -> void {
    core_guard(guard)->abort();
}

auto atexit(ccrt::ExitFunction function) noexcept -> int {
    return ccrt::Registry::instance().register_exit_function(function) ? 0 : -1;
}
}
// NOLINTEND(readability-identifier-naming)

#if defined(PHYS_ENVIRONMENT)
// The physical image has no exception runtime or heap.  libc++ still emits
// cold failure paths for bounds-checked helpers, so route them to phys panic.
_LIBCPP_BEGIN_NAMESPACE_STD
[[noreturn]]
auto __libcpp_verbose_abort(char const *, ...) noexcept -> void {
    ours::panic("libc++ invariant failed in physical boot");
}
_LIBCPP_END_NAMESPACE_STD

// Formatting cold paths and other libc++ corners reference operator new;
// physical boot never allocates, so an attempted allocation is a bug.
auto operator new(std::size_t) -> void * {
    ours::panic("allocation in physical boot");
}
auto operator new[](std::size_t) -> void * {
    ours::panic("array allocation in physical boot");
}

// Deleting destructors for statically owned polymorphic boot objects refer to
// the ordinary delete overloads even though physical boot never allocates.
auto operator delete(void *) noexcept -> void {}
auto operator delete[](void *) noexcept -> void {}
auto operator delete(void *, std::size_t) noexcept -> void {}
auto operator delete[](void *, std::size_t) noexcept -> void {}
auto operator delete(void *, std::align_val_t) noexcept -> void {}
auto operator delete[](void *, std::align_val_t) noexcept -> void {}
auto operator delete(void *, std::size_t, std::align_val_t) noexcept -> void {}
auto operator delete[](void *, std::size_t, std::align_val_t) noexcept -> void {}

extern "C" [[noreturn]]
auto __cxa_pure_virtual() -> void {
    ours::panic("pure virtual call in physical boot");
}
#endif
