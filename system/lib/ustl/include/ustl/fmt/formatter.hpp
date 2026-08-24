// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <format>

namespace ustl::fmt {

template <typename Type, typename Char = char>
using Formatter = std::formatter<Type, Char>;

} // namespace ustl::fmt
