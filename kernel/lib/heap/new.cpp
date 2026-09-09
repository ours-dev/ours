#include <heap/new.hpp>

auto operator new(usize size, ours::mm::Gaf gaf) -> void * {
    return heap::kmalloc(size, gaf);
}

auto operator new[](usize size, ours::mm::Gaf gaf) -> void * {
    return heap::kmalloc(size, gaf);
}

auto operator new(usize size, ours::mm::Gaf gaf, ours::NnId nid) -> void * {
    return heap::kmalloc(size, gaf, nid);
}

auto operator new[](usize size, ours::mm::Gaf gaf, ours::NnId nid) -> void * {
    return heap::kmalloc(size, gaf, nid);
}

auto operator delete(void *ptr) noexcept -> void {
    return heap::kfree(ptr);
}

auto operator delete[](void *ptr) noexcept -> void {
    return heap::kfree(ptr);
}