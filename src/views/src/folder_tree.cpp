#include "nordend/views/folder_tree.hpp"

#include <algorithm>

#include "nordend/core/log.hpp"
#include "nordend/fs/entry.hpp"
#include "nordend/fs/sort.hpp"

namespace nordend::views {
namespace {

// path is inside (or is) folder, comparing whole components. Both are built the same way (a
// place, then names), so no normalising is needed.
bool within(const std::filesystem::path& path, const std::filesystem::path& folder) {
    auto p = path.begin();
    for (auto f = folder.begin(); f != folder.end(); ++f, ++p) {
        if (p == path.end() || *p != *f) {
            return false;
        }
    }
    return true;
}

bool natural_less(const std::string& a, const std::string& b) {
    return fs::natural_compare(a, b) < 0;
}

}  // namespace

FolderTree::FolderTree(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                       std::chrono::milliseconds settle)
    : loop_{loop}, pool_{pool}, watcher_{watcher}, settle_{settle} {}

FolderTree::~FolderTree() {
    for (auto& [key, level] : levels_) {
        stop(level);
    }
}

void FolderTree::unfold(const std::filesystem::path& place, const std::filesystem::path& folder) {
    const Key key{place, folder};
    if (levels_.contains(key)) {
        return;
    }
    Level& level = levels_[key];
    std::error_code error;
    level.watch = watcher_.watch(
        folder,
        [this, key](const std::vector<fs::DirectoryEvent>& events) { on_events(key, events); },
        error);
    if (error == std::errc::no_space_on_device) {
        // The per-user inotify watch limit: show the level anyway, without live updates.
        log::warn("{}: cannot watch for changes (inotify watch limit reached)", folder.string());
    } else if (error) {
        level.failed = true;
        changed();
        if (fail_) {
            fail_(folder, error);
        }
        return;
    }
    read(key, true);
    changed();
}

void FolderTree::fold(const std::filesystem::path& place, const std::filesystem::path& folder) {
    if (release_if(
            [&](const Key& key) { return key.first == place && within(key.second, folder); })) {
        changed();
    }
}

FolderTree::Fold FolderTree::fold_of(const std::filesystem::path& place,
                                     const std::filesystem::path& folder, bool show_hidden) const {
    const auto it = levels_.find(Key{place, folder});
    if (it == levels_.end()) {
        return Fold::folded;
    }
    const Level& level = it->second;
    if (level.failed) {
        return Fold::failed;
    }
    if (!level.loaded || level.more > 0) {
        return Fold::unfolded;
    }
    const bool shown = std::ranges::any_of(level.names, [show_hidden](const std::string& name) {
        return show_hidden || !name.starts_with('.');
    });
    return shown ? Fold::unfolded : Fold::leaf;
}

void FolderTree::rows_under(const std::filesystem::path& place, bool show_hidden,
                            std::vector<Row>& out) const {
    add_rows(place, place, 1, show_hidden, out);
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as the levels unfolded
void FolderTree::add_rows(const std::filesystem::path& place, const std::filesystem::path& folder,
                          std::size_t depth, bool show_hidden, std::vector<Row>& out) const {
    const auto it = levels_.find(Key{place, folder});
    if (it == levels_.end() || !it->second.loaded) {
        return;
    }
    for (const std::string& name : it->second.names) {
        if (!show_hidden && name.starts_with('.')) {
            continue;
        }
        std::filesystem::path child = folder / name;
        out.push_back({.folder = child, .depth = depth});
        add_rows(place, child, depth + 1, show_hidden, out);
    }
    if (it->second.more > 0) {
        out.push_back({.folder = {}, .depth = depth, .more = it->second.more});
    }
}

void FolderTree::release_under(const std::filesystem::path& mount_point) {
    if (mount_point.empty()) {
        return;
    }
    if (release_if([&](const Key& key) { return within(key.second, mount_point); })) {
        changed();
    }
}

void FolderTree::keep_places(std::span<const fs::Place> places) {
    if (release_if([places](const Key& key) {
            return std::ranges::find(places, key.first, &fs::Place::path) == places.end();
        })) {
        changed();
    }
}

bool FolderTree::busy() const {
    return std::ranges::any_of(levels_, [](const auto& entry) {
        const Level& level = entry.second;
        return (!level.loaded && !level.failed) || level.refreshing || level.settle != 0 ||
               !level.dirty.empty();
    });
}

void FolderTree::read(const Key& key, bool asked) {
    Level& level = levels_.at(key);
    level.load.cancel();
    level.load = pool_.submit(
        core::Priority::visible,
        [folder = key.second](const core::CancelToken& token) {
            return fs::read_subfolders(folder, token, kMaxNames);
        },
        [this, key, asked](fs::Subfolders read) { loaded(key, std::move(read), asked); });
}

void FolderTree::loaded(const Key& key, fs::Subfolders read, bool asked) {
    const auto it = levels_.find(key);
    if (it == levels_.end()) {
        return;
    }
    Level& level = it->second;
    const bool loop = !read.error && asked && leads_back(key, read);
    if (read.error || loop) {
        stop(level);
        level.names = {};
        level.more = 0;
        level.dirty.clear();
        level.loaded = false;
        level.failed = true;
    } else {
        level.names = std::move(read.names);
        level.more = read.more;
        level.device = read.device;
        level.inode = read.inode;
        level.loaded = true;
    }
    // After a re-read, the folders gone from the level go with their levels under it.
    release_if([&](const Key& other) {
        if (other.first != key.first || other.second == key.second ||
            !within(other.second, key.second)) {
            return false;
        }
        const std::string name = other.second.lexically_relative(key.second).begin()->string();
        return !std::ranges::binary_search(level.names, name, natural_less);
    });
    if (!level.dirty.empty() && !level.failed) {
        schedule_refresh(key);
    }
    changed();
    if (asked && (read.error || loop) && fail_) {
        fail_(key.second, read.error);
    }
}

bool FolderTree::leads_back(const Key& key, const fs::Subfolders& read) const {
    std::filesystem::path above = key.second;
    while (above != key.first && above.parent_path() != above) {
        above = above.parent_path();
        const auto it = levels_.find(Key{key.first, above});
        if (it != levels_.end() && it->second.loaded && it->second.device == read.device &&
            it->second.inode == read.inode) {
            return true;
        }
    }
    return false;
}

void FolderTree::on_events(const Key& key, const std::vector<fs::DirectoryEvent>& events) {
    const auto it = levels_.find(key);
    if (it == levels_.end()) {
        return;
    }
    Level& level = it->second;
    using Kind = fs::DirectoryEvent::Kind;
    for (const fs::DirectoryEvent& event : events) {
        switch (event.kind) {
            case Kind::directory_gone:
                // Its parent's own events take the name away.
                fold(key.first, key.second);
                return;
            case Kind::overflow:
                // Events were lost: read the whole level again.
                level.refresh.cancel();
                level.refreshing = false;
                loop_.remove(level.settle);
                level.settle = 0;
                level.dirty.clear();
                read(key, !level.loaded);
                return;
            case Kind::changed:
                break;  // written or touched: no folder came or went
            case Kind::created:
            case Kind::deleted:
            case Kind::moved_from:
            case Kind::moved_to:
                level.dirty.insert(event.name);
                break;
        }
    }
    if (level.loaded && !level.dirty.empty()) {
        schedule_refresh(key);
    }
}

void FolderTree::schedule_refresh(const Key& key) {
    Level& level = levels_.at(key);
    if (level.settle != 0 || level.refreshing) {
        return;  // already coming; a running refresh reschedules when it lands
    }
    level.settle = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this, key] {
        const auto it = levels_.find(key);
        if (it != levels_.end()) {
            it->second.settle = 0;
            refresh(key);
        }
    });
}

void FolderTree::refresh(const Key& key) {
    Level& level = levels_.at(key);
    if (level.dirty.empty()) {
        return;
    }
    std::vector<std::string> names(level.dirty.begin(), level.dirty.end());
    level.dirty.clear();
    level.refreshing = true;
    level.refresh = pool_.submit(
        core::Priority::visible,
        [folder = key.second, names](const core::CancelToken& /*token*/) {
            return fs::read_entries(folder, names, {});
        },
        [this, key, names](const std::vector<std::optional<fs::Entry>>& entries) {
            const auto it = levels_.find(key);
            if (it == levels_.end()) {
                return;
            }
            it->second.refreshing = false;
            apply(key, names, entries);
            if (!it->second.dirty.empty()) {
                schedule_refresh(key);
            }
        });
}

void FolderTree::apply(const Key& key, const std::vector<std::string>& names,
                       const std::vector<std::optional<fs::Entry>>& entries) {
    Level& level = levels_.at(key);
    bool any = false;
    for (std::size_t i = 0; i < names.size() && i < entries.size(); ++i) {
        const std::optional<fs::Entry>& entry = entries[i];
        const bool folder = entry && entry->kind == fs::EntryKind::directory;
        const auto at = std::ranges::lower_bound(level.names, names[i], natural_less);
        const bool listed = at != level.names.end() && *at == names[i];
        if (folder && !listed) {
            // Past the limit only the count grows. It is not taken down again (a name left out
            // was never known to be a folder), until the level is read again.
            if (level.names.size() < kMaxNames) {
                level.names.insert(at, names[i]);
            } else {
                ++level.more;
            }
            any = true;
        } else if (!folder && listed) {
            level.names.erase(at);
            const std::filesystem::path gone = key.second / names[i];
            release_if([&](const Key& other) {
                return other.first == key.first && within(other.second, gone);
            });
            any = true;
        }
    }
    if (any) {
        changed();
    }
}

void FolderTree::stop(Level& level) {
    level.load.cancel();
    level.refresh.cancel();
    level.refreshing = false;
    loop_.remove(level.settle);
    level.settle = 0;
    watcher_.unwatch(level.watch);
    level.watch = 0;
}

bool FolderTree::release_if(const std::function<bool(const Key& key)>& pred) {
    bool any = false;
    for (auto it = levels_.begin(); it != levels_.end();) {
        if (pred(it->first)) {
            stop(it->second);
            it = levels_.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    return any;
}

void FolderTree::changed() const {
    if (changed_) {
        changed_();
    }
}

}  // namespace nordend::views
