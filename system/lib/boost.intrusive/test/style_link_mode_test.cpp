#include <boost/intrusive/link_mode.hpp>

static_assert(boost::intrusive::IsSafeAutounlink<boost::intrusive::SafeLink>::VALUE);
static_assert(!boost::intrusive::IsSafeAutounlink<boost::intrusive::NormalLink>::VALUE);
