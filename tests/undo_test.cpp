#include "nordend/views/undo.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/conflict_dialog.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::test::TempDir;
using nordend::ui::KeyEvent;
using nordend::ui::Modifiers;
using nordend::views::Browser;
using nordend::views::FileOperation;
using nordend::views::Folder;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

Names listing(const fs::path& directory) {
    Names names;
    std::error_code ignored;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, ignored}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

const Modifiers kCtrl{.ctrl = true};
const Modifiers kCtrlShift{.ctrl = true, .shift = true};

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/}, a trash of its own.
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    fs::path photos = root / "photos";
    fs::path album = root / "album";
    fs::path trash = dir.path / "Trash";
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(photos);
        fs::create_directories(album);
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P2.ORF"}) {
            std::ofstream{photos / name} << name;
        }
        browser.set_home_trash(trash);
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    // Presses event and waits for what it started.
    void run(const KeyEvent& event) {
        CHECK(press(event));
        REQUIRE(settle());
    }

    std::shared_ptr<Folder> show(const fs::path& folder, const std::string& cursor_on) {
        browser.open(folder);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
        std::shared_ptr<Folder> shown = browser.details().folder();
        if (!cursor_on.empty()) {
            shown->select_name(cursor_on);
            REQUIRE(run_until(loop, [&] { return shown->cursor_entry() != nullptr; }));
        }
        return shown;
    }

    [[nodiscard]] std::string message() { return browser.status_bar().message(); }
};

}  // namespace

TEST_CASE("undo: a rename, sidecars along; redo; then nothing left to undo") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    CHECK(f.press(key(XKB_KEY_NoSymbol, {}, "Lake")));
    f.run(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.name_editor().active(); }));
    CHECK(listing(f.photos) == Names{"Lake.ORF", "Lake.ORF.xmp", "P2.ORF"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “Lake.ORF” is “P1.ORF” again");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "Nothing to undo");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.photos) == Names{"Lake.ORF", "Lake.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Redone: “P1.ORF” is “Lake.ORF” again");
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(f.message() == "Nothing to redo");
    f.run(key(XKB_KEY_z, kCtrl));  // the redo is undone like any rename
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("undo: a move goes back, and comes again on redo") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “P1.ORF” back in photos");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(f.message() == "Redone: “P1.ORF” back in album");
}

TEST_CASE("undo: copies go to the trash, never deleted; redo brings them back") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.trash / "files") == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});  // the originals stay
    CHECK(f.message() == "Undone: “P1.ORF” moved to the trash");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(listing(f.trash / "files").empty());
    CHECK(f.message() == "Redone: “P1.ORF” restored");
}

TEST_CASE("undo: new links go to the trash, what they lead to stays; redo brings them back") {
    Fixture f;
    std::ofstream{f.album / "kept.txt"} << "kept";
    const std::shared_ptr<Folder> shown = f.show(f.root, "album");
    f.browser.create_links();
    REQUIRE(f.settle());
    REQUIRE(listing(f.root) == Names{"Link to album", "album", "photos"});
    CHECK(fs::read_symlink(f.root / "Link to album") == f.album);
    CHECK(f.message() == "Made 1 link");
    REQUIRE(run_until(f.loop, [&] { return shown->selected_names() == Names{"Link to album"}; }));

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.root) == Names{"album", "photos"});
    CHECK(listing(f.trash / "files") == Names{"Link to album"});
    CHECK(fs::is_symlink(f.trash / "files" / "Link to album"));
    CHECK(listing(f.album) == Names{"kept.txt"});  // what the link led to stays
    CHECK(f.message() == "Undone: “Link to album” moved to the trash");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.root) == Names{"Link to album", "album", "photos"});
    CHECK(fs::read_symlink(f.root / "Link to album") == f.album);
    CHECK(f.message() == "Redone: “Link to album” restored");

    // Several at once, one operation.
    const std::shared_ptr<Folder> photos = f.show(f.photos, "");
    for (const char* name : {"P1.ORF", "P2.ORF"}) {
        const auto& entries = photos->entries();
        const auto it = std::ranges::find(entries, name, &nordend::fs::Entry::name);
        REQUIRE(it != entries.end());
        photos->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                           nordend::views::SelectMode::toggle);
    }
    REQUIRE(photos->selected_names() == Names{"P1.ORF", "P2.ORF"});
    f.browser.create_links();
    REQUIRE(f.settle());
    CHECK(listing(f.photos) ==
          Names{"Link to P1.ORF", "Link to P2.ORF", "P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Made 2 links");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("undo: after a merge only what the copy made goes; what was there stays") {
    Fixture f;
    // album/photos is there already, with a file of its own and an older P2.ORF.
    fs::create_directories(f.album / "photos");
    std::ofstream{f.album / "photos" / "mine.txt"} << "mine";
    std::ofstream{f.album / "photos" / "P2.ORF"} << "album's P2";
    nordend::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.root, "photos");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));  // merge the folders
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open() || !f.browser.loading(); }));
    if (dialog.is_open()) {
        CHECK(dialog.conflict().name == "P2.ORF");
        CHECK(f.press(key(XKB_KEY_r)));  // replace the older P2.ORF
    }
    REQUIRE(f.settle());
    REQUIRE(listing(f.album / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "mine.txt"});

    f.run(key(XKB_KEY_z, kCtrl));
    // The new files go to the trash; the folder and what it held before stay.
    CHECK(listing(f.album / "photos") == Names{"P2.ORF", "mine.txt"});
    CHECK(listing(f.trash / "files") == Names{"P1.ORF", "P1.ORF.xmp"});
}

TEST_CASE("undo: a move merged into a folder goes back into its own, made again") {
    Fixture f;
    fs::create_directories(f.album / "photos");
    std::ofstream{f.album / "photos" / "mine.txt"} << "mine";
    std::ofstream{f.album / "photos" / "P2.ORF"} << "album's P2";
    nordend::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.root, "photos");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));  // merge
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_s)));  // keep album's P2.ORF: photos/P2.ORF stays behind
    REQUIRE(f.settle());
    REQUIRE(listing(f.album / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "mine.txt"});
    REQUIRE(listing(f.photos) == Names{"P2.ORF"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.album / "photos") == Names{"P2.ORF", "mine.txt"});
    CHECK(f.message() == "Undone: “P1.ORF” back in photos");

    // All of it moved this time: the emptied photos folder is gone, and made again by the undo.
    fs::remove(f.album / "photos" / "P2.ORF");
    f.show(f.root, "photos");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE_FALSE(fs::exists(f.photos));
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.album / "photos") == Names{"mine.txt"});
}

TEST_CASE("undo: replaced files are said, never brought back") {
    Fixture f;
    std::ofstream{f.album / "P2.ORF"} << "album's P2";
    nordend::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.photos, "P2.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"P2.ORF"});

    CHECK(f.press(key(XKB_KEY_z, kCtrl)));  // only a replaced file: nothing to bring back
    CHECK(f.message() == "1 replaced file cannot be brought back");
    CHECK(listing(f.album) == Names{"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_z, kCtrl)));
    CHECK(f.message() == "Nothing to undo");

    // Everything in photos: P1.ORF is new, P2.ORF replaces album's again.
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_a, kCtrl)));
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album) == Names{"P2.ORF"});
    CHECK(f.message() ==
          "Undone: “P1.ORF” moved to the trash; 1 replaced file cannot be brought back");
}

TEST_CASE("undo: Delete is undone by restoring; a permanent delete cannot be") {
    Fixture f;
    f.show(f.photos, "P2.ORF");
    f.run(key(XKB_KEY_Delete));
    REQUIRE(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “P2.ORF” restored");
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_Delete, {.shift = true})));
    f.run(key(XKB_KEY_Return));
    REQUIRE(listing(f.photos).empty());
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "A permanent delete cannot be undone");
}

TEST_CASE("undo: a new folder goes to the trash while it is empty") {
    Fixture f;
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_n, kCtrlShift)));
    REQUIRE(run_until(f.loop, [&] { return f.browser.name_editor().active(); }));
    CHECK(f.press(key(XKB_KEY_Escape)));  // keeps "New Folder"
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"New Folder"});

    // Something put in it: not taken away.
    std::ofstream{f.album / "New Folder" / "note.txt"} << "mine";
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "“New Folder” has changed since it was made: not undone");
    CHECK(listing(f.album) == Names{"New Folder"});

    fs::remove(f.album / "New Folder" / "note.txt");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.trash / "files") == Names{"New Folder"});
}

TEST_CASE("undo: what changed meanwhile is said, and undo goes on working") {
    Fixture f;
    f.show(f.photos, "P2.ORF");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P2.ORF"});
    fs::remove(f.album / "P2.ORF");  // another program took it away

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message().find("P2.ORF") != std::string::npos);
    CHECK(f.message().find("could not be") != std::string::npos);
    // Nothing came back: nothing to redo, and Ctrl+Z still answers.
    CHECK_FALSE(f.browser.undone_operation().has_value());
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() != "Still undoing the last one");
}

TEST_CASE("undo: what the messages say") {
    using Kind = FileOperation::Kind;
    CHECK(nordend::views::describe({.kind = Kind::trash,
                                    .items = {{.from = "/a/x.txt", .to = "/T/files/x.2.txt"},
                                              {.from = "/a/y.txt", .to = "/T/files/y.txt"}},
                                    .modes = {}}) == "2 items moved to the trash");
    CHECK(nordend::views::describe({.kind = Kind::move,
                                    .items = {{.from = "/a/x.txt", .to = "/b/x.txt"}},
                                    .modes = {}}) == "“x.txt” back in b");
}
