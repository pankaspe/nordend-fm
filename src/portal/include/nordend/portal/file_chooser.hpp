#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "nordend/core/event_loop.hpp"
#include "nordend/platform/bus_loop.hpp"

// Nordend as the desktop's file chooser (ROADMAP 4): the backend xdg-desktop-portal asks when an
// application wants a file. The service (nordend-portal) answers on the session bus and starts one
// `nordend --pick` per request, which shows the files and prints what was chosen.
namespace nordend::portal {

// The bus name and object xdg-desktop-portal calls (named in nordend.portal).
inline constexpr std::string_view kBusName = "org.freedesktop.impl.portal.desktop.nordend";
inline constexpr std::string_view kObjectPath = "/org/freedesktop/portal/desktop";
inline constexpr std::string_view kFileChooser = "org.freedesktop.impl.portal.FileChooser";
inline constexpr std::string_view kRequest = "org.freedesktop.impl.portal.Request";

// The portal's answers.
inline constexpr std::uint32_t kResponseOk = 0;
inline constexpr std::uint32_t kResponseCancelled = 1;
inline constexpr std::uint32_t kResponseOther = 2;

using Options = std::map<std::string, sdbus::Variant>;

// What an application asked for, as far as Nordend follows it.
struct PickRequest {
    std::string title;
    bool multiple{false};
    bool directory{false};         // folders, not files (4b)
    std::filesystem::path folder;  // where to start; empty: the picker's own choice
};

// OpenFile's arguments: options of another type than the spec's are left out.
[[nodiscard]] PickRequest open_file_request(std::string title, const Options& options);

// The picker's command line after its program: --pick [--multiple] [--title TITLE] [FOLDER].
[[nodiscard]] std::vector<std::string> picker_arguments(const PickRequest& request);

// What a picker gave back.
struct PickResult {
    std::uint32_t response{kResponseOther};
    std::vector<std::string> uris;
};

// From the picker's output (one file:// URI a line; other lines are not ours and left out) and
// its waitpid() status: URIs and a clean exit are a choice; a clean exit without URIs, or the
// SIGTERM of a closed request, a cancel; anything else (an error exit, a crash) "other".
[[nodiscard]] PickResult pick_result(std::string_view output, int wait_status);

// One picker process: its standard output read as it comes, its end followed by a pidfd.
// Loop thread only.
class Picker {
public:
    using Done = std::function<void(PickResult)>;

    Picker(core::EventLoop& loop, Done done);
    ~Picker();  // a picker still running is killed

    Picker(const Picker&) = delete;
    Picker& operator=(const Picker&) = delete;
    Picker(Picker&&) = delete;
    Picker& operator=(Picker&&) = delete;

    // Starts program with arguments; false (and done never called) when it cannot be started.
    bool start(const std::filesystem::path& program, const std::vector<std::string>& arguments);

    // Asks the picker to close (SIGTERM): done follows when it has.
    void stop();

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

// org.freedesktop.impl.portal.FileChooser on connection at kObjectPath: OpenFile starts a picker
// and answers when it ends; Request.Close at the request's handle stops it. SaveFile and
// SaveFiles answer "other" for now (4c). Loop thread only; connection and bus outlive it.
class FileChooser {
public:
    FileChooser(core::EventLoop& loop, sdbus::IConnection& connection, platform::BusLoop& bus,
                std::filesystem::path picker);
    ~FileChooser();

    FileChooser(const FileChooser&) = delete;
    FileChooser& operator=(const FileChooser&) = delete;
    FileChooser(FileChooser&&) = delete;
    FileChooser& operator=(FileChooser&&) = delete;

    // Called once nothing has been asked for `after` (from now, or since the last request
    // ended): the service closes then.
    void set_idle_handler(std::chrono::milliseconds after, std::function<void()> idle);

    [[nodiscard]] std::size_t pending() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The picker next to the service: <prefix>/libexec/nordend/nordend-portal runs
// <prefix>/bin/nordend; in the build tree, the nordend beside it.
[[nodiscard]] std::filesystem::path default_picker();

}  // namespace nordend::portal
