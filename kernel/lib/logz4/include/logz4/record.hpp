/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_RECORD_HPP
#define LOGZ4_RECORD_HPP 1

namespace logz4 {
    enum class Level {
        Status,
        Warn,
        Info,
        Debug,
        Trace,
        Error,
    };

    struct Metadata {
    };

    struct Record {
        Metadata *metadata;
        Level level;
    };
} // namespace logz4

#endif // #ifndef LOGZ4_RECORD_HPP