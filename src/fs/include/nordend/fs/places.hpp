#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/file_watcher.hpp"

namespace nordend::fs {

// A place the sidebar lists: a folder worth one click.
struct Place {
    enum class Section : std::uint8_t { places, bookmarks, devices };

    Section section{Section::places};
    std::string label;
    std::filesystem::path path;  // empty for a disk not mounted
    std::string_view icon;       // a Tabler icon name

    // A disk (Section::devices) as udisks2 knows it; empty without udisks2.
    struct Disk {
        std::string volume;     // its object
        bool mounted{true};     // false: listed by udisks2, not mounted (path empty)
        bool ejectable{false};  // an external drive, which can be powered off

        bool operator==(const Disk&) const = default;
    };

    Disk disk{};

    bool operator==(const Place&) const = default;
};

// Where the places come from: the home, the XDG config dir and the mount table.
struct PlaceSources {
    std::filesystem::path home;
    std::filesystem::path config_home;  // user-dirs.dirs and gtk-3.0/bookmarks are in it
    std::filesystem::path mountinfo{"/proc/self/mountinfo"};

    // $HOME, $XDG_CONFIG_HOME (or ~/.config).
    [[nodiscard]] static PlaceSources from_environment();
};

// The XDG user dirs in user-dirs.dirs's text ("XDG_PICTURES_DIR=\"$HOME/Pictures\""), by their
// key without XDG_ and _DIR ("PICTURES"), in file order. A dir that is the home itself is left
// out: the spec reads it as disabled.
struct UserDir {
    std::string key;
    std::filesystem::path path;

    bool operator==(const UserDir&) const = default;
};

[[nodiscard]] std::vector<UserDir> parse_user_dirs(std::string_view text,
                                                   const std::filesystem::path& home);

// A /proc file whole (/proc/self/mountinfo): they report a size of 0, so core::read_text_file
// reads nothing; this reads until the end. Empty when it cannot be opened.
[[nodiscard]] std::string read_proc_file(const std::filesystem::path& path);

// The folder of templates "New File" offers: XDG_TEMPLATES_DIR from user-dirs.dirs, or ~/Templates
// without that file (as the sidebar's user dirs); empty when there is none.
[[nodiscard]] std::filesystem::path templates_dir(const PlaceSources& sources);

// The local folders in a GTK bookmarks file ("file:///path%20with%20spaces Label"); other URIs
// (sftp://, smb://) are left out. Without a label, the folder's name.
[[nodiscard]] std::vector<Place> parse_bookmarks(std::string_view text);

// The GTK bookmarks file: gtk-3.0/bookmarks in config_home (GTK 4 reads the same one).
[[nodiscard]] std::filesystem::path bookmarks_file(const PlaceSources& sources);

// A bookmarks file's text with folder added at the end, its URI as GLib writes it; nullopt when
// it is there already. The other lines stay as they are.
[[nodiscard]] std::optional<std::string> add_bookmark(std::string_view text,
                                                      const std::filesystem::path& folder);

// The text without folder's lines; nullopt when it has none. The other lines stay as they are.
[[nodiscard]] std::optional<std::string> remove_bookmark(std::string_view text,
                                                         const std::filesystem::path& folder);

// The text with folder's bookmark called label: its URI kept as written, the label after it; an
// empty label, or the folder's own name, leaves the line without one (the sidebar then shows the
// folder's name). nullopt when folder has no bookmark. The other lines stay as they are.
[[nodiscard]] std::optional<std::string> rename_bookmark(std::string_view text,
                                                         const std::filesystem::path& folder,
                                                         std::string_view label);

// The text with from's bookmark pointing to to instead (its URI as GLib writes it), keeping its
// label and its place; nullopt when from has none. The caller checks that to has none already.
[[nodiscard]] std::optional<std::string> repoint_bookmark(std::string_view text,
                                                          const std::filesystem::path& from,
                                                          const std::filesystem::path& to);

// The text with folder's bookmark swapped with the shown bookmark before it (step -1) or after it
// (+1); lines the sidebar does not show (sftp://, blank) stay where they are. nullopt when folder
// has none or is already first (last).
[[nodiscard]] std::optional<std::string> move_bookmark(std::string_view text,
                                                       const std::filesystem::path& folder,
                                                       int step);

// The mount points in /proc/self/mountinfo's text under /run/media, /media and /mnt: where
// removable and data disks land. Octal escapes (\040 for a space) decoded.
[[nodiscard]] std::vector<Place> parse_mounts(std::string_view text);

// Every place, in the order the sidebar shows them: home, the user dirs that exist (Desktop,
// Documents, Downloads, Music, Pictures, Videos; without user-dirs.dirs, the folders with those
// names in the home), the file system, the bookmarks, the mounts.
[[nodiscard]] std::vector<Place> load_places(const PlaceSources& sources);

// The places, kept current: reloaded when something is mounted or unmounted (the mount table
// polls as a priority event) and when user-dirs.dirs or the bookmarks change.
//
// Loop thread only. Must not outlive the loop.
class PlacesWatcher {
public:
    PlacesWatcher(core::EventLoop& loop, PlaceSources sources, std::function<void()> changed);
    ~PlacesWatcher();

    PlacesWatcher(const PlacesWatcher&) = delete;
    PlacesWatcher& operator=(const PlacesWatcher&) = delete;
    PlacesWatcher(PlacesWatcher&&) = delete;
    PlacesWatcher& operator=(PlacesWatcher&&) = delete;

    [[nodiscard]] const std::vector<Place>& places() const noexcept { return places_; }

    // Reads the places again now (Nordend wrote the bookmarks), and starts watching a file whose
    // folder was made meanwhile.
    void reload();

private:
    void watch_files();

    core::EventLoop& loop_;
    PlaceSources sources_;
    std::function<void()> changed_;
    std::vector<Place> places_;
    int mountinfo_fd_{-1};
    core::EventLoop::SourceId mountinfo_source_{0};
    std::vector<std::unique_ptr<core::FileWatcher>> files_;
    std::set<std::filesystem::path> watched_;  // the files in files_
};

}  // namespace nordend::fs
