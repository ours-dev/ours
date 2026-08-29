#include <ours/time/tick_device.hpp>
#include <ours/time/clock_source.hpp>

#include <ours/irq/mod.hpp>
#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>

namespace ours::time {
    INIT_DATA
    static ClockEventDevice s_noop_device {
        "Noop Device",
        0, 
        ClockEventDevice::Feature::None, 
        0
    };

    TickDevice::TickDevice()
        : m_canary(),
          m_clkdev(&s_noop_device),
          m_mode(Mode::Periodic)
    {}

    auto TickDevice::program_event(TimePoint<> expired) -> Status {
        m_canary.verify<Self>();
        if (ustl::NumericLimits<decltype(expired)>::max() == expired) {
            m_clkdev->switch_mode(ClockEventDevice::Mode::Shutdown);
            return Status::Ok;
        }

        // When this device is sleeping, wake it up now.
        if (m_clkdev->is_mode_oneshot_stopped()) {
            m_clkdev->switch_mode(ClockEventDevice::Mode::Oneshot);
        }

        return m_clkdev->program_event(expired);
    }

    auto TickDevice::check_new_device(ClockEventDevice *new_clkdev) -> Status {
        m_canary.verify<Self>();
        return rebind_device(new_clkdev);
    }

    auto TickDevice::rebind_device(ClockEventDevice *new_clkdev) -> Status {
        m_canary.verify<Self>();

        auto handler = m_clkdev->get_event_handler();
        new_clkdev->set_event_handler(ustl::move(handler));
        m_clkdev->set_event_handler(nullptr);

        if (m_clkdev->cpumask() != new_clkdev->cpumask()) {
            irq::set_affinity(m_clkdev->irqnum(), m_clkdev->cpumask());
        }

        if (m_mode == Mode::Periodic) {
            new_clkdev->set_event_handler([] (ClockEventDevice *device) { handle_periodic_event(device); });
            new_clkdev->switch_mode(ClockEventDevice::Mode::Periodic);
        } else if (m_mode == Mode::Oneshot) {
            switch_to_oneshot(handler);
        }

        m_clkdev->shutdown();
        m_clkdev = new_clkdev;
        return Status::Ok;
    }

    auto TickDevice::switch_to_oneshot(ClockEventDevice::Handler const &handler) -> void {
        m_mode = Mode::Oneshot;
        m_clkdev->set_event_handler(handler);
        m_clkdev->switch_mode(ClockEventDevice::Mode::Oneshot);
    }

    auto TickDevice::handle_periodic_event(ClockEventDevice *device) -> void {
        ClockSource::Default::advance_ticks(1);

        // Some devices may not support periodic events, so we need to manually
        // reprogram the next periodic event.
        if (!device->is_mode_oneshot()) {
            return;
        }

        auto next = device->next_event();
        while (1) {
            next += kNsecPerTick;

            if (Status::Ok == device->program_event(next)) {
                return;
            }
        }
    }

} // namespace ours::time