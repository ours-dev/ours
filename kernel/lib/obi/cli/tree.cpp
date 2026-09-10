#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "command.hpp"
#include "common.hpp"
#include <bootfs/view.hpp>
#include <cxxopts.hpp>
#include <obi/command_line_view.hpp>
#include <obi/package_format.hpp>

namespace obit {
namespace {
    auto type_tag(obi::ItemType const type) -> std::string_view {
        switch (type) {
            case obi::ItemType::StorageKernel:
                return "KSTR";
            case obi::ItemType::StorageBootFs:
                return "BTSF";
            case obi::ItemType::Executable:
                return "EXEC";
            case obi::ItemType::Plugin:
                return "PLUG";
            case obi::ItemType::Vdso:
                return "VDSO";
            case obi::ItemType::Config:
                return "CONF";
            case obi::ItemType::Data:
                return "DATA";
        }
        return "????";
    }

    /// One rendered line plus its descendants.  Labels are fully formatted
    /// before insertion, so the renderer never has to know what an item
    /// or a BootFS path is.
    struct Node final {
        std::string label;
        std::vector<Node> children;
    };

    /// Insert one BootFS path (e.g. "ours/vmours.elf") as a chain of
    /// directory nodes ending in a size-annotated file node.  Directory
    /// nodes are shared between paths so repeated prefixes collapse.
    auto insert_path(Node &root, std::string_view const path, u64 const size) -> void {
        auto *node = &root;
        auto begin = usize{0};
        while (begin <= path.size()) {
            auto const slash = path.find('/', begin);
            auto const is_file = slash == std::string_view::npos;
            auto const segment = path.substr(begin, slash - begin);
            auto label = std::string(segment);
            if (is_file) {
                label += std::format(" ({} bytes)", size);
            } else {
                label += "/";
            }

            Node *child = nullptr;
            for (auto &candidate: node->children) {
                if (candidate.label == label) {
                    child = &candidate;
                    break;
                }
            }
            if (child == nullptr) {
                node->children.push_back(Node{label, {}});
                child = &node->children.back();
            }
            node = child;

            if (is_file) {
                return;
            }
            begin = slash + 1;
        }
    }

    auto render_children(std::vector<Node> const &children, std::string const &prefix) -> void {
        for (usize index = 0; index < children.size(); ++index) {
            auto const &child = children[index];
            auto const is_last = index + 1 == children.size();
            if (is_last) {
                std::println("{}└── {}", prefix, child.label);
                render_children(child.children, prefix + "    ");
            } else {
                std::println("{}├── {}", prefix, child.label);
                render_children(child.children, prefix + "│   ");
            }
        }
    }

    /// Materialize a storage item and append its BootFS contents below the
    /// item node.  Returns 0 on success, 1 when the payload is not a valid
    /// BootFS image (reported to stderr, matching extract's error style).
    auto expand_bootfs(obi::ItemView const &item, Node &node) -> int {
        auto materialized = std::vector<u8>(item.memory_size());
        auto const bytes =
            item.materialize(obi::ItemView::MutableBytes(materialized.data(), materialized.size()));
        if (bytes.is_err()) {
            std::println(std::cerr, "obit: cannot materialize '{}': status {}", item.name(),
                         static_cast<int>(bytes.unwrap_err()));
            return 1;
        }

        auto const bootfs =
            bootfs::View::from(bootfs::View::Bytes(materialized.data(), materialized.size()));
        if (bootfs.is_err()) {
            std::println(std::cerr, "obit: item '{}': not a BootFS image (status {})", item.name(),
                         static_cast<int>(bootfs.unwrap_err()));
            return 1;
        }
        auto const &files = bootfs.unwrap();

        for (u32 index = 0; index < files.file_count(); ++index) {
            auto const entry = files.file(index);
            if (entry.is_err()) {
                std::println(std::cerr, "obit: item '{}': entry {} invalid (status {})",
                             item.name(), index, static_cast<int>(entry.unwrap_err()));
                return 1;
            }
            insert_path(node, files.file_name(index), entry.unwrap().data_size);
        }

        node.label += std::format(" — BootFS, {} files", files.file_count());
        return 0;
    }

    auto expand_config(obi::ItemView const &item, Node &node) -> void {
        auto const command_line = obi::CommandLineView::from(item);
        if (command_line.is_err()) {
            node.label += std::format(" (not a command line: status {})",
                                      static_cast<int>(command_line.unwrap_err()));
            return;
        }

        auto const text = command_line.unwrap().text();
        if (text.empty()) {
            node.children.push_back(Node{"<empty>", {}});
            return;
        }
        node.children.push_back(Node{std::string(text), {}});
    }

    auto build_item_node(obi::ItemView const &item, int &status) -> Node {
        auto node = Node{std::format("[{}] {}", type_tag(item.type()), item.name()), {}};
        switch (item.type()) {
            case obi::ItemType::StorageKernel:
            case obi::ItemType::StorageBootFs:
                if (expand_bootfs(item, node) != 0) {
                    status = 1;
                }
                break;
            case obi::ItemType::Config:
                expand_config(item, node);
                break;
            case obi::ItemType::Executable:
            case obi::ItemType::Plugin:
            case obi::ItemType::Vdso:
            case obi::ItemType::Data:
                break;
        }
        return node;
    }
} // namespace

class TreeCommand final: public AutoCommand<TreeCommand> {
  public:
    TreeCommand()
        : AutoCommand("tree", "print the item and BootFS tree of an OBI container",
                      "INPUT [NAME]") {}

    auto execute(std::vector<std::string> const &args) const -> int override {
        cxxopts::Options opts("obit tree", "print the item and BootFS tree of an OBI container");
        auto adder = opts.add_options();
        adder("input", "OBI container path", cxxopts::value<std::string>());
        adder("name", "restrict the tree to this item", cxxopts::value<std::string>());
        opts.parse_positional({"input", "name"});
        opts.positional_help("INPUT [NAME]");

        auto const argv = make_argv("obit tree", args);
        cxxopts::ParseResult parsed;
        try {
            parsed = opts.parse(static_cast<int>(argv.size()), argv.data());
        } catch (std::exception const &ex) {
            std::println(std::cerr, "obit tree: {}", ex.what());
            return 2;
        }
        if (parsed.count("input") == 0) {
            std::println(std::cerr, "obit tree: expected INPUT [NAME]");
            return 2;
        }

        auto const input = parsed["input"].as<std::string>();
        auto const container = open_container(input);
        if (!container) {
            std::println(std::cerr, "obit: {}: {}", message(container.error()), input);
            return (container.error() == Error::Io) ? 2 : 1;
        }
        auto const &c = container->view();

        // NAME selects one item; without it every item is laid out in the
        // container's own order (item 0 is the boot entry point, which the
        // tree should not reshuffle away).
        auto roots = std::vector<Node>();
        if (parsed.count("name") != 0) {
            auto const name = parsed["name"].as<std::string>();
            auto const item = find_item(c, name);
            if (!item) {
                if (item.error() == Error::NotFound) {
                    std::println(std::cerr, "obit: item not found: {}", name);
                } else {
                    std::println(std::cerr, "obit: ambiguous item: {}", name);
                }
                return 1;
            }
            auto status = int{0};
            roots.push_back(build_item_node(item.value(), status));
            if (status != 0) {
                return status;
            }
            std::println("{} (OBI container, item '{}')", input, name);
        } else {
            auto status = int{0};
            for (usize index = 0; index < c.item_count(); ++index) {
                auto item = c.item(index);
                if (item.is_err()) {
                    return 1;
                }
                roots.push_back(build_item_node(item.unwrap(), status));
            }
            if (status != 0) {
                return status;
            }
            std::println("{} (OBI container, {} items)", input, c.item_count());
        }

        render_children(roots, "");
        return 0;
    }
};
} // namespace obit
