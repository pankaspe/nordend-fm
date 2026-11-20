// Symbolic links the user asks for ("Create Link"): run_link and the names and targets it picks.
#include "nordend/fs/links.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/transfer.hpp"
#include "temp_dir.hpp"

using nordend::core::CancelToken;
using nordend::fs::Conflict;
using nordend::fs::link_contents;
using nordend::fs::link_name;
using nordend::fs::PlacedItem;
using nordend::fs::Resolution;
using nordend::fs::run_link;
using nordend::fs::same_tree;
using nordend::fs::TransferCallbacks;
using nordend::fs::TransferKind;
using nordend::fs::TransferResult;
using nordend::test::TempDir;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

Names listing(const fs::path& directory) {
    Names names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

TransferResult make_links(const fs::path& source, Names names, const fs::path& destination,
                          const TransferCallbacks& callbacks = {}, const CancelToken& token = {}) {
    return run_link({.kind = TransferKind::link,
                     .source = source,
                     .names = std::move(names),
                     .destination = destination},
                    token, callbacks);
}

// Answers every conflict the same way, keeping what was asked.
struct Answer {
    Resolution resolution{Resolution::skip};
    std::vector<Conflict> asked;

    TransferCallbacks callbacks() {
        return {.progress = nullptr, .resolve = [this](const Conflict& conflict) {
                    asked.push_back(conflict);
                    return resolution;
                }};
    }
};

// disk/{a/{P1.ORF, P1.ORF.xmp, notes.txt, .bashrc, Trip/x.jpg, Trip.2023/, Days -> Trip}, b/}.
struct Tree {
    TempDir dir;
    fs::path a = dir.path / "disk" / "a";
    fs::path b = dir.path / "disk" / "b";

    Tree() {
        fs::create_directories(a / "Trip");
        fs::create_directories(a / "Trip.2023");
        fs::create_directories(b);
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "notes.txt", ".bashrc", "Trip/x.jpg"}) {
            std::ofstream{a / name} << name;
        }
        fs::create_directory_symlink("Trip", a / "Days");
    }
};

}  // namespace

TEST_CASE("links: one tree is one file system and a folder below / in common") {
    CHECK(same_tree("/home/a/x", "/home/a/y", true));
    CHECK(same_tree("/home/rgl/b", "/home/other", true));
    CHECK(same_tree("/mnt/data2/Foto/b", "/mnt/data2/Foto/a", true));
    CHECK_FALSE(same_tree("/home/rgl", "/usr/share", true));  // only / in common
    CHECK_FALSE(same_tree("/", "/home/a", true));
    CHECK_FALSE(same_tree("/home/a/x", "/home/a/y", false));  // another disk
    CHECK_FALSE(same_tree("home/a", "home/b", true));         // not real paths
}

TEST_CASE("links: the target is relative from the real paths, else the item itself") {
    CHECK(link_contents("/home/rgl/a/P1.ORF", "/home/rgl/b") == "../a/P1.ORF");
    CHECK(link_contents("/home/rgl/a/P1.ORF", "/home/rgl/a/deep/er") == "../../P1.ORF");
    CHECK(link_contents("/home/rgl/P1.ORF", "/home/rgl") == "P1.ORF");
    CHECK(link_contents("/home/rgl/P1.ORF", "relative") == "/home/rgl/P1.ORF");  // none
}

TEST_CASE(
    "links: names are numbered before the item's extension, at the end for folders and dot "
    "names") {
    CHECK(link_name("P1.ORF", false, true) == "Link to P1.ORF");
    CHECK(link_name("P1.ORF", false, true, 2) == "Link to P1 2.ORF");
    CHECK(link_name(".bashrc", false, true, 2) == "Link to .bashrc 2");
    CHECK(link_name("Trip.2023", true, true, 3) == "Link to Trip.2023 3");
    CHECK(link_name("notes", false, true, 2) == "Link to notes 2");
    CHECK(link_name("P1.ORF", false, false) == "P1.ORF");
    CHECK(link_name("P1.ORF", false, false, 2) == "P1 2.ORF");
    CHECK(link_name("Trip", true, false, 2) == "Trip 2");
}

TEST_CASE("links: in the items' own folder each is \"Link to X\", absolute, numbered when taken") {
    Tree t;
    Answer answer;
    TransferResult made = make_links(t.a, {"P1.ORF", "Trip", ".bashrc", "Trip.2023", "Days"}, t.a,
                                     answer.callbacks());
    CHECK(made.refused.empty());
    CHECK(made.errors.empty());
    CHECK(made.placed == Names{"Link to P1.ORF", "Link to Trip", "Link to .bashrc",
                               "Link to Trip.2023", "Link to Days"});
    CHECK(made.files_done == 5);
    REQUIRE(made.items.size() == 5);
    CHECK(made.items[0] == PlacedItem{.from = t.a / "P1.ORF", .to = t.a / "Link to P1.ORF"});
    CHECK(fs::read_symlink(t.a / "Link to P1.ORF") == t.a / "P1.ORF");  // absolute
    CHECK(read_file(t.a / "Link to P1.ORF") == "P1.ORF");
    CHECK(fs::read_symlink(t.a / "Link to Trip") == t.a / "Trip");
    CHECK(fs::read_symlink(t.a / "Link to Days") == t.a / "Days");  // the link, not where it goes
    CHECK_FALSE(fs::exists(t.a / "Link to P1.ORF.xmp"));            // sidecars are not linked
    CHECK(read_file(t.a / "Trip" / "x.jpg") == "Trip/x.jpg");

    // Again: the next number, never a question.
    made = make_links(t.a, {"P1.ORF", "Trip", ".bashrc", "Trip.2023", "Days"}, t.a,
                      answer.callbacks());
    CHECK(made.placed == Names{"Link to P1 2.ORF", "Link to Trip 2", "Link to .bashrc 2",
                               "Link to Trip.2023 2", "Link to Days 2"});
    CHECK(answer.asked.empty());
    CHECK(fs::read_symlink(t.a / "Link to P1 2.ORF") == t.a / "P1.ORF");
}

TEST_CASE("links: a link made in another folder of the tree is relative and leads to its item") {
    Tree t;
    const TransferResult made = make_links(t.a, {"P1.ORF", "Trip"}, t.b);
    CHECK(made.errors.empty());
    CHECK(made.placed == Names{"P1.ORF", "Trip"});
    CHECK(fs::read_symlink(t.b / "P1.ORF") == "../a/P1.ORF");
    CHECK(fs::read_symlink(t.b / "Trip") == "../a/Trip");
    CHECK(read_file(t.b / "P1.ORF") == "P1.ORF");
    CHECK(read_file(t.b / "Trip" / "x.jpg") == "Trip/x.jpg");
    CHECK(listing(t.b) == Names{"P1.ORF", "Trip"});  // no sidecar
    REQUIRE(made.items.size() == 2);
    CHECK(made.items[1] == PlacedItem{.from = t.a / "Trip", .to = t.b / "Trip"});

    // The whole tree moved: the links still lead to their items.
    fs::rename(t.dir.path / "disk", t.dir.path / "moved");
    CHECK(read_file(t.dir.path / "moved" / "b" / "P1.ORF") == "P1.ORF");
}

TEST_CASE("links: into a folder reached through a link, the target is relative from where it is") {
    Tree t;
    const fs::path pictures = t.dir.path / "Pictures";  // a ~/Pictures that leads to another disk
    fs::create_directories(t.dir.path / "disk" / "b" / "deep");
    fs::create_directory_symlink(t.dir.path / "disk" / "b" / "deep", pictures);
    const TransferResult made = make_links(t.a, {"P1.ORF"}, pictures);
    CHECK(made.errors.empty());
    CHECK(fs::read_symlink(t.b / "deep" / "P1.ORF") == "../../a/P1.ORF");
    CHECK(read_file(pictures / "P1.ORF") == "P1.ORF");
    CHECK(fs::is_symlink(pictures));
}

TEST_CASE("links: on another file system the target is absolute") {
    std::string pattern = "/dev/shm/nordend-test-XXXXXX";
    if (mkdtemp(pattern.data()) == nullptr) {
        MESSAGE("no /dev/shm: skipped");
        return;
    }
    const fs::path other{pattern};
    Tree t;
    struct stat here{};
    struct stat there{};
    REQUIRE(stat(t.a.c_str(), &here) == 0);
    REQUIRE(stat(other.c_str(), &there) == 0);
    if (here.st_dev != there.st_dev) {
        const TransferResult made = make_links(t.a, {"P1.ORF"}, other);
        CHECK(made.errors.empty());
        CHECK(fs::read_symlink(other / "P1.ORF") == t.a / "P1.ORF");
    } else {
        MESSAGE("/dev/shm on the same file system as the temporary folder: skipped");
    }
    std::error_code ignored;
    fs::remove_all(other, ignored);
}

TEST_CASE("links: taken names are asked; replace a file, skip, keep both, cancel") {
    Tree t;
    std::ofstream{t.b / "P1.ORF"} << "old";
    Answer answer;

    answer.resolution = Resolution::skip;
    TransferResult made = make_links(t.a, {"P1.ORF", "notes.txt"}, t.b, answer.callbacks());
    CHECK(made.placed == Names{"notes.txt"});
    REQUIRE(answer.asked.size() == 1);
    CHECK(answer.asked[0].name == "P1.ORF");
    CHECK(answer.asked[0].destination == t.b);
    CHECK_FALSE(answer.asked[0].directory);
    CHECK_FALSE(answer.asked[0].target_directory);
    CHECK_FALSE(answer.asked[0].merge);
    CHECK(read_file(t.b / "P1.ORF") == "old");

    answer.resolution = Resolution::keep_both;
    made = make_links(t.a, {"P1.ORF"}, t.b, answer.callbacks());
    CHECK(made.placed == Names{"P1 2.ORF"});
    CHECK(fs::read_symlink(t.b / "P1 2.ORF") == "../a/P1.ORF");

    answer.resolution = Resolution::replace;
    made = make_links(t.a, {"P1.ORF"}, t.b, answer.callbacks());
    CHECK(made.placed == Names{"P1.ORF"});
    CHECK(made.replaced == 1);
    CHECK(made.items.empty());  // what it replaced cannot come back: Ctrl+Z leaves it
    CHECK(fs::read_symlink(t.b / "P1.ORF") == "../a/P1.ORF");
    CHECK(listing(t.b) == Names{"P1 2.ORF", "P1.ORF", "notes.txt"});  // nothing hidden left

    answer.resolution = Resolution::cancel;
    made = make_links(t.a, {"notes.txt", "Trip"}, t.b, answer.callbacks());
    CHECK(made.cancelled);
    CHECK(made.placed.empty());
    CHECK_FALSE(fs::exists(t.b / "Trip"));
}

TEST_CASE("links: a link never replaces a folder") {
    Tree t;
    fs::create_directories(t.b / "Trip" / "kept");
    Answer answer{.resolution = Resolution::replace, .asked = {}};
    const TransferResult made = make_links(t.a, {"Trip"}, t.b, answer.callbacks());
    REQUIRE(answer.asked.size() == 1);
    CHECK(answer.asked[0].target_directory);  // the card dims Replace
    CHECK(made.placed.empty());
    CHECK(made.errors == Names{"Trip: a folder has its name; a link does not replace a folder"});
    CHECK(fs::is_directory(t.b / "Trip" / "kept"));
    CHECK_FALSE(fs::is_symlink(t.b / "Trip"));
}

TEST_CASE("links: keep both counts on in the folder opened, even when its path moved meanwhile") {
    Tree t;
    std::ofstream{t.b / "P1.ORF"} << "old";
    std::ofstream{t.b / "P1 2.ORF"} << "old 2";
    const fs::path moved = t.dir.path / "disk" / "b-moved";
    // While the question waits, the folder is renamed and another made under its name.
    const TransferCallbacks callbacks{.progress = nullptr,
                                      .resolve = [&](const Conflict& /*conflict*/) {
                                          fs::rename(t.b, moved);
                                          fs::create_directories(t.b);
                                          return Resolution::keep_both;
                                      }};
    const TransferResult made = make_links(t.a, {"P1.ORF"}, t.b, callbacks);
    CHECK(made.errors.empty());
    CHECK(made.placed == Names{"P1 3.ORF"});
    CHECK(fs::is_symlink(moved / "P1 3.ORF"));
    CHECK(listing(t.b).empty());
}

TEST_CASE("links: what cannot be linked is said, the rest is made") {
    Tree t;
    const std::string long_name(250, 'x');
    std::ofstream{t.a / long_name} << "long";
    const TransferResult made = make_links(t.a, {"gone.txt", long_name, "notes.txt"}, t.a);
    CHECK(made.placed == Names{"Link to notes.txt"});
    REQUIRE(made.errors.size() == 2);
    CHECK(made.errors[0] == "gone.txt: No such file or directory");
    CHECK(made.errors[1] == long_name + ": The name is too long (255 bytes at most)");

    // A destination that is not a folder: nothing is done.
    const TransferResult refused = make_links(t.a, {"notes.txt"}, t.a / "notes.txt");
    CHECK(refused.refused == std::format("{}: Not a directory", (t.a / "notes.txt").string()));
    CHECK(refused.placed.empty());
}

TEST_CASE("links: nothing hidden is left behind when replacing fails") {
    if (geteuid() == 0) {
        return;
    }
    Tree t;
    std::ofstream{t.b / "P1.ORF"} << "old";
    fs::permissions(t.b, fs::perms::owner_read | fs::perms::owner_exec);
    Answer answer{.resolution = Resolution::replace, .asked = {}};
    const TransferResult made = make_links(t.a, {"P1.ORF", "notes.txt"}, t.b, answer.callbacks());
    fs::permissions(t.b, fs::perms::owner_all);
    CHECK(made.placed.empty());
    CHECK(made.errors == Names{"P1.ORF: Permission denied", "notes.txt: Permission denied"});
    CHECK(listing(t.b) == Names{"P1.ORF"});
    CHECK(read_file(t.b / "P1.ORF") == "old");
}

TEST_CASE("links: cancelled between items, what is made stays") {
    Tree t;
    const CancelToken token;
    const TransferCallbacks callbacks{.progress =
                                          [&](const nordend::fs::TransferProgress& progress) {
                                              CHECK(progress.files_total == 3);
                                              if (progress.files_done == 1) {
                                                  token.cancel();
                                              }
                                          },
                                      .resolve = nullptr};
    const TransferResult made =
        make_links(t.a, {"P1.ORF", "notes.txt", "Trip"}, t.b, callbacks, token);
    CHECK(made.cancelled);
    CHECK(made.placed == Names{"P1.ORF"});
    CHECK(listing(t.b) == Names{"P1.ORF"});
}
