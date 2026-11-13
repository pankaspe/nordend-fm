// Nordend's own header (ROADMAP 3⅞): the Browser's path row as the window's header, the title
// bar's buttons on it, and where the window moves from.
#include <GLES3/gl3.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/config.hpp"
#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/theme/gtk.hpp"
#include "nordend/theme/palette.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/offscreen.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/text.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/title_bar.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::test::TempDir;
using nordend::ui::PointerButton;
using nordend::ui::PointerEvent;
using nordend::views::Browser;
using nordend::views::TitleBar;
using Kind = PointerEvent::Kind;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr float kWidth = 1000.0F;
constexpr float kHeight = 700.0F;
constexpr float kReserve = 120.0F;  // the window's buttons
constexpr float kRow = TitleBar::kIntegratedHeight / 2.0F;

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

std::unique_ptr<nordend::ui::Offscreen> offscreen() {
    try {
        return std::make_unique<nordend::ui::Offscreen>(static_cast<std::int32_t>(kWidth),
                                                        static_cast<std::int32_t>(kHeight));
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

// root/{alpha, beta, a long path of long names}, the Browser drawn with its own header.
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    fs::path deep = root / "a folder with a rather long name" / "and another long one inside it" /
                    "then a third that is long" / "the last";
    std::unique_ptr<nordend::ui::Offscreen> gl = offscreen();
    std::unique_ptr<nordend::ui::FontSystem> fonts;
    std::unique_ptr<nordend::ui::Renderer> renderer;
    std::unique_ptr<nordend::ui::IconSet> icons;
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(root / "alpha");
        fs::create_directories(root / "beta");
        fs::create_directories(deep);
        if (gl) {
            fonts = std::make_unique<nordend::ui::FontSystem>();
            renderer = std::make_unique<nordend::ui::Renderer>(*fonts);
            icons = std::make_unique<nordend::ui::IconSet>(*fonts);
        }
        browser.set_window_header(kReserve);
    }

    void draw() {
        renderer->begin_frame(static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight),
                              1.0F, {});
        browser.draw(*renderer, {0.0F, 0.0F, kWidth, kHeight}, nordend::theme::Palette{}, *icons,
                     Browser::Clock::now());
        renderer->end_frame();
    }

    bool open(const fs::path& folder) {
        browser.open(folder);
        const bool loaded = run_until(loop, [this] { return !browser.loading(); });
        draw();
        return loaded;
    }

    void click(float x, float y) {
        for (const Kind kind : {Kind::motion, Kind::press, Kind::release}) {
            browser.pointer(
                {.kind = kind, .x = x, .y = y, .button = PointerButton::left, .click_count = 1},
                Browser::Clock::now());
        }
        draw();
    }

    // The first point of the header row, from the left, where a press does not move the window.
    [[nodiscard]] std::optional<float> first_control() const {
        for (float x = 0.0F; x < kWidth; x += 1.0F) {
            if (!browser.window_drag_at(x, kRow)) {
                return x;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] fs::path here() const {
        return browser.history().at(browser.history_index()).folder;
    }
};

}  // namespace

TEST_CASE("header: the sidebar to the top, the path on top, the buttons' room kept free") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.open(f.deep));
    // The sidebar's top and the header's free space move the window; below them, nothing does.
    CHECK(f.browser.window_drag_at(10.0F, kRow));
    CHECK(f.browser.window_drag_at(kWidth - kReserve - 2.0F, kRow) ==
          !f.browser.breadcrumb().clickable_at(kWidth - kReserve - 2.0F, kRow));
    CHECK_FALSE(f.browser.window_drag_at(kWidth / 2.0F, TitleBar::kIntegratedHeight + 30.0F));
    // The path is in the header, never under the window's buttons.
    bool crumb = false;
    for (float x = 0.0F; x < kWidth; x += 2.0F) {
        if (f.browser.breadcrumb().clickable_at(x, kRow)) {
            crumb = true;
            CHECK_FALSE(f.browser.window_drag_at(x, kRow));  // a step is a step, not a handle
            CHECK(x < kWidth - kReserve);
        }
    }
    CHECK(crumb);
    CHECK_FALSE(f.browser.breadcrumb().clickable_at(kWidth / 2.0F, kRow + kRow));

    // Without it: the path row as before, nothing moves the window.
    f.browser.set_window_header(std::nullopt);
    f.draw();
    CHECK_FALSE(f.browser.window_drag_at(10.0F, kRow));
    CHECK_FALSE(f.browser.window_drag_at(kWidth - kReserve - 2.0F, kRow));
}

TEST_CASE("header: back and forward, acting on release over them") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.open(f.root / "alpha"));
    REQUIRE(f.open(f.root / "beta"));
    const std::optional<float> back = f.first_control();
    REQUIRE(back);
    const float back_x = back.value_or(0.0F) + 8.0F;
    const float forward_x = back_x + 28.0F;
    CHECK_FALSE(f.browser.window_drag_at(forward_x, kRow));

    f.click(back_x, kRow);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.here() == f.root / "alpha");
    f.click(back_x, kRow);  // nothing further back
    CHECK(f.here() == f.root / "alpha");
    f.click(forward_x, kRow);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.here() == f.root / "beta");

    // A press that leaves the button before the release does nothing.
    f.browser.pointer({.kind = Kind::press,
                       .x = back_x,
                       .y = kRow,
                       .button = PointerButton::left,
                       .click_count = 1},
                      Browser::Clock::now());
    f.browser.pointer({.kind = Kind::release,
                       .x = back_x,
                       .y = kRow + 200.0F,
                       .button = PointerButton::left,
                       .click_count = 1},
                      Browser::Clock::now());
    CHECK(f.here() == f.root / "beta");
}

TEST_CASE("header: a menu or a dialog open, the window does not move from the header") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.open(f.root));
    REQUIRE(f.browser.window_drag_at(10.0F, kRow));
    nordend::ui::KeyEvent f1;
    f1.keysym = XKB_KEY_F1;
    f.browser.key(f1, Browser::Clock::now());  // the shortcuts card
    f.draw();
    CHECK_FALSE(f.browser.window_drag_at(10.0F, kRow));
}

TEST_CASE("title bar, integrated: buttons in the header row, the window moved where told") {
    std::vector<std::string> actions;
    TitleBar bar{{.move = [&] { actions.emplace_back("move"); },
                  .resize = [&](nordend::ui::Edge /*edge*/) { actions.emplace_back("resize"); },
                  .minimize = [&] { actions.emplace_back("minimize"); },
                  .toggle_maximized = [&] { actions.emplace_back("maximize"); },
                  .window_menu = [&](float /*x*/, float /*y*/) { actions.emplace_back("menu"); },
                  .close = [&] { actions.emplace_back("close"); }},
                 [] {},
                 [](nordend::ui::CursorShape /*shape*/) {}};
    bar.set_integrated([](float x, float y) { return x < 200.0F && y < 40.0F; });
    const nordend::ui::Rect window{0.0F, 0.0F, 800.0F, 600.0F};
    bar.layout(window);
    CHECK(bar.integrated());
    // The content gets the whole window.
    CHECK(bar.content(window).y == 0.0F);
    CHECK(bar.content(window).height == 600.0F);
    // Close, maximize, minimize from the right, centred in the 40 px row.
    const float close = 800.0F - 8.0F - 12.0F;
    CHECK(bar.hit(close, kRow).part == TitleBar::Part::close);
    CHECK(bar.hit(close - 30.0F, kRow).part == TitleBar::Part::maximize);
    CHECK(bar.hit(close - 60.0F, kRow).part == TitleBar::Part::minimize);
    CHECK(bar.buttons_width() == 800.0F - (close - 60.0F - 12.0F) + 6.0F);  // to minimize's left
    // Moves only where the content says; elsewhere in the row the content keeps the pointer.
    CHECK(bar.hit(100.0F, kRow).part == TitleBar::Part::title);
    CHECK(bar.hit(400.0F, kRow).part == TitleBar::Part::none);
    CHECK(bar.hit(100.0F, 300.0F).part == TitleBar::Part::none);
    CHECK(bar.pointer({.kind = Kind::press,
                       .x = 100.0F,
                       .y = kRow,
                       .button = PointerButton::left,
                       .click_count = 2}));
    CHECK(actions == std::vector<std::string>{"maximize"});
    CHECK_FALSE(bar.pointer({.kind = Kind::press,
                             .x = 400.0F,
                             .y = kRow,
                             .button = PointerButton::left,
                             .click_count = 1}));

    // Fullscreen: no buttons, nothing moves the window.
    bar.set_state({.fullscreen = true});
    CHECK(bar.buttons_width() == 0.0F);
    CHECK(bar.hit(100.0F, kRow).part == TitleBar::Part::none);
}

TEST_CASE("config: [window] title_bar") {
    using TitleBarChoice = nordend::core::Config::Window::TitleBar;
    CHECK(nordend::core::parse_config("", "c.toml").config.window.title_bar ==
          TitleBarChoice::integrated);
    nordend::core::ConfigResult result =
        nordend::core::parse_config("[window]\ntitle_bar = \"compositor\"\n", "c.toml");
    CHECK(result.problems.empty());
    CHECK(result.config.window.title_bar == TitleBarChoice::compositor);
    result = nordend::core::parse_config("[window]\ntitle_bar = \"none\"\n", "c.toml");
    CHECK(result.config.window.title_bar == TitleBarChoice::integrated);
    REQUIRE(result.problems.size() == 1);
    CHECK(result.problems[0].find("\"integrated\" or \"compositor\"") != std::string::npos);
    result = nordend::core::parse_config("[window]\ntitle_bar = 3\n", "c.toml");
    CHECK(result.config.window.title_bar == TitleBarChoice::integrated);
    CHECK(result.problems.size() == 1);
}

TEST_CASE("palette: the header's colour, from colors.toml or GTK's header bar") {
    using nordend::theme::Color;
    using nordend::theme::parse_palette;
    nordend::theme::PaletteResult result =
        parse_palette("[colors]\nsurface = \"#102030\"\n", "colors.toml");
    CHECK(result.palette.header == Color::rgb(0x102030));  // the surface when not given
    result =
        parse_palette("[colors]\nsurface = \"#102030\"\nheader = \"#405060\"\n", "colors.toml");
    CHECK(result.palette.header == Color::rgb(0x405060));
    result = parse_palette("[colors]\nheader = \"blue\"\n", "colors.toml");
    CHECK(result.palette.header == result.palette.surface);

    nordend::theme::GtkColors gtk{.colors = {{"window_bg_color", Color::rgb(0x181A1B)},
                                             {"window_fg_color", Color::rgb(0xDBE0E4)},
                                             {"sidebar_bg_color", Color::rgb(0x202325)}},
                                  .source = "gtk.css",
                                  .files = {}};
    CHECK(nordend::theme::gtk_palette(gtk, true).value_or(nordend::theme::Palette{}).header ==
          Color::rgb(0x181A1B));
    gtk.colors.emplace("headerbar_bg_color", Color::rgb(0x303336));
    const nordend::theme::Palette p =
        nordend::theme::gtk_palette(gtk, true).value_or(nordend::theme::Palette{});
    CHECK(p.header == Color::rgb(0x303336));
    CHECK(p.surface_container_low == Color::rgb(0x202325));  // the sidebar, not the header bar
}

TEST_CASE("header: opacity preserves the header palette and sidebar background") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    REQUIRE(f.open(f.root));
    nordend::theme::Palette palette;
    palette.header = nordend::ui::Color::rgb(0xFF0000);
    palette.surface_container_low = nordend::ui::Color::rgb(0x00FF00);
    for (const float opacity : {0.0F, 0.5F, 1.0F}) {
        CAPTURE(opacity);
        f.renderer->begin_frame(static_cast<std::int32_t>(kWidth),
                                static_cast<std::int32_t>(kHeight), 1.0F,
                                nordend::ui::Color::rgb(0x0000FF), 0.0F, opacity);
        f.browser.draw(*f.renderer, {0, 0, kWidth, kHeight}, palette, *f.icons,
                       Browser::Clock::now());
        f.renderer->end_frame();
        const auto pixel = [](int x, int y) {
            std::array<std::uint8_t, 4> rgba{};
            glReadPixels(x, static_cast<int>(kHeight) - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba.data());
            return rgba;
        };
        // Blank header space reserved for buttons, and the sidebar above and below the header.
        const auto header = pixel(900, 2);
        CHECK(header[0] == header[3]);
        CHECK(header[1] == 0);
        CHECK(header[2] == 0);
        for (const int y : {2, 100}) {
            const auto sidebar = pixel(2, y);
            CHECK(sidebar[0] == 0);
            CHECK(sidebar[1] == sidebar[3]);
            CHECK(sidebar[2] == 0);
            CHECK(sidebar[3] == doctest::Approx(static_cast<double>(opacity) * 255).epsilon(0.005));
        }
        CHECK(header[3] == doctest::Approx(static_cast<double>(opacity) * 255).epsilon(0.005));
        CHECK(glGetError() == GL_NO_ERROR);
    }
}
