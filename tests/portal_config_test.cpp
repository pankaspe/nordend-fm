#include "nordend/platform/portal_config.hpp"

#include <filesystem>
#include <string>

#include <doctest/doctest.h>

#include "nordend/core/file.hpp"
#include "nordend/platform/desktop_entry.hpp"
#include "temp_dir.hpp"

using nordend::platform::Environment;
using nordend::platform::names_nordend;
using nordend::platform::set_file_chooser;
using nordend::platform::unset_file_chooser;
using nordend::platform::with_file_chooser;
using nordend::platform::without_file_chooser;
using nordend::test::TempDir;

namespace {

constexpr const char* kGnome =
    "[preferred]\ndefault=gnome;gtk;\n"
    "org.freedesktop.impl.portal.Secret=gnome-keyring;\n";

// A user's and a system's config folder in dir, for a desktop no real system has a file for.
struct Places {
    TempDir dir;
    Environment environment;
    std::filesystem::path user;    // the user's xdg-desktop-portal folder
    std::filesystem::path system;  // the first system one
    std::filesystem::path portals;

    Places() {
        environment.config_dirs = {dir.path / "home", dir.path / "etc"};
        environment.data_dirs = {dir.path / "share"};
        environment.desktops = {"nordendtest"};
        user = dir.path / "home" / "xdg-desktop-portal";
        system = dir.path / "etc" / "xdg-desktop-portal";
        portals = dir.path / "portals";
        std::filesystem::create_directories(system);
        std::filesystem::create_directories(portals);
        write(portals / "nordend.portal", "[portal]\n");
    }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        REQUIRE_FALSE(nordend::core::write_file_atomically(path, text));
    }

    static std::string read(const std::filesystem::path& path) {
        return nordend::core::read_text_file(path).text;
    }
};

}  // namespace

TEST_CASE("portal config: Nordend's line goes first in [preferred]") {
    const std::string text = with_file_chooser(kGnome);
    CHECK(text ==
          "[preferred]\norg.freedesktop.impl.portal.FileChooser=nordend\n"
          "default=gnome;gtk;\norg.freedesktop.impl.portal.Secret=gnome-keyring;\n");
    CHECK(names_nordend(text));
    CHECK_FALSE(names_nordend(kGnome));
    CHECK(with_file_chooser(text) == text);  // already: unchanged
    CHECK(without_file_chooser(text) == kGnome);
}

TEST_CASE("portal config: another file chooser is kept as a comment and comes back") {
    const std::string mine =
        "# mine\n[preferred]\ndefault=kde\n"
        "org.freedesktop.impl.portal.FileChooser = gtk\n";
    const std::string text = with_file_chooser(mine);
    CHECK(text ==
          "# mine\n[preferred]\norg.freedesktop.impl.portal.FileChooser=nordend\n"
          "default=kde\n# Replaced by nordend --set-file-chooser: "
          "org.freedesktop.impl.portal.FileChooser = gtk\n");
    CHECK(names_nordend(text));
    CHECK(without_file_chooser(text) == mine);
}

TEST_CASE("portal config: a file without [preferred] gets the group") {
    CHECK(with_file_chooser("") ==
          "[preferred]\norg.freedesktop.impl.portal.FileChooser=nordend\n");
    CHECK(with_file_chooser("[other]\nkey=value\n") ==
          "[other]\nkey=value\n\n[preferred]\norg.freedesktop.impl.portal.FileChooser=nordend\n");
}

TEST_CASE("portal config: only [preferred]'s line counts, the last one as GKeyFile reads it") {
    CHECK_FALSE(names_nordend("[other]\norg.freedesktop.impl.portal.FileChooser=nordend\n"));
    CHECK_FALSE(names_nordend("[preferred]\n# org.freedesktop.impl.portal.FileChooser=nordend\n"));
    CHECK(names_nordend("[preferred]\norg.freedesktop.impl.portal.FileChooser=nordend;gtk\n"));
    CHECK_FALSE(
        names_nordend("[preferred]\norg.freedesktop.impl.portal.FileChooser=nordend\n"
                      "org.freedesktop.impl.portal.FileChooser=gtk\n"));
    CHECK_FALSE(
        names_nordend("[preferred]\norg.freedesktop.impl.portal.FileChooser=gtk;nordend\n"));
    // Another key that starts the same is not the file chooser.
    CHECK_FALSE(names_nordend("[preferred]\norg.freedesktop.impl.portal.FileChooserX=nordend\n"));
}

TEST_CASE("portal config: set copies the desktop's file, unset removes the copy") {
    Places places;
    Places::write(places.system / "nordendtest-portals.conf", kGnome);
    const auto set = set_file_chooser(places.environment, places.portals);
    CHECK(set.ok);
    CHECK(set.message.find("Nordend is now the file chooser on nordendtest") == 0);
    CHECK(set.message.find("warning") == std::string::npos);
    const std::filesystem::path mine = places.user / "nordendtest-portals.conf";
    const std::string written = Places::read(mine);
    CHECK(written.starts_with("# Written by nordend --set-file-chooser from " +
                              (places.system / "nordendtest-portals.conf").string() + ".\n"));
    CHECK(names_nordend(written));
    CHECK(written.find("default=gnome;gtk;") != std::string::npos);

    const auto again = set_file_chooser(places.environment, places.portals);
    CHECK(again.ok);
    CHECK(again.message.starts_with("Nordend already is the file chooser"));
    CHECK(Places::read(mine) == written);

    const auto unset = unset_file_chooser(places.environment);
    CHECK(unset.ok);
    CHECK(unset.message.find("removed") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(mine));

    const auto nothing = unset_file_chooser(places.environment);
    CHECK(nothing.ok);
    CHECK(nothing.message.starts_with("Nordend is not the file chooser"));
}

TEST_CASE("portal config: a copy the user changed since is kept on unset") {
    Places places;
    Places::write(places.system / "portals.conf", kGnome);
    REQUIRE(set_file_chooser(places.environment, places.portals).ok);
    const std::filesystem::path mine = places.user / "nordendtest-portals.conf";
    Places::write(mine, Places::read(mine) + "org.freedesktop.impl.portal.Print=gtk\n");
    const auto unset = unset_file_chooser(places.environment);
    CHECK(unset.ok);
    CHECK(Places::read(mine) == std::string{kGnome} + "org.freedesktop.impl.portal.Print=gtk\n");
}

TEST_CASE("portal config: the user's own file is edited where xdg-desktop-portal reads it") {
    Places places;
    // The common portals.conf of the user's beats any desktop file of the system's.
    Places::write(places.system / "nordendtest-portals.conf", kGnome);
    const std::string own = "[preferred]\ndefault=gtk\n";
    Places::write(places.user / "portals.conf", own);
    REQUIRE(set_file_chooser(places.environment, places.portals).ok);
    CHECK_FALSE(std::filesystem::exists(places.user / "nordendtest-portals.conf"));
    CHECK(names_nordend(Places::read(places.user / "portals.conf")));
    REQUIRE(unset_file_chooser(places.environment).ok);
    CHECK(Places::read(places.user / "portals.conf") == own);  // the user's: never removed
}

TEST_CASE("portal config: a missing nordend.portal is warned about") {
    Places places;
    std::filesystem::remove(places.portals / "nordend.portal");
    const auto set = set_file_chooser(places.environment, places.portals);
    CHECK(set.ok);
    CHECK(set.message.find("warning: " + (places.portals / "nordend.portal").string()) !=
          std::string::npos);
}
