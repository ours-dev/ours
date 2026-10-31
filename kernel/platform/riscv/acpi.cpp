/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/acpi.hpp>

namespace ours {
/// This platform reads no ACPI tables yet, so get_acpi_tables() answers
/// nullptr and user space is handed none.
acpi::TableSet *ACPI_TABLES = nullptr;
} // namespace ours
