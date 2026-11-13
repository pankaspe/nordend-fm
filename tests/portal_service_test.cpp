// The file chooser service (ROADMAP 4a): OpenFile's options, the picker's command line and
// output, and the whole round on a private bus with a made-up picker in place of nordend --pick.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/platform/bus_loop.hpp"
#include "nordend/portal/file_chooser.hpp"
#include "private_bus.hpp"
#include "temp_dir.hpp"

namespace portal = nordend::portal;
using nordend::core::EventLoop;
using nordend::platform::BusLoop;
using nordend::test::PrivateBus;
using nordend::test::TempDir;
using namespace std::chrono_literals;

namespace {

int exited(int code) { return code << 8; }  // as waitpid() puts an exit status

std::vector<std::uint8_t> bytes(const std::string& text) {
    std::vector<std::uint8_t> out(text.begin(), text.end());
    out.push_back(0);
    return out;
}

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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// A service on a private bus and a client calling it, both on one loop.
struct Round {
    TempDir dir;
    std::filesystem::path socket{dir.path / "bus"};
    PrivateBus daemon{socket};
    EventLoop loop;
    std::unique_ptr<sdbus::IConnection> service;
    std::unique_ptr<BusLoop> service_bus;
    std::unique_ptr<portal::FileChooser> chooser;
    std::unique_ptr<sdbus::IConnection> client;
    std::unique_ptr<BusLoop> client_bus;
    std::unique_ptr<sdbus::IProxy> proxy;

    // The made-up picker: a shell script with body, its arguments written to "args".
    void start(const std::string& body) {
        const std::filesystem::path program = dir.path / "picker";
        {
            std::ofstream out{program};
            out << "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" << (dir.path / "args").string() << "'\n"
                << body << "\n";
        }
        std::filesystem::permissions(program, std::filesystem::perms::owner_all);
        start_with(program);
    }

    void start_with(const std::filesystem::path& program) {
        const std::string address = "unix:path=" + socket.string();
        service = sdbus::createSessionBusConnectionWithAddress(address);
        service_bus = std::make_unique<BusLoop>(loop, *service, "service");
        chooser = std::make_unique<portal::FileChooser>(loop, *service, *service_bus, program);
        service->requestName(sdbus::ServiceName{std::string{portal::kBusName}});
        service_bus->pump();
        client = sdbus::createSessionBusConnectionWithAddress(address);
        client_bus = std::make_unique<BusLoop>(loop, *client, "client");
        proxy = sdbus::createProxy(*client, sdbus::ServiceName{std::string{portal::kBusName}},
                                   sdbus::ObjectPath{std::string{portal::kObjectPath}});
    }

    struct Answer {
        bool arrived{false};
        std::optional<std::string> error;
        std::uint32_t response{99};
        portal::Options results;
    };

    std::shared_ptr<Answer> call(const char* method, const std::string& handle,
                                 const portal::Options& options) const {
        auto answer = std::make_shared<Answer>();
        proxy->callMethodAsync(method)
            .onInterface(std::string{portal::kFileChooser})
            .withArguments(sdbus::ObjectPath{handle}, std::string{"org.example.App"}, std::string{},
                           std::string{"Open a Photo"}, options)
            .uponReplyInvoke([answer](std::optional<sdbus::Error> error, std::uint32_t response,
                                      portal::Options results) {
                answer->arrived = true;
                if (error) {
                    answer->error = error->getMessage();
                    return;
                }
                answer->response = response;
                answer->results = std::move(results);
            });
        client_bus->pump();
        return answer;
    }

    void close(const std::string& handle) const {
        const std::unique_ptr<sdbus::IProxy> request = sdbus::createProxy(
            *client, sdbus::ServiceName{std::string{portal::kBusName}}, sdbus::ObjectPath{handle});
        request->callMethodAsync("Close")
            .onInterface(std::string{portal::kRequest})
            // By value: sdbus-c++ 2.2 (Nix) tells the error from a reply value by its type.
            // NOLINTNEXTLINE(performance-unnecessary-value-param)
            .uponReplyInvoke([](std::optional<sdbus::Error> /*error*/) {});
        client_bus->pump();
    }

    [[nodiscard]] std::string arguments() const { return read_file(dir.path / "args"); }
};

std::vector<std::string> uris_of(const portal::Options& results) {
    const auto it = results.find("uris");
    return it == results.end() ? std::vector<std::string>{}
                               : it->second.get<std::vector<std::string>>();
}

constexpr const char* kHandle = "/org/freedesktop/portal/desktop/request/1_42/t1";

}  // namespace

TEST_CASE("portal: OpenFile's options as the picker follows them") {
    portal::Options options;
    options.emplace("multiple", sdbus::Variant{true});
    options.emplace("current_folder", sdbus::Variant{bytes("/home/me/Pictures")});
    options.emplace("accept_label", sdbus::Variant{std::string{"_Open"}});
    const portal::Filter images{"Images", {{0U, "*.jpg"}, {1U, "image/*"}, {7U, "odd"}}};
    const portal::Filter text{"Text", {{1U, "text/plain"}}};
    options.emplace("filters", sdbus::Variant{std::vector<portal::Filter>{images, text}});
    options.emplace("current_filter", sdbus::Variant{text});
    const portal::PickRequest request =
        portal::open_file_request("Open a Photo", "wayland:abc", options);
    CHECK(request.title == "Open a Photo");
    CHECK(request.multiple);
    CHECK_FALSE(request.directory);
    CHECK(request.folder == "/home/me/Pictures");
    CHECK(request.parent_window == "wayland:abc");
    CHECK(request.accept_label == "_Open");
    REQUIRE(request.filters.size() == 2);
    CHECK(request.filter == std::optional<std::size_t>{1});
    const nordend::fs::FileFilter ours = portal::file_filter(images);
    CHECK(ours.label == "Images");
    CHECK(ours.globs == std::vector<std::string>{"*.jpg"});
    CHECK(ours.types == std::vector<std::string>{"image/*"});  // rule 7 is no kind we know
    const std::vector<std::string> expected{
        "--pick",           "--multiple", "--title",  "Open a Photo",
        "--accept-label",   "_Open",      "--filter", "Images",
        "--glob",           "*.jpg",      "--mime",   "image/*",
        "--filter",         "Text",       "--mime",   "text/plain",
        "--current-filter", "1",          "--parent", "wayland:abc",
        "/home/me/Pictures"};
    CHECK(portal::picker_arguments(request) == expected);

    SUBCASE("nothing asked: one file, the picker's own folder and title") {
        const portal::PickRequest plain = portal::open_file_request("", "", {});
        CHECK_FALSE(plain.multiple);
        CHECK(plain.folder.empty());
        CHECK(portal::picker_arguments(plain) == std::vector<std::string>{"--pick"});
    }
    SUBCASE("options of another type, and folders that are not absolute paths, are left out") {
        portal::Options odd;
        odd.emplace("multiple", sdbus::Variant{std::string{"yes"}});
        odd.emplace("directory", sdbus::Variant{std::uint32_t{1}});
        odd.emplace("current_folder", sdbus::Variant{bytes("relative/folder")});
        const portal::PickRequest left = portal::open_file_request("", "x11:42", odd);
        CHECK(left.parent_window.empty());  // X11 windows: not ours to follow
        CHECK_FALSE(left.multiple);
        CHECK_FALSE(left.directory);
        CHECK(left.folder.empty());
        odd.erase("current_folder");
        odd.emplace("current_folder", sdbus::Variant{std::string{"/as/a/string"}});
        CHECK(portal::open_file_request("", "x11:42", odd).folder.empty());
    }
    SUBCASE("folders") {
        portal::Options folders;
        folders.emplace("directory", sdbus::Variant{true});
        const portal::PickRequest folder = portal::open_file_request("", "", folders);
        CHECK(folder.directory);
        CHECK(portal::picker_arguments(folder) == std::vector<std::string>{"--pick", "--folders"});
    }
    SUBCASE("a current filter without a list is the only one; one not in the list is left out") {
        portal::Options alone;
        alone.emplace("current_filter", sdbus::Variant{text});
        const portal::PickRequest one = portal::open_file_request("", "", alone);
        REQUIRE(one.filters.size() == 1);
        CHECK(one.filter == std::optional<std::size_t>{0});
        portal::Options other;
        other.emplace("filters", sdbus::Variant{std::vector<portal::Filter>{images}});
        other.emplace("current_filter", sdbus::Variant{text});
        const portal::PickRequest kept = portal::open_file_request("", "", other);
        CHECK(kept.filters.size() == 1);
        CHECK_FALSE(kept.filter);
    }
}

TEST_CASE("portal: SaveFile's and SaveFiles's options") {
    portal::Options options;
    options.emplace("current_file", sdbus::Variant{bytes("/home/me/notes/today.txt")});
    portal::PickRequest request = portal::save_file_request("Save", "", options);
    CHECK(request.save);
    CHECK(request.name == "today.txt");  // the file being saved again, in its folder
    CHECK(request.folder == "/home/me/notes");
    options.emplace("current_name", sdbus::Variant{std::string{"copy.txt"}});
    options.emplace("current_folder", sdbus::Variant{bytes("/tmp")});
    request = portal::save_file_request("Save", "", options);
    CHECK(request.name == "copy.txt");
    CHECK(request.folder == "/tmp");
    // A name with a path in it keeps its last part.
    portal::Options odd;
    odd.emplace("current_name", sdbus::Variant{std::string{"../../etc/passwd"}});
    CHECK(portal::save_file_request("", "", odd).name == "passwd");

    portal::Options several;
    several.emplace("files", sdbus::Variant{std::vector<std::vector<std::uint8_t>>{
                                 bytes("a.jpg"), bytes("/x/b.jpg"), bytes(".."), bytes("")}});
    const portal::PickRequest files = portal::save_files_request("", "", several);
    CHECK(files.save);
    CHECK(files.files == std::vector<std::string>{"a.jpg", "b.jpg"});
}

TEST_CASE("portal: the picker's output and status make the answer") {
    const std::string output = "a stray line\nfile:///home/me/a%20b.jpg\nfile:///home/me/c.ORF\n";
    portal::PickResult chosen = portal::pick_result(output, exited(0));
    CHECK(chosen.response == portal::kResponseOk);
    CHECK(chosen.uris ==
          std::vector<std::string>{"file:///home/me/a%20b.jpg", "file:///home/me/c.ORF"});
    CHECK(portal::pick_result(output + "filter:1\n", exited(0)).filter ==
          std::optional<std::size_t>{1});
    CHECK_FALSE(portal::pick_result(output + "filter:x\n", exited(0)).filter);
    CHECK_FALSE(portal::pick_result("filter:1\n", exited(0)).filter);  // a cancel has none
    CHECK(portal::pick_result(output + "choice:enc=a=b\nchoice:bad\n", exited(0)).choices ==
          std::vector<std::pair<std::string, std::string>>{{"enc", "a=b"}});
    // The last line without its newline still counts.
    CHECK(portal::pick_result("file:///x", exited(0)).uris ==
          std::vector<std::string>{"file:///x"});

    CHECK(portal::pick_result("", exited(0)).response == portal::kResponseCancelled);
    // 1 is nordend's error (no display, say), not a cancel.
    CHECK(portal::pick_result("", exited(1)).response == portal::kResponseOther);
    CHECK(portal::pick_result("", SIGTERM).response == portal::kResponseCancelled);
    // URIs from a picker that did not end well are not trusted.
    const portal::PickResult crashed = portal::pick_result(output, SIGSEGV);
    CHECK(crashed.response == portal::kResponseOther);
    CHECK(crashed.uris.empty());
    CHECK(portal::pick_result(output, exited(2)).response == portal::kResponseOther);
    CHECK(portal::pick_result(output, exited(1)).response == portal::kResponseOther);
}

TEST_CASE("portal: OpenFile on the bus runs the picker and answers with its choice") {
    Round round;
    if (!round.daemon.running()) {
        MESSAGE("no dbus-daemon: skipped");
        return;
    }
    SUBCASE("a choice") {
        round.start("printf 'file:///tmp/a%%20b.jpg\\nfile:///tmp/c.txt\\n'");
        portal::Options options;
        options.emplace("multiple", sdbus::Variant{true});
        options.emplace("current_folder", sdbus::Variant{bytes("/tmp")});
        bool idle = false;
        round.chooser->set_idle_handler(50ms, [&idle] { idle = true; });
        const auto answer = round.call("OpenFile", kHandle, options);
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK_FALSE(answer->error);
        CHECK(answer->response == portal::kResponseOk);
        CHECK(uris_of(answer->results) ==
              std::vector<std::string>{"file:///tmp/a%20b.jpg", "file:///tmp/c.txt"});
        CHECK(round.arguments() == "--pick\n--multiple\n--title\nOpen a Photo\n/tmp\n");
        CHECK(round.chooser->pending() == 0);
        // Nothing more asked: the service may go.
        CHECK(run_until(round.loop, [&] { return idle; }));
    }
    SUBCASE("Escape in the picker: nothing printed") {
        round.start("exit 0");
        const auto answer = round.call("OpenFile", kHandle, {});
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(answer->response == portal::kResponseCancelled);
        CHECK(answer->results.empty());
    }
    SUBCASE("a picker that crashes") {
        round.start("printf 'file:///tmp/half\\n'; kill -SEGV $$");
        const auto answer = round.call("OpenFile", kHandle, {});
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(answer->response == portal::kResponseOther);
        CHECK(answer->results.empty());
    }
    SUBCASE("no picker to start") {
        round.start_with(round.dir.path / "missing");
        const auto answer = round.call("OpenFile", kHandle, {});
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(answer->response == portal::kResponseOther);
        CHECK(round.chooser->pending() == 0);
    }
    SUBCASE("the application closes the request: the picker is closed, the answer a cancel") {
        round.start("exec sleep 30");
        const auto answer = round.call("OpenFile", kHandle, {});
        REQUIRE(run_until(round.loop, [&] {
            return round.chooser->pending() == 1 &&
                   std::filesystem::exists(round.dir.path / "args");
        }));
        CHECK_FALSE(answer->arrived);
        round.close(kHandle);
        const auto before = std::chrono::steady_clock::now();
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(std::chrono::steady_clock::now() - before < 2s);  // SIGTERM, not the kill later
        CHECK(answer->response == portal::kResponseCancelled);
        CHECK(round.chooser->pending() == 0);
    }
    SUBCASE("the filter in use comes back as current_filter") {
        round.start("printf 'file:///tmp/notes.txt\\nfilter:1\\n'");
        const portal::Filter images{"Images", {{1U, "image/*"}}};
        const portal::Filter text{"Text", {{1U, "text/plain"}}};
        portal::Options options;
        options.emplace("filters", sdbus::Variant{std::vector<portal::Filter>{images, text}});
        const auto answer = round.call("OpenFile", kHandle, options);
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(answer->response == portal::kResponseOk);
        const auto it = answer->results.find("current_filter");
        REQUIRE(it != answer->results.end());
        CHECK(it->second.get<portal::Filter>() == text);
    }
    SUBCASE("folders go to the picker") {
        round.start("printf 'file:///tmp\\n'");
        portal::Options folders;
        folders.emplace("directory", sdbus::Variant{true});
        const auto folder = round.call("OpenFile", kHandle, folders);
        REQUIRE(run_until(round.loop, [&] { return folder->arrived; }));
        CHECK(folder->response == portal::kResponseOk);
        CHECK(round.arguments() == "--pick\n--folders\n--title\nOpen a Photo\n");
    }
    SUBCASE("SaveFile: the name proposed, the choices answered as the picker left them") {
        round.start(R"(printf 'file:///tmp/sea.txt\nchoice:enc=latin1\nchoice:stranger=1\n')");
        portal::Options options;
        options.emplace("current_name", sdbus::Variant{std::string{"notes.txt"}});
        options.emplace(
            "choices",
            sdbus::Variant{std::vector<portal::Choice>{
                {"enc", "Encoding", {{"utf8", "UTF-8"}, {"latin1", "Latin-1"}}, "utf8"}}});
        const auto answer = round.call("SaveFile", kHandle, options);
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(answer->response == portal::kResponseOk);
        CHECK(uris_of(answer->results) == std::vector<std::string>{"file:///tmp/sea.txt"});
        const auto it = answer->results.find("choices");
        REQUIRE(it != answer->results.end());
        // Only the application's own ids come back.
        CHECK(it->second.get<std::vector<portal::ChoiceOption>>() ==
              std::vector<portal::ChoiceOption>{{"enc", "latin1"}});
        CHECK(round.arguments() ==
              "--pick\n--title\nOpen a Photo\n--save\n--name\nnotes.txt\n--choice\nenc\n"
              "Encoding\nutf8\n--option\nutf8\nUTF-8\n--option\nlatin1\nLatin-1\n");
    }
    SUBCASE("SaveFiles: the names, saved into the folder chosen") {
        round.start("printf 'file:///tmp/a.jpg\\nfile:///tmp/b.jpg\\n'");
        portal::Options options;
        options.emplace("files", sdbus::Variant{std::vector<std::vector<std::uint8_t>>{
                                     bytes("a.jpg"), bytes("/elsewhere/b.jpg")}});
        const auto answer = round.call("SaveFiles", kHandle, options);
        REQUIRE(run_until(round.loop, [&] { return answer->arrived; }));
        CHECK(answer->response == portal::kResponseOk);
        CHECK(uris_of(answer->results).size() == 2);
        CHECK(round.arguments() ==
              "--pick\n--title\nOpen a Photo\n--save\n--file\na.jpg\n--file\nb.jpg\n");
    }
}
