#include <gktl/chain_buffer.hpp>

namespace gktl {
auto ChainBuffer::create(usize initial_nodes, usize capacity) -> ustl::Pair<Writer &, Reader &> {}

auto ChainBuffer::Writer::write(u8 *buffer, usize leng) -> usize {}

auto ChainBuffer::Reader::read(u8 *buffer, usize leng) -> usize {}
} // namespace gktl