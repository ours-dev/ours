// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <format>

namespace ustl::fmt {

template <typename Context>
using BasicFormatArgs = std::basic_format_args<Context>;

using FormatArgs = std::format_args;

} // namespace ustl::fmt
