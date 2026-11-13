// nordend-portal: Nordend as the desktop's file chooser (ROADMAP 4). Started by the session bus
// when xdg-desktop-portal first asks for org.freedesktop.impl.portal.desktop.nordend; one
// `nordend --pick` per request; gone after 30 seconds without one.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/log.hpp"
#include "nordend/platform/bus_loop.hpp"
#include "nordend/portal/file_chooser.hpp"

namespace {

namespace core = nordend::core;
namespace log = nordend::log;
namespace platform = nordend::platform;
namespace portal = nordend::portal;

constexpr std::chrono::milliseconds kIdle{30'000};

constexpr const char* kUsage =
    "usage: nordend-portal [--picker PROGRAM]\n"
    "  Nordend's file chooser for xdg-desktop-portal, started by the session bus.\n"
    "  --picker PROGRAM  run PROGRAM --pick ... instead of nordend\n";

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path picker;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--picker" && i + 1 < argc) {
            picker = argv[++i];
        } else {
            std::fprintf(stderr, "nordend-portal: unknown option '%s'\n%s", argv[i], kUsage);
            return 2;
        }
    }
    if (picker.empty()) {
        picker = portal::default_picker();
    }
    log::init_from_env();

    // SIGINT and SIGTERM end the loop, so open pickers are closed on the way out.
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    const int signals = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signals < 0) {
        log::error("nordend-portal: signalfd failed");
        return 1;
    }

    int code = 0;
    try {
        core::EventLoop loop;
        loop.add_fd(signals, POLLIN, [&](short /*revents*/) {
            signalfd_siginfo info{};
            if (read(signals, &info, sizeof info) == sizeof info && info.ssi_signo != SIGPIPE) {
                log::info("portal: signal received, closing");
                loop.quit(0);
            }
        });
        const std::unique_ptr<sdbus::IConnection> connection = sdbus::createSessionBusConnection();
        platform::BusLoop bus{loop, *connection, "portal"};
        bus.set_lost_handler([&loop] { loop.quit(0); });
        portal::FileChooser chooser{loop, *connection, bus, picker};
        chooser.set_idle_handler(kIdle, [&loop] {
            log::info("portal: nothing asked for {} s, closing", kIdle.count() / 1000);
            loop.quit(0);
        });
        // The objects first, then the name: a caller started us for it and calls at once.
        connection->requestName(sdbus::ServiceName{std::string{portal::kBusName}});
        log::info("portal: serving {} with {}", portal::kBusName, picker.string());
        bus.pump();
        code = loop.run();
    } catch (const sdbus::Error& error) {
        log::error("nordend-portal: {}", error.getMessage());
        code = 1;
    } catch (const std::exception& error) {
        log::error("nordend-portal: {}", error.what());
        code = 1;
    }
    close(signals);
    return code;
}
