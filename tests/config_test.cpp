#include "nordend/core/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

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
    CHECK(config.font.monospace == defaults.font.monospace);
    CHECK(config.pointer.double_click_ms == defaults.pointer.double_click_ms);
    CHECK(config.pointer.double_click_distance == defaults.pointer.double_click_distance);
    CHECK(config.icon.follow_accent == defaults.icon.follow_accent);
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

[pointer]
double_click_ms = 250
double_click_distance = 0

[icon]
follow_accent = false
)");
    CHECK(result.problems.empty());
    CHECK(result.config.font.family == "Inter:weight=medium");
    CHECK(result.config.font.monospace == "JetBrains Mono");
    CHECK(result.config.pointer.double_click_ms == 250);
    CHECK(result.config.pointer.double_click_distance == 0);
    CHECK_FALSE(result.config.icon.follow_accent);
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
