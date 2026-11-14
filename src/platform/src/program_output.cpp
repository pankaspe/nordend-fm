#include "nordend/platform/program_output.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "nordend/core/log.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): for posix_spawn

namespace nordend::platform {
namespace {

// More than any program asked for an answer prints: one past it is not listened to any further.
constexpr std::size_t kMaxOutput = std::size_t{16} << 20U;
// A program asked to close that has not after this is killed.
constexpr std::chrono::milliseconds kStopGrace{3000};

// pidfd calls by number: <sys/pidfd.h> is missing or not C++-ready on some systems.
int open_pidfd(pid_t pid) { return static_cast<int>(syscall(SYS_pidfd_open, pid, 0U)); }

void signal_pidfd(int pidfd, int signal) {
    syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0U);
}

}  // namespace

ProgramOutput::ProgramOutput(core::EventLoop& loop, Done done)
    : loop_{loop}, done_{std::move(done)} {}

ProgramOutput::~ProgramOutput() {
    if (pid_ > 0) {
        kill(pid_, SIGKILL);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }
    close_fds();
}

bool ProgramOutput::start(const std::filesystem::path& program,
                          const std::vector<std::string>& arguments) {
    std::array<int, 2> pipe_fds{-1, -1};
    if (pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
        log::warn("no pipe for {}: {}", program.string(), std::strerror(errno));
        return false;
    }
    std::vector<std::string> words{program.string()};
    words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(words.size() + 1);
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    // Nordend and its services block the signals they read from a signalfd: the program starts
    // without that.
    posix_spawnattr_t attributes{};
    posix_spawnattr_init(&attributes);
    sigset_t none{};
    sigemptyset(&none);
    sigset_t defaults{};
    sigemptyset(&defaults);
    for (const int signal : {SIGTERM, SIGINT, SIGPIPE, SIGUSR1}) {
        sigaddset(&defaults, signal);
    }
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const int spawned =
        posix_spawn(&pid_, argv.front(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (spawned != 0) {
        pid_ = -1;
        close(pipe_fds[0]);
        log::warn("cannot start {}: {}", program.string(), std::strerror(spawned));
        return false;
    }
    output_ = pipe_fds[0];
    fcntl(output_, F_SETFL, fcntl(output_, F_GETFL) | O_NONBLOCK);
    pidfd_ = open_pidfd(pid_);
    if (pidfd_ < 0) {
        log::warn("no pidfd for {}: {}", program.string(), std::strerror(errno));
        kill(pid_, SIGKILL);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        close_fds();
        return false;
    }
    output_source_ = loop_.add_fd(output_, POLLIN, [this](short /*revents*/) { read_output(); });
    // Readable once the program has ended: what it printed is in the pipe by then, even when
    // something it started keeps the pipe open.
    exit_source_ = loop_.add_fd(pidfd_, POLLIN, [this](short /*revents*/) { finish(); });
    log::debug("{} started as {}", program.filename().string(), pid_);
    return true;
}

void ProgramOutput::stop() {
    if (pid_ <= 0) {
        return;
    }
    signal_pidfd(pidfd_, SIGTERM);
    if (kill_timer_ == 0) {
        kill_timer_ = loop_.add_timer(kStopGrace, std::chrono::milliseconds{0}, [this] {
            kill_timer_ = 0;
            signal_pidfd(pidfd_, SIGKILL);
            log::warn("{} did not close: killed", pid_);
        });
    }
}

void ProgramOutput::read_output() {
    if (output_ < 0) {
        return;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t got = read(output_, buffer.data(), buffer.size());
        if (got > 0) {
            if (received_.size() < kMaxOutput) {
                received_.append(buffer.data(), static_cast<std::size_t>(got));
            }
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got == 0 || errno != EAGAIN) {
            // The end, or an error: nothing more comes from it.
            loop_.remove(output_source_);
            output_source_ = 0;
            close(output_);
            output_ = -1;
        }
        return;
    }
}

void ProgramOutput::finish() {
    read_output();  // what is left in the pipe
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    log::debug("{} ended ({})", pid_, status);
    pid_ = -1;
    close_fds();
    // done may end this object: nothing of it is touched after.
    const Done done = std::move(done_);
    const std::string output = std::move(received_);
    done(output, status);
}

void ProgramOutput::close_fds() {
    loop_.remove(output_source_);
    loop_.remove(exit_source_);
    loop_.remove(kill_timer_);
    output_source_ = 0;
    exit_source_ = 0;
    kill_timer_ = 0;
    if (output_ >= 0) {
        close(output_);
        output_ = -1;
    }
    if (pidfd_ >= 0) {
        close(pidfd_);
        pidfd_ = -1;
    }
}

}  // namespace nordend::platform
