#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

#include "nordend/core/event_loop.hpp"

namespace nordend::platform {

// One program run for its answer: its standard output read as it comes (stdin on /dev/null,
// stderr Nordend's), its end followed by a pidfd; done(output, waitpid status) once it has
// ended. Nordend's own pickers (`nordend --pick`) run this way, for the desktop portal and for
// Nordend itself. Loop thread only.
class ProgramOutput {
public:
    using Done = std::function<void(const std::string& output, int wait_status)>;

    ProgramOutput(core::EventLoop& loop, Done done);
    ~ProgramOutput();  // a program still running is killed

    ProgramOutput(const ProgramOutput&) = delete;
    ProgramOutput& operator=(const ProgramOutput&) = delete;
    ProgramOutput(ProgramOutput&&) = delete;
    ProgramOutput& operator=(ProgramOutput&&) = delete;

    // Starts program with arguments; false (and done never called) when it cannot be started.
    bool start(const std::filesystem::path& program, const std::vector<std::string>& arguments);

    // Asks the program to close (SIGTERM, SIGKILL 3 s later): done follows when it has.
    void stop();

    [[nodiscard]] bool running() const noexcept { return pid_ > 0; }

private:
    void read_output();
    void finish();
    void close_fds();

    core::EventLoop& loop_;
    Done done_;
    pid_t pid_{-1};
    int output_{-1};
    int pidfd_{-1};
    core::EventLoop::SourceId output_source_{0};
    core::EventLoop::SourceId exit_source_{0};
    core::EventLoop::SourceId kill_timer_{0};
    std::string received_;
};

}  // namespace nordend::platform
