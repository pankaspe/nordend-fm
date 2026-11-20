#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// User settings from config.toml. Every key is optional: a missing key keeps its default, a bad
// value (wrong type, out of range) is reported and keeps its default, an unknown key is reported
// and ignored, and a file that does not parse is ignored as a whole. Reading a config never fails.
namespace nordend::core {

struct Config {
    struct Font {
        static constexpr float kUsualSize = 13.0F;  // the interface's text, as drawn at zoom 1

        std::string family{"sans-serif"};  // fontconfig pattern, e.g. "Inter" or "Inter:weight=500"
        std::string monospace{"monospace"};  // text previews, e.g. "JetBrains Mono"
        // The usual text size in logical pixels (8-32); everything else follows in proportion:
        // the other text, rows, paddings, icons. Followed live.
        float size{kUsualSize};

        // The interface's zoom: 1 at the usual size.
        [[nodiscard]] float zoom() const noexcept { return size / kUsualSize; }
    };

    struct Pointer {
        int double_click_ms{400};      // presses closer in time count as a double click
        int double_click_distance{4};  // ... and closer than this, in logical pixels
    };

    struct Icon {
        // Nordend's icon in the desktop's accent, written to ~/.local/share/icons (ROADMAP 9.2);
        // false takes it away and leaves the installed one.
        bool follow_accent{true};
    };

    struct Window {
        enum class TitleBar : std::uint8_t {
            integrated,  // Nordend's own header: the sidebar to the top, the path and the buttons
            compositor,  // the compositor's title bar, where it draws one
        };
        TitleBar title_bar{TitleBar::integrated};  // read at start
        float background_opacity{1.0F};  // background only; foreground content stays solid
        // The window's corners, in logical pixels (0-24; 0 = square), where Nordend draws its
        // own frame; followed live.
        int corner_radius{12};
    };

    struct Terminal {
        // The terminal "Open in Terminal" and Terminal=true applications use, tried before
        // $TERMINAL; empty: $TERMINAL, xdg-terminal-exec, then the known ones. Followed live.
        std::string program;
        std::vector<std::string> args;  // its own options, before the command
    };

    struct Open {
        // Enter on a program (an ELF executable or AppImage that may run) starts it; false:
        // Nordend never starts one, programs open as any other file. Followed live.
        bool programs{true};
    };

    struct Sidebar {
        // A folder tree under the places: a chevron unfolds a folder's subfolders, read on a
        // worker one level at a time. Followed live.
        bool tree{false};
    };

    // A custom entry of the context menu ([[action]]): a program run on the selected entries,
    // shown only when every one of them passes the filters. Followed live.
    struct Action {
        std::string name;  // the menu's label
        // The command's words: split from a string by the Exec quoting rules, or the array as
        // written. Field codes: %f (one path a process), %F (all paths), %u, %U (file:// URIs),
        // %d (the folder shown), %%. No shell of Nordend's own.
        std::vector<std::string> exec;
        bool exec_words{false};  // given as an array: a code expands only as a whole word ("%F")
        bool files{true};        // kinds: shown for files...
        bool folders{false};     // ...and for folders
        // Case-insensitive globs on the whole name ("*.tar.gz"); any name when empty. With mime,
        // a match on either is enough.
        std::vector<std::string> patterns;
        std::vector<std::string> mime;  // types, subtypes included (as Open With)
        int min{1};                     // how many entries may be selected
        int max{0};                     // 0: no limit
        bool background{false};         // also on the folder's empty space, run on the folder
        std::string submenu;            // actions with the same one are grouped under it
        bool terminal{false};           // run in the terminal ([terminal], in_terminal)
    };

    Font font;
    Pointer pointer;
    Icon icon;
    Window window;
    Terminal terminal;
    Open open;
    Sidebar sidebar;
    std::vector<Action> actions;  // in the file's order; a broken one is left out
};

struct ConfigResult {
    Config config;
    bool from_file{false};  // the file existed and parsed; false means pure defaults
    // One line per problem, ready to print: "<file>:<line>:<column>: <what>".
    std::vector<std::string> problems;
};

// $XDG_CONFIG_HOME/nordend/config.toml, else $HOME/.config/nordend/config.toml; empty when
// neither variable is set.
[[nodiscard]] std::filesystem::path default_config_path();

// Parses TOML text. source names the file in problem messages.
[[nodiscard]] ConfigResult parse_config(std::string_view text, std::string_view source);

// Reads and parses a file. A missing file is not a problem: defaults, from_file = false.
[[nodiscard]] ConfigResult load_config(const std::filesystem::path& path);

}  // namespace nordend::core
