#include "nordend/views/scroll_bar.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/theme/palette.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/offscreen.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/scroller.hpp"
#include "nordend/ui/text.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/shortcuts_overlay.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::test::TempDir;
using nordend::ui::KeyEvent;
using nordend::ui::PointerButton;
using nordend::ui::PointerEvent;
using nordend::ui::Rect;
using nordend::ui::Scroller;
using nordend::views::Browser;
using nordend::views::ScrollBar;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

bool near(float value, float expected, float tolerance = 0.01F) {
    return std::abs(value - expected) <= tolerance;
}

// 1000 px of content in a 200 px area: a 40 px thumb travelling 160 px for an offset of 0-800.
struct Bar {
    Scroller scroller;
    ScrollBar bar;
    Rect track{0.0F, 0.0F, 100.0F, 200.0F};

    Bar() {
        scroller.set_extent(1000.0F, 200.0F);
        bar.place(track, scroller);
    }

    void jump(float offset) {
        scroller.jump_to(offset);
        bar.place(track, scroller);
    }
};

}  // namespace

TEST_CASE("scroll bar: a thin thumb sized and placed by the scroller") {
    Bar b;
    REQUIRE(b.bar.shown());
    const Rect top = b.bar.thumb();
    CHECK(near(top.x, 100.0F - 2.0F - ScrollBar::kThin));
    CHECK(near(top.width, ScrollBar::kThin));
    CHECK(near(top.y, 0.0F));
    CHECK(near(top.height, 40.0F));
    b.jump(400.0F);
    CHECK(near(b.bar.thumb().y, 80.0F));
    b.jump(800.0F);
    CHECK(near(b.bar.thumb().y, 160.0F));

    // A long list still gets a thumb one can take.
    b.scroller.set_extent(1'000'000.0F, 200.0F);
    b.bar.place(b.track, b.scroller);
    CHECK(near(b.bar.thumb().height, ScrollBar::kMinThumb));

    // Nothing to scroll: no bar, and nothing to take.
    b.scroller.set_extent(150.0F, 200.0F);
    b.bar.place(b.track, b.scroller);
    CHECK_FALSE(b.bar.shown());
    CHECK_FALSE(b.bar.claims(95.0F, 50.0F));
    CHECK_FALSE(b.bar.set_hovered(true));
}

TEST_CASE("scroll bar: the pointer finds it near the edge, and it widens there") {
    Bar b;
    CHECK(b.bar.claims(99.0F, 50.0F));
    CHECK(b.bar.claims(100.0F - 2.0F - ScrollBar::kReach, 50.0F));
    CHECK_FALSE(b.bar.claims(100.0F - 2.0F - ScrollBar::kReach - 1.0F, 50.0F));
    CHECK_FALSE(b.bar.claims(100.0F, 50.0F));  // past the edge
    CHECK_FALSE(b.bar.claims(95.0F, 200.0F));  // below the track

    CHECK(b.bar.set_hovered(true));
    CHECK_FALSE(b.bar.set_hovered(true));  // no change
    CHECK(near(b.bar.thumb().width, ScrollBar::kWide));
    CHECK(near(b.bar.thumb().x, 100.0F - 2.0F - ScrollBar::kWide));
    CHECK(b.bar.set_hovered(false));
    CHECK(near(b.bar.thumb().width, ScrollBar::kThin));
}

TEST_CASE("scroll bar: dragging the thumb moves the content, held where it was taken") {
    Bar b;
    const auto now = Scroller::Clock::now();
    REQUIRE(b.bar.press(95.0F, 10.0F, b.scroller, now));  // 10 px into the thumb
    CHECK(b.bar.dragging());
    CHECK(near(b.bar.thumb().width, ScrollBar::kWide));
    CHECK(b.bar.drag(95.0F, 90.0F, b.scroller));  // the thumb's top at 80: half way
    CHECK(near(b.scroller.offset(), 400.0F));
    // Beside the bar, past its ends: the drag goes on, clamped.
    CHECK(b.bar.drag(20.0F, 500.0F, b.scroller));
    CHECK(near(b.scroller.offset(), 800.0F));
    CHECK(b.bar.drag(20.0F, -50.0F, b.scroller));
    CHECK(near(b.scroller.offset(), 0.0F));
    CHECK(b.bar.release());
    CHECK_FALSE(b.bar.release());
    CHECK_FALSE(b.bar.drag(95.0F, 90.0F, b.scroller));  // over: motion is not the bar's
    CHECK(near(b.scroller.offset(), 0.0F));

    // Away from the bar a press is someone else's.
    CHECK_FALSE(b.bar.press(50.0F, 10.0F, b.scroller, now));
    CHECK_FALSE(b.bar.dragging());
}

TEST_CASE("scroll bar: a press on the track brings the thumb's middle there") {
    Bar b;
    const auto now = Scroller::Clock::now();
    REQUIRE(b.bar.press(95.0F, 100.0F, b.scroller, now));  // the thumb is at 0-40
    CHECK(b.scroller.animating());                         // it glides there
    b.scroller.advance(now + 1s);
    b.bar.place(b.track, b.scroller);
    // The thumb from 80 to 120: its middle under the pointer, at offset 400.
    CHECK(near(b.scroller.offset(), 400.0F, 4.0F));
    CHECK(near(b.bar.thumb().y, 80.0F, 4.0F));
    // The drag carries on from the middle.
    CHECK(b.bar.drag(95.0F, 180.0F, b.scroller));
    CHECK(near(b.scroller.offset(), 800.0F));
    CHECK(b.bar.release());
}

TEST_CASE("scroll bar: a horizontal one runs along the bottom") {
    Scroller scroller;
    scroller.set_extent(800.0F, 200.0F);
    ScrollBar bar{ScrollBar::Axis::horizontal};
    bar.place({0.0F, 0.0F, 200.0F, 100.0F}, scroller);
    CHECK(bar.claims(50.0F, 95.0F));
    CHECK_FALSE(bar.claims(50.0F, 50.0F));
    const Rect thumb = bar.thumb();
    CHECK(near(thumb.y, 100.0F - 2.0F - ScrollBar::kThin));
    CHECK(near(thumb.width, 50.0F));
    REQUIRE(bar.press(10.0F, 95.0F, scroller, Scroller::Clock::now()));
    CHECK(bar.drag(85.0F, 10.0F, scroller));  // the thumb's start at 75 of 150
    CHECK(near(scroller.offset(), 300.0F));
}

namespace {

constexpr std::int32_t kWidth = 1000;
constexpr std::int32_t kHeight = 400;  // too low for every shortcut

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

KeyEvent key(xkb_keysym_t keysym) {
    KeyEvent event;
    event.keysym = keysym;
    return event;
}

std::unique_ptr<nordend::ui::Offscreen> offscreen() {
    try {
        return std::make_unique<nordend::ui::Offscreen>(kWidth, kHeight);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

// A folder of 300 files, in a low window drawn offscreen so the hit tests find what is shown.
struct Fixture {
    TempDir dir;
    fs::path many = dir.path / "many";
    std::unique_ptr<nordend::ui::Offscreen> gl = offscreen();
    std::unique_ptr<nordend::ui::FontSystem> fonts;
    std::unique_ptr<nordend::ui::Renderer> renderer;
    std::unique_ptr<nordend::ui::IconSet> icons;
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape) {}};

    Fixture() {
        fs::create_directories(many);
        for (int i = 0; i < 300; ++i) {
            std::ofstream{many / ("file-" + std::to_string(1000 + i) + ".txt")} << "x";
        }
        browser.set_home_trash(dir.path / "Trash");
        if (gl) {
            fonts = std::make_unique<nordend::ui::FontSystem>();
            renderer = std::make_unique<nordend::ui::Renderer>(*fonts);
            icons = std::make_unique<nordend::ui::IconSet>(*fonts);
        }
    }

    void draw() {
        renderer->begin_frame(kWidth, kHeight, 1.0F, {});
        browser.draw(*renderer, {0.0F, 0.0F, kWidth, kHeight}, nordend::theme::Palette{}, *icons,
                     Browser::Clock::now());
        renderer->end_frame();
    }

    // Until nothing loads and nothing moves, drawing every frame.
    bool settle() {
        return run_until(loop, [this] {
            draw();
            return !browser.loading() && !browser.animating();
        });
    }

    void pointer(PointerEvent event) { browser.pointer(event, Browser::Clock::now()); }

    void press(float x, float y) {
        pointer({.kind = PointerEvent::Kind::press,
                 .x = x,
                 .y = y,
                 .button = PointerButton::left,
                 .click_count = 1});
    }

    void release(float x, float y) {
        pointer(
            {.kind = PointerEvent::Kind::release, .x = x, .y = y, .button = PointerButton::left});
    }

    void move(float x, float y) { pointer({.kind = PointerEvent::Kind::motion, .x = x, .y = y}); }
};

}  // namespace

TEST_CASE("scroll bar: the details' thumb widens under the pointer and drags the list") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.browser.open(f.many);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    const ScrollBar& bar = f.browser.details().scroll_bar();
    REQUIRE(bar.shown());
    const Rect thumb = bar.thumb();
    const float x = thumb.x + (thumb.width / 2.0F);
    const float y = thumb.y + (thumb.height / 2.0F);

    f.move(x, y);
    CHECK(bar.hovered());
    f.draw();
    CHECK(near(bar.thumb().width, ScrollBar::kWide));

    f.press(x, y);
    CHECK(bar.dragging());
    f.move(x, y + 100.0F);
    f.move(x, y + 150.0F);  // past the drag threshold: no band, no entry dragged
    f.draw();
    CHECK(f.browser.details().scroller().offset() > 0.0F);
    f.release(x, y + 150.0F);
    CHECK_FALSE(bar.dragging());
    CHECK(f.browser.details().folder()->selected_names().empty());  // the rows saw nothing

    // Away from the bar it goes thin again.
    f.move(x - 200.0F, y);
    f.draw();
    CHECK_FALSE(bar.hovered());
    CHECK(near(bar.thumb().width, ScrollBar::kThin));
}

TEST_CASE("scroll bar: the shortcuts scroll in a low window (#12)") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.browser.open(f.many);
    REQUIRE(f.settle());
    f.browser.key(key(XKB_KEY_F1), Browser::Clock::now());
    nordend::views::ShortcutsOverlay& overlay = f.browser.shortcuts_overlay();
    REQUIRE(overlay.is_open());
    REQUIRE(f.settle());
    const Scroller& scroller = overlay.scroller();
    REQUIRE(scroller.max_offset() > 0.0F);
    CHECK(overlay.scroll_bar().shown());

    // Keys: End to the last group, Home back, arrows and pages a step at a time.
    CHECK(f.browser.key(key(XKB_KEY_End), Browser::Clock::now()));
    REQUIRE(f.settle());
    CHECK(near(scroller.offset(), scroller.max_offset()));
    CHECK(f.browser.key(key(XKB_KEY_Home), Browser::Clock::now()));
    REQUIRE(f.settle());
    CHECK(near(scroller.offset(), 0.0F));
    f.browser.key(key(XKB_KEY_Down), Browser::Clock::now());
    REQUIRE(f.settle());
    CHECK(scroller.offset() > 0.0F);
    f.browser.key(key(XKB_KEY_Page_Up), Browser::Clock::now());
    REQUIRE(f.settle());
    CHECK(near(scroller.offset(), 0.0F));
    CHECK(overlay.is_open());

    // The wheel.
    const Rect list = overlay.list_area();
    f.pointer({.kind = PointerEvent::Kind::scroll,
               .x = list.x + 50.0F,
               .y = list.y + 50.0F,
               .wheel_y = 1.0F});
    REQUIRE(f.settle());
    CHECK(scroller.offset() > 0.0F);

    // The scrollbar: a press on it keeps the card open, a drag moves it.
    const Rect thumb = overlay.scroll_bar().thumb();
    const float x = thumb.x + (thumb.width / 2.0F);
    f.press(x, thumb.y + 2.0F);
    f.release(x, thumb.y + 2.0F);
    CHECK(overlay.is_open());
    f.press(x, thumb.y + 2.0F);
    f.move(x, list.bottom() + 100.0F);
    f.release(x, list.bottom() + 100.0F);
    f.draw();
    CHECK(near(scroller.offset(), scroller.max_offset()));
    CHECK(overlay.is_open());

    // Anywhere else a click closes it, as before.
    f.press(list.x + 50.0F, list.y + 50.0F);
    CHECK_FALSE(overlay.is_open());
}
