// The path typed in the header (Ctrl+L): what a text names, Tab's completion, the red state, the
// field's place in the browser.
#include "nordend/views/path_entry.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/fs/trash.hpp"
#include "nordend/theme/palette.hpp"
#include "nordend/ui/clipboard.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/offscreen.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/file_clipboard.hpp"
#include "nordend/views/folder.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::test::TempDir;
using nordend::ui::KeyEvent;
using nordend::ui::Modifiers;
using nordend::ui::PointerButton;
using nordend::ui::PointerEvent;
using nordend::views::Browser;
using nordend::views::complete;
using nordend::views::Completion;
using nordend::views::Folder;
using nordend::views::PathEntry;
using nordend::views::split_typed;
using nordend::views::typed_path;
using Kind = PointerEvent::Kind;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr float kWidth = 1000.0F;
constexpr float kHeight = 700.0F;
constexpr float kReserve = 120.0F;  // the window's buttons, kept free in the header
constexpr float kRow = 20.0F;       // the path row, as the header's middle

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

KeyEvent chord(xkb_keysym_t keysym, Modifiers modifiers = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

// One character, typed; more than one goes in as one event, too.
KeyEvent typed(std::string_view text) {
    KeyEvent event;
    event.text = std::string{text};
    return event;
}

std::string listing_of(const fs::path& folder) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator{folder}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    std::string out;
    for (const std::string& name : names) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out;
}

PathEntry::Listing listing(PathEntry::Listing listing) {
    std::ranges::sort(listing.folders);
    std::ranges::sort(listing.files);
    return listing;
}

// A PathEntry wired to a fake listing: what it would ask a worker is answered by hand, in the
// order it was asked.
struct Fake {
    std::vector<fs::path> asked;
    std::deque<std::function<void(PathEntry::Listing)>> pending;
    std::vector<std::pair<fs::path, std::string>> gone;
    std::vector<std::string> messages;
    PathEntry entry;

    Fake()
        : entry{[] {},
                [this](const fs::path& folder, std::function<void(PathEntry::Listing)> done) {
                    asked.push_back(folder);
                    pending.push_back(std::move(done));
                    return nordend::core::CancelToken{};
                },
                [this](const fs::path& path, const std::string& file) {
                    gone.emplace_back(path, file);
                },
                [this](std::string problem) { messages.push_back(std::move(problem)); },
                "/home/me"} {}

    // The oldest request answers, as the worker would, on the loop thread.
    void arrives(PathEntry::Listing answer) {
        REQUIRE(!pending.empty());
        std::function<void(PathEntry::Listing)> done = std::move(pending.front());
        pending.pop_front();
        done(std::move(answer));
    }

    bool key(const KeyEvent& event) { return entry.key(event); }

    void type(std::string_view text) {
        for (const char c : text) {
            REQUIRE(entry.key(typed(std::string_view{&c, 1})));
        }
    }
};

// root/{Documents/, Downloads/, a.txt}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(root / "Documents");
        fs::create_directories(root / "Downloads");
        std::ofstream{root / "a.txt"} << "a";
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    void type(std::string_view text) {
        for (const char c : text) {
            REQUIRE(press(typed(std::string_view{&c, 1})));
        }
    }

    std::shared_ptr<Folder> shown() {
        switch (browser.mode()) {
            case Browser::Mode::grid:
                return browser.grid().folder();
            case Browser::Mode::details:
                return browser.details().folder();
            case Browser::Mode::columns:
                break;
        }
        return browser.columns().focused_folder();
    }

    // The folder shown, loaded, with the keyboard free.
    bool show(const fs::path& folder, Browser::Mode mode = Browser::Mode::columns) {
        browser.open(folder);
        browser.set_mode(mode);
        REQUIRE(settle());
        return shown() != nullptr && shown()->path() == folder;
    }
};

std::unique_ptr<nordend::ui::Offscreen> offscreen() {
    try {
        return std::make_unique<nordend::ui::Offscreen>(static_cast<std::int32_t>(kWidth),
                                                        static_cast<std::int32_t>(kHeight));
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

// What the app's Wayland side would do, recorded.
struct FakeDnd {
    std::optional<std::string> accepted;  // the type taken where the pointer is
    std::vector<std::string> types;
    std::string data;  // what a read of another program's drop gives
};

// The same fixture drawn offscreen, so the pointer finds what is shown.
struct DrawnFixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    fs::path alpha = root / "alpha";
    std::unique_ptr<nordend::ui::Offscreen> gl = offscreen();
    std::unique_ptr<nordend::ui::FontSystem> fonts;
    std::unique_ptr<nordend::ui::Renderer> renderer;
    std::unique_ptr<nordend::ui::IconSet> icons;
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};
    FakeDnd dnd;

    DrawnFixture() {
        fs::create_directories(alpha);
        if (gl) {
            fonts = std::make_unique<nordend::ui::FontSystem>();
            renderer = std::make_unique<nordend::ui::Renderer>(*fonts);
            icons = std::make_unique<nordend::ui::IconSet>(*fonts);
        }
        browser.set_places({{.section = nordend::fs::Place::Section::places,
                             .label = "Root",
                             .path = root,
                             .icon = "folder",
                             .disk = {}},
                            {.section = nordend::fs::Place::Section::places,
                             .label = "Alpha",
                             .path = alpha,
                             .icon = "folder",
                             .disk = {}}});
        browser.set_drag_and_drop({
            .start = {},
            .cancel = {},
            .types = [this] { return dnd.types; },
            .source_actions = [] { return nordend::ui::kDndCopy; },
            .action = [] { return nordend::ui::kDndCopy; },
            .accept = [this](std::optional<std::string> type, std::uint32_t /*actions*/,
                             std::uint32_t /*preferred*/) { dnd.accepted = std::move(type); },
            .read =
                [this](const std::string& /*type*/, nordend::views::DragAndDrop::Done done) {
                    loop.post([this, done = std::move(done)] { done(dnd.data); });
                },
            .finish = {},
            .modifiers = [] { return Modifiers{}; },
        });
    }

    void draw() {
        if (!gl) {
            return;
        }
        renderer->begin_frame(static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight),
                              1.0F, {});
        browser.draw(*renderer, {0.0F, 0.0F, kWidth, kHeight}, nordend::theme::Palette{}, *icons,
                     Browser::Clock::now());
        renderer->end_frame();
    }

    bool settle() {
        const bool done = run_until(loop, [this] { return !browser.loading(); });
        draw();
        return done;
    }

    bool show(const fs::path& folder) {
        browser.open(folder);
        REQUIRE(settle());
        return shown() != nullptr && shown()->path() == folder;
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    std::shared_ptr<Folder> shown() {
        switch (browser.mode()) {
            case Browser::Mode::grid:
                return browser.grid().folder();
            case Browser::Mode::details:
                return browser.details().folder();
            case Browser::Mode::columns:
                break;
        }
        return browser.columns().focused_folder();
    }

    void click(float x, float y) {
        for (const Kind kind : {Kind::motion, Kind::press, Kind::release}) {
            browser.pointer(
                {.kind = kind, .x = x, .y = y, .button = PointerButton::left, .click_count = 1},
                Browser::Clock::now());
        }
        draw();
    }
};

}  // namespace

TEST_CASE("path entry: what a typed text names") {
    const fs::path home{"/home/me"};
    CHECK(typed_path("/a/b", "/x", home) == fs::path{"/a/b"});
    CHECK(typed_path("/a/b/", "/x", home) == fs::path{"/a/b"});
    CHECK(typed_path("/a//b/./c/..", "/x", home) == fs::path{"/a/b"});
    CHECK(typed_path("/", "/x", home) == fs::path{"/"});
    CHECK(typed_path("~", "/x", home) == home);
    CHECK(typed_path("~/", "/x", home) == home);
    CHECK(typed_path("~/Pictures", "/x", home) == fs::path{"/home/me/Pictures"});
    CHECK(typed_path("~user", "/x", home) == fs::path{"/x/~user"});  // no logins, a plain name
    CHECK(typed_path("sub", "/a/b", home) == fs::path{"/a/b/sub"});
    CHECK(typed_path("../x", "/a/b", home) == fs::path{"/a/x"});
    CHECK(typed_path("..", "/x/a.zip/docs", home) == fs::path{"/x/a.zip"});  // out of the archive
    CHECK(typed_path("file:///a%20b", "/x", home) == fs::path{"/a b"});
    CHECK(typed_path("file://otherhost/a", "/x", home) == std::nullopt);
    CHECK(typed_path("trash:///", "/x", home) == nordend::fs::trash_path());
    CHECK(typed_path("trash:///x", "/x", home) == std::nullopt);  // no folders inside the trash
    CHECK(typed_path("", "/x", home) == std::nullopt);
    CHECK(typed_path("  ", "/x", home) == fs::path{"/x/  "});        // spaces are a name too
    CHECK(typed_path(" a b ", "/x", home) == fs::path{"/x/ a b "});  // not trimmed
    CHECK(typed_path("x", nordend::fs::trash_path(), home) == std::nullopt);  // nothing relative
    CHECK(typed_path("~", "/x", "") == std::nullopt);                         // no home
}

TEST_CASE("path entry: the text split at its last slash") {
    CHECK(split_typed("~/Pic").folder == "~/");
    CHECK(split_typed("~/Pic").name == "Pic");
    CHECK(split_typed("~").folder == "~");  // a lone "~": the home folder, nothing typed inside
    CHECK(split_typed("~").name == "");
    CHECK(split_typed("/").folder == "/");
    CHECK(split_typed("/").name == "");
    CHECK(split_typed("abc").folder == "");
    CHECK(split_typed("abc").name == "abc");
    CHECK(split_typed("/a/b/").folder == "/a/b/");
    CHECK(split_typed("/a/b/").name == "");
    CHECK(split_typed("").folder == "");
    CHECK(split_typed("").name == "");
    CHECK(split_typed("trash:///x").folder == "trash:///");
    CHECK(split_typed("trash:///x").name == "x");
}

TEST_CASE("path entry: Tab's matches and the start they share") {
    const std::vector<std::string> folders{"Downloads", "Documents", ".cache",
                                           ".config",   "Zdjęcia",   "Żółw"};
    Completion found = complete("Do", folders, false);
    CHECK(found.matches == std::vector<std::string>{"Documents", "Downloads"});
    CHECK(found.common == "Do");
    found = complete("Dox", folders, false);
    CHECK(found.matches.empty());
    CHECK(found.common.empty());
    // Hidden names only with a dot typed, or when hidden files are shown.
    found = complete("", folders, false);
    CHECK(found.matches == std::vector<std::string>{"Documents", "Downloads", "Zdjęcia", "Żółw"});
    CHECK(found.common.empty());
    found = complete(".", folders, false);
    CHECK(found.matches == std::vector<std::string>{".cache", ".config"});
    CHECK(found.common == ".c");
    found = complete("", folders, true);
    CHECK(found.matches.front() == ".cache");
    CHECK(found.matches.back() == "Żółw");
    // A name that is not UTF-8 cannot go into the field.
    const std::vector<std::string> mixed{"Documents", "D\xff"};
    found = complete("D", mixed, false);
    CHECK(found.matches == std::vector<std::string>{"Documents"});
    found = complete("\xff", mixed, false);
    CHECK(found.matches.empty());
}

TEST_CASE("path entry: Ctrl+L opens on the folder's path, all selected; Escape closes") {
    Fixture f;
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    const std::string text = f.root.string() + "/";
    REQUIRE(f.browser.path_entry().field().text() == text);
    const auto [first, last] = f.browser.path_entry().field().selection();
    CHECK(first == 0);
    CHECK(last == text.size());
    // Nothing moves yet, whatever is typed.
    f.type("/nowhere/at/all");
    CHECK(f.shown()->path() == f.root);
    CHECK(f.press(chord(XKB_KEY_Escape)));
    CHECK_FALSE(f.browser.path_entry().active());
    CHECK(f.shown()->path() == f.root);
    // Ctrl+L again takes the whole text, as in a browser.
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.type("x");
    REQUIRE(f.browser.path_entry().field().text() == "x");
    CHECK(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    CHECK(f.browser.path_entry().field().selection().second == 1);
}

TEST_CASE(
    "path entry: red only when nothing starts with the typed name; Enter only on a path "
    "that exists") {
    Fake f;
    f.entry.open("/home/me", false);
    REQUIRE(f.asked == std::vector<fs::path>{"/home/me"});
    f.arrives(
        listing({.folder = "/home/me", .folders = {"Documents", "Downloads"}, .files = {"a.txt"}}));
    f.type("~/Doc");
    CHECK_FALSE(f.entry.invalid());  // a name half typed is on its way somewhere
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone.empty());  // Enter only on a path that exists
    CHECK(f.entry.active());
    f.type("uments");
    CHECK_FALSE(f.entry.invalid());
    f.key(chord(XKB_KEY_Return));
    REQUIRE(f.gone == std::vector<std::pair<fs::path, std::string>>{{"/home/me/Documents", ""}});
    CHECK_FALSE(f.entry.active());  // going there closes the field

    f.entry.open("/home/me", false);  // the listing is asked for again
    REQUIRE(f.asked.size() == 2);
    f.arrives(
        listing({.folder = "/home/me", .folders = {"Documents", "Downloads"}, .files = {"a.txt"}}));
    f.type("Dox");
    CHECK(f.entry.invalid());  // nothing starts with it
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone.size() == 1);
    CHECK(f.entry.active());  // Enter does nothing, the field stays
}

TEST_CASE("path entry: a text ending in \"..\" or \".\" is a folder, and Enter goes there") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    f.type("..");  // the parent, not a name in the folder shown
    CHECK_FALSE(f.entry.invalid());
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone == std::vector<std::pair<fs::path, std::string>>{{"/home", ""}});
    CHECK_FALSE(f.entry.active());

    f.entry.open("/home/me", false);
    f.type("/home/me/.");  // through other folders on the way: their listing comes after
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    CHECK_FALSE(f.entry.invalid());
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone.back() == std::pair<fs::path, std::string>{"/home/me", ""});
}

TEST_CASE("path entry: a text that names nothing is red; an empty one is not") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    f.entry.paste("file://otherhost/a");
    CHECK(f.entry.field().text() == "file://otherhost/a");
    CHECK(f.entry.invalid());
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone.empty());
    // Empty: no colour, and Enter does nothing.
    f.key(chord(XKB_KEY_a, {.ctrl = true}));
    f.key(chord(XKB_KEY_BackSpace));
    CHECK(f.entry.field().text().empty());
    CHECK_FALSE(f.entry.invalid());
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone.empty());
    CHECK(f.entry.active());
}

TEST_CASE("path entry: a lone tilde is the home folder, and Enter goes there") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents", "Downloads"}}));
    f.type("~");  // the home folder, as "~/" is, not a name in the folder shown
    CHECK(f.entry.field().text() == "~");
    CHECK_FALSE(f.entry.invalid());
    CHECK(f.asked.size() == 1);  // the home folder is listed already: no new ask
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone == std::vector<std::pair<fs::path, std::string>>{{"/home/me", ""}});
    CHECK_FALSE(f.entry.active());  // going there closes the field

    Fake g;
    g.entry.open("/somewhere/else", false);
    g.arrives(listing({.folder = "/somewhere/else", .folders = {"Documents"}}));
    g.type("~");
    CHECK_FALSE(g.entry.invalid());
    REQUIRE(g.asked.size() == 2);  // the home folder's own listing is asked for
    CHECK(g.asked.back() == "/home/me");
    g.arrives(listing({.folder = "/home/me", .folders = {"Music"}}));
    g.key(chord(XKB_KEY_Tab));
    CHECK(g.entry.field().text() == "~/Music/");  // completed inside it, slash and all
    g.arrives(listing({.folder = "/home/me/Music"}));
    g.key(chord(XKB_KEY_Return));
    CHECK(g.gone == std::vector<std::pair<fs::path, std::string>>{{"/home/me/Music", ""}});
}

TEST_CASE("path entry: the trash is never listed; a text there is red, its own path is not") {
    Fake f;
    f.entry.open(nordend::fs::trash_path(), false);
    CHECK(f.asked.empty());
    CHECK(f.entry.field().text() == "trash:///");
    CHECK_FALSE(f.entry.invalid());
    f.type("x");  // a name relative to the trash names nothing
    CHECK(f.entry.invalid());
    CHECK(f.asked.empty());
    f.key(chord(XKB_KEY_l, {.ctrl = true}));  // the whole text again, so the paste replaces it
    f.entry.paste("trash:///");
    CHECK_FALSE(f.entry.invalid());
    f.key(chord(XKB_KEY_l, {.ctrl = true}));
    f.entry.paste("trash:///x");
    CHECK(f.entry.invalid());
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "trash:///x");  // Tab has nothing to offer in the trash
    CHECK(f.asked.empty());
    f.key(chord(XKB_KEY_l, {.ctrl = true}));
    f.entry.paste("trash:///");
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone == std::vector<std::pair<fs::path, std::string>>{{nordend::fs::trash_path(), ""}});
    CHECK(f.asked.empty());
}

TEST_CASE("path entry: moving the caret neither lists again nor ends Tab's cycle") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Downloads", "Desktop", "Documents"}}));
    const std::size_t asks = f.asked.size();
    f.key(chord(XKB_KEY_End));
    CHECK(f.asked.size() == asks);
    f.key(chord(XKB_KEY_Tab));  // the three agree on "D"
    CHECK(f.entry.field().text() == "/home/me/D");
    CHECK(f.messages == std::vector<std::string>{"3 folders start with “D”"});
    f.key(chord(XKB_KEY_Left));  // the caret only
    CHECK(f.asked.size() == asks);
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "/home/me/Desktop");
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "/home/me/Documents");
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "/home/me/Downloads");
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "/home/me/Desktop");  // around again
    f.key(chord(XKB_KEY_ISO_Left_Tab, {.shift = true}));
    CHECK(f.entry.field().text() == "/home/me/Downloads");  // and back
    CHECK(f.asked.size() == asks);
}

TEST_CASE("path entry: Tab on the selected text completes inside the folder shown") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    f.key(chord(XKB_KEY_Tab));  // the whole path is selected: still the folder's own completion
    CHECK(f.entry.field().text() == "/home/me/Documents/");
    CHECK(f.asked == std::vector<fs::path>{"/home/me", "/home/me/Documents"});
}

TEST_CASE(
    "path entry: a name with a leading or trailing space can be typed; a pasted line loses "
    "its line end") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {" leading"}}));
    f.type("~/ le");
    CHECK_FALSE(f.entry.invalid());  // the space typed is part of the name
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "~/ leading/");  // the folder part stays as typed

    Fake g;
    g.entry.open("/home/me", false);
    g.arrives(listing({.folder = "/home/me", .folders = {"Documents", "Downloads"}}));
    g.entry.paste("/home/me/Downloads\r\n");
    CHECK(g.entry.field().text() == "/home/me/Downloads");  // no line end in the field
    g.key(chord(XKB_KEY_Return));
    CHECK(g.gone == std::vector<std::pair<fs::path, std::string>>{{"/home/me/Downloads", ""}});
}

TEST_CASE("path entry: Tab leaves out names that are not UTF-8, the red state does not") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me",
                       .folders = {"Documents", "D\xff",
                                   "\xff"
                                   "bad"}}));
    f.type("~/D");
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "~/Documents/");  // the only UTF-8 match

    Fake g;
    g.entry.open("/home/me", false);
    g.arrives(listing({.folder = "/home/me",
                       .folders = {"Documents", "D\xff",
                                   "\xff"
                                   "bad"}}));
    g.type("~/\xff");
    CHECK(g.entry.field().text() == "~/\xff");
    CHECK_FALSE(g.entry.invalid());  // the red state counts "\xff" "bad", bytes are enough for it
    g.key(chord(XKB_KEY_Tab));
    CHECK(g.entry.field().text() == "~/\xff");  // but Tab offers none of them
}

TEST_CASE("path entry: one listing per folder typed, none while typing inside it") {
    Fake f;
    f.entry.open("/home/me", false);
    REQUIRE(f.asked == std::vector<fs::path>{"/home/me"});
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    f.type("~/Doc");
    CHECK(f.asked.size() == 1);  // the folder part never moved
    f.key(chord(XKB_KEY_Tab));   // writes "Documents/" and goes one folder deeper
    CHECK(f.entry.field().text() == "~/Documents/");
    REQUIRE(f.asked.size() == 2);
    CHECK(f.asked.back() == "/home/me/Documents");
    f.arrives(listing({.folder = "/home/me/Documents", .folders = {"inner"}}));
    f.type("in");  // a name in the new folder
    CHECK(f.asked.size() == 2);
}

TEST_CASE(
    "path entry: a Tab before the listing comes is applied when it does, unless the text "
    "changed") {
    Fake f;
    f.entry.open("/home/me", false);
    f.key(chord(XKB_KEY_Tab));  // the listing is still on its way
    CHECK(f.entry.field().text() == "/home/me/");
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    CHECK(f.entry.field().text() == "/home/me/Documents/");  // applied, and its own folder listed
    REQUIRE(f.asked.size() == 2);
    CHECK(f.asked.back() == "/home/me/Documents");

    Fake g;
    g.entry.open("/home/me", false);
    g.key(chord(XKB_KEY_Tab));  // the Tab collapses the selection and then waits
    g.type("x");                // the text moved on: the Tab is forgotten
    g.arrives(listing({.folder = "/home/me", .folders = {"Documents"}}));
    CHECK(g.entry.field().text() == "/home/me/x");  // not the completion the Tab would write
}

TEST_CASE(
    "path entry: Tab writes the one match and a slash, the common start, then goes through "
    "them") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Pictures", "Pictures2026"}}));
    f.type("~/Pi");
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "~/Pictures");  // only as far as they agree
    CHECK(f.messages == std::vector<std::string>{"2 folders start with “Pictures”"});
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "~/Pictures");  // the first of them, nothing new in it
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.entry.field().text() == "~/Pictures2026");  // then the next one
    f.key(chord(XKB_KEY_ISO_Left_Tab, {.shift = true}));
    CHECK(f.entry.field().text() == "~/Pictures");
    f.key(chord(XKB_KEY_Return));  // the whole name: it exists
    CHECK(f.gone == std::vector<std::pair<fs::path, std::string>>{{"/home/me/Pictures", ""}});
}

TEST_CASE("path entry: an ambiguous Tab says how many folders it found") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Dobre", "Dokumenty"}}));
    f.type("~/Do");  // their common start is the whole member: nothing new to write
    f.key(chord(XKB_KEY_Tab));
    CHECK(f.messages == std::vector<std::string>{"2 folders start with “Do”"});
    CHECK(f.entry.field().text() == "~/Dobre");
    f.key(chord(XKB_KEY_Tab));  // each step of the cycle says it again
    CHECK(f.messages ==
          std::vector<std::string>{"2 folders start with “Do”", "2 folders start with “Do”"});
    CHECK(f.entry.field().text() == "~/Dokumenty");

    Fake g;
    g.entry.open("/home/me", false);
    g.arrives(listing({.folder = "/home/me", .folders = {"Music", "Pictures"}}));
    g.key(chord(XKB_KEY_Tab));  // the whole path selected, no member: no start to name
    CHECK(g.messages == std::vector<std::string>{"Tab goes through 2 folders"});
    CHECK(g.entry.field().text() == "/home/me/Music");
}

TEST_CASE("path entry: Enter on a file goes to its folder with the cursor on it") {
    Fake f;
    f.entry.open("/home/me", false);
    f.arrives(listing({.folder = "/home/me", .folders = {"Documents"}, .files = {"a.txt"}}));
    f.type("~/a.txt");
    f.key(chord(XKB_KEY_Return));
    CHECK(f.gone == std::vector<std::pair<fs::path, std::string>>{{"/home/me", "a.txt"}});
}

TEST_CASE("path entry: Enter goes to the typed folder in every view") {
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        REQUIRE(f.show(f.root, mode));
        REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
        f.type("Documents");  // a name of the folder shown, the whole path replaced
        CHECK(f.press(chord(XKB_KEY_Return)));
        REQUIRE(run_until(f.loop, [&] {
            return f.shown() != nullptr && f.shown()->path() == f.root / "Documents";
        }));
        CHECK_FALSE(f.browser.path_entry().active());
        // It is a step of the history: Alt+Left goes back.
        CHECK(f.press(chord(XKB_KEY_Left, {.alt = true})));
        REQUIRE(run_until(f.loop, [&] { return f.shown()->path() == f.root; }));
    }
}

TEST_CASE("path entry: Tab completes from a real folder listed on a worker") {
    Fixture f;
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    CHECK(f.press(chord(XKB_KEY_Tab)));  // may wait for the listing; it is applied when it lands
    REQUIRE(run_until(
        f.loop, [&] { return f.browser.path_entry().field().text() == f.root.string() + "/Do"; }));
    CHECK(f.press(chord(XKB_KEY_Tab)));  // the cycle's first match
    REQUIRE(run_until(f.loop, [&] {
        return f.browser.path_entry().field().text() == f.root.string() + "/Documents";
    }));
    CHECK(f.press(chord(XKB_KEY_Return)));
    REQUIRE(run_until(
        f.loop, [&] { return f.shown() != nullptr && f.shown()->path() == f.root / "Documents"; }));
}

TEST_CASE("path entry: Tab's count lands in the status bar and stays while it cycles") {
    Fixture f;
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.type("Do");  // the whole path replaced: a name of the folder shown
    REQUIRE(f.press(chord(XKB_KEY_Tab)));
    REQUIRE(run_until(f.loop, [&] {
        return f.browser.path_entry().field().text() == "Documents" &&
               f.browser.status_bar().message() == "2 folders start with “Do”";
    }));
    CHECK(f.browser.path_entry().active());  // the field open, the count in the status bar
    REQUIRE(f.press(chord(XKB_KEY_Tab)));    // the next match: said again, so it does not time out
    REQUIRE(
        run_until(f.loop, [&] { return f.browser.path_entry().field().text() == "Downloads"; }));
    CHECK(f.browser.status_bar().message() == "2 folders start with “Do”");
}

TEST_CASE("path entry: a path into an archive completes and opens it as a folder") {
    const fs::path archives = fs::path{NORDEND_TEST_DATA} / "archive";
    Fixture f;
    fs::copy_file(archives / "basic.zip", f.root / "basic.zip");
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.type("basic.zip/");
    CHECK(f.press(chord(XKB_KEY_Tab)));
    REQUIRE(run_until(f.loop,
                      [&] { return f.browser.path_entry().field().text() == "basic.zip/docs/"; }));
    CHECK(f.press(chord(XKB_KEY_Return)));
    REQUIRE(run_until(f.loop, [&] {
        return f.shown() != nullptr && f.shown()->path() == f.root / "basic.zip" / "docs" &&
               !f.browser.loading();
    }));
    CHECK(f.shown()->is_archive());
}

TEST_CASE("path entry: in the file chooser Escape closes the field, not the dialog") {
    Fixture f;
    REQUIRE(f.show(f.root));
    int cancelled = 0;
    f.browser.set_pick(Browser::Pick{}, [](Browser::Picked) {}, [&] { ++cancelled; });
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    CHECK(f.press(chord(XKB_KEY_Escape)));
    CHECK_FALSE(f.browser.path_entry().active());
    CHECK(cancelled == 0);
    CHECK(f.press(chord(XKB_KEY_Escape)));  // the second one is the dialog's
    CHECK(cancelled == 1);
}

TEST_CASE(
    "path entry: Ctrl+L from the save bar's name moves the keyboard and Ctrl+V to the "
    "path") {
    Fixture f;
    REQUIRE(f.show(f.root));
    f.browser.set_system_clipboard(
        {.offer = {},
         .clear = {},
         .owned = [] { return false; },
         .generation = [] { return 0U; },
         .types = [] { return std::vector<std::string>{std::string{nordend::views::kText}}; },
         .read =
             [](const std::string& /*type*/, nordend::views::SystemClipboard::Done done) {
                 done(std::string{"/tmp/somewhere.txt\n"});
             }});
    int cancelled = 0;
    Browser::Pick saving;
    saving.save = true;
    saving.name = "a.txt";
    f.browser.set_pick(saving, [](Browser::Picked) {}, [&] { ++cancelled; });
    REQUIRE(f.browser.pick_bar().name_focused());  // saving: the name has the keyboard
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    CHECK_FALSE(f.browser.pick_bar().name_focused());  // the path field has it now
    REQUIRE(f.browser.path_entry().active());
    CHECK(f.press(chord(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(run_until(
        f.loop, [&] { return f.browser.path_entry().field().text() == "/tmp/somewhere.txt"; }));
    CHECK(f.browser.pick_bar().field().text() == "a.txt");  // the name was left alone
    CHECK(cancelled == 0);
}

TEST_CASE("path entry: Ctrl+L in Quick Look closes it and opens the field") {
    Fixture f;
    REQUIRE(f.show(f.root));
    f.shown()->select_name("a.txt");
    REQUIRE(run_until(f.loop, [&] { return f.shown()->cursor_entry() != nullptr; }));
    CHECK(f.press(chord(XKB_KEY_space)));
    REQUIRE(f.browser.quick_look().is_open());
    CHECK(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(f.browser.path_entry().active());
}

TEST_CASE("path entry: Ctrl+T and Ctrl+W close the field and still make or close a tab") {
    Fixture f;
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.type("x");  // something typed, which the close throws away
    REQUIRE(f.browser.path_entry().active());
    CHECK(f.press(chord(XKB_KEY_t, {.ctrl = true})));  // a new tab: the field closes, the tab opens
    CHECK_FALSE(f.browser.path_entry().active());
    CHECK(f.browser.tab_count() == 2);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.shown()->path() == f.root);  // the folder stays where it was
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    CHECK(f.press(chord(XKB_KEY_w, {.ctrl = true})));  // the tab closes, the field with it
    CHECK_FALSE(f.browser.path_entry().active());
    CHECK(f.browser.tab_count() == 1);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.shown()->path() == f.root);
    // Every other key is still the field's: commands go nowhere while it is open.
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    f.type("x");
    CHECK(f.browser.path_entry().field().text() == "x");
    CHECK(f.press(chord(XKB_KEY_f, {.ctrl = true})));  // find files: the field takes the key
    CHECK(f.browser.path_entry().active());
    CHECK(f.browser.file_finder() == nullptr);
    CHECK(
        f.press(chord(XKB_KEY_w, {.ctrl = true})));  // one tab only: the field gives way, it stays
    CHECK_FALSE(f.browser.path_entry().active());
    CHECK(f.browser.tab_count() == 1);
}

TEST_CASE("path entry: a click outside the field, another tab or a sidebar place closes it") {
    DrawnFixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    f.draw();
    f.click(kWidth / 2.0F, kHeight / 2.0F);  // in the view
    CHECK_FALSE(f.browser.path_entry().active());
    CHECK(f.shown()->path() == f.root);

    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.draw();
    std::optional<float> alpha_y;
    for (float y = 0.0F; y < kHeight && !alpha_y; y += 2.0F) {
        if (const nordend::fs::Place* place = f.browser.sidebar().place_under(40.0F, y);
            place != nullptr && place->path == f.alpha) {
            alpha_y = y;
        }
    }
    REQUIRE(alpha_y);
    f.click(40.0F, *alpha_y);  // a place of the sidebar
    CHECK_FALSE(f.browser.path_entry().active());
    REQUIRE(run_until(f.loop, [&] { return f.shown()->path() == f.alpha; }));

    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.browser.new_tab();  // a new tab: the field belongs to the tab that opened it
    CHECK_FALSE(f.browser.path_entry().active());
}

TEST_CASE("path entry: a click on the folder shown opens the field, the caret at its end") {
    DrawnFixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.show(f.alpha));
    // The last step, alpha, opens the field; the one before it, root, goes there.
    std::optional<float> last_x;
    std::optional<float> before_x;
    for (float x = 0.0F; x < kWidth; x += 1.0F) {
        if (!last_x && f.browser.breadcrumb().editable_at(x, kRow)) {
            last_x = x;
        }
        if (f.browser.breadcrumb().path_at(x, kRow) == f.root) {
            before_x = x;
        }
    }
    REQUIRE(last_x);
    REQUIRE(before_x);
    CHECK_FALSE(f.browser.breadcrumb().clickable_at(*last_x, kRow));
    CHECK(f.browser.cursor_at(*last_x, kRow) == nordend::ui::CursorShape::text);

    // Another button does nothing there.
    f.browser.breadcrumb().pointer({.kind = Kind::press,
                                    .x = *last_x,
                                    .y = kRow,
                                    .button = PointerButton::right,
                                    .click_count = 1});
    CHECK_FALSE(f.browser.path_entry().active());

    f.click(*last_x, kRow);
    REQUIRE(f.browser.path_entry().active());
    const nordend::ui::TextField& field = f.browser.path_entry().field();
    CHECK(field.text() == f.alpha.string() + "/");
    CHECK(field.caret() == field.text().size());
    CHECK(field.selection().first == field.selection().second);  // nothing selected
    CHECK(f.shown()->path() == f.alpha);

    // Ctrl+L still takes the whole path.
    CHECK(f.press(chord(XKB_KEY_Escape)));
    REQUIRE_FALSE(f.browser.path_entry().active());
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    CHECK(f.browser.path_entry().field().selection() ==
          std::pair<std::size_t, std::size_t>{0, f.browser.path_entry().field().text().size()});
    CHECK(f.press(chord(XKB_KEY_Escape)));
    f.draw();

    // An earlier step goes to its folder, and the field stays closed.
    f.click(*before_x, kRow);
    REQUIRE(run_until(f.loop, [&] { return f.shown()->path() == f.root; }));
    CHECK_FALSE(f.browser.path_entry().active());
}

TEST_CASE("path entry: while it is open, files dragged over the header drop nowhere") {
    DrawnFixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.show(f.root));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    f.draw();
    std::ofstream{f.dir.path / "elsewhere.txt"} << "elsewhere";
    f.dnd.types = {"text/uri-list"};
    f.dnd.data = nordend::views::uri_list(std::vector<fs::path>{f.dir.path / "elsewhere.txt"});
    f.dnd.accepted.reset();
    f.browser.drop_event(
        {.kind = nordend::ui::DropEvent::Kind::enter, .x = kWidth / 2.0F, .y = kRow});
    CHECK_FALSE(f.dnd.accepted);  // the steps are hidden: nothing takes the drop
    f.browser.drop_event(
        {.kind = nordend::ui::DropEvent::Kind::motion, .x = kWidth / 2.0F, .y = kRow});
    f.browser.drop_event(
        {.kind = nordend::ui::DropEvent::Kind::drop, .x = kWidth / 2.0F, .y = kRow});
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(listing_of(f.root) == "alpha");
    CHECK(f.browser.transfers_pending() == 0);
}

TEST_CASE("path entry: in the integrated header the field's row does not move the window") {
    DrawnFixture f;
    if (!f.gl) {
        return;
    }
    f.browser.set_window_header(kReserve);
    REQUIRE(f.show(f.root));
    // The breadcrumb's row, before the field: the free ends of the header still move it.
    CHECK(f.browser.window_drag_at(10.0F, kRow));  // the sidebar's top
    bool last = false;
    for (float x = 0.0F; x < kWidth; x += 2.0F) {
        if (f.browser.breadcrumb().editable_at(x, kRow)) {
            last = true;
            CHECK_FALSE(f.browser.window_drag_at(x, kRow));  // the folder shown opens the field
        }
    }
    CHECK(last);
    CHECK(f.browser.window_drag_at(kWidth - (kReserve / 2.0F), kRow));
    REQUIRE(f.press(chord(XKB_KEY_l, {.ctrl = true})));
    REQUIRE(f.browser.path_entry().active());
    f.draw();
    for (float x = 0.0F; x < kWidth; x += 2.0F) {
        if (f.browser.breadcrumb().claims(x, kRow)) {
            CHECK_FALSE(f.browser.window_drag_at(x, kRow));  // the field's row, margins too
        }
    }
    CHECK(f.browser.window_drag_at(10.0F, kRow));  // the sidebar's top still does
    CHECK(f.browser.window_drag_at(kWidth - (kReserve / 2.0F), kRow));  // and the buttons' room
    // The text cursor over the field, the arrow over the margin beside it.
    std::optional<float> field_x;
    for (float x = 0.0F; x < kWidth && !field_x; x += 1.0F) {
        if (f.browser.path_entry().claims(x, kRow)) {
            field_x = x;
        }
    }
    REQUIRE(field_x);
    CHECK(f.browser.cursor_at(*field_x, kRow) == nordend::ui::CursorShape::text);
}
