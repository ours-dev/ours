/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <fsio/path.hpp>

namespace fsio {
// fdio's CleanPath() (sdk/lib/fdio/cleanpath.cc), step for step, on a
// string_view instead of a NUL-terminated string. Its step numbers are kept
// in the comments.
//
// Nothing below can write past m_chars: every step writes at most as many
// bytes as it consumes from |path|, so |out| never grows longer than |path|,
// which is at most MAX_PATH_LENGTH. A name copies itself, and the '/' put in
// front of it stands for the separator that came before it in |path|; a kept
// ".." with its '/' stands for the same three bytes of |path|. The one
// exception is the empty path, which becomes "." -- one byte.
auto clean_path(std::string_view const path, PathBuffer &out) -> ours::Status {
    out.m_size = 0;
    if (path.size() > MAX_PATH_LENGTH) {
        return ours::Status::BadPath;
    }

    auto const append = [&out](std::string_view const text) {
        for (auto const c: text) {
            out.m_chars[out.m_size] = c;
            out.m_size += 1;
        }
    };

    if (path.empty()) {
        append(".");
        return ours::Status::Ok;
    }

    auto const rooted = path.front() == '/';
    auto at = usize{0};
    if (rooted) {
        append("/");
        at = 1;
    }
    // Below this length of |out|, a ".." cannot be cleaned away: the root of
    // a rooted path, or the ".." names a relative path starts with.
    auto dotdot = out.m_size;

    // A name ends at a '/' or at the end of the path, where fdio finds the
    // NUL.
    auto const ends_name = [&path](usize const index) {
        return index >= path.size() || path[index] == '/';
    };

    while (at < path.size()) {
        if (path[at] == '/') {
            // 1. Reduce multiple slashes to a single slash.
            at += 1;
        } else if (path[at] == '.' && ends_name(at + 1)) {
            // 2. Eliminate "." names, the current directory.
            at += 1;
        } else if (path[at] == '.' && at + 1 < path.size() && path[at + 1] == '.' &&
                   ends_name(at + 2)) {
            at += 2;
            if (out.m_size > dotdot) {
                // 3. Eliminate a ".." and the name before it, along with the
                // '/' between them.
                auto last = out.m_size - 1;
                while (last > dotdot && out.m_chars[last] != '/') {
                    last -= 1;
                }
                out.m_size = last;
            } else if (!rooted) {
                // 5. Keep a ".." that begins a relative path: there is
                // nothing before it to eliminate.
                if (out.m_size > 0) {
                    append("/");
                }
                append("..");
                dotdot = out.m_size;
            }
            // 4. Otherwise the path is rooted and |out| is "/": "/.." is
            // "/", so the ".." is dropped.
        } else {
            // A name: a '/' in front of it unless it is the first, then the
            // name itself.
            auto const first = (rooted && out.m_size == 1) || (!rooted && out.m_size == 0);
            if (!first) {
                append("/");
            }
            auto const start = at;
            while (!ends_name(at)) {
                at += 1;
            }
            append(path.substr(start, at - start));
        }
    }

    if (out.m_size == 0) {
        append(".");
    }
    return ours::Status::Ok;
}
} // namespace fsio
