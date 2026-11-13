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
constexpr float kButtonPadding = 16.0F;
constexpr float kMinButtonWidth = 88.0F;
constexpr float kTextSize = 13.0F;
constexpr float kIconSize = 14.0F;
constexpr float kMaxItemWidth = 280.0F;
constexpr float kFieldWidth = 260.0F;
constexpr float kMinFieldWidth = 120.0F;
constexpr float kCheckSize = 14.0F;

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

void PickBar::set_items(std::vector<Item> items) {
    items_ = std::move(items);
    hovered_.reset();
    redraw_();
}

void PickBar::set_save(std::optional<std::string> name) {
    saving_ = name.has_value();
    name_focused_ = saving_;
    if (name) {
        set_name(std::move(*name));
    }
    redraw_();
}

void PickBar::set_name(std::string name) {
    // "photo.jpg": "photo" selected, so typing replaces the name and keeps the extension; a
    // name starting with its only dot (".bashrc") is all stem.
    const std::size_t dot = name.rfind('.');
    const std::size_t stem = dot == std::string::npos || dot == 0 ? name.size() : dot;
    field_.set_text(std::move(name));
    field_.select(0, stem);
    redraw_();
}

void PickBar::set_name_focused(bool focused) {
    if (focused != name_focused_) {
        name_focused_ = focused;
        redraw_();
    }
}

bool PickBar::name_key(const ui::KeyEvent& event) {
    if (!name_focused()) {
        return false;
    }
    const bool taken = field_.key(event);
    if (taken) {
        redraw_();
    }
    return taken;
}

void PickBar::paste(std::string_view text) {
    if (!name_focused()) {
        return;
    }
    field_.insert(text.substr(0, text.find_first_of("\r\n")));
    redraw_();
}

void PickBar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons) {
    bounds_ = bounds;
    renderer.fill_rect({bounds.x, bounds.y, bounds.width, 1.0F / renderer.scale()},
                       palette.outline_variant);
    const float top = bounds.y + ((bounds.height - kButtonHeight) / 2.0F);
    const float text_y = top + ((kButtonHeight - kTextSize) / 2.0F) - 2.0F;
    const auto width_of = [&](std::string_view text) {
        return std::max(kMinButtonWidth,
                        text_width(renderer, text, kTextSize) + (2 * kButtonPadding));
    };

    // The right side, from the right.
    const float accept_width = width_of(accept_);
    accept_button_ = {bounds.right() - kPadding - accept_width, top, accept_width, kButtonHeight};
    const float cancel_width = width_of("Cancel");
    cancel_button_ = {accept_button_.x - kButtonGap - cancel_width, top, cancel_width,
                      kButtonHeight};
    new_folder_button_ = {};
    float right = cancel_button_.x - kButtonGap;
    if (saving_) {
        const float width = width_of("New Folder");
        new_folder_button_ = {right - width, top, width, kButtonHeight};
        right = new_folder_button_.x - (2 * kButtonGap);
    }

    // The left side: the name, then the items. The items' room comes first (an application's
    // choice must not go missing): the name field gives way down to its minimum.
    float x = bounds.x + kPadding;
    std::vector<float> widths;
    float items_width = 0.0F;
    for (const Item& item : items_) {
        const float extra = item.checkbox ? kCheckSize + 8.0F : kIconSize + 6.0F;
        const float wanted =
            text_width(renderer, item.text, kTextSize) + (2 * kButtonPadding) + extra;
        widths.push_back(std::min(wanted, kMaxItemWidth));
        items_width += widths.back() + kButtonGap;
    }
    if (saving_) {
        const float width = std::clamp(right - x - items_width, kMinFieldWidth, kFieldWidth);
        if (x + width <= right) {
            field_.draw(
                renderer, {x, top, width, kButtonHeight}, kTextSize,
                {.background = palette.surface_container_highest,
                 .border = name_focused_ ? palette.primary : palette.outline_variant,
                 .text = palette.on_surface,
                 .selection = palette.secondary_container,
                 .caret = name_focused_ ? palette.on_surface : palette.surface_container_highest});
            x += width + kButtonGap;
        }
    }
    item_rects_.assign(items_.size(), ui::Rect{});
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const Item& item = items_[i];
        const float width = widths[i];
        if (x + width > right) {
            break;  // a window too narrow even for them: the rest waits for more room
        }
        const ui::Rect area{x, top, width, kButtonHeight};
        item_rects_[i] = area;
        const bool hovered = hovered_ == Hit{.button = Button::item, .item = i};
        renderer.draw_box(area, {.fill = hovered ? palette.surface_container_highest
                                                 : palette.surface_container_high,
                                 .radius = ui::Corners::all(8.0F)});
        float text_x = area.x + kButtonPadding;
        if (item.checkbox) {
            const ui::Rect box{text_x, top + ((kButtonHeight - kCheckSize) / 2.0F), kCheckSize,
                               kCheckSize};
            renderer.draw_box(
                box, {.fill = item.checked ? palette.primary : palette.surface_container_highest,
                      .radius = ui::Corners::all(3.0F),
                      .border_width = item.checked ? 0.0F : 1.0F,
                      .border = palette.outline});
            if (item.checked) {
                draw_centered(renderer, "✓", box, box.y - 1.0F, kCheckSize - 2.0F,
                              palette.on_primary);
            }
            text_x += kCheckSize + 8.0F;
        }
        const float room =
            area.right() - kButtonPadding - text_x - (item.checkbox ? 0.0F : kIconSize + 6.0F);
        renderer.draw_text(item.text, text_x, text_y, kTextSize, palette.on_surface,
                           std::max(1.0F, room), ui::Ellipsis::end);
        if (!item.checkbox) {
            icons.draw(renderer, "chevron-down", area.right() - kButtonPadding - kIconSize + 6.0F,
                       top + ((kButtonHeight - kIconSize) / 2.0F), kIconSize,
                       palette.on_surface_variant);
        }
        x += width + kButtonGap;
    }

    const auto button = [&](const ui::Rect& area, Button which, std::string_view text, bool main) {
        const bool hovered = hovered_ && hovered_->button == which;
        ui::Color fill = main ? palette.primary : palette.surface_container_high;
        if (hovered) {
            fill = main ? palette.primary.with_alpha(0.85F) : palette.surface_container_highest;
        }
        renderer.draw_box(area, {.fill = fill, .radius = ui::Corners::all(8.0F)});
        draw_centered(renderer, text, area, text_y, kTextSize,
                      main ? palette.on_primary : palette.on_surface);
    };
    if (saving_) {
        button(new_folder_button_, Button::new_folder, "New Folder", false);
    }
    button(cancel_button_, Button::cancel, "Cancel", false);
    button(accept_button_, Button::accept, accept_, true);
}

std::optional<PickBar::Hit> PickBar::hit(float x, float y) const {
    for (std::size_t i = 0; i < item_rects_.size(); ++i) {
        if (!item_rects_[i].empty() && item_rects_[i].contains(x, y)) {
            return Hit{.button = Button::item, .item = i};
        }
    }
    for (const auto& [area, which] :
         {std::pair{new_folder_button_, Button::new_folder},
          std::pair{cancel_button_, Button::cancel}, std::pair{accept_button_, Button::accept}}) {
        if (!area.empty() && area.contains(x, y)) {
            return Hit{.button = which, .item = 0};
        }
    }
    return std::nullopt;
}

bool PickBar::claims(float x, float y) const { return !bounds_.empty() && bounds_.contains(x, y); }

bool PickBar::clickable_at(float x, float y) const { return hit(x, y).has_value(); }

bool PickBar::editable_at(float x, float y) const {
    return saving_ && !field_.bounds().empty() && field_.bounds().contains(x, y);
}

void PickBar::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    const std::optional<Hit> over =
        event.kind == Kind::leave ? std::nullopt : hit(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left) {
        return;
    }
    if (editable_at(event.x, event.y)) {
        set_name_focused(true);
        field_.click(event.x, false);
        redraw_();
        return;
    }
    if (!over) {
        return;
    }
    switch (over->button) {
        case Button::item:
            if (actions_.item) {
                const ui::Rect& area = item_rects_.at(over->item);
                actions_.item(over->item, area.x, area.y);
            }
            break;
        case Button::new_folder:
            if (actions_.new_folder) {
                actions_.new_folder();
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
