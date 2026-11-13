#include "nordend/views/pick_bar.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "draw_helpers.hpp"

namespace nordend::views {
namespace {

constexpr float kPadding = 12.0F;
constexpr float kButtonHeight = 32.0F;
constexpr float kButtonGap = 8.0F;
constexpr float kButtonPadding = 18.0F;
constexpr float kMinButtonWidth = 96.0F;
constexpr float kTextSize = 13.0F;
constexpr float kIconSize = 14.0F;
constexpr float kMaxFilterWidth = 320.0F;

}  // namespace

std::string without_mnemonic(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '_') {
            if (i + 1 < label.size() && label[i + 1] == '_') {
                out += '_';
                ++i;
            }
            continue;
        }
        out += label[i];
    }
    return out;
}

PickBar::PickBar(std::function<void()> redraw, Actions actions)
    : redraw_{std::move(redraw)}, actions_{std::move(actions)} {}

void PickBar::set_accept_label(const std::string& accept) {
    accept_ = without_mnemonic(accept);
    if (accept_.empty()) {
        accept_ = "Open";
    }
    redraw_();
}

void PickBar::set_filter_label(std::string filter) {
    filter_ = std::move(filter);
    redraw_();
}

void PickBar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons) {
    bounds_ = bounds;
    renderer.fill_rect({bounds.x, bounds.y, bounds.width, 1.0F / renderer.scale()},
                       palette.outline_variant);
    const float top = bounds.y + ((bounds.height - kButtonHeight) / 2.0F);
    const auto width_of = [&](std::string_view text) {
        return std::max(kMinButtonWidth,
                        text_width(renderer, text, kTextSize) + (2 * kButtonPadding));
    };
    const float accept_width = width_of(accept_);
    accept_button_ = {bounds.right() - kPadding - accept_width, top, accept_width, kButtonHeight};
    const float cancel_width = width_of("Cancel");
    cancel_button_ = {accept_button_.x - kButtonGap - cancel_width, top, cancel_width,
                      kButtonHeight};
    filter_button_ = {};
    if (!filter_.empty()) {
        const float room = cancel_button_.x - kButtonGap - (bounds.x + kPadding);
        const float wanted =
            text_width(renderer, filter_, kTextSize) + (2 * kButtonPadding) + kIconSize + 6.0F;
        const float width = std::clamp(wanted, 0.0F, std::min(room, kMaxFilterWidth));
        if (width > 3 * kButtonPadding) {
            filter_button_ = {bounds.x + kPadding, top, width, kButtonHeight};
        }
    }

    const auto box = [&](const ui::Rect& button, Button which, ui::Color fill, ui::Color hover) {
        renderer.draw_box(
            button, {.fill = hovered_ == which ? hover : fill, .radius = ui::Corners::all(8.0F)});
    };
    const float text_y = top + ((kButtonHeight - kTextSize) / 2.0F) - 2.0F;
    if (!filter_button_.empty()) {
        box(filter_button_, Button::filter, palette.surface_container_high,
            palette.surface_container_highest);
        const float room = filter_button_.width - (2 * kButtonPadding) - kIconSize - 6.0F;
        renderer.draw_text(filter_, filter_button_.x + kButtonPadding, text_y, kTextSize,
                           palette.on_surface, room, ui::Ellipsis::end);
        icons.draw(
            renderer, "chevron-down", filter_button_.right() - kButtonPadding - kIconSize + 6.0F,
            top + ((kButtonHeight - kIconSize) / 2.0F), kIconSize, palette.on_surface_variant);
    }
    box(cancel_button_, Button::cancel, palette.surface_container_high,
        palette.surface_container_highest);
    draw_centered(renderer, "Cancel", cancel_button_, text_y, kTextSize, palette.on_surface);
    box(accept_button_, Button::accept, palette.primary, palette.primary.with_alpha(0.85F));
    draw_centered(renderer, accept_, accept_button_, text_y, kTextSize, palette.on_primary);
}

const ui::Rect& PickBar::rect(Button button) const {
    switch (button) {
        case Button::filter:
            return filter_button_;
        case Button::cancel:
            return cancel_button_;
        case Button::accept:
            break;
    }
    return accept_button_;
}

std::optional<PickBar::Button> PickBar::button_at(float x, float y) const {
    for (const Button button : {Button::filter, Button::cancel, Button::accept}) {
        const ui::Rect& area = rect(button);
        if (!area.empty() && area.contains(x, y)) {
            return button;
        }
    }
    return std::nullopt;
}

bool PickBar::claims(float x, float y) const { return !bounds_.empty() && bounds_.contains(x, y); }

bool PickBar::clickable_at(float x, float y) const { return button_at(x, y).has_value(); }

void PickBar::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    const std::optional<Button> over =
        event.kind == Kind::leave ? std::nullopt : button_at(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left || !over) {
        return;
    }
    switch (*over) {
        case Button::filter:
            if (actions_.filters) {
                actions_.filters(filter_button_.x, filter_button_.y);
            }
            break;
        case Button::cancel:
            if (actions_.cancel) {
                actions_.cancel();
            }
            break;
        case Button::accept:
            if (actions_.accept) {
                actions_.accept();
            }
            break;
    }
}

}  // namespace nordend::views
