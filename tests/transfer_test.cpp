#include "nordend/fs/transfer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/transfer_queue.hpp"
#include "temp_dir.hpp"

using nordend::core::CancelToken;
using nordend::core::EventLoop;
using nordend::fs::Conflict;
using nordend::fs::PlacedItem;
using nordend::fs::Resolution;
using nordend::fs::TransferCallbacks;
using nordend::fs::TransferKind;
using nordend::fs::TransferProgress;
using nordend::fs::TransferQueue;
using nordend::fs::TransferRequest;
using nordend::fs::TransferResult;
using nordend::test::TempDir;
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
    const auto watchdog = loop.add_timer(10s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

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

struct stat stat_of(const fs::path& path) {
    struct stat info{};
    REQUIRE(lstat(path.c_str(), &info) == 0);
    return info;
}

// Answers every conflict the same way, counting them.
struct Answer {
    Resolution resolution{Resolution::skip};
    int asked{0};

    TransferCallbacks callbacks() {
        return {.progress = nullptr, .resolve = [this](const Conflict& /*conflict*/) {
                    ++asked;
                    return resolution;
                }};
    }
};

// from/{P1.ORF, P1.ORF.xmp, P1_01.ORF.xmp, notes.txt, link -> notes.txt, Trip/{a.jpg, deep/b.jpg}}
// and an empty to/.
struct Tree {
    TempDir dir;
    fs::path from = dir.path / "from";
    fs::path to = dir.path / "to";

    Tree() {
        fs::create_directories(from / "Trip" / "deep");
        fs::create_directories(to);
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "notes.txt", "Trip/a.jpg",
                                 "Trip/deep/b.jpg"}) {
            std::ofstream{from / name} << name;
        }
        fs::create_symlink("notes.txt", from / "link");
        // An old date and odd permissions, to see them kept.
        const std::array<timespec, 2> times{timespec{.tv_sec = 1'000'000'000, .tv_nsec = 5},
                                            timespec{.tv_sec = 1'100'000'000, .tv_nsec = 7}};
        REQUIRE(utimensat(AT_FDCWD, (from / "P1.ORF").c_str(), times.data(), 0) == 0);
        fs::permissions(from / "notes.txt",
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    }

    TransferResult run(TransferKind kind, Names names, const TransferCallbacks& callbacks = {},
                       bool force_copy = false, const CancelToken& token = {}) {
        return nordend::fs::run_transfer({.kind = kind,
                                          .source = from,
                                          .names = std::move(names),
                                          .destination = to,
                                          .force_copy = force_copy},
                                         token, callbacks);
    }
};

// A folder on /dev/shm, removed with everything in it.
struct Removed {
    fs::path path;

    explicit Removed(fs::path made) : path{std::move(made)} {}

    Removed(const Removed&) = delete;
    Removed& operator=(const Removed&) = delete;
    Removed(Removed&&) = delete;
    Removed& operator=(Removed&&) = delete;

    ~Removed() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

}  // namespace

TEST_CASE("transfer: a copy keeps contents, dates, permissions, links; sidecars come along") {
    Tree t;
    std::vector<TransferProgress> seen;
    const TransferResult result =
        t.run(TransferKind::copy, {"P1.ORF", "notes.txt", "link", "Trip"},
              {.progress = [&](const TransferProgress& progress) { seen.push_back(progress); },
               .resolve = nullptr});
    CHECK(result.refused.empty());
    CHECK(result.errors.empty());
    CHECK(result.placed == Names{"P1.ORF", "notes.txt", "link", "Trip"});
    CHECK(listing(t.to) ==
          Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "Trip", "link", "notes.txt"});
    CHECK(listing(t.to / "Trip") == Names{"a.jpg", "deep"});
    CHECK(read_file(t.to / "Trip" / "deep" / "b.jpg") == "Trip/deep/b.jpg");
    CHECK(read_file(t.to / "P1_01.ORF.xmp") == "P1_01.ORF.xmp");
    CHECK(fs::read_symlink(t.to / "link") == "notes.txt");
    CHECK(stat_of(t.to / "P1.ORF").st_mtim.tv_sec == 1'100'000'000);
    CHECK(stat_of(t.to / "P1.ORF").st_mtim.tv_nsec == 7);
    CHECK((stat_of(t.to / "notes.txt").st_mode & 0777) == 0640);
    CHECK(listing(t.from).size() == 6);  // the sources stay

    // Progress: adding up first, then every file, all of the bytes.
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().preparing);
    CHECK(seen.back().files_done == 7);
    CHECK(seen.back().files_total == 7);
    CHECK(seen.back().bytes_done == seen.back().bytes_total);
    CHECK(seen.back().bytes_total > 0);
}

TEST_CASE("transfer: a move on one disk renames; between disks it copies, then deletes") {
    Tree t;
    const ino_t inode = stat_of(t.from / "P1.ORF").st_ino;
    TransferResult result = t.run(TransferKind::move, {"P1.ORF", "Trip"});
    CHECK(result.errors.empty());
    CHECK(stat_of(t.to / "P1.ORF").st_ino == inode);  // the same file: renamed
    CHECK(listing(t.to) == Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "Trip"});
    CHECK(listing(t.from) == Names{"link", "notes.txt"});

    // The path between disks, forced: a new file, the old one gone, the folder too.
    Tree u;
    const ino_t before = stat_of(u.from / "P1.ORF").st_ino;
    result = u.run(TransferKind::move, {"P1.ORF", "Trip", "link"}, {}, true);
    CHECK(result.errors.empty());
    CHECK(stat_of(u.to / "P1.ORF").st_ino != before);
    CHECK(stat_of(u.to / "P1.ORF").st_mtim.tv_sec == 1'100'000'000);
    CHECK(read_file(u.to / "Trip" / "deep" / "b.jpg") == "Trip/deep/b.jpg");
    CHECK(fs::is_symlink(u.to / "link"));
    CHECK(listing(u.from) == Names{"notes.txt"});

    // Into the folder it is in: nothing to do.
    result = nordend::fs::run_transfer({.kind = TransferKind::move,
                                        .source = u.from,
                                        .names = {"notes.txt"},
                                        .destination = u.from},
                                       {}, {});
    CHECK(result.placed == Names{"notes.txt"});
}

TEST_CASE("transfer: between two file systems the bytes go through read and write") {
    // /dev/shm is a tmpfs of its own: copy_file_range refuses (EXDEV), the fallback copies.
    std::string pattern = "/dev/shm/nordend-test-XXXXXX";
    if (mkdtemp(pattern.data()) == nullptr) {
        MESSAGE("no /dev/shm: skipped");
        return;
    }

    const Removed other_disk{fs::path{pattern}};
    Tree t;
    if (stat_of(other_disk.path).st_dev == stat_of(t.from).st_dev) {
        MESSAGE("/dev/shm on the same file system as the temporary folder: skipped");
        return;
    }
    // Several buffers and a piece: every byte different from its neighbours.
    std::string data(std::size_t{1} << 20U, '\0');
    data += "-a tail";
    for (std::size_t i = 0; i < data.size() - 7; ++i) {
        data[i] = static_cast<char>((i * 31U) % 251U);
    }
    std::ofstream{t.from / "big.raw", std::ios::binary} << data;

    std::vector<TransferProgress> seen;
    const TransferResult result = nordend::fs::run_transfer(
        {.kind = TransferKind::copy,
         .source = t.from,
         .names = {"big.raw"},
         .destination = other_disk.path,
         .force_copy = false},
        {},
        {.progress = [&](const TransferProgress& p) { seen.push_back(p); }, .resolve = nullptr});
    CHECK(result.errors.empty());
    CHECK(read_file(other_disk.path / "big.raw") == data);
    CHECK(listing(other_disk.path) == Names{"big.raw"});  // no temporary left
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back().bytes_done == data.size());
}

TEST_CASE("transfer: loose files copied at once, each with its sidecars, in the order asked") {
    TempDir dir;
    const fs::path from = dir.path / "from";
    const fs::path to = dir.path / "to";
    fs::create_directories(from);
    fs::create_directories(to);
    Names names;
    for (int i = 0; i < 120; ++i) {
        const std::string photo = std::format("P{:03}.ORF", i);
        std::ofstream{from / photo} << std::string(std::size_t{4096} + (std::size_t(i) * 97U),
                                                   static_cast<char>('a' + (i % 26)));
        if (i % 3 == 0) {
            std::ofstream{from / (photo + ".xmp")} << "sidecar of " << photo;
        }
        names.push_back(photo);
    }
    std::ranges::reverse(names);  // not the folder's order
    const TransferResult result = nordend::fs::run_transfer({.kind = TransferKind::copy,
                                                             .source = from,
                                                             .names = names,
                                                             .destination = to,
                                                             .force_copy = false,
                                                             .threads = 8},
                                                            {}, {});
    CHECK(result.errors.empty());
    CHECK(result.placed == names);
    REQUIRE(result.items.size() == names.size());
    CHECK(result.items.front().to == to / names.front());
    CHECK(listing(to).size() == 160);  // 120 photos, 40 sidecars
    for (int i = 0; i < 120; i += 7) {
        const std::string photo = std::format("P{:03}.ORF", i);
        CHECK(read_file(to / photo) == read_file(from / photo));
    }
    CHECK(read_file(to / "P099.ORF.xmp") == "sidecar of P099.ORF");
    CHECK(result.files_done == 160);
}

TEST_CASE("transfer: a name kept for one file still being copied is not given to another") {
    // "x" is there: the new "x" becomes "x 2" while it is copied; "x 2" then finds its name
    // taken although nothing is on the disk yet, and becomes "x 2 2".
    Tree t;
    std::ofstream{t.from / "x"} << std::string(std::size_t{1} << 20U, 'x');
    std::ofstream{t.from / "x 2"} << "the second";
    std::ofstream{t.to / "x"} << "there before";
    Answer answer{.resolution = Resolution::keep_both};
    const TransferResult result = nordend::fs::run_transfer({.kind = TransferKind::copy,
                                                             .source = t.from,
                                                             .names = {"x", "x 2"},
                                                             .destination = t.to,
                                                             .force_copy = false,
                                                             .threads = 8},
                                                            {}, answer.callbacks());
    CHECK(result.errors.empty());
    CHECK(answer.asked == 2);
    CHECK(result.placed == Names{"x 2", "x 2 2"});
    CHECK(read_file(t.to / "x") == "there before");
    CHECK(fs::file_size(t.to / "x 2") == std::size_t{1} << 20U);
    CHECK(read_file(t.to / "x 2 2") == "the second");
}

TEST_CASE("transfer: conflicts are asked; replace, skip, keep both, cancel") {
    Tree t;
    std::ofstream{t.to / "notes.txt"} << "old notes";
    std::ofstream{t.to / "P1.ORF.xmp"} << "someone else's sidecar";
    Answer skip{.resolution = Resolution::skip};
    TransferResult result = t.run(TransferKind::copy, {"notes.txt", "P1.ORF"}, skip.callbacks());
    CHECK(skip.asked == 2);  // P1.ORF too: its sidecar's name is taken
    CHECK(result.placed.empty());
    CHECK(read_file(t.to / "notes.txt") == "old notes");

    Answer both{.resolution = Resolution::keep_both};
    result = t.run(TransferKind::copy, {"notes.txt", "P1.ORF"}, both.callbacks());
    CHECK(result.placed == Names{"notes 2.txt", "P1 2.ORF"});
    CHECK(read_file(t.to / "P1 2.ORF.xmp") == "P1.ORF.xmp");  // the sidecars follow the new name
    CHECK(read_file(t.to / "P1 2_01.ORF.xmp") == "P1_01.ORF.xmp");

    Answer replace{.resolution = Resolution::replace};
    result = t.run(TransferKind::copy, {"notes.txt", "P1.ORF"}, replace.callbacks());
    CHECK(result.errors.empty());
    CHECK(read_file(t.to / "notes.txt") == "notes.txt");
    CHECK(read_file(t.to / "P1.ORF.xmp") == "P1.ORF.xmp");

    Answer cancel{.resolution = Resolution::cancel};
    result = t.run(TransferKind::copy, {"notes.txt", "Trip"}, cancel.callbacks());
    CHECK(result.cancelled);
    CHECK_FALSE(fs::exists(t.to / "Trip"));  // the next item never started
}

TEST_CASE("transfer: folders merge on replace; a file never replaces a folder") {
    Tree t;
    fs::create_directories(t.to / "Trip");
    std::ofstream{t.to / "Trip" / "a.jpg"} << "old";
    std::ofstream{t.to / "Trip" / "mine.jpg"} << "mine";
    Answer replace{.resolution = Resolution::replace};
    TransferResult result = t.run(TransferKind::copy, {"Trip"}, replace.callbacks());
    CHECK(result.errors.empty());
    CHECK(listing(t.to / "Trip") == Names{"a.jpg", "deep", "mine.jpg"});
    CHECK(read_file(t.to / "Trip" / "a.jpg") == "Trip/a.jpg");

    fs::create_directories(t.to / "x");
    fs::rename(t.to / "x", t.to / "link");  // a folder where the link would go
    result = t.run(TransferKind::copy, {"link"}, replace.callbacks());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors.front() == "link: a folder has its name; a file does not replace a folder");
    CHECK(fs::is_directory(t.to / "link"));
}

TEST_CASE("transfer: inside a merge each taken file is asked; photos keep their sidecars") {
    for (const std::size_t threads : {std::size_t{1}, std::size_t{8}}) {
        for (const bool between_disks : {false, true}) {
            CAPTURE(threads);
            CAPTURE(between_disks);
            Tree t;
            for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp"}) {
                fs::copy_file(t.from / name, t.from / "Trip" / name);
            }
            fs::create_directories(t.to / "Trip" / "deep");
            std::ofstream{t.to / "Trip" / "P1.ORF"} << "old P1";
            std::ofstream{t.to / "Trip" / "deep" / "b.jpg"} << "old b";
            std::ofstream{t.to / "Trip" / "deep" / "mine.jpg"} << "mine";
            std::vector<std::string> asked;
            const TransferCallbacks callbacks{
                .progress = nullptr, .resolve = [&](const Conflict& conflict) {
                    asked.push_back(
                        (conflict.destination / conflict.name).lexically_relative(t.to).string());
                    if (conflict.name == "Trip") {
                        return Resolution::replace;  // merge
                    }
                    return conflict.name == "P1.ORF" ? Resolution::keep_both : Resolution::replace;
                }};
            const TransferResult result = nordend::fs::run_transfer(
                {.kind = between_disks ? TransferKind::move : TransferKind::copy,
                 .source = t.from,
                 .names = {"Trip"},
                 .destination = t.to,
                 .force_copy = between_disks,
                 .threads = threads},
                {}, callbacks);
            CHECK(result.errors.empty());
            // Trip itself, then its files only: deep merged without a question.
            CHECK(asked == Names{"Trip", "Trip/P1.ORF", "Trip/deep/b.jpg"});
            CHECK(listing(t.to / "Trip") ==
                  Names{"P1 2.ORF", "P1 2.ORF.xmp", "P1 2_01.ORF.xmp", "P1.ORF", "a.jpg", "deep"});
            CHECK(read_file(t.to / "Trip" / "P1.ORF") == "old P1");
            CHECK(read_file(t.to / "Trip" / "P1 2.ORF.xmp") == "P1.ORF.xmp");
            CHECK(read_file(t.to / "Trip" / "deep" / "b.jpg") == "Trip/deep/b.jpg");
            CHECK(read_file(t.to / "Trip" / "deep" / "mine.jpg") == "mine");
            CHECK(result.placed == Names{"Trip"});
            CHECK(result.replaced == 1);  // deep/b.jpg
            // What an undo takes away: what was not there before, never the folders that were.
            std::vector<std::string> made;
            made.reserve(result.items.size());
            for (const PlacedItem& item : result.items) {
                made.push_back(item.to.lexically_relative(t.to).string());
            }
            std::ranges::sort(made);
            CHECK(made == Names{"Trip/P1 2.ORF", "Trip/a.jpg"});
            if (between_disks) {
                CHECK_FALSE(fs::exists(t.from / "Trip"));  // all of it moved
            }
        }
    }
}

TEST_CASE("transfer: a copy into its own folder keeps both by itself") {
    Tree t;
    Answer never{.resolution = Resolution::cancel};
    const TransferResult result = nordend::fs::run_transfer({.kind = TransferKind::copy,
                                                             .source = t.from,
                                                             .names = {"P1.ORF", "Trip"},
                                                             .destination = t.from},
                                                            {}, never.callbacks());
    CHECK(never.asked == 0);
    CHECK(result.placed == Names{"P1 2.ORF", "Trip 2"});
    CHECK(fs::exists(t.from / "P1 2.ORF.xmp"));
    CHECK(fs::exists(t.from / "Trip 2" / "deep" / "b.jpg"));
}

TEST_CASE(
    "transfer: a folder reached through a link is followed, the links copied into it are not") {
    Tree t;
    const fs::path pictures = t.dir.path / "Pictures";  // a ~/Pictures on another disk
    fs::create_directory_symlink(t.to, pictures);
    const auto into = [&t](TransferKind kind, Names names, const fs::path& destination) {
        return nordend::fs::run_transfer(
            {.kind = kind, .source = t.from, .names = std::move(names), .destination = destination},
            {}, {});
    };
    TransferResult result = into(TransferKind::copy, {"P1.ORF", "link"}, pictures);
    CHECK(result.refused.empty());
    CHECK(result.placed == Names{"P1.ORF", "link"});
    CHECK(listing(t.to) == Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "link"});
    CHECK(fs::read_symlink(t.to / "link") == "notes.txt");
    CHECK(fs::is_symlink(pictures));

    result = into(TransferKind::move, {"notes.txt"}, pictures);
    CHECK(result.refused.empty());
    CHECK(read_file(t.to / "notes.txt") == "notes.txt");
    CHECK_FALSE(fs::exists(t.from / "notes.txt"));

    // Through a link, a folder inside the one copied is still inside it.
    fs::create_directory_symlink(t.from / "Trip" / "deep", t.dir.path / "Deep");
    result = into(TransferKind::copy, {"Trip"}, t.dir.path / "Deep");
    CHECK(result.refused == "Cannot copy Trip into itself");

    // A link that leads nowhere is not a folder.
    fs::create_symlink(t.dir.path / "gone", t.dir.path / "Gone");
    result = into(TransferKind::copy, {"P1.ORF"}, t.dir.path / "Gone");
    CHECK(result.refused == std::format("{} is not a folder", (t.dir.path / "Gone").string()));
}

TEST_CASE("transfer: a move out of a folder reached through a link is a rename on its disk") {
    std::string pattern = "/dev/shm/nordend-test-XXXXXX";
    if (mkdtemp(pattern.data()) == nullptr) {
        MESSAGE("no /dev/shm: skipped");
        return;
    }
    const Removed other_disk{fs::path{pattern}};
    const TempDir here;
    if (stat_of(other_disk.path).st_dev == stat_of(here.path).st_dev) {
        MESSAGE("/dev/shm on the same file system as the temporary folder: skipped");
        return;
    }
    fs::create_directories(other_disk.path / "from");
    fs::create_directories(other_disk.path / "to");
    std::ofstream{other_disk.path / "from" / "P1.ORF"} << "P1.ORF";
    const ino_t inode = stat_of(other_disk.path / "from" / "P1.ORF").st_ino;
    fs::create_directory_symlink(other_disk.path / "from", here.path / "Camera");

    const TransferResult result = nordend::fs::run_transfer({.kind = TransferKind::move,
                                                             .source = here.path / "Camera",
                                                             .names = {"P1.ORF"},
                                                             .destination = other_disk.path / "to"},
                                                            {}, {});
    CHECK(result.refused.empty());
    CHECK(result.errors.empty());
    CHECK(stat_of(other_disk.path / "to" / "P1.ORF").st_ino == inode);  // renamed, not copied
}

TEST_CASE("transfer: refused before anything moves") {
    Tree t;
    TransferResult result = nordend::fs::run_transfer({.kind = TransferKind::copy,
                                                       .source = t.from,
                                                       .names = {"Trip"},
                                                       .destination = t.from / "Trip" / "deep"},
                                                      {}, {});
    CHECK(result.refused == "Cannot copy Trip into itself");
    CHECK(listing(t.from / "Trip" / "deep") == Names{"b.jpg"});

    // A sparse file 1 GiB larger than the free space: its size counts, so it does not fit. Not a
    // fixed huge size: ext4 refuses files over 16 TiB, tmpfs does not.
    {
        std::ofstream{t.from / "huge.raw"};
    }
    fs::resize_file(t.from / "huge.raw",
                    fs::space(t.to).available + (std::uintmax_t{1} << 30U));  // no blocks
    result = t.run(TransferKind::copy, {"huge.raw"});
    CHECK(result.refused.starts_with("Not enough space: "));
    CHECK(listing(t.to).empty());
}

TEST_CASE("transfer: cancelled halfway, nothing half-written is left") {
    Tree t;
    {
        std::ofstream big{t.from / "big.bin"};
        const std::string block(1 << 20, 'x');
        for (int i = 0; i < 40; ++i) {
            big << block;  // 40 MB: five pieces of 8 MB
        }
    }
    const CancelToken token;
    const TransferResult result = t.run(TransferKind::move, {"big.bin", "Trip"},
                                        {.progress =
                                             [&](const TransferProgress& progress) {
                                                 if (progress.bytes_done > 0) {
                                                     token.cancel();  // after the first piece
                                                 }
                                             },
                                         .resolve = nullptr},
                                        true, token);
    CHECK(result.cancelled);
    CHECK(listing(t.to).empty());                            // no temporary, no partial file
    CHECK(fs::file_size(t.from / "big.bin") == 40U << 20U);  // the source is whole
}

TEST_CASE("transfer: a folder's files go straight in; cancelled deep inside, nothing is left") {
    Tree t;
    {
        std::ofstream big{t.from / "Trip" / "deep" / "big.bin"};
        const std::string block(1 << 20, 'x');
        for (int i = 0; i < 40; ++i) {
            big << block;  // 40 MB: five pieces of 8 MB
        }
    }
    const CancelToken token;
    TransferResult result = t.run(TransferKind::copy, {"Trip"},
                                  {.progress =
                                       [&](const TransferProgress& progress) {
                                           if (progress.bytes_done > (1U << 20U)) {
                                               token.cancel();  // inside big.bin
                                           }
                                       },
                                   .resolve = nullptr},
                                  false, token);
    CHECK(result.cancelled);
    CHECK(listing(t.to).empty());  // the hidden folder went, with all in it

    // Whole: every name its own, no hidden temporary anywhere, the dates kept.
    const std::array<timespec, 2> times{timespec{.tv_sec = 1'200'000'000, .tv_nsec = 0},
                                        timespec{.tv_sec = 1'300'000'000, .tv_nsec = 0}};
    REQUIRE(utimensat(AT_FDCWD, (t.from / "Trip" / "deep").c_str(), times.data(), 0) == 0);
    result = t.run(TransferKind::copy, {"Trip"});
    CHECK(result.errors.empty());
    Names names;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator{t.to}) {
        names.push_back(fs::relative(entry.path(), t.to).string());
    }
    std::ranges::sort(names);
    CHECK(names ==
          Names{"Trip", "Trip/a.jpg", "Trip/deep", "Trip/deep/b.jpg", "Trip/deep/big.bin"});
    CHECK(fs::file_size(t.to / "Trip" / "deep" / "big.bin") == 40U << 20U);
    CHECK(stat_of(t.to / "Trip" / "deep").st_mtim.tv_sec == 1'300'000'000);
}

TEST_CASE("transfer: a folder moved between disks: what did not arrive stays, with its folders") {
    if (geteuid() == 0) {
        return;  // root reads anything
    }
    for (const std::size_t threads : {std::size_t{1}, std::size_t{8}}) {
        CAPTURE(threads);
        Tree t;
        for (int i = 0; i < 40; ++i) {
            t.dir.write(std::format("from/Trip/deep/n{:02}.jpg", i), std::format("photo {}", i));
        }
        const fs::path locked = t.from / "Trip" / "deep" / "locked.jpg";
        std::ofstream{locked} << "unreadable";
        fs::permissions(locked, fs::perms::none);
        const TransferResult result = nordend::fs::run_transfer({.kind = TransferKind::move,
                                                                 .source = t.from,
                                                                 .names = {"Trip"},
                                                                 .destination = t.to,
                                                                 .force_copy = true,
                                                                 .threads = threads},
                                                                {}, {});
        fs::permissions(locked, fs::perms::owner_all);
        REQUIRE(result.errors.size() == 1);
        CHECK(result.errors.front() == "Trip/deep/locked.jpg: Permission denied");
        // Everything else arrived and left; the file that did not stays, with its folders.
        CHECK(listing(t.to / "Trip" / "deep").size() == 41);  // b.jpg and the 40
        CHECK(listing(t.from / "Trip") == Names{"deep"});
        CHECK(listing(t.from / "Trip" / "deep") == Names{"locked.jpg"});
        CHECK(result.files_done == 42);  // a.jpg, b.jpg, the 40
    }
}

TEST_CASE("transfer: a folder without write permission gives errors, not a crash") {
    if (geteuid() == 0) {
        return;
    }
    Tree t;
    fs::permissions(t.to, fs::perms::owner_read | fs::perms::owner_exec);
    const TransferResult result = t.run(TransferKind::copy, {"notes.txt", "Trip"});
    fs::permissions(t.to, fs::perms::owner_all);
    REQUIRE(result.errors.size() == 2);
    CHECK(result.errors.front() == "notes.txt: Permission denied");
    CHECK(result.placed.empty());
}

TEST_CASE("transfer queue: progress, a question answered later, the result") {
    Tree t;
    std::ofstream{t.to / "notes.txt"} << "old";
    EventLoop loop;
    int progress_seen = 0;
    std::optional<std::function<void(Resolution)>> answer;
    std::optional<TransferResult> done;
    TransferQueue queue{loop,
                        [&](const TransferRequest& /*request*/,
                            const TransferProgress& /*progress*/) { ++progress_seen; },
                        [&](const Conflict& conflict, std::function<void(Resolution)> reply) {
                            CHECK(conflict.name == "notes.txt");
                            answer = std::move(reply);
                        },
                        [&](const TransferRequest& request, const TransferResult& result) {
                            CHECK(request.names == Names{"notes.txt", "Trip"});
                            done = result;
                        }};
    queue.submit({.kind = TransferKind::copy,
                  .source = t.from,
                  .names = {"notes.txt", "Trip"},
                  .destination = t.to});
    CHECK(queue.pending() == 1);
    REQUIRE(run_until(loop, [&] { return answer.has_value(); }));
    CHECK_FALSE(done.has_value());  // waiting for the answer
    answer.value_or(std::function<void(Resolution)>{})(Resolution::keep_both);
    REQUIRE(run_until(loop, [&] { return done.has_value(); }));
    CHECK(queue.pending() == 0);
    CHECK(done.value_or(TransferResult{}).placed == Names{"notes 2.txt", "Trip"});
    CHECK(progress_seen > 0);

    // Gone while a question waits: the transfer is cancelled and the thread joins.
    answer.reset();
    auto second = std::make_unique<TransferQueue>(
        loop, nullptr,
        [&](const Conflict& /*conflict*/, std::function<void(Resolution)> reply) {
            answer = std::move(reply);
        },
        nullptr);
    second->submit({.kind = TransferKind::copy,
                    .source = t.from,
                    .names = {"notes.txt"},
                    .destination = t.to});
    REQUIRE(run_until(loop, [&] { return answer.has_value(); }));
    second.reset();
    answer.value_or(std::function<void(Resolution)>{})(Resolution::replace);  // too late
    CHECK(read_file(t.to / "notes.txt") == "old");
}

TEST_CASE("transfer queue: a link request runs and reports like the others") {
    Tree t;
    EventLoop loop;
    std::optional<TransferResult> done;
    TransferQueue queue{loop, nullptr, nullptr,
                        [&](const TransferRequest& request, const TransferResult& result) {
                            CHECK(request.kind == TransferKind::link);
                            done = result;
                        }};
    queue.submit(
        {.kind = TransferKind::link, .source = t.from, .names = {"P1.ORF"}, .destination = t.to});
    REQUIRE(run_until(loop, [&] { return done.has_value(); }));
    CHECK(done.value_or(TransferResult{}).placed == Names{"P1.ORF"});
    CHECK(fs::read_symlink(t.to / "P1.ORF") == "../from/P1.ORF");
    CHECK(listing(t.to) == Names{"P1.ORF"});  // the sidecars stay with the photo
}
