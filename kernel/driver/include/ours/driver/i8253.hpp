#ifndef OURS_DRIVER_I8253_HPP
#define OURS_DRIVER_I8253_HPP

#include <ours/init.hpp>
#include <ours/time/types.hpp>
#include <ours/types.hpp>

namespace ours {
extern time::ClockEventDevice *I8253_CLKEVT;

/// Builds the PIT's clock event device and offers it to this CPU's tick
/// device. It takes no mode: which mode the PIT runs in is TickDevice's
/// decision, made through switch_mode(). The `bool oneshot` this used to take
/// was never read.
INIT_CODE auto i8253_init() -> void;

} // namespace ours

#endif // OURS_DRIVER_I8253_HPP
