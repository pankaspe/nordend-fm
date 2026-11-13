#include "nordend/portal/file_chooser.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "nordend/core/log.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): for posix_spawn

namespace nordend::portal {
namespace {

// More than any choice of files prints: a picker past it is not listened to any further.
constexpr std::size_t kMaxOutput = std::size_t{16} << 20U;
// A picker asked to close that has not after this is killed.
constexpr std::chrono::milliseconds kStopGrace{3000};

// pidfd calls by number: <sys/pidfd.h> is missing or not C++-ready on some systems.
int open_pidfd(pid_t pid) { return static_cast<int>(syscall(SYS_pidfd_open, pid, 0U)); }

void signal_pidfd(int pidfd, int signal) {
    syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0U);
}

template <typename T>
std::optional<T> option(const Options& options, const char* name) {
    const auto it = options.find(name);
    if (it == options.end()) {
        return std::nullopt;
    }
    try {
        return it->second.get<T>();
    } catch (const sdbus::Error&) {
        return std::nullopt;  // another type than the spec's
    }
}

}  // namespace

PickRequest open_file_request(std::string title, const Options& options) {
    PickRequest request;
    request.title = std::move(title);
    request.multiple = option<bool>(options, "multiple").value_or(false);
    request.directory = option<bool>(options, "directory").value_or(false);
    // A byte string ending with a NUL, as GLib writes paths.
    if (const auto bytes = option<std::vector<std::uint8_t>>(options, "current_folder")) {
        std::string path(bytes->begin(), bytes->end());
        while (!path.empty() && path.back() == '\0') {
            path.pop_back();
        }
        if (!path.empty() && path.front() == '/' && path.find('\0') == std::string::npos) {
            request.folder = path;
        }
    }
    return request;
}

std::vector<std::string> picker_arguments(const PickRequest& request) {
    std::vector<std::string> arguments{"--pick"};
    if (request.multiple) {
        arguments.emplace_back("--multiple");
    }
    if (!request.title.empty()) {
        arguments.emplace_back("--title");
        arguments.push_back(request.title);
    }
    if (!request.folder.empty()) {
        arguments.push_back(request.folder.string());
    }
    return arguments;
}

PickResult pick_result(std::string_view output, int wait_status) {
    PickResult result;
    while (!output.empty()) {
        const std::size_t end = output.find('\n');
        const std::string_view line = output.substr(0, end);
        if (line.starts_with("file://")) {
            result.uris.emplace_back(line);
        }
        output = end == std::string_view::npos ? std::string_view{} : output.substr(end + 1);
    }
    const bool exited = WIFEXITED(wait_status);
    const int code = exited ? WEXITSTATUS(wait_status) : -1;
    if (exited && code == 0 && !result.uris.empty()) {
        result.response = kResponseOk;
        return result;
    }
    result.uris.clear();
    const bool closed = WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGTERM;
    result.response = (exited && code == 0) || closed ? kResponseCancelled : kResponseOther;
    return result;
}

// ---------------------------------------------------------------------------------------------
// Picker

Picker::Picker(core::EventLoop& loop, Done done) : loop_{loop}, done_{std::move(done)} {}

Picker::~Picker() {
    if (pid_ > 0) {
        kill(pid_, SIGKILL);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }
    close_fds();
}

bool Picker::start(const std::filesystem::path& program,
                   const std::vector<std::string>& arguments) {
    std::array<int, 2> pipe_fds{-1, -1};
    if (pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
        log::warn("portal: no pipe for the picker: {}", std::strerror(errno));
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
    // The service blocks the signals it reads from a signalfd: the picker starts without that.
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
        log::warn("portal: cannot start {}: {}", program.string(), std::strerror(spawned));
        return false;
    }
    output_ = pipe_fds[0];
    fcntl(output_, F_SETFL, fcntl(output_, F_GETFL) | O_NONBLOCK);
    pidfd_ = open_pidfd(pid_);
    if (pidfd_ < 0) {
        log::warn("portal: no pidfd for the picker: {}", std::strerror(errno));
        kill(pid_, SIGKILL);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        close_fds();
        return false;
    }
    output_source_ = loop_.add_fd(output_, POLLIN, [this](short /*revents*/) { read_output(); });
    // Readable once the picker has ended: what it printed is in the pipe by then, even when
    // something it started keeps the pipe open.
    exit_source_ = loop_.add_fd(pidfd_, POLLIN, [this](short /*revents*/) { finish(); });
    log::debug("portal: picker {} started", pid_);
    return true;
}

void Picker::stop() {
    if (pid_ <= 0) {
        return;
    }
    signal_pidfd(pidfd_, SIGTERM);
    if (kill_timer_ == 0) {
        kill_timer_ = loop_.add_timer(kStopGrace, std::chrono::milliseconds{0}, [this] {
            kill_timer_ = 0;
            signal_pidfd(pidfd_, SIGKILL);
            log::warn("portal: picker {} did not close: killed", pid_);
        });
    }
}

void Picker::read_output() {
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

void Picker::finish() {
    read_output();  // what is left in the pipe
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    log::debug("portal: picker {} ended ({})", pid_, status);
    pid_ = -1;
    close_fds();
    // done may end this picker: nothing of it is touched after.
    const Done done = std::move(done_);
    const PickResult result = pick_result(received_, status);
    done(result);
}

void Picker::close_fds() {
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

// ---------------------------------------------------------------------------------------------
// FileChooser

using Reply = sdbus::Result<std::uint32_t, Options>;

struct FileChooser::Impl {
    struct Pending {
        Reply reply;
        std::unique_ptr<sdbus::IObject> request;  // Request.Close at the handle
        std::unique_ptr<Picker> picker;
    };

    core::EventLoop& loop;
    sdbus::IConnection& connection;
    platform::BusLoop& bus;
    std::filesystem::path picker;
    std::unique_ptr<sdbus::IObject> object;
    std::map<std::string, std::unique_ptr<Pending>> pending;
    std::chrono::milliseconds idle_after{0};
    std::function<void()> idle;
    core::EventLoop::SourceId idle_timer{0};

    Impl(core::EventLoop& event_loop, sdbus::IConnection& bus_connection,
         platform::BusLoop& bus_loop, std::filesystem::path program)
        : loop{event_loop}, connection{bus_connection}, bus{bus_loop}, picker{std::move(program)} {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        loop.remove(idle_timer);
        // Pickers still open are killed with their Pending; their callers get no answer, which
        // xdg-desktop-portal reports to the application when this service leaves the bus.
    }

    void rest() {
        loop.remove(idle_timer);
        idle_timer = 0;
        if (pending.empty() && idle) {
            idle_timer = loop.add_timer(idle_after, std::chrono::milliseconds{0}, [this] {
                idle_timer = 0;
                idle();
            });
        }
    }

    void busy() {
        loop.remove(idle_timer);
        idle_timer = 0;
    }

    void open_file(Reply reply, const sdbus::ObjectPath& handle, const std::string& app_id,
                   std::string title, const Options& options) {
        const PickRequest request = open_file_request(std::move(title), options);
        log::info("portal: OpenFile for \"{}\"{}{}", app_id.empty() ? "?" : app_id,
                  request.multiple ? ", several files" : "", request.directory ? ", a folder" : "");
        if (request.directory) {
            log::info("portal: choosing folders is not there yet");
            reply.returnResults(kResponseOther, Options{});
            rest();
            return;
        }
        if (pending.contains(handle)) {
            reply.returnResults(kResponseOther, Options{});
            return;
        }
        busy();
        auto entry = std::make_unique<Pending>();
        entry->reply = std::move(reply);
        const std::string& key = handle;
        try {
            entry->request = sdbus::createObject(connection, handle);
            entry->request
                ->addVTable(
                    sdbus::registerMethod("Close").implementedAs([this, key] { close(key); }))
                .forInterface(std::string{kRequest});
        } catch (const sdbus::Error& error) {
            log::warn("portal: request {}: {}", key, error.getMessage());
            entry->reply.returnResults(kResponseOther, Options{});
            rest();
            return;
        }
        entry->picker = std::make_unique<Picker>(
            loop, [this, key](PickResult result) { answer(key, std::move(result)); });
        Picker& started = *entry->picker;
        pending.emplace(key, std::move(entry));
        if (!started.start(picker, picker_arguments(request))) {
            answer(key, {});
        }
    }

    void close(const std::string& key) {
        log::info("portal: request closed by the application");
        if (const auto it = pending.find(key); it != pending.end()) {
            it->second->picker->stop();
        }
    }

    void answer(const std::string& key, PickResult result) {
        const auto it = pending.find(key);
        if (it == pending.end()) {
            return;
        }
        // Out of the map first: the picker calling this ends with it, after this returns.
        std::unique_ptr<Pending> done = std::move(it->second);
        pending.erase(it);
        log::info("portal: {} file(s), response {}", result.uris.size(), result.response);
        Options results;
        if (result.response == kResponseOk) {
            results.emplace("uris", sdbus::Variant{result.uris});
        }
        try {
            done->reply.returnResults(result.response, results);
        } catch (const sdbus::Error& error) {
            log::warn("portal: answer: {}", error.getMessage());
        }
        done->request.reset();
        // The picker is still on the stack that called this: let it go from the loop.
        loop.post([picker = std::shared_ptr<Picker>(std::move(done->picker))] {});
        bus.pump();
        rest();
    }

    std::tuple<std::uint32_t, Options> not_yet(const char* method) {
        log::info("portal: {} is not there yet", method);
        rest();
        return {kResponseOther, Options{}};
    }
};

FileChooser::FileChooser(core::EventLoop& loop, sdbus::IConnection& connection,
                         platform::BusLoop& bus, std::filesystem::path picker)
    : impl_{std::make_unique<Impl>(loop, connection, bus, std::move(picker))} {
    Impl& impl = *impl_;
    impl.object = sdbus::createObject(connection, sdbus::ObjectPath{std::string{kObjectPath}});
    impl.object
        ->addVTable(
            sdbus::registerMethod("OpenFile")
                .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
                .withOutputParamNames("response", "results")
                .implementedAs([&impl](Reply&& reply, const sdbus::ObjectPath& handle,
                                       const std::string& app_id,
                                       const std::string& /*parent_window*/, std::string title,
                                       const Options& options) {
                    impl.open_file(std::move(reply), handle, app_id, std::move(title), options);
                }),
            sdbus::registerMethod("SaveFile")
                .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
                .withOutputParamNames("response", "results")
                .implementedAs(
                    [&impl](const sdbus::ObjectPath& /*handle*/, const std::string& /*app_id*/,
                            const std::string& /*parent_window*/, const std::string& /*title*/,
                            const Options& /*options*/) { return impl.not_yet("SaveFile"); }),
            sdbus::registerMethod("SaveFiles")
                .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
                .withOutputParamNames("response", "results")
                .implementedAs(
                    [&impl](const sdbus::ObjectPath& /*handle*/, const std::string& /*app_id*/,
                            const std::string& /*parent_window*/, const std::string& /*title*/,
                            const Options& /*options*/) { return impl.not_yet("SaveFiles"); }))
        .forInterface(std::string{kFileChooser});
}

FileChooser::~FileChooser() = default;

void FileChooser::set_idle_handler(std::chrono::milliseconds after, std::function<void()> idle) {
    impl_->idle_after = after;
    impl_->idle = std::move(idle);
    impl_->rest();
}

std::size_t FileChooser::pending() const noexcept { return impl_->pending.size(); }

std::filesystem::path default_picker() {
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return "nordend";
    }
    std::filesystem::path installed =
        self.parent_path().parent_path().parent_path() / "bin" / "nordend";
    if (self.parent_path().filename() == "nordend" && std::filesystem::exists(installed, error)) {
        return installed;
    }
    return self.parent_path() / "nordend";  // the build tree
}

}  // namespace nordend::portal
