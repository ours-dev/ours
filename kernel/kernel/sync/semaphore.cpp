#include <ours/sync/semaphore.hpp>

namespace ours::sync {
auto Semaphore::post() -> void {}

auto Semaphore::wait() -> Status {
    return Status::Unimplemented;
}
} // namespace ours::sync