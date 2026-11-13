#pragma once

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;  // NOLINT(readability-redundant-declaration): for posix_spawnp

namespace nordend::test {

// A private dbus-daemon listening on socket, or none when dbus-daemon is not installed.
class PrivateBus {
public:
    explicit PrivateBus(const std::filesystem::path& socket) {
        const std::string address = "--address=unix:path=" + socket.string();
        std::vector<std::string> args{"dbus-daemon", "--session", "--nofork", address};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (posix_spawnp(&pid_, "dbus-daemon", &actions, nullptr, argv.data(), environ) != 0) {
            pid_ = -1;
        }
        posix_spawn_file_actions_destroy(&actions);
        for (int i = 0; pid_ > 0 && i < 500 && !std::filesystem::exists(socket); ++i) {
            usleep(10'000);
        }
        if (pid_ > 0 && !std::filesystem::exists(socket)) {
            kill();
        }
    }

    ~PrivateBus() { kill(); }

    PrivateBus(const PrivateBus&) = delete;
    PrivateBus& operator=(const PrivateBus&) = delete;
    PrivateBus(PrivateBus&&) = delete;
    PrivateBus& operator=(PrivateBus&&) = delete;

    [[nodiscard]] bool running() const { return pid_ > 0; }

    void kill() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
    }

private:
    pid_t pid_{-1};
};

}  // namespace nordend::test
