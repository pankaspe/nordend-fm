#include "nordend/views/scroll_bar.hpp"

#include <algorithm>

namespace nordend::views {

void ScrollBar::place(const ui::Rect& track, const ui::Scroller& scroller) {
    track_ = track;
    max_ = scroller.max_offset();
    offset_ = scroller.offset();
    if (!shown() && !dragging_) {
        hovered_ = false;
    }
}

float ScrollBar::thumb_length() const noexcept {
    const float content = max_ + length();
    return std::min(length(), std::max(kMinThumb, length() * length() / content));
}

float ScrollBar::offset_for(float thumb_start) const noexcept {
    const float travel = length() - thumb_length();
    if (travel <= 0.0F) {
        return 0.0F;
    }
    return std::clamp((thumb_start - start()) / travel, 0.0F, 1.0F) * max_;
}

ui::Rect ScrollBar::thumb() const noexcept {
    if (!shown()) {
        return {};
    }
    const float thickness = hovered_ || dragging_ ? kWide : kThin;
    const float along = thumb_length();
    const float at = start() + ((length() - along) * std::clamp(offset_ / max_, 0.0F, 1.0F));
    if (axis_ == Axis::vertical) {
        return {track_.right() - inset_ - thickness, at, thickness, along};
    }
    return {at, track_.bottom() - inset_ - thickness, along, thickness};
}

bool ScrollBar::claims(float x, float y) const noexcept {
    if (!shown()) {
        return false;
    }
    if (axis_ == Axis::vertical) {
        return y >= track_.y && y < track_.bottom() && x < track_.right() &&
               x >= track_.right() - inset_ - kReach;
    }
    return x >= track_.x && x < track_.right() && y < track_.bottom() &&
           y >= track_.bottom() - inset_ - kReach;
}

bool ScrollBar::set_hovered(bool hovered) noexcept {
    hovered = hovered && shown();
    if (hovered == hovered_) {
        return false;
    }
    hovered_ = hovered;
    return true;
}

bool ScrollBar::press(float x, float y, ui::Scroller& scroller, Clock::time_point now) {
    if (!claims(x, y)) {
        return false;
    }
    const float along = axis_ == Axis::vertical ? y : x;
    const ui::Rect shown_thumb = thumb();
    const float thumb_start = axis_ == Axis::vertical ? shown_thumb.y : shown_thumb.x;
    const float thumb_end = thumb_start + thumb_length();
    dragging_ = true;
    hovered_ = true;
    if (along >= thumb_start && along < thumb_end) {
        grab_ = along - thumb_start;
    } else {
        // Beside the thumb: its middle comes here, and the drag carries on from there.
        grab_ = thumb_length() / 2.0F;
        scroller.glide_to(offset_for(along - grab_), now);
    }
    return true;
}

bool ScrollBar::drag(float x, float y, ui::Scroller& scroller) {
    if (!dragging_) {
        return false;
    }
    const float along = axis_ == Axis::vertical ? y : x;
    scroller.jump_to(offset_for(along - grab_));
    offset_ = scroller.offset();
    return true;
}

bool ScrollBar::release() noexcept {
    const bool was = dragging_;
    dragging_ = false;
    return was;
}

void ScrollBar::draw(ui::Renderer& renderer, const theme::Palette& palette, float fade) const {
    if (!shown()) {
        return;
    }
    float alpha = 0.45F;
    if (dragging_) {
        alpha = 0.8F;
    } else if (hovered_) {
        alpha = 0.65F;
    }
    const ui::Rect box = thumb();
    const float thickness = axis_ == Axis::vertical ? box.width : box.height;
    renderer.draw_box(box, {.fill = palette.on_surface_variant.with_alpha(alpha * fade),
                            .radius = ui::Corners::all(thickness / 2.0F)});
}

}  // namespace nordend::views
