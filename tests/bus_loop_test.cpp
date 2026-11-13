// BusLoop when its bus goes away, as the session bus does at logout before Nordend's window
// closes: it stops following the bus, and the loop carries on (and Nordend saves its state).
#include "nordend/platform/bus_loop.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <spawn.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "private_bus.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::platform::BusLoop;
using nordend::test::PrivateBus;
using nordend::test::TempDir;
using namespace std::chrono_literals;

namespace {

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    const int code = loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return code == 0;
}

std::chrono::microseconds thread_cpu() {
    rusage usage{};
    getrusage(RUSAGE_THREAD, &usage);
    return std::chrono::seconds{usage.ru_utime.tv_sec + usage.ru_stime.tv_sec} +
           std::chrono::microseconds{usage.ru_utime.tv_usec + usage.ru_stime.tv_usec};
}

}  // namespace

TEST_CASE("bus loop: a bus that goes away is let go, without an error out of the loop") {
    const TempDir dir;
    const std::filesystem::path socket = dir.path / "bus";
    PrivateBus daemon{socket};
    if (!daemon.running()) {
        MESSAGE("no dbus-daemon: skipped");
        return;
    }
    EventLoop loop;
    const std::unique_ptr<sdbus::IConnection> connection =
        sdbus::createSessionBusConnectionWithAddress("unix:path=" + socket.string());
    BusLoop bus{loop, *connection, "test"};
    bus.pump();
    CHECK_FALSE(bus.lost());

    daemon.kill();  // as at logout
    bool ran = false;
    CHECK_NOTHROW(ran = run_until(loop, [&] { return bus.lost(); }));
    CHECK(ran);
    CHECK(bus.lost());
    CHECK_NOTHROW(bus.pump());  // nothing to pump any more

    // Nothing left to wake the loop: a closed socket must not keep it spinning.
    const auto before = thread_cpu();
    const auto stop = loop.add_timer(200ms, 0ms, [&loop] { loop.quit(0); });
    CHECK(loop.run() == 0);
    loop.remove(stop);
    CHECK(thread_cpu() - before < 50ms);
}
