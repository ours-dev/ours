/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <gktl/command.hpp>
#include <gktl/options.hpp>
#include <gktl/radix_tree.hpp>

#include <array>
#include <string_view>

#include <ktl/memory_resource.hpp>
#include <logz4/log.hpp>
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
            // Latching TREE_BUILT is what stops a broken image from retrying
            // the failure on every dispatch; saying so once is what keeps it
            // from being a mystery -- a console where nothing answers and no
            // line says why is the worst shape a broken image can take.
            TREE_BUILT = true;
            log::error("gktl: ERROR the command registry could not reserve {} nodes; no command "
                       "will answer",
                       capacity);
            return;
        }

        for (auto const *command = COMMANDS_START; command != COMMANDS_END; ++command) {
            // The insert answers false for a name the key encoder cannot
            // carry -- anything outside [a-zA-Z0-9. -] -- and for a name some
            // earlier command already took. Both are authoring bugs the
            // section walk cannot see, so the first dispatch is where they
            // must speak, not a registry that quietly omits one command.
            ASSERT(TREE.insert(*command), "command '{}' did not register", command->cmd);
        }

        TREE_BUILT = true;
    }
} // namespace

auto run_command(int const argc, char **const argv) -> void {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return;
    }

    build_radix_tree();

    auto const *command = TREE.find(argv[0]);
    if (command == nullptr || command->callback == nullptr) {
        return;
    }

    // The one place the console's C shape exists: the count and the pointers
    // become the invocation's views here, and every handler behind the
    // callback sees the value and not the array.
    // The console passes at most MAX_OPTIONS words after the command, and
    // the array is sized to that; a caller with more has them stop here
    // rather than run past the array -- the truncation itself is what
    // docs/repairs/261114/kshell-word-cap.md would make a report.
    auto words = std::array<std::string_view, MAX_OPTIONS>{};
    auto count = usize{0};
    for (auto i = 1; i < argc && argv[i] != nullptr && count != MAX_OPTIONS; ++i) {
        words[count] = argv[i];
        count += 1;
    }
    auto const invocation = Invocation{argv[0], {words.data(), count}};

    auto _ = command->callback(invocation);
}

} // namespace gktl
