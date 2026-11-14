#include "nordend/portal/file_chooser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <optional>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "nordend/core/log.hpp"

namespace nordend::portal {
namespace {

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

fs::FileFilter file_filter(const Filter& filter) {
    fs::FileFilter out;
    out.label = filter.get<0>();
    for (const FilterRule& rule : filter.get<1>()) {
        if (rule.get<0>() == 0) {
            out.globs.push_back(rule.get<1>());
        } else if (rule.get<0>() == 1) {
            out.types.push_back(rule.get<1>());
        }
    }
    return out;
}

namespace {

// A NUL-ended byte string ("ay"), as GLib writes paths; empty when absent or odd.
std::string path_bytes(const Options& options, const char* name) {
    const auto bytes = option<std::vector<std::uint8_t>>(options, name);
    if (!bytes) {
        return {};
    }
    std::string path(bytes->begin(), bytes->end());
    while (!path.empty() && path.back() == '\0') {
        path.pop_back();
    }
    return path.find('\0') == std::string::npos ? path : std::string{};
}

// What every method shares: the window, the button's word, filters, choices, the folder.
PickRequest common_request(std::string title, std::string parent_window, const Options& options) {
    PickRequest request;
    request.title = std::move(title);
    if (parent_window.starts_with("wayland:")) {
        request.parent_window = std::move(parent_window);
    }
    request.accept_label = option<std::string>(options, "accept_label").value_or("");
    request.filters =
        option<std::vector<Filter>>(options, "filters").value_or(std::vector<Filter>{});
    // The filter to start with: one of the list, or alone when there is no list.
    if (const auto current = option<Filter>(options, "current_filter")) {
        const auto same = std::ranges::find(request.filters, *current);
        if (same != request.filters.end()) {
            request.filter = static_cast<std::size_t>(same - request.filters.begin());
        } else if (request.filters.empty()) {
            request.filters.push_back(*current);
            request.filter = 0;
        }
    }
    request.choices =
        option<std::vector<Choice>>(options, "choices").value_or(std::vector<Choice>{});
    if (const std::string folder = path_bytes(options, "current_folder"); folder.starts_with('/')) {
        request.folder = folder;
    }
    return request;
}

// A name to save as: its last part, never "." or "..".
std::string file_name(std::string_view path) {
    const std::string_view name = path.substr(path.rfind('/') + 1);
    return name == "." || name == ".." ? std::string{} : std::string{name};
}

}  // namespace

PickRequest open_file_request(std::string title, std::string parent_window,
                              const Options& options) {
    PickRequest request = common_request(std::move(title), std::move(parent_window), options);
    request.multiple = option<bool>(options, "multiple").value_or(false);
    request.directory = option<bool>(options, "directory").value_or(false);
    return request;
}

PickRequest save_file_request(std::string title, std::string parent_window,
                              const Options& options) {
    PickRequest request = common_request(std::move(title), std::move(parent_window), options);
    request.save = true;
    const std::string current = path_bytes(options, "current_file");
    request.name = file_name(option<std::string>(options, "current_name").value_or(""));
    if (request.name.empty()) {
        request.name = file_name(current);
    }
    if (request.folder.empty() && current.starts_with('/')) {
        request.folder = std::filesystem::path{current}.parent_path();
    }
    return request;
}

PickRequest save_files_request(std::string title, std::string parent_window,
                               const Options& options) {
    PickRequest request = common_request(std::move(title), std::move(parent_window), options);
    request.save = true;
    for (const std::vector<std::uint8_t>& bytes :
         option<std::vector<std::vector<std::uint8_t>>>(options, "files")
             .value_or(std::vector<std::vector<std::uint8_t>>{})) {
        std::string path(bytes.begin(), bytes.end());
        while (!path.empty() && path.back() == '\0') {
            path.pop_back();
        }
        if (std::string name = file_name(path);
            !name.empty() && name.find('\0') == std::string::npos) {
            request.files.push_back(std::move(name));
        }
    }
    return request;
}

std::vector<std::string> picker_arguments(const PickRequest& request) {
    std::vector<std::string> arguments{"--pick"};
    if (request.multiple) {
        arguments.emplace_back("--multiple");
    }
    if (request.directory) {
        arguments.emplace_back("--folders");
    }
    const auto with = [&arguments](const char* name, const std::string& value) {
        if (!value.empty()) {
            arguments.emplace_back(name);
            arguments.push_back(value);
        }
    };
    with("--title", request.title);
    with("--accept-label", request.accept_label);
    for (const Filter& filter : request.filters) {
        const fs::FileFilter ours = file_filter(filter);
        arguments.emplace_back("--filter");
        arguments.push_back(ours.label);
        for (const std::string& glob : ours.globs) {
            with("--glob", glob);
        }
        for (const std::string& type : ours.types) {
            with("--mime", type);
        }
    }
    if (request.filter) {
        with("--current-filter", std::to_string(*request.filter));
    }
    with("--parent", request.parent_window);
    if (request.save) {
        arguments.emplace_back("--save");
        with("--name", request.name);
        for (const std::string& file : request.files) {
            arguments.emplace_back("--file");
            arguments.push_back(file);
        }
    }
    for (const Choice& choice : request.choices) {
        arguments.insert(arguments.end(),
                         {"--choice", choice.get<0>(), choice.get<1>(), choice.get<3>()});
        for (const ChoiceOption& option : choice.get<2>()) {
            arguments.insert(arguments.end(), {"--option", option.get<0>(), option.get<1>()});
        }
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
        } else if (line.starts_with("filter:")) {
            std::size_t index = 0;
            const std::string_view number = line.substr(7);
            const auto [last, error] =
                std::from_chars(number.data(), number.data() + number.size(), index);
            if (error == std::errc{} && last == number.data() + number.size()) {
                result.filter = index;
            }
        } else if (line.starts_with("choice:") && line.find('=') != std::string_view::npos) {
            const std::string_view pair = line.substr(7);
            const std::size_t equals = pair.find('=');
            result.choices.emplace_back(pair.substr(0, equals), pair.substr(equals + 1));
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
    result.filter.reset();
    result.choices.clear();
    const bool closed = WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGTERM;
    result.response = (exited && code == 0) || closed ? kResponseCancelled : kResponseOther;
    return result;
}

// ---------------------------------------------------------------------------------------------
// Picker

Picker::Picker(core::EventLoop& loop, Done done)
    : done_{std::move(done)}, program_{loop, [this](const std::string& output, int wait_status) {
                                           ended(output, wait_status);
                                       }} {}

void Picker::ended(const std::string& output, int wait_status) {
    // done may end this picker: nothing of it is touched after.
    const Done chosen = std::move(done_);
    chosen(pick_result(output, wait_status));
}

Picker::~Picker() = default;

bool Picker::start(const std::filesystem::path& program,
                   const std::vector<std::string>& arguments) {
    return program_.start(program, arguments);
}

void Picker::stop() { program_.stop(); }

// ---------------------------------------------------------------------------------------------
// FileChooser

using Reply = sdbus::Result<std::uint32_t, Options>;

struct FileChooser::Impl {
    struct Pending {
        Reply reply;
        std::unique_ptr<sdbus::IObject> request;  // Request.Close at the handle
        std::unique_ptr<Picker> picker;
        std::vector<Filter> filters;       // as asked: current_filter is answered with one of them
        std::vector<std::string> choices;  // their ids: only those are answered
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

    void start(Reply reply, const sdbus::ObjectPath& handle, const std::string& app_id,
               const char* method, const PickRequest& request) {
        log::info("portal: {} for \"{}\"{}{}, {} filter(s), {} choice(s){}", method,
                  app_id.empty() ? "?" : app_id, request.multiple ? ", several" : "",
                  request.directory ? ", folders" : "", request.filters.size(),
                  request.choices.size(), request.parent_window.empty() ? "" : ", a parent");
        if (pending.contains(handle)) {
            reply.returnResults(kResponseOther, Options{});
            return;
        }
        busy();
        auto entry = std::make_unique<Pending>();
        entry->reply = std::move(reply);
        entry->filters = request.filters;
        for (const Choice& choice : request.choices) {
            entry->choices.push_back(choice.get<0>());
        }
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
            if (result.filter && *result.filter < done->filters.size()) {
                results.emplace("current_filter", sdbus::Variant{done->filters.at(*result.filter)});
            }
            std::vector<ChoiceOption> choices;
            for (const auto& [id, value] : result.choices) {
                if (std::ranges::find(done->choices, id) != done->choices.end()) {
                    choices.emplace_back(id, value);
                }
            }
            if (!choices.empty()) {
                results.emplace("choices", sdbus::Variant{choices});
            }
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
                                       const std::string& app_id, std::string parent_window,
                                       std::string title, const Options& options) {
                    impl.start(
                        std::move(reply), handle, app_id, "OpenFile",
                        open_file_request(std::move(title), std::move(parent_window), options));
                }),
            sdbus::registerMethod("SaveFile")
                .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
                .withOutputParamNames("response", "results")
                .implementedAs([&impl](Reply&& reply, const sdbus::ObjectPath& handle,
                                       const std::string& app_id, std::string parent_window,
                                       std::string title, const Options& options) {
                    impl.start(
                        std::move(reply), handle, app_id, "SaveFile",
                        save_file_request(std::move(title), std::move(parent_window), options));
                }),
            sdbus::registerMethod("SaveFiles")
                .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
                .withOutputParamNames("response", "results")
                .implementedAs([&impl](Reply&& reply, const sdbus::ObjectPath& handle,
                                       const std::string& app_id, std::string parent_window,
                                       std::string title, const Options& options) {
                    impl.start(
                        std::move(reply), handle, app_id, "SaveFiles",
                        save_files_request(std::move(title), std::move(parent_window), options));
                }))
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
