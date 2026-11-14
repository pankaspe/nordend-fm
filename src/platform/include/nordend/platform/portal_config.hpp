#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "nordend/platform/desktop_entry.hpp"

// `nordend --set-file-chooser` / `--unset-file-chooser`: Nordend as the desktop's file chooser,
// by one line in the user's portals.conf (portals.conf(5)), which xdg-desktop-portal reads when
// it starts.
namespace nordend::platform {

// The portals.conf key of the file chooser; its value "nordend" names our nordend.portal.
inline constexpr std::string_view kFileChooserKey = "org.freedesktop.impl.portal.FileChooser";

// Whether a portals.conf's [preferred] group puts Nordend first for the file chooser.
[[nodiscard]] bool names_nordend(std::string_view text);

// A portals.conf with Nordend as the file chooser: our line first in [preferred] (the group added
// when missing), any other file chooser line kept as a comment that without_file_chooser() brings
// back. The text unchanged when Nordend already is.
[[nodiscard]] std::string with_file_chooser(std::string_view text);

// The reverse: Nordend's line out, the lines it replaced back, the header of a file it wrote gone.
[[nodiscard]] std::string without_file_chooser(std::string_view text);

struct ChooserChange {
    bool ok{true};        // false: the file could not be read, written or removed
    std::string message;  // what was done, lines for the terminal
};

// The user's portals.conf for the current desktop (the first of xdg-desktop-portal's names that
// exists in $XDG_CONFIG_HOME/xdg-desktop-portal, else "<desktop>-portals.conf") made to name
// Nordend. A new file starts as a copy of the desktop's own (the system one xdg-desktop-portal
// would read), so versions that read only one file keep the desktop's other portals. Warns when
// portal_dir (where xdg-desktop-portal looks for .portal files) has no nordend.portal.
[[nodiscard]] ChooserChange set_file_chooser(const Environment& environment,
                                             const std::filesystem::path& portal_dir);

// Takes Nordend out of that file again; a file set_file_chooser() wrote that is left as the
// desktop's own copy is removed.
[[nodiscard]] ChooserChange unset_file_chooser(const Environment& environment);

}  // namespace nordend::platform
