/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <gktl/command.hpp>
#include <gktl/radix_tree.hpp>

#include <ktl/memory_resource.hpp>
#include <ustl/collections/allocator.hpp>

namespace gktl {
namespace {
    extern "C" Command const COMMANDS_START[] LINK_NAME("__commands_start");
    extern "C" Command const COMMANDS_END[] LINK_NAME("__commands_end");

    struct CommandKeyOfValue {
        auto operator()(Command const &command) const -> char const * {
            return command.cmd;
        }
    };

    struct CommandKeyEncoder {
        static constexpr auto NUM_CHILDREN = 66_usize;
        static constexpr auto BITS_PER_CODE = 7_usize;
        static constexpr auto CODES_PER_KEY = 8_usize;

        constexpr auto operator()(u64 byte) const -> u64 {
            if (byte >= 0x61 && byte <= 0x7a) {
                return byte - 0x61 + 1;
            }
            if (byte >= 0x41 && byte <= 0x5a) {
                return byte - 0x41 + 27;
            }
            if (byte >= 0x30 && byte <= 0x39) {
                return byte - 0x30 + 53;
            }
            if (byte == 0x60) {
                return 63;
            }
            if (byte == 0x2d) {
                return 64;
            }
            if (byte == 0x2e) {
                return 65;
            }
            return 0;
        }
    };

    using CommandAllocator = ustl::collections::Allocator<Command>;
    using CommandTree = RadixTree<Command, CommandAllocator, CommandKeyOfValue, CommandKeyEncoder>;

    /// The kernel heap is named explicitly instead of taken from the ustl
    /// default resource. A default-constructed allocator latches whatever
    /// the default is at construction, and TREE is constructed by
    /// ccrt::init_global_static_objects() at the top of start_kernel(), long
    /// before init_pmm() installs ktl::kernel_memory_resource() as that
    /// default -- so it would latch the null resource, reserve() would fail,
    /// and every command would quietly be "not found". Naming the resource
    /// only records which heap to use; nothing is allocated until the first
    /// dispatch, by which time the heap is up.
    CommandTree TREE{CommandAllocator{ktl::kernel_memory_resource()}};
    bool TREE_BUILT{};

    auto build_radix_tree() -> void {
        if (TREE_BUILT) {
            return;
        }

        usize capacity = 1;
        for (auto const *command = COMMANDS_START; command != COMMANDS_END; ++command) {
            if (command->cmd != nullptr && command->callback != nullptr) {
                capacity += TREE.max_required_nodes(*command);
            }
        }

        if (!TREE.reserve(capacity)) {
            TREE_BUILT = true;
            return;
        }

        for (auto const *command = COMMANDS_START; command != COMMANDS_END; ++command) {
            static_cast<void>(TREE.insert(*command));
        }

        TREE_BUILT = true;
    }
} // namespace

auto run_command(int argc, char **argv) -> void {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return;
    }

    build_radix_tree();

    auto const *command = TREE.find(argv[0]);
    if (command == nullptr || command->callback == nullptr) {
        return;
    }

    static_cast<void>(command->callback(argc, argv));
}

} // namespace gktl
