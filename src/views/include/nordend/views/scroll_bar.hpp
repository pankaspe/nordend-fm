#pragma once

#include <cstdint>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/scroller.hpp"

namespace nordend::views {

// The scrollbar of one scrolling area along one axis: a thin thumb on the area's right edge
// (vertical) or bottom edge (horizontal), shown only when there is something to scroll. Under the
// pointer it widens; a press on the thumb drags it (the content follows, the thumb held where it
// was taken), a press beside it on the track brings the thumb's middle there.
//
// The owner places it at every draw, after the scroller has its extent (place()), draws it over
// the content (draw()), and offers it a left press before its own targets (press()); while
// dragging() the motion and the release are the bar's too.
//
// Loop thread only.
class ScrollBar {
public:
    using Clock = ui::Scroller::Clock;

    enum class Axis : std::uint8_t { vertical, horizontal };

    static constexpr float kThin = 4.0F;    // at rest
    static constexpr float kWide = 8.0F;    // under the pointer and while dragged
    static constexpr float kReach = 14.0F;  // from the edge inwards, where the pointer finds it
    static constexpr float kMinThumb = 24.0F;

    // inset: the gap between the bar and the area's edge.
    explicit ScrollBar(Axis axis = Axis::vertical, float inset = 2.0F) noexcept
        : axis_{axis}, inset_{inset} {}

    // The bar runs along track (its y range for a vertical bar, x range for a horizontal one) at
    // track's right or bottom edge; the scroller gives the range and the offset.
    void place(const ui::Rect& track, const ui::Scroller& scroller);

    // Something to scroll, as last placed.
    [[nodiscard]] bool shown() const noexcept { return max_ > 0.0F && length() > 0.0F; }

    // The point is near enough to the bar to take it.
    [[nodiscard]] bool claims(float x, float y) const noexcept;

    // Widened while hovered or dragged; returns true if that changed.
    bool set_hovered(bool hovered) noexcept;

    [[nodiscard]] bool hovered() const noexcept { return hovered_; }

    // A left press at (x, y): taken (true) when the bar claims the point. On the thumb the drag
    // starts there; on the track the scroller glides so the thumb's middle comes under the
    // pointer, and the drag goes on from that middle.
    bool press(float x, float y, ui::Scroller& scroller, Clock::time_point now);

    // Pointer motion while dragging: the scroller jumps to follow. Returns true when dragging.
    bool drag(float x, float y, ui::Scroller& scroller);

    // The button went up (or the pointer left): ends a drag. Returns true if one was running.
    bool release() noexcept;

    [[nodiscard]] bool dragging() const noexcept { return dragging_; }

    // The thumb, as last placed (empty when not shown).
    [[nodiscard]] ui::Rect thumb() const noexcept;

    // fade multiplies the colour's alpha (Quick Look fades in).
    void draw(ui::Renderer& renderer, const theme::Palette& palette, float fade = 1.0F) const;

private:
    [[nodiscard]] float length() const noexcept {
        return axis_ == Axis::vertical ? track_.height : track_.width;
    }

    [[nodiscard]] float start() const noexcept {
        return axis_ == Axis::vertical ? track_.y : track_.x;
    }

    [[nodiscard]] float thumb_length() const noexcept;

    // The offset that puts the thumb's start at position (along the axis).
    [[nodiscard]] float offset_for(float thumb_start) const noexcept;

    Axis axis_;
    float inset_;
    ui::Rect track_{};
    float max_{0.0F};     // the scroller's max_offset()
    float offset_{0.0F};  // the scroller's offset when placed
    bool hovered_{false};
    bool dragging_{false};
    float grab_{0.0F};  // pointer position minus the thumb's start, along the axis
};

}  // namespace nordend::views
