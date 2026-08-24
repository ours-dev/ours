#ifndef OURS_DRIVER_I8253_HPP
#define OURS_DRIVER_I8253_HPP

#include <ours/types.hpp>
#include <ours/init.hpp>

#include <ours/time/types.hpp>

namespace ours {
    extern time::ClockEventDevice *g_i8253_clkevt;

    INIT_CODE
    auto i8253_init(bool oneshot) -> void;

} // namespace ours

#endif // OURS_DRIVER_I8253_HPP
