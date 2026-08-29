/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef LIB_CODE_PATCHING_PATCHER_HPP
#define LIB_CODE_PATCHING_PATCHER_HPP

#include <span>

namespace code_patching {
struct Directive {
    u32 start;
    u32 end;
    u32 id;
};

class Patcher {
    using Self = Patcher;

  public:
    auto patch_with_alternative() -> void;

  private:
    std::span<Directive const> m_directives;
};
} // namespace code_patching

#endif // LIB_CODE_PATCHING_PATCHER_HPP