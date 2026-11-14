#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/scroller.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/views/scroll_bar.hpp"

namespace nordend::views {

// The groups the overlay shows, column by column: a shortcut in another group would not be shown.
[[nodiscard]] std::vector<std::string_view> overlay_groups();

// Every keyboard shortcut (shortcuts()), by group, on a card over the window. F1, "?", the
// status bar's "?" open it; those, Escape or a click close it. When the window is too low for
// them all, the groups scroll under the title: wheel, touchpad, arrows, page keys, Home/End and
// the scrollbar.
//
// Loop thread only.
class ShortcutsOverlay {
public:
    using Clock = ui::Scroller::Clock;

    // Logical pixels the arrows move.
    static constexpr float kArrowStep = 66.0F;

    explicit ShortcutsOverlay(std::function<void()> redraw);

    void open();
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // Scrolling: keep drawing frames.
    [[nodiscard]] bool animating() const noexcept { return open_ && scroller_.animating(); }

    // The pointer while open: returns true when the event was the scrollbar's or a scroll (a
    // press it did not take closes the card).
    bool pointer(const ui::PointerEvent& event, Clock::time_point now);

    // Arrows, page keys, Home and End scroll; returns true when the key did.
    bool key(const ui::KeyEvent& event, Clock::time_point now);

    [[nodiscard]] const ui::Scroller& scroller() const noexcept { return scroller_; }

    [[nodiscard]] const ScrollBar& scroll_bar() const noexcept { return scroll_bar_; }

    // Where the groups were last drawn (what scrolls).
    [[nodiscard]] const ui::Rect& list_area() const noexcept { return list_; }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              Clock::time_point now);

private:
    std::function<void()> redraw_;
    bool open_{false};
    ui::Scroller scroller_;
    ScrollBar scroll_bar_{ScrollBar::Axis::vertical, 8.0F};  // inside the card's rounded corner
    ui::Rect list_{};
};

}  // namespace nordend::views
