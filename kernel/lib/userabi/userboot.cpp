#include <ours/object/handle.hpp>
#include <ours/object/job-dispatcher.hpp>
#include <ours/object/process-dispatcher.hpp>
#include <ours/object/vm-area-dispatcher.hpp>

namespace {
    auto userboot_init() -> void {
        using namespace ours;
        using namespace object;

        KernelHandle<ProcessDispatcher> process_handle;
        KernelHandle<VmAreaDispatcher> vma_handle;
        Rights process_rights, vma_rights;
        auto status = ProcessDispatcher::create(JobDispatcher::root_job(), "userboot",
                                                process_handle, process_rights,
                                                vma_handle, vma_rights);
        ASSERT(Status::Ok != status);

        auto process = process_handle.dispatcher();
        auto vma = process_handle.dispatcher();

        auto process_handle_owner = object::Handle::make(ustl::move(process_handle), process_rights);
        auto vma_handle_owner = object::Handle::make(ustl::move(vma_handle), vma_rights);
    }
} // namespace 