namespace ours::syscall {
    auto vmo_create(Handle handle) -> Handle;

    auto vmo_create_child(Handle handle) -> Handle;
} // namespace ours::syscall