#include "nordend/core/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "temp_dir.hpp"

using nordend::core::Config;
using nordend::core::ConfigResult;
using nordend::core::default_config_path;
using nordend::core::load_config;
using nordend::core::parse_config;

using nordend::test::TempDir;

namespace {

ConfigResult parse(std::string_view text) { return parse_config(text, "test.toml"); }

void check_defaults(const Config& config) {
    const Config defaults;
    CHECK(config.window.background_opacity == defaults.window.background_opacity);
    CHECK(config.font.family == defaults.font.family);
    CHECK(config.window.corner_radius == defaults.window.corner_radius);
    CHECK(config.font.monospace == defaults.font.monospace);
    CHECK(config.font.size == defaults.font.size);
    CHECK(config.pointer.double_click_ms == defaults.pointer.double_click_ms);
    CHECK(config.pointer.double_click_distance == defaults.pointer.double_click_distance);
    CHECK(config.icon.follow_accent == defaults.icon.follow_accent);
    CHECK(config.terminal.program.empty());
    CHECK(config.terminal.args.empty());
    CHECK(config.open.programs);
    CHECK_FALSE(config.sidebar.tree);
    CHECK(config.actions.empty());
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Sets an environment variable for one scope, restoring the old value after.
struct ScopedEnv {
    std::string name;
    bool had_value{false};
    std::string old_value;

    ScopedEnv(std::string variable, const char* value) : name{std::move(variable)} {
        if (const char* old = std::getenv(name.c_str()); old != nullptr) {
            had_value = true;
            old_value = old;
        }
        if (value != nullptr) {
            setenv(name.c_str(), value, 1);
        } else {
            unsetenv(name.c_str());
        }
    }

    ~ScopedEnv() {
        if (had_value) {
            setenv(name.c_str(), old_value.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;
};

}  // namespace

TEST_CASE("an empty file gives the defaults and no problems") {
    const ConfigResult result = parse("");
    CHECK(result.from_file);
    CHECK(result.problems.empty());
    check_defaults(result.config);
}

TEST_CASE("every key is read") {
    const ConfigResult result = parse(R"(
[font]
family = "Inter:weight=medium"
monospace = "JetBrains Mono"
size = 15

[pointer]
double_click_ms = 250
double_click_distance = 0

[window]
title_bar = "compositor"
corner_radius = 0

[icon]
follow_accent = false

[terminal]
program = "ghostty"
args = ["--gtk-single-instance=false"]

[sidebar]
tree = true
)");
    CHECK(result.problems.empty());
    CHECK(result.config.font.family == "Inter:weight=medium");
    CHECK(result.config.font.monospace == "JetBrains Mono");
    CHECK(result.config.font.size == 15.0F);
    CHECK(result.config.pointer.double_click_ms == 250);
    CHECK(result.config.pointer.double_click_distance == 0);
    CHECK(result.config.window.title_bar == Config::Window::TitleBar::compositor);
    CHECK(result.config.window.corner_radius == 0);
    CHECK_FALSE(result.config.icon.follow_accent);
    CHECK(result.config.terminal.program == "ghostty");
    CHECK(result.config.terminal.args == std::vector<std::string>{"--gtk-single-instance=false"});
    CHECK(result.config.sidebar.tree);
}

TEST_CASE("dotted keys work like sections") {
    const ConfigResult result = parse("pointer.double_click_ms = 500\n");
    CHECK(result.problems.empty());
    CHECK(result.config.pointer.double_click_ms == 500);
}

TEST_CASE("a syntax error ignores the whole file and says where") {
    const ConfigResult result = parse(R"([pointer]
double_click_ms = 250
double_click_distance = = 3
)");
    CHECK_FALSE(result.from_file);
    REQUIRE(result.problems.size() == 1);
    CHECK(result.problems[0].starts_with("test.toml:3:"));
    CHECK(contains(result.problems[0], "whole file is ignored"));
    check_defaults(result.config);
}

TEST_CASE("unknown sections and keys are reported, the rest still applies") {
    const ConfigResult result = parse(R"([pointer]
dubble_click_ms = 250
double_click_distance = 8

[colours]
accent = "#ff0000"
)");
    CHECK(result.from_file);
    REQUIRE(result.problems.size() == 2);
    CHECK(result.problems[0] == "test.toml:2:1: unknown key 'pointer.dubble_click_ms'; ignored");
    CHECK(contains(result.problems[1], "test.toml:5:"));
    CHECK(contains(result.problems[1], "'colours'"));
    CHECK(result.config.pointer.double_click_distance == 8);
    CHECK(result.config.pointer.double_click_ms == Config{}.pointer.double_click_ms);
}

TEST_CASE("a wrong type keeps the default and names both types") {
    const ConfigResult result = parse(R"([pointer]
double_click_ms = "fast"
double_click_distance = 4.5

[font]
family = 12

[icon]
follow_accent = "no"
)");
    REQUIRE(result.problems.size() == 4);
    CHECK(result.problems[0] ==
          "test.toml:2:19: 'pointer.double_click_ms' must be an integer, got a string; using 400");
    CHECK(contains(result.problems[1], "got a number with decimals"));
    CHECK(contains(result.problems[2], "'font.family' must be a string, got an integer"));
    CHECK(result.problems[3] ==
          "test.toml:9:17: 'icon.follow_accent' must be true or false, got a string; using true");
    check_defaults(result.config);
}

TEST_CASE("values out of range keep the default") {
    const ConfigResult result = parse(R"([pointer]
double_click_ms = 99
double_click_distance = 51
)");
    REQUIRE(result.problems.size() == 2);
    CHECK(contains(result.problems[0], "between 100 and 2000, got 99"));
    CHECK(contains(result.problems[1], "between 0 and 50, got 51"));
    check_defaults(result.config);

    const ConfigResult limits = parse(R"([pointer]
double_click_ms = 2000
double_click_distance = 50
)");
    CHECK(limits.problems.empty());
    CHECK(limits.config.pointer.double_click_ms == 2000);
    CHECK(limits.config.pointer.double_click_distance == 50);
}

TEST_CASE("an empty font family keeps the default") {
    const ConfigResult result = parse("font.family = \"\"\n");
    REQUIRE(result.problems.size() == 1);
    CHECK(contains(result.problems[0], "must not be empty"));
    check_defaults(result.config);
}

TEST_CASE("a section given as a plain value is reported") {
    const ConfigResult result = parse("font = \"Inter\"\n");
    REQUIRE(result.problems.size() == 1);
    CHECK(contains(result.problems[0], "'font' must be a section ([font]), got a string"));
    check_defaults(result.config);
}

TEST_CASE("load_config: missing file, real file, directory") {
    const TempDir dir;

    const ConfigResult missing = load_config(dir.path / "absent.toml");
    CHECK_FALSE(missing.from_file);
    CHECK(missing.problems.empty());
    check_defaults(missing.config);

    const std::filesystem::path file = dir.write("config.toml", "pointer.double_click_ms = 300\n");
    const ConfigResult loaded = load_config(file);
    CHECK(loaded.from_file);
    CHECK(loaded.problems.empty());
    CHECK(loaded.config.pointer.double_click_ms == 300);

    const std::filesystem::path broken = dir.write("broken.toml", "[pointer\n");
    const ConfigResult bad = load_config(broken);
    REQUIRE(bad.problems.size() == 1);
    CHECK(bad.problems[0].starts_with(broken.string() + ":1:"));

    const ConfigResult directory = load_config(dir.path);
    REQUIRE(directory.problems.size() == 1);
    CHECK(contains(directory.problems[0], "not a regular file"));
}

TEST_CASE("the example config parses clean and matches the defaults") {
    const ConfigResult result = load_config(NORDEND_EXAMPLE_CONFIG);
    CHECK(result.from_file);
    CHECK(result.problems.empty());
    check_defaults(result.config);
}

TEST_CASE("default_config_path follows XDG_CONFIG_HOME, then HOME") {
    const ScopedEnv home{"HOME", "/home/someone"};
    {
        const ScopedEnv xdg{"XDG_CONFIG_HOME", "/tmp/xdg"};
        CHECK(default_config_path() == "/tmp/xdg/nordend/config.toml");
    }
    {
        const ScopedEnv xdg{"XDG_CONFIG_HOME", "relative/ignored"};
        CHECK(default_config_path() == "/home/someone/.config/nordend/config.toml");
    }
    {
        const ScopedEnv xdg{"XDG_CONFIG_HOME", nullptr};
        CHECK(default_config_path() == "/home/someone/.config/nordend/config.toml");
        const ScopedEnv no_home{"HOME", nullptr};
        CHECK(default_config_path().empty());
    }
}

TEST_CASE("background opacity accepts finite numbers from zero to one") {
    for (const std::string value : {"0", "1", "0.7", "1.0", "0.0"}) {
        const auto result = parse("[window]\nbackground_opacity = " + value);
        CHECK(result.problems.empty());
        CHECK(result.config.window.background_opacity == doctest::Approx(std::stod(value)));
    }
    for (const std::string value : {"-0.1", "1.1", "nan", "inf", "-inf", "true", "\"0.7\""}) {
        const auto result = parse("[window]\nbackground_opacity = " + value);
        CAPTURE(value);
        REQUIRE(result.problems.size() == 1);
        CHECK(contains(result.problems[0],
                       "'window.background_opacity' must be a number between 0 and 1"));
        CHECK(result.config.window.background_opacity == 1.0F);
    }
}

TEST_CASE("window corner radius: 0 to 24, else the default") {
    CHECK(parse("").config.window.corner_radius == 12);
    for (const int value : {0, 1, 24}) {
        const auto result = parse("[window]\ncorner_radius = " + std::to_string(value));
        CAPTURE(value);
        CHECK(result.problems.empty());
        CHECK(result.config.window.corner_radius == value);
    }
    for (const std::string value : {"-1", "25", "12.5", "\"0\"", "true"}) {
        const auto result = parse("[window]\ncorner_radius = " + value);
        CAPTURE(value);
        CHECK(result.problems.size() == 1);
        CHECK(result.config.window.corner_radius == 12);
    }
}

TEST_CASE("font size: 8 to 32, whole or not, else the default") {
    const Config defaults;
    CHECK(defaults.font.size == 13.0F);
    CHECK(defaults.font.zoom() == 1.0F);
    for (const std::string value : {"8", "13", "15", "17.5", "32", "32.0"}) {
        const auto result = parse("[font]\nsize = " + value);
        CAPTURE(value);
        CHECK(result.problems.empty());
        CHECK(result.config.font.size == doctest::Approx(std::stod(value)));
    }
    CHECK(parse("[font]\nsize = 26").config.font.zoom() == 2.0F);
    for (const std::string value : {"7.9", "33", "0", "-13", "nan", "inf", "\"15\"", "true"}) {
        const auto result = parse("[font]\nsize = " + value);
        CAPTURE(value);
        REQUIRE(result.problems.size() == 1);
        CHECK(contains(result.problems[0], "'font.size' must be"));
        CHECK(result.config.font.size == 13.0F);
    }
}

TEST_CASE("terminal: a program and a list of strings, else nothing changes") {
    const auto alone = parse("[terminal]\nprogram = \"foot\"\n");
    CHECK(alone.problems.empty());
    CHECK(alone.config.terminal.program == "foot");
    CHECK(alone.config.terminal.args.empty());
    CHECK(parse("[terminal]\nargs = []\n").problems.empty());

    for (const std::string text : {"program = 3", "program = \"\"", "args = \"--big\"",
                                   "args = [\"--big\", 1]", "args = [[\"--big\"]]"}) {
        const auto result = parse("[terminal]\n" + text + "\n");
        CAPTURE(text);
        REQUIRE(result.problems.size() == 1);
        CHECK(contains(result.problems[0],
                       text.starts_with("program") ? "'terminal.program'" : "'terminal.args'"));
        CHECK(result.config.terminal.program.empty());
        CHECK(result.config.terminal.args.empty());
    }
}

TEST_CASE("open: programs may be turned off; anything but a boolean is reported") {
    const auto off = parse("[open]\nprograms = false\n");
    CHECK(off.problems.empty());
    CHECK_FALSE(off.config.open.programs);
    const auto wrong = parse("[open]\nprograms = \"no\"\n");
    REQUIRE(wrong.problems.size() == 1);
    CHECK(contains(wrong.problems[0], "'open.programs'"));
    CHECK(wrong.config.open.programs);
    const auto unknown = parse("[open]\nscripts = true\n");
    CHECK(unknown.problems.size() == 1);
}

TEST_CASE("sidebar: the tree may be turned on; anything but a boolean is reported") {
    const auto on = parse("[sidebar]\ntree = true\n");
    CHECK(on.problems.empty());
    CHECK(on.config.sidebar.tree);
    const auto wrong = parse("[sidebar]\ntree = \"yes\"\n");
    REQUIRE(wrong.problems.size() == 1);
    CHECK(contains(wrong.problems[0], "'sidebar.tree'"));
    CHECK_FALSE(wrong.config.sidebar.tree);
}

TEST_CASE("actions: every key, the defaults, a line or a list, in the file's order") {
    const auto result = parse(R"(
[[action]]
name = "Extract Here"
patterns = ["*.zip", "*.tar.gz"]
exec = "peazip -ext2here %F"

[[action]]
name = "Add to ZIP"
submenu = "PeaZip"
kinds = ["file", "folder"]
mime = ["image/jpeg"]
min = 2
max = 9
background = true
terminal = true
exec = ["sh", "-c", 'echo "$@" %f', "sh", "%F"]
)");
    CHECK(result.problems.empty());
    REQUIRE(result.config.actions.size() == 2);
    const Config::Action& extract = result.config.actions[0];
    CHECK(extract.name == "Extract Here");
    CHECK(extract.exec == std::vector<std::string>{"peazip", "-ext2here", "%F"});
    CHECK_FALSE(extract.exec_words);
    CHECK(extract.patterns == std::vector<std::string>{"*.zip", "*.tar.gz"});
    CHECK(extract.files);
    CHECK_FALSE(extract.folders);
    CHECK(extract.mime.empty());
    CHECK(extract.min == 1);
    CHECK(extract.max == 0);
    CHECK_FALSE(extract.background);
    CHECK(extract.submenu.empty());
    CHECK_FALSE(extract.terminal);
    const Config::Action& zip = result.config.actions[1];
    CHECK(zip.exec == std::vector<std::string>{"sh", "-c", "echo \"$@\" %f", "sh", "%F"});
    CHECK(zip.exec_words);  // %f inside the script's text is text
    CHECK(zip.files);
    CHECK(zip.folders);
    CHECK(zip.mime == std::vector<std::string>{"image/jpeg"});
    CHECK(zip.min == 2);
    CHECK(zip.max == 9);
    CHECK(zip.background);
    CHECK(zip.submenu == "PeaZip");
    CHECK(zip.terminal);
}

TEST_CASE("actions: a broken one is left out and said so, the others load") {
    const std::vector<std::pair<std::string, std::string>> broken{
        {"exec = \"tool %F\"", "'action[2]' needs a name"},
        {"name = \"X\"", "'action[2]' needs an exec"},
        {"name = \"X\"\nexec = \"tool \\\"open\"", "cannot be split"},
        {"name = \"X\"\nexec = \"   \"", "cannot be split"},
        {"name = \"X\"\nexec = \"tool %i %F\"", "unknown field code '%i'"},
        {"name = \"X\"\nexec = \"tool 50%\"", "unknown field code '%'"},
        {"name = \"X\"\nexec = \"tool %F %f\"", "both as a list"},
        {"name = \"X\"\nexec = [\"tool\", \"%k\"]", "unknown field code '%k'"},
        {"name = \"X\"\nexec = [\"tool\", \"%F\", \"%u\"]", "both as a list"},
        {"name = \"X\"\nexec = []", "must start with a program"},
        {"name = \"X\"\nexec = [\"tool\", 3]", "must be a list of strings"},
        {"name = \"X\"\nexec = 3", "must be a string or a list of strings"},
        {"name = \"X\"\nexec = \"t\"\nkinds = []", R"(must name "file", "folder" or both)"},
        {"name = \"X\"\nexec = \"t\"\nkinds = [\"link\"]", "got \"link\""},
        {"name = \"X\"\nexec = \"t\"\nmime = [\"image/*\"]", "wildcards"},
        {"name = \"X\"\nexec = \"t\"\nmime = [\"png\"]", "is not a type"},
        {"name = \"X\"\nexec = \"t\"\nmin = 0", "must be between 1"},
        {"name = \"X\"\nexec = \"t\"\nmin = 3\nmax = 2", "max (2) is less than min (3)"},
        {"name = \"X\"\nexec = \"t\"\npattern = [\"*.zip\"]", "unknown key 'action[2].pattern'"},
        {"name = \"\"\nexec = \"t\"", "must not be empty"},
    };
    for (const auto& [body, said] : broken) {
        CAPTURE(body);
        const auto result = parse("[[action]]\nname = \"A\"\nexec = \"a\"\n\n[[action]]\n" + body +
                                  "\n\n[[action]]\nname = \"C\"\nexec = \"c\"\n");
        REQUIRE(result.config.actions.size() == 2);
        CHECK(result.config.actions[0].name == "A");
        CHECK(result.config.actions[1].name == "C");
        REQUIRE(result.problems.size() >= 2);
        CHECK(contains(result.problems.front(), said));
        CHECK(contains(result.problems.back(), "'action[2]'"));
        CHECK(contains(result.problems.back(), "is left out"));
    }
    const auto plain = parse("action = \"x\"\n");
    REQUIRE(plain.problems.size() == 1);
    CHECK(contains(plain.problems[0], "'action' must be a list of tables ([[action]])"));
}
