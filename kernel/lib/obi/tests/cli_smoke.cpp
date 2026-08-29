#define main obit_main
#include "../cli/common.hpp"
#include "../cli/main.cpp"
#undef main

#include <ours/types.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <cstdlib>
#include <unistd.h>

namespace {
auto require(bool const condition) -> void {
    if (!condition) {
        std::abort();
    }
}

auto read_text(std::filesystem::path const &path) -> std::string {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

auto main() -> int {
    auto const directory_name =
        (std::filesystem::temp_directory_path() / "obi-cli-XXXXXX").string();
    auto directory_template = std::vector<char>(directory_name.begin(), directory_name.end());
    directory_template.push_back('\0');
    auto *directory = ::mkdtemp(directory_template.data());
    require(directory != nullptr);

    auto const root = std::filesystem::path(directory);
    auto const target = root / "target";
    auto const output = root / "output";
    {
        auto sentinel = std::ofstream(target, std::ios::binary);
        sentinel << "sentinel";
    }
    std::filesystem::create_symlink(target, output);

    // write_file() writes to a same-directory temp file and renames it into
    // place, so writing through a symlink replaces the symlink itself rather
    // than following it into `target` — `target`'s content must survive.
    constexpr auto payload = std::array<u8, 4>{1, 2, 3, 4};
    auto const wrote = obit::write_file(output, obit::Bytes(payload.data(), payload.size()));
    require(wrote.has_value());
    require(read_text(target) == "sentinel");
    require(!std::filesystem::is_symlink(output));

    auto const read_back = obit::read_file(output);
    require(read_back.has_value());
    require(read_back->size() == payload.size());
    for (usize index = 0; index < payload.size(); ++index) {
        require((*read_back)[index] == payload[index]);
    }

    // A missing file must be reported as an error, not crash or return
    // partial/garbage data.
    auto const missing = obit::read_file(root / "does-not-exist");
    require(!missing.has_value());

    std::filesystem::remove_all(root);
    return 0;
}
