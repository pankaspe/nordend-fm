// The file chooser's side in Nordend (ROADMAP 4b): filters (fs::FileFilterMatch), what a listing
// leaves out for them (Listing::set_only), and the Browser choosing files and folders.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/entry.hpp"
#include "nordend/fs/file_filter.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/fs/sort.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/folder.hpp"
#include "nordend/views/pick_bar.hpp"
#include "nordend/views/status_bar.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::core::WorkerPool;
using nordend::fs::DirectoryWatcher;
using nordend::fs::Entry;
using nordend::fs::EntryKind;
using nordend::fs::FileFilter;
using nordend::fs::FileFilterMatch;
using nordend::fs::MimeDatabase;
using nordend::test::TempDir;
using nordend::views::Browser;
using nordend::views::Folder;
using namespace std::chrono_literals;
namespace stdfs = std::filesystem;

namespace {

const std::shared_ptr<const MimeDatabase>& mime() {
    static const std::shared_ptr<const MimeDatabase> kDatabase = MimeDatabase::load();
    return kDatabase;
}

// An entry as the loader makes it: the type from its name when the name decides it.
Entry file(std::string name) {
    Entry entry;
    entry.name = std::move(name);
    entry.kind = EntryKind::file;
    const MimeDatabase::NameMatch match = mime()->by_name(entry.name);
    (match.certain ? entry.mime : entry.name_guess) = match.mime;
    return entry;
}

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

std::vector<std::string> names(const Folder& folder) {
    std::vector<std::string> out;
    for (const Entry& entry : folder.entries()) {
        out.push_back(entry.name);
    }
    return out;
}

struct Fixture {
    TempDir dir;
    stdfs::path root = dir.path / "root";
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, mime(), [] {}, [](nordend::ui::CursorShape /*shape*/) {}};
    std::vector<std::vector<stdfs::path>> chosen;
    std::vector<std::optional<std::size_t>> filters;
    int cancelled = 0;

    Fixture() {
        stdfs::create_directories(root / "photos");
        for (const char* name : {"a.jpg", "B.JPG", "notes.txt", "main.cpp", "data-0001"}) {
            std::ofstream{root / name} << "x";
        }
    }

    void pick(Browser::Pick options) {
        browser.set_pick(
            std::move(options),
            [this](std::vector<stdfs::path> paths, std::optional<std::size_t> filter) {
                chosen.push_back(std::move(paths));
                filters.push_back(filter);
            },
            [this] { ++cancelled; });
        browser.columns().open(root);
        REQUIRE(run_until(loop, [this] { return !browser.loading(); }));
    }

    [[nodiscard]] std::shared_ptr<Folder> shown() { return browser.columns().focused_folder(); }

    void press(xkb_keysym_t key, nordend::ui::Modifiers modifiers = {}) {
        nordend::ui::KeyEvent event;
        event.keysym = key;
        event.modifiers = modifiers;
        browser.key(event, Browser::Clock::now());
    }

    // The cursor onto name, alone.
    void cursor_on(const std::string& name) {
        shown()->select_name(name);
        REQUIRE(shown()->cursor_entry() != nullptr);
    }
};

const FileFilter kImages{.label = "Images", .globs = {"*.jpg"}, .types = {}};
const FileFilter kText{.label = "Text", .globs = {}, .types = {"text/plain"}};

}  // namespace

TEST_CASE("pick filter: globs without case, types with their subtypes, by the name alone") {
    const FileFilterMatch jpg{kImages, mime()};
    CHECK(jpg.passes(file("a.jpg")));
    CHECK(jpg.passes(file("B.JPG")));  // as in GTK 4
    CHECK_FALSE(jpg.passes(file("a.jpeg")));

    const FileFilterMatch text{kText, mime()};
    CHECK(text.passes(file("notes.txt")));
    CHECK(text.passes(file("main.cpp")));  // text/x-c++src is a text/plain
    CHECK_FALSE(text.passes(file("a.jpg")));
    CHECK(text.passes(file("README")));           // README* is text/x-readme, a text/plain
    CHECK_FALSE(text.passes(file("data-0001")));  // nothing in the name: octet-stream

    const FileFilterMatch images{{.label = "Images", .globs = {}, .types = {"image/*"}}, mime()};
    CHECK(images.passes(file("a.jpg")));
    CHECK(images.passes(file("P1010001.ORF")));  // image/x-olympus-orf
    CHECK_FALSE(images.passes(file("notes.txt")));

    const FileFilterMatch all{{.label = "All", .globs = {"*"}, .types = {}}, mime()};
    CHECK(all.passes(file("data-0001")));
    const FileFilterMatch every_type{{.label = "All", .globs = {}, .types = {"*/*"}}, mime()};
    CHECK(every_type.passes(file("data-0001")));
    // A type the database does not know matches nothing, without failing.
    const FileFilterMatch unknown{{.label = "?", .globs = {}, .types = {"x-no/such"}}, mime()};
    CHECK_FALSE(unknown.passes(file("a.jpg")));
}

TEST_CASE("listing: only what a filter passes, the rest kept aside and back when it changes") {
    nordend::fs::Listing listing;
    listing.add({file("a.jpg"), file("notes.txt"), file(".hidden.jpg")});
    auto moved = listing.set_only([](const Entry& entry) { return entry.name.ends_with(".jpg"); });
    CHECK(moved.hidden == std::vector<std::string>{"notes.txt"});
    REQUIRE(listing.entries().size() == 2);
    // Hidden files keep their own rule on top.
    listing.set_filter(false, {});
    REQUIRE(listing.entries().size() == 1);
    CHECK(listing.entries()[0].name == "a.jpg");
    // A new entry follows the filter as it arrives.
    listing.add({file("b.txt"), file("c.jpg")});
    CHECK(listing.entries().size() == 2);
    moved = listing.set_only({});
    std::ranges::sort(moved.shown);  // kept aside in no order
    CHECK(moved.shown == std::vector<std::string>{"b.txt", "notes.txt"});
    CHECK(listing.entries().size() == 4);
}

TEST_CASE("pick: the filter decides the files listed; folders always show") {
    Fixture f;
    f.pick({.multiple = false,
            .folders = false,
            .accept_label = "_Open",
            .filters = {kImages, kText},
            .filter = 0});
    CHECK(f.browser.pick_bar().accept_label() == "Open");
    CHECK(f.browser.pick_bar().filter_label() == "Images");
    CHECK(names(*f.shown()) == std::vector<std::string>{"photos", "a.jpg", "B.JPG"});
    CHECK(nordend::views::status_of(*f.shown()) == "3 items, 3 filtered out");

    f.browser.set_pick_filter(1);
    CHECK(f.browser.pick_bar().filter_label() == "Text");
    CHECK(names(*f.shown()) == std::vector<std::string>{"photos", "main.cpp", "notes.txt"});

    // Enter on a file chooses it, with the filter in use.
    f.cursor_on("notes.txt");
    f.press(XKB_KEY_Return);
    REQUIRE(f.chosen.size() == 1);
    CHECK(f.chosen[0] == std::vector<stdfs::path>{f.root / "notes.txt"});
    CHECK(f.filters[0] == std::optional<std::size_t>{1});

    // Two selected without --multiple: a word in the status bar, nothing chosen.
    f.press(XKB_KEY_Up, {.shift = true});
    f.press(XKB_KEY_Return);
    CHECK(f.chosen.size() == 1);
    CHECK(f.browser.status_bar().message() == "Choose one file");

    // Ctrl+Enter on a folder goes into it, as Enter does.
    f.cursor_on("photos");
    f.press(XKB_KEY_Return, {.ctrl = true});
    REQUIRE(run_until(f.loop, [&f] { return !f.browser.loading(); }));
    CHECK(f.chosen.size() == 1);
    CHECK(f.shown()->path() == f.root / "photos");

    f.press(XKB_KEY_Escape);
    CHECK(f.cancelled == 1);
}

TEST_CASE("pick: several files with --multiple; no filter, every file and no filter line") {
    Fixture f;
    f.pick({.multiple = true, .folders = false, .accept_label = {}, .filters = {}, .filter = 0});
    CHECK(f.browser.pick_bar().filter_label().empty());
    CHECK(names(*f.shown()).size() == 6);
    f.cursor_on("a.jpg");
    f.press(XKB_KEY_Down, {.shift = true});  // a.jpg, B.JPG
    f.browser.accept_pick();
    REQUIRE(f.chosen.size() == 1);
    CHECK(f.chosen[0] == std::vector<stdfs::path>{f.root / "a.jpg", f.root / "B.JPG"});
    CHECK_FALSE(f.filters[0].has_value());
}

TEST_CASE("pick: folders, the one under the cursor or else the one shown") {
    Fixture f;
    f.pick({.multiple = false,
            .folders = true,
            .accept_label = {},
            .filters = {kImages},
            .filter = 0});
    CHECK(f.browser.pick_bar().accept_label() == "Select");
    CHECK(f.browser.pick_bar().filter_label().empty());  // filters are for files
    CHECK(names(*f.shown()) == std::vector<std::string>{"photos"});

    f.cursor_on("photos");
    f.press(XKB_KEY_Return, {.ctrl = true});
    REQUIRE(f.chosen.size() == 1);
    CHECK(f.chosen[0] == std::vector<stdfs::path>{f.root / "photos"});

    // Enter opens it as always; inside, nothing under the cursor: the folder shown.
    f.press(XKB_KEY_Return);
    REQUIRE(run_until(f.loop, [&f] { return !f.browser.loading(); }));
    REQUIRE(f.shown()->path() == f.root / "photos");
    CHECK(f.shown()->cursor_entry() == nullptr);
    f.browser.accept_pick();
    REQUIRE(f.chosen.size() == 2);
    CHECK(f.chosen[1] == std::vector<stdfs::path>{f.root / "photos"});
}

TEST_CASE("pick bar: the application's word without its mnemonic") {
    CHECK(nordend::views::without_mnemonic("_Open") == "Open");
    CHECK(nordend::views::without_mnemonic("Save __as") == "Save _as");
    CHECK(nordend::views::without_mnemonic("") == "");
}
