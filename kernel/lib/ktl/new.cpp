#include <ktl/new.hpp>

auto operator new(usize size, ktl::Gaf gaf) -> void * {
    return ktl::kmalloc(size, gaf);
}

auto operator new[](usize size, ktl::Gaf gaf) -> void * {
    return ktl::kmalloc(size, gaf);
}

auto operator new(usize size, ktl::Gaf gaf, ktl::NnId nid) -> void * {
    return ktl::kmalloc(size, gaf, nid);
}

auto operator new[](usize size, ktl::Gaf gaf, ktl::NnId nid) -> void * {
    return ktl::kmalloc(size, gaf, nid);
}

auto operator delete(void *ptr) noexcept -> void {
    return ktl::kfree(ptr);
}

auto operator delete[](void *ptr) noexcept -> void {
    return ktl::kfree(ptr);
}