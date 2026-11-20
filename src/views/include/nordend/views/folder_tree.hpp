#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/places.hpp"

namespace nordend::views {

// The folders unfolded in the sidebar's tree, one level at a time: a level is read on a worker
// when it is unfolded, kept up to date through the watcher while it stays so (the names the
// events give are stat'ed after settle, as in DirectoryModel), and let go when it is folded.
// Levels are kept by place and folder, so Pictures unfolded under Home does not unfold the
// Pictures place; two places with the same folder share theirs.
//
// Folders that inotify cannot follow (/proc, network mounts) show what they held when read.
//
// Loop thread only. Must not outlive the loop, pool or watcher.
class FolderTree {
public:
    // Per level, hidden ones too: past it, a row says how many more there are.
    static constexpr std::size_t kMaxNames = 2048;
    static constexpr std::chrono::milliseconds kDefaultSettle{30};

    enum class Fold : std::uint8_t {
        folded,    // not read yet: it may hold folders
        unfolded,  // read (or being read), its folders shown
        leaf,      // read: no folders to show
        failed,    // could not be read, or a link back to a folder above it
    };

    struct Row {
        std::filesystem::path folder;  // empty for the "more folders" row
        std::size_t depth{1};          // 1: right under the place
        std::size_t more{0};           // the "more folders" row: how many
    };

    // Why a folder unfolded by hand could not be; empty error for a link that leads back to a
    // folder above it.
    using FailHandler =
        std::function<void(const std::filesystem::path& folder, std::error_code error)>;

    FolderTree(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
               std::chrono::milliseconds settle = kDefaultSettle);
    ~FolderTree();

    // Not copyable or movable: the jobs, timers and watches keep `this`.
    FolderTree(const FolderTree&) = delete;
    FolderTree& operator=(const FolderTree&) = delete;
    FolderTree(FolderTree&&) = delete;
    FolderTree& operator=(FolderTree&&) = delete;

    // A level came, changed or went.
    void set_change_handler(std::function<void()> changed) { changed_ = std::move(changed); }

    void set_fail_handler(FailHandler fail) { fail_ = std::move(fail); }

    // folder is place or a folder in its tree.
    void unfold(const std::filesystem::path& place, const std::filesystem::path& folder);
    // Lets the level go, with every level under it.
    void fold(const std::filesystem::path& place, const std::filesystem::path& folder);

    [[nodiscard]] Fold fold_of(const std::filesystem::path& place,
                               const std::filesystem::path& folder, bool show_hidden) const;

    // The rows under place, depth first, as shown: hidden folders only with show_hidden.
    void rows_under(const std::filesystem::path& place, bool show_hidden,
                    std::vector<Row>& out) const;

    // Lets go of every level in the disk mounted there, before it is unmounted or ejected.
    void release_under(const std::filesystem::path& mount_point);

    // Lets go of the levels of places no longer listed.
    void keep_places(std::span<const fs::Place> places);

    // Levels held (read, being read or failed): for tests and the memory report.
    [[nodiscard]] std::size_t levels() const noexcept { return levels_.size(); }

    // Being read or refreshed, or a refresh waiting to settle: for tests.
    [[nodiscard]] bool busy() const;

private:
    using Key = std::pair<std::filesystem::path, std::filesystem::path>;  // place, folder

    struct Level {
        std::vector<std::string> names;  // natural order
        std::size_t more{0};
        std::uint64_t device{0};
        std::uint64_t inode{0};
        bool loaded{false};
        bool failed{false};  // unreadable, or a link back to a folder above it
        core::CancelToken load;
        core::CancelToken refresh;
        bool refreshing{false};
        fs::DirectoryWatcher::WatchId watch{0};
        core::EventLoop::SourceId settle{0};
        std::set<std::string> dirty;  // names the watcher gave since the last refresh
    };

    void read(const Key& key, bool asked);
    void loaded(const Key& key, fs::Subfolders read, bool asked);
    [[nodiscard]] bool leads_back(const Key& key, const fs::Subfolders& read) const;
    void on_events(const Key& key, const std::vector<fs::DirectoryEvent>& events);
    void schedule_refresh(const Key& key);
    void refresh(const Key& key);
    void apply(const Key& key, const std::vector<std::string>& names,
               const std::vector<std::optional<fs::Entry>>& entries);
    void stop(Level& level);
    // Lets go of the levels pred picks; true when there were any.
    bool release_if(const std::function<bool(const Key& key)>& pred);
    void add_rows(const std::filesystem::path& place, const std::filesystem::path& folder,
                  std::size_t depth, bool show_hidden, std::vector<Row>& out) const;
    void changed() const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::chrono::milliseconds settle_;
    std::function<void()> changed_;
    FailHandler fail_;
    std::map<Key, Level> levels_;
};

}  // namespace nordend::views
