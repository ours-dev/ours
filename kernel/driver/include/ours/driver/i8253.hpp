#ifndef OURS_DRIVER_I8253_HPP
#define OURS_DRIVER_I8253_HPP

#include <ours/init.hpp>
#include <ours/time/types.hpp>
#include <ours/types.hpp>

namespace ours {
extern time::ClockEventDevice *I8253_CLKEVT;

INIT_CODE auto i8253_init(bool oneshot) -> void;

} // namespace ours

#endif // OURS_DRIVER_I8253_HPP
