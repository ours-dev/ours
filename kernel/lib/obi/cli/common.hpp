/// Shared utilities used by all obit commands.
#ifndef OBI_CLI_COMMON_HPP
#define OBI_CLI_COMMON_HPP

#include <obi/container-view.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace obit {
namespace fs = std::filesystem;
using obi::ContainerView;
using Bytes = obi::ItemView::Bytes;
using MutableBytes = obi::ItemView::MutableBytes;

enum class Error {
    Io,
    BadContainer,
    NotFound,
    Ambiguous,
};

inline auto message(Error const error) -> std::string_view {
    using enum Error;
    switch (error) {
        case Io:
            return "I/O error";
        case BadContainer:
            return "invalid OBI container";
        case NotFound:
            return "item not found";
        case Ambiguous:
            return "ambiguous item name";
    }
    return "unknown error";
}

inline auto read_file(fs::path const &path) -> std::expected<std::vector<u8>, Error> {
    std::error_code ec;
    auto const size = fs::file_size(path, ec);
    if (ec || size > std::numeric_limits<std::streamsize>::max()) {
        return std::unexpected(Error::Io);
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error::Io);
    }
    std::vector<u8> buffer(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
    if (!input) {
        return std::unexpected(Error::Io);
    }
    return buffer;
}

inline auto write_file(fs::path const &path, Bytes const bytes) -> std::expected<void, Error> {
    auto const parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    auto const tmp = parent / ("." + path.filename().string() + ".tmp");
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(Error::Io);
        }
        output.write(reinterpret_cast<char const *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            fs::remove(tmp);
            return std::unexpected(Error::Io);
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return std::unexpected(Error::Io);
    }
    return {};
}

inline auto open_container(fs::path const &path) -> std::expected<ContainerView, Error> {
    auto const data = read_file(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    auto container = ContainerView::from(data->data(), data->size());
    if (container.is_err()) {
        return std::unexpected(Error::BadContainer);
    }
    return container.unwrap();
}

inline auto find_item(ContainerView const container, std::string_view const name)
    -> std::expected<obi::ItemView, Error> {
    ustl::Option<obi::ItemView> match{ustl::NONE};
    for (usize i = 0; i < container.item_count(); ++i) {
        auto candidate = container.item(i);
        if (candidate.is_err()) {
            return std::unexpected(Error::BadContainer);
        }
        if (candidate.unwrap().name() != name) {
            continue;
        }
        if (match.is_some()) {
            return std::unexpected(Error::Ambiguous);
        }
        match = candidate.unwrap();
    }
    if (match.is_none()) {
        return std::unexpected(Error::NotFound);
    }
    return match.value();
}
} // namespace obit

#endif
