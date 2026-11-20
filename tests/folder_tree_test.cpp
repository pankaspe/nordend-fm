#include "nordend/views/folder_tree.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/places.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::core::WorkerPool;
using nordend::fs::DirectoryWatcher;
using nordend::test::TempDir;
using nordend::views::FolderTree;
using Fold = nordend::views::FolderTree::Fold;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

// Runs the loop until done() holds (checked after every iteration's timer) or 5 s pass.
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

// A tree over home/{a/inner/, b/, notes.txt}, its failures recorded.
struct Fixture {
    TempDir dir;
    fs::path home = dir.path / "home";
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    FolderTree tree{loop, pool, watcher, 5ms};
    int changes = 0;
    std::vector<std::pair<fs::path, std::error_code>> failed;

    Fixture() {
        fs::create_directories(home / "a" / "inner");
        fs::create_directories(home / "b");
        dir.write("home/notes.txt", "");
        tree.set_change_handler([this] { ++changes; });
        tree.set_fail_handler([this](const fs::path& folder, std::error_code error) {
            failed.emplace_back(folder, error);
        });
    }

    // The rows under home as paths inside it ("a/inner"), "+N" for the count of the rest.
    [[nodiscard]] Names rows(const fs::path& place, bool show_hidden = false) const {
        std::vector<FolderTree::Row> out;
        tree.rows_under(place, show_hidden, out);
        Names names;
        for (const FolderTree::Row& row : out) {
            names.push_back(row.more > 0 ? "+" + std::to_string(row.more)
                                         : row.folder.lexically_relative(place).string());
        }
        return names;
    }

    [[nodiscard]] Names rows() const { return rows(home); }

    void unfold(const fs::path& folder) {
        tree.unfold(home, folder);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !tree.busy(); });
    }

    bool settle_on(const Names& expected) {
        return run_until(loop, [&] { return !tree.busy() && rows() == expected; });
    }
};

}  // namespace

TEST_CASE("folder tree: a level is read when unfolded, and let go when folded") {
    Fixture f;
    CHECK(f.tree.fold_of(f.home, f.home, false) == Fold::folded);
    CHECK(f.rows().empty());
    f.unfold(f.home);
    CHECK(f.rows() == Names{"a", "b"});  // folders only
    CHECK(f.tree.fold_of(f.home, f.home, false) == Fold::unfolded);
    CHECK(f.tree.fold_of(f.home, f.home / "a", false) == Fold::folded);
    CHECK(f.changes > 0);

    f.unfold(f.home / "a");
    f.unfold(f.home / "b");
    CHECK(f.rows() == Names{"a", "a/inner", "b"});
    CHECK(f.tree.fold_of(f.home, f.home / "b", false) == Fold::leaf);  // nothing in it
    std::vector<FolderTree::Row> rows;
    f.tree.rows_under(f.home, false, rows);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].depth == 1);
    CHECK(rows[1].depth == 2);
    CHECK(f.tree.levels() == 3);

    f.tree.fold(f.home, f.home / "a");
    CHECK(f.rows() == Names{"a", "b"});
    CHECK(f.tree.levels() == 2);
    f.tree.fold(f.home, f.home);  // with every level under it
    CHECK(f.rows().empty());
    CHECK(f.tree.levels() == 0);
    CHECK(f.failed.empty());
}

TEST_CASE("folder tree: folders made, renamed and removed show; files do not") {
    Fixture f;
    f.unfold(f.home);

    fs::create_directory(f.home / "img10");
    fs::create_directory(f.home / "img2");
    f.dir.write("home/c.txt", "");
    REQUIRE(f.settle_on({"a", "b", "img2", "img10"}));

    fs::rename(f.home / "img10", f.home / "c");
    REQUIRE(f.settle_on({"a", "b", "c", "img2"}));
    fs::remove(f.home / "img2");
    REQUIRE(f.settle_on({"a", "b", "c"}));

    // A link to a folder is one; a folder that becomes a file is not any more.
    fs::create_directory_symlink("a", f.home / "link");
    REQUIRE(f.settle_on({"a", "b", "c", "link"}));
    fs::remove(f.home / "c");
    f.dir.write("home/c", "");
    REQUIRE(f.settle_on({"a", "b", "link"}));
}

TEST_CASE("folder tree: a burst of 500 files and 3 folders adds the 3 folders") {
    Fixture f;
    f.unfold(f.home);
    for (int i = 0; i < 500; ++i) {
        f.dir.write("home/f" + std::to_string(i), "");
    }
    for (const char* name : {"x", "y", "z"}) {
        fs::create_directory(f.home / name);
    }
    REQUIRE(f.settle_on({"a", "b", "x", "y", "z"}));
}

TEST_CASE("folder tree: an unfolded folder removed goes with the levels under it") {
    Fixture f;
    f.unfold(f.home);
    f.unfold(f.home / "a");
    f.unfold(f.home / "a" / "inner");
    CHECK(f.tree.levels() == 3);
    fs::remove_all(f.home / "a");
    REQUIRE(f.settle_on({"b"}));
    CHECK(run_until(f.loop, [&] { return f.tree.levels() == 1; }));
    CHECK(f.tree.fold_of(f.home, f.home / "a", false) == Fold::folded);
}

TEST_CASE("folder tree: hidden folders only when they are shown") {
    Fixture f;
    fs::create_directories(f.home / "b" / ".cache");
    f.unfold(f.home);
    f.unfold(f.home / "b");
    CHECK(f.rows(f.home) == Names{"a", "b"});
    CHECK(f.rows(f.home, true) == Names{"a", "b", "b/.cache"});
    CHECK(f.tree.fold_of(f.home, f.home / "b", false) == Fold::leaf);
    CHECK(f.tree.fold_of(f.home, f.home / "b", true) == Fold::unfolded);
}

TEST_CASE("folder tree: a link back to a folder above does not unfold") {
    Fixture f;
    fs::create_directory_symlink("..", f.home / "a" / "inner" / "up");  // a
    fs::create_directory_symlink("../b", f.home / "a" / "side");        // b, beside it
    f.unfold(f.home);
    f.unfold(f.home / "a");
    f.unfold(f.home / "a" / "inner");
    CHECK(f.rows() == Names{"a", "a/inner", "a/inner/up", "a/side", "b"});

    f.unfold(f.home / "a" / "inner" / "up");
    CHECK(f.tree.fold_of(f.home, f.home / "a" / "inner" / "up", false) == Fold::failed);
    REQUIRE(f.failed.size() == 1);
    CHECK(f.failed[0].first == f.home / "a" / "inner" / "up");
    CHECK_FALSE(f.failed[0].second);  // not an error: a loop

    f.unfold(f.home / "a" / "side");
    CHECK(f.tree.fold_of(f.home, f.home / "a" / "side", false) == Fold::leaf);
    CHECK(f.failed.size() == 1);
}

TEST_CASE("folder tree: a folder that cannot be read says why") {
    if (geteuid() == 0) {
        return;  // root reads it anyway
    }
    Fixture f;
    fs::permissions(f.home / "b", fs::perms::none);
    f.unfold(f.home);
    f.unfold(f.home / "b");
    CHECK(f.tree.fold_of(f.home, f.home / "b", false) == Fold::failed);
    REQUIRE(f.failed.size() == 1);
    CHECK(f.failed[0].second == std::errc::permission_denied);
    fs::permissions(f.home / "b", fs::perms::owner_all);
}

TEST_CASE("folder tree: levels go with their disk and with their place") {
    Fixture f;
    const fs::path disk = f.home / "a";  // as if mounted there
    const nordend::fs::Place home{.section = nordend::fs::Place::Section::places,
                                  .label = "Home",
                                  .path = f.home,
                                  .icon = "home",
                                  .disk = {}};
    const nordend::fs::Place usb{.section = nordend::fs::Place::Section::devices,
                                 .label = "USB",
                                 .path = disk,
                                 .icon = "usb",
                                 .disk = {}};
    f.tree.unfold(disk, disk);
    f.unfold(f.home);
    f.unfold(disk);  // the same folder under Home: a level of its own
    CHECK(f.tree.levels() == 3);
    CHECK(f.rows(disk) == Names{"inner"});
    f.tree.fold(f.home, disk);
    CHECK(f.tree.fold_of(disk, disk, false) == Fold::unfolded);  // its place's stays

    f.unfold(disk);
    f.tree.release_under(disk);
    CHECK(f.tree.levels() == 1);
    CHECK(f.tree.fold_of(f.home, f.home, false) == Fold::unfolded);

    f.tree.unfold(disk, disk);
    REQUIRE(f.settle());
    f.tree.keep_places(std::vector<nordend::fs::Place>{usb});
    CHECK(f.tree.levels() == 1);
    CHECK(f.tree.fold_of(disk, disk, false) == Fold::unfolded);
    f.tree.keep_places(std::vector<nordend::fs::Place>{home});
    CHECK(f.tree.levels() == 0);
}
