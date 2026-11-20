#include "nordend/fs/directory.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/sort.hpp"
#include "temp_dir.hpp"

using nordend::core::CancelToken;
using nordend::core::EventLoop;
using nordend::core::WorkerPool;
using nordend::fs::Entry;
using nordend::fs::EntryKind;
using nordend::fs::Listing;
using nordend::fs::load_directory;
using nordend::fs::read_directory;
using nordend::fs::read_subfolders;
using nordend::fs::ReadOptions;
using nordend::fs::ReadResult;
using nordend::fs::Subfolders;
using nordend::test::TempDir;
using namespace std::chrono_literals;

namespace {

std::vector<Entry> read_all(const std::filesystem::path& path, ReadResult& result,
                            std::vector<std::size_t>* chunk_sizes = nullptr,
                            const ReadOptions& options = {}) {
    std::vector<Entry> entries;
    result = read_directory(path, CancelToken{}, options, [&](std::vector<Entry> chunk) {
        if (chunk_sizes != nullptr) {
            chunk_sizes->push_back(chunk.size());
        }
        std::ranges::move(chunk, std::back_inserter(entries));
    });
    std::ranges::sort(entries, nordend::fs::SortOrder{});
    return entries;
}

const Entry* find(const std::vector<Entry>& entries, std::string_view name) {
    const auto it = std::ranges::find(entries, name, &Entry::name);
    return it == entries.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("read_directory: kinds, sizes, links, hidden files") {
    const TempDir dir;
    dir.write("hello.txt", "hello");
    dir.write(".hidden", "");
    std::filesystem::create_directory(dir.path / "sub");
    std::filesystem::create_symlink("hello.txt", dir.path / "link-to-file");
    std::filesystem::create_directory_symlink("sub", dir.path / "link-to-dir");
    std::filesystem::create_symlink("missing", dir.path / "dangling");

    ReadResult result;
    const std::vector<Entry> entries = read_all(dir.path, result);
    CHECK_FALSE(result.error);
    CHECK(result.count == 6);
    REQUIRE(entries.size() == 6);

    const Entry* hello = find(entries, "hello.txt");
    REQUIRE(hello != nullptr);
    CHECK(hello->kind == EntryKind::file);
    CHECK(hello->size == 5);
    CHECK(hello->modified_ns > 0);
    CHECK_FALSE(hello->is_link);
    CHECK(find(entries, ".hidden")->hidden());
    CHECK(find(entries, "sub")->is_directory());

    const Entry* link_dir = find(entries, "link-to-dir");
    CHECK(link_dir->is_directory());
    CHECK(link_dir->is_link);
    CHECK(find(entries, "link-to-file")->kind == EntryKind::file);
    CHECK(find(entries, "link-to-file")->size == 5);
    CHECK(find(entries, "dangling")->kind == EntryKind::other);
    CHECK(find(entries, "dangling")->is_link);
}

TEST_CASE("read_directory: chunks grow from first_chunk to max_chunk") {
    const TempDir dir;
    for (int i = 0; i < 100; ++i) {
        dir.write("f" + std::to_string(i), "");
    }
    ReadResult result;
    std::vector<std::size_t> sizes;
    const auto entries = read_all(dir.path, result, &sizes, {.first_chunk = 4, .max_chunk = 16});
    CHECK(entries.size() == 100);
    CHECK(sizes == std::vector<std::size_t>{4, 8, 16, 16, 16, 16, 16, 8});
}

TEST_CASE("read_directory: errors") {
    const TempDir dir;
    ReadResult result;
    CHECK(read_all(dir.path / "absent", result).empty());
    CHECK(result.error == std::errc::no_such_file_or_directory);
    dir.write("file", "");
    CHECK(read_all(dir.path / "file", result).empty());
    CHECK(result.error == std::errc::not_a_directory);
}

TEST_CASE("read_directory: a cancelled read stops and delivers nothing more") {
    const TempDir dir;
    for (int i = 0; i < 500; ++i) {
        dir.write("f" + std::to_string(i), "");
    }
    const CancelToken token;
    std::size_t delivered = 0;
    read_directory(dir.path, token, {.first_chunk = 10, .max_chunk = 10},
                   [&](const std::vector<Entry>& chunk) {
                       delivered += chunk.size();
                       token.cancel();
                   });
    CHECK(delivered == 10);
}

TEST_CASE("read_subfolders: folders and links to them, in natural order, hidden ones kept") {
    const TempDir dir;
    for (const char* name : {"img10", "img2", "Sub", ".hidden"}) {
        std::filesystem::create_directory(dir.path / name);
    }
    dir.write("file.txt", "");
    std::filesystem::create_directory_symlink("img2", dir.path / "link-to-dir");
    std::filesystem::create_symlink("file.txt", dir.path / "link-to-file");
    std::filesystem::create_symlink("missing", dir.path / "dangling");

    const Subfolders read = read_subfolders(dir.path, CancelToken{}, 100);
    CHECK_FALSE(read.error);
    CHECK(read.names == std::vector<std::string>{".hidden", "img2", "img10", "link-to-dir", "Sub"});
    CHECK(read.more == 0);
}

TEST_CASE("read_subfolders: past the limit, the rest is counted") {
    const TempDir dir;
    for (const char* name : {"e", "d", "c", "b", "a"}) {
        std::filesystem::create_directory(dir.path / name);
    }
    const Subfolders read = read_subfolders(dir.path, CancelToken{}, 3);
    CHECK(read.names == std::vector<std::string>{"a", "b", "c"});
    CHECK(read.more == 2);
}

TEST_CASE("read_subfolders: errors, and the folder it opened") {
    const TempDir dir;
    CHECK(read_subfolders(dir.path / "absent", CancelToken{}, 10).error ==
          std::errc::no_such_file_or_directory);
    dir.write("file", "");
    CHECK(read_subfolders(dir.path / "file", CancelToken{}, 10).error ==
          std::errc::not_a_directory);

    std::filesystem::create_directory(dir.path / "real");
    std::filesystem::create_directory_symlink("real", dir.path / "link");
    struct stat real{};
    REQUIRE(stat((dir.path / "real").c_str(), &real) == 0);
    for (const char* name : {"real", "link"}) {
        const Subfolders read = read_subfolders(dir.path / name, CancelToken{}, 10);
        CHECK_FALSE(read.error);
        CHECK(read.device == real.st_dev);
        CHECK(read.inode == real.st_ino);
    }
}

TEST_CASE("read_subfolders: a cancelled read gives nothing") {
    const TempDir dir;
    for (int i = 0; i < 200; ++i) {
        std::filesystem::create_directory(dir.path / ("d" + std::to_string(i)));
    }
    const CancelToken token;
    token.cancel();
    const Subfolders read = read_subfolders(dir.path, token, 1000);
    CHECK_FALSE(read.error);
    CHECK(read.names.empty());
    CHECK(read.more == 0);
}

TEST_CASE("load_directory: chunks on the loop thread, then done, into a sorted Listing") {
    const TempDir dir;
    for (int i = 0; i < 300; ++i) {
        dir.write("IMG_" + std::to_string(i) + ".ORF", "");
    }
    EventLoop loop;
    loop.add_timer(10s, 0ms, [&loop] {
        FAIL_CHECK("event loop watchdog fired");
        loop.quit(-1);
    });
    WorkerPool pool{loop, 2};
    Listing listing;
    int chunks = 0;
    bool done_after_chunks = false;
    load_directory(
        pool, dir.path,
        [&](std::vector<Entry> chunk) {
            ++chunks;
            listing.add(std::move(chunk));
        },
        [&](const ReadResult& result) {
            CHECK_FALSE(result.error);
            CHECK(result.count == 300);
            done_after_chunks = listing.entries().size() == 300;
            loop.quit(0);
        },
        {.first_chunk = 16, .max_chunk = 64});
    CHECK(loop.run() == 0);
    CHECK(done_after_chunks);
    CHECK(chunks == 6);  // 16 32 64 64 64 60
    CHECK(listing.entries().front().name == "IMG_0.ORF");
    CHECK(listing.entries().back().name == "IMG_299.ORF");
}
