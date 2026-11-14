#include "nordend/platform/portal_config.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nordend/core/file.hpp"
#include "nordend/platform/desktop_entry.hpp"

namespace nordend::platform {

namespace {

constexpr std::string_view kOurLine = "org.freedesktop.impl.portal.FileChooser=nordend";
constexpr std::string_view kWrittenBy = "# Written by nordend --set-file-chooser";
constexpr std::string_view kTakeOut = "# nordend --unset-file-chooser takes Nordend out again.";
constexpr std::string_view kReplaced = "# Replaced by nordend --set-file-chooser: ";

std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        lines.emplace_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return lines;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

// "[name]" -> "name"; nullopt for any other line.
std::optional<std::string_view> group_name(std::string_view line) {
    line = trim(line);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return std::nullopt;
    }
    return line.substr(1, line.size() - 2);
}

// A "key=value" line, both trimmed; nullopt for comments, blanks and groups.
std::optional<std::pair<std::string_view, std::string_view>> key_value(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() == '#' || line.front() == '[') {
        return std::nullopt;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{trim(line.substr(0, equals)), trim(line.substr(equals + 1))};
}

// A file chooser line whose first backend is nordend ("nordend", "nordend;gtk").
bool is_nordend_line(std::string_view line) {
    const auto pair = key_value(line);
    if (!pair || pair->first != kFileChooserKey) {
        return false;
    }
    return trim(pair->second.substr(0, pair->second.find(';'))) == "nordend";
}

bool is_file_chooser_line(std::string_view line) {
    const auto pair = key_value(line);
    return pair && pair->first == kFileChooserKey;
}

// xdg-desktop-portal's file names in one folder, in its order: each desktop's, then the common one.
std::vector<std::string> conf_names(const Environment& environment) {
    std::vector<std::string> names;
    names.reserve(environment.desktops.size() + 1);
    for (const std::string& desktop : environment.desktops) {
        names.push_back(desktop + "-portals.conf");
    }
    names.emplace_back("portals.conf");
    return names;
}

std::filesystem::path user_folder(const Environment& environment) {
    return environment.config_dirs.empty() ? std::filesystem::path{}
                                           : environment.config_dirs.front() / "xdg-desktop-portal";
}

// The folders after the user's, as xdg-desktop-portal searches them: $XDG_CONFIG_DIRS, its
// sysconfdir, $XDG_DATA_HOME, $XDG_DATA_DIRS, its datadir.
std::vector<std::filesystem::path> system_folders(const Environment& environment) {
    std::vector<std::filesystem::path> folders;
    for (std::size_t i = 1; i < environment.config_dirs.size(); ++i) {
        folders.push_back(environment.config_dirs[i] / "xdg-desktop-portal");
    }
    folders.emplace_back("/etc/xdg-desktop-portal");
    for (const std::filesystem::path& dir : environment.data_dirs) {
        folders.push_back(dir / "xdg-desktop-portal");
    }
    folders.emplace_back("/usr/share/xdg-desktop-portal");
    return folders;
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// The first file of names in folder that exists; empty when none.
std::filesystem::path first_in(const std::filesystem::path& folder,
                               const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (is_file(folder / name)) {
            return folder / name;
        }
    }
    return {};
}

// The system file xdg-desktop-portal would read without the user's; empty when none.
std::filesystem::path desktop_file(const Environment& environment) {
    const std::vector<std::string> names = conf_names(environment);
    for (const std::filesystem::path& folder : system_folders(environment)) {
        if (std::filesystem::path found = first_in(folder, names); !found.empty()) {
            return found;
        }
    }
    return {};
}

std::string desktop_label(const Environment& environment) {
    return environment.desktops.empty() ? std::string{"this desktop"}
                                        : environment.desktops.front();
}

}  // namespace

bool names_nordend(std::string_view text) {
    bool preferred = false;
    bool nordend = false;
    for (const std::string& line : lines_of(text)) {
        if (const auto group = group_name(line)) {
            preferred = *group == "preferred";
        } else if (preferred && is_file_chooser_line(line)) {
            nordend = is_nordend_line(line);  // the last one counts, as GKeyFile reads it
        }
    }
    return nordend;
}

std::string with_file_chooser(std::string_view text) {
    if (names_nordend(text)) {
        return std::string{text};
    }
    std::vector<std::string> lines = lines_of(text);
    bool preferred = false;
    std::optional<std::size_t> first_preferred;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (const auto group = group_name(lines[i])) {
            preferred = *group == "preferred";
            if (preferred && !first_preferred) {
                first_preferred = i;
            }
        } else if (preferred && is_file_chooser_line(lines[i])) {
            lines[i] = std::string{kReplaced} + lines[i];
        }
    }
    if (first_preferred) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*first_preferred + 1),
                     std::string{kOurLine});
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[preferred]");
        lines.emplace_back(kOurLine);
    }
    return joined(lines);
}

std::string without_file_chooser(std::string_view text) {
    std::vector<std::string> kept;
    bool preferred = false;
    for (std::string& line : lines_of(text)) {
        if (line.starts_with(kWrittenBy) || line == kTakeOut) {
            continue;
        }
        if (const auto group = group_name(line)) {
            preferred = *group == "preferred";
        } else if (preferred && is_nordend_line(line)) {
            continue;
        }
        if (line.starts_with(kReplaced)) {
            line.erase(0, kReplaced.size());
        }
        kept.push_back(std::move(line));
    }
    return joined(kept);
}

ChooserChange set_file_chooser(const Environment& environment,
                               const std::filesystem::path& portal_dir) {
    const std::filesystem::path folder = user_folder(environment);
    const std::vector<std::string> names = conf_names(environment);
    std::filesystem::path target = first_in(folder, names);
    std::string text;
    std::string origin;
    if (!target.empty()) {
        const core::TextFile file = core::read_text_file(target);
        if (!file.error.empty()) {
            return {.ok = false,
                    .message = std::format("cannot read {}: {}\n", target.string(), file.error)};
        }
        text = file.text;
    } else {
        target = folder / names.front();
        const std::filesystem::path source = desktop_file(environment);
        text = std::format("{}{}.\n{}\n", kWrittenBy,
                           source.empty() ? std::string{} : " from " + source.string(), kTakeOut);
        if (!source.empty()) {
            const core::TextFile file = core::read_text_file(source);
            text += file.text;
            origin = std::format(" (a copy of {})", source.string());
        }
    }
    ChooserChange change;
    const std::string wanted = with_file_chooser(text);
    if (wanted == text) {
        change.message = std::format("Nordend already is the file chooser on {}: {}\n",
                                     desktop_label(environment), target.string());
    } else if (const std::error_code error = core::write_file_atomically(target, wanted)) {
        return {.ok = false,
                .message = std::format("cannot write {}: {}\n", target.string(), error.message())};
    } else {
        change.message = std::format(
            "Nordend is now the file chooser on {}: wrote {}{}.\n"
            "xdg-desktop-portal reads it when it starts: log out and in again.\n"
            "Flatpak applications use it at once; others may need asking: Firefox with\n"
            "widget.use-xdg-desktop-portal.file-picker = 1 in about:config, GTK 4 applications\n"
            "with GDK_DEBUG=portals, GTK 3 ones with GTK_USE_PORTAL=1, Qt ones with\n"
            "QT_QPA_PLATFORMTHEME=xdgdesktopportal.\n",
            desktop_label(environment), target.string(), origin);
    }
    if (!is_file(portal_dir / "nordend.portal")) {
        change.message += std::format(
            "warning: {} is missing, and xdg-desktop-portal looks for its backends only there:\n"
            "install Nordend under /usr (as its package does) before it can be chosen.\n",
            (portal_dir / "nordend.portal").string());
    }
    return change;
}

ChooserChange unset_file_chooser(const Environment& environment) {
    const std::filesystem::path target =
        first_in(user_folder(environment), conf_names(environment));
    if (target.empty()) {
        return {.message = std::format("Nordend is not the file chooser on {}: no portals.conf "
                                       "of yours names it.\n",
                                       desktop_label(environment))};
    }
    const core::TextFile file = core::read_text_file(target);
    if (!file.error.empty()) {
        return {.ok = false,
                .message = std::format("cannot read {}: {}\n", target.string(), file.error)};
    }
    if (!names_nordend(file.text)) {
        return {.message =
                    std::format("Nordend is not the file chooser in {}.\n", target.string())};
    }
    const std::string left = without_file_chooser(file.text);
    // A file we wrote from the desktop's own and nobody changed since: back to no file at all.
    if (file.text.starts_with(kWrittenBy)) {
        const std::filesystem::path source = desktop_file(environment);
        const std::string original =
            source.empty() ? std::string{} : core::read_text_file(source).text;
        if (left == without_file_chooser(with_file_chooser(original))) {
            std::error_code error;
            std::filesystem::remove(target, error);
            if (error) {
                return {.ok = false,
                        .message = std::format("cannot remove {}: {}\n", target.string(),
                                               error.message())};
            }
            return {.message = std::format(
                        "Nordend is no longer the file chooser on {}: removed {}, which it had "
                        "written.\nxdg-desktop-portal reads the change when it starts: log out "
                        "and in again.\n",
                        desktop_label(environment), target.string())};
        }
    }
    if (const std::error_code error = core::write_file_atomically(target, left)) {
        return {.ok = false,
                .message = std::format("cannot write {}: {}\n", target.string(), error.message())};
    }
    return {.message = std::format(
                "Nordend is no longer the file chooser on {}: took its line out of {}.\n"
                "xdg-desktop-portal reads the change when it starts: log out and in again.\n",
                desktop_label(environment), target.string())};
}

}  // namespace nordend::platform
