#include "nordend/views/shortcuts_overlay.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "nordend/views/shortcuts.hpp"

namespace nordend::views {
namespace {

constexpr float kMaxCardWidth = 820.0F;
constexpr float kWideCardWidth = 1200.0F;  // three columns, when the window has room
constexpr float kMargin = 32.0F;           // around the card, at least
constexpr float kPadding = 28.0F;          // inside the card
constexpr float kColumnGap = 40.0F;        // between the columns
constexpr float kTitleSize = 16.0F;
constexpr float kHeadHeight = 44.0F;
constexpr float kGroupSize = 11.0F;
constexpr float kGroupHeight = 30.0F;
constexpr float kGroupGap = 10.0F;  // before every group but a column's first
constexpr float kRowHeight = 22.0F;
constexpr float kTextSize = 13.0F;
constexpr float kKeysShare = 0.38F;  // of a column, for the keys

using Column = std::array<std::string_view, 5>;
using Layout = std::array<Column, 3>;

// The groups in each column of the card: two columns, or three in a wide window. Both hold every
// group.
constexpr Layout kTwoColumns{{
    {"General", "Moving", "Zoom", "Quick Look", ""},
    {"Selecting", "Files", "Sidebar", "Tabs", ""},
    {"", "", "", "", ""},
}};
constexpr Layout kThreeColumns{{
    {"General", "Zoom", "Quick Look", "", ""},
    {"Moving", "Selecting", "Sidebar", "", ""},
    {"Files", "Tabs", "", "", ""},
}};

std::string upper(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(),
                           [](char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// The height of a column of groups.
float column_height(const Column& groups) {
    float height = 0.0F;
    bool first = true;
    for (const std::string_view group : groups) {
        if (group.empty()) {
            continue;
        }
        height += (first ? 0.0F : kGroupGap) + kGroupHeight;
        first = false;
        for (const Shortcut& shortcut : shortcuts()) {
            if (shortcut.group == group && !shortcut.keys.empty()) {
                height += kRowHeight;
            }
        }
    }
    return height;
}

}  // namespace

std::vector<std::string_view> overlay_groups() {
    std::vector<std::string_view> groups;
    for (const Column& column : kTwoColumns) {
        for (const std::string_view group : column) {
            if (!group.empty()) {
                groups.push_back(group);
            }
        }
    }
    return groups;
}

ShortcutsOverlay::ShortcutsOverlay(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ShortcutsOverlay::open() {
    if (!open_) {
        open_ = true;
        scroller_.jump_to(0.0F);  // from the top, every time
        redraw_();
    }
}

void ShortcutsOverlay::close() {
    if (open_) {
        open_ = false;
        scroll_bar_.release();
        scroll_bar_.set_hovered(false);
        redraw_();
    }
}

bool ShortcutsOverlay::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    if (!open_) {
        return false;
    }
    switch (event.kind) {
        case Kind::scroll:
            if (event.wheel_y != 0.0F) {
                scroller_.wheel(event.wheel_y, now);
            } else if (event.scroll_y != 0.0F) {
                scroller_.finger(event.scroll_y, now);
            }
            if (event.scroll_finger && event.scroll_stop) {
                scroller_.release(now);
            }
            redraw_();
            return true;
        case Kind::press:
            if (event.button == ui::PointerButton::left &&
                scroll_bar_.press(event.x, event.y, scroller_, now)) {
                redraw_();
                return true;
            }
            return false;
        case Kind::release:
            if (event.button == ui::PointerButton::left && scroll_bar_.release()) {
                scroll_bar_.set_hovered(scroll_bar_.claims(event.x, event.y));
                redraw_();
                return true;
            }
            return false;
        case Kind::enter:
        case Kind::motion:
            if (scroll_bar_.drag(event.x, event.y, scroller_)) {
                redraw_();
                return true;
            }
            if (scroll_bar_.set_hovered(scroll_bar_.claims(event.x, event.y))) {
                redraw_();
            }
            return false;
        case Kind::leave:
            scroll_bar_.release();
            if (scroll_bar_.set_hovered(false)) {
                redraw_();
            }
            return false;
    }
    return false;
}

bool ShortcutsOverlay::key(const ui::KeyEvent& event, Clock::time_point now) {
    if (!open_ || event.modifiers != ui::Modifiers{}) {
        return false;
    }
    // A page: what is shown less a row, so that one stays in sight.
    const float page = std::max(kRowHeight, list_.height - kRowHeight);
    const float from = scroller_.destination();
    switch (event.keysym) {
        case XKB_KEY_Up:
            scroller_.glide_to(from - kArrowStep, now);
            break;
        case XKB_KEY_Down:
            scroller_.glide_to(from + kArrowStep, now);
            break;
        case XKB_KEY_Page_Up:
            scroller_.glide_to(from - page, now);
            break;
        case XKB_KEY_Page_Down:
            scroller_.glide_to(from + page, now);
            break;
        case XKB_KEY_Home:
            scroller_.glide_to(0.0F, now);
            break;
        case XKB_KEY_End:
            scroller_.glide_to(scroller_.max_offset(), now);
            break;
        default:
            return false;
    }
    redraw_();
    return true;
}

void ShortcutsOverlay::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                            const theme::Palette& palette, Clock::time_point now) {
    if (!open_) {
        return;
    }
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const bool wide = bounds.width - (2.0F * kMargin) >= kWideCardWidth;
    const Layout& layout = wide ? kThreeColumns : kTwoColumns;
    const std::size_t columns = wide ? 3 : 2;
    const float width =
        std::min(wide ? kWideCardWidth : kMaxCardWidth, bounds.width - (2.0F * kMargin));
    float content = 0.0F;
    for (const Column& column : layout) {
        content = std::max(content, column_height(column));
    }
    const float height =
        std::min(bounds.height - (2.0F * kMargin), kHeadHeight + content + (2.0F * kPadding));
    const ui::Rect card{bounds.x + ((bounds.width - width) / 2.0F),
                        bounds.y + ((bounds.height - height) / 2.0F), width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(14.0F),
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    renderer.draw_text("Keyboard shortcuts", card.x + kPadding, card.y + kPadding, kTitleSize,
                       palette.on_surface);
    // The groups scroll under the title when the card cannot hold them all.
    const float list_top = card.y + kPadding + kHeadHeight;
    list_ = {card.x, list_top, width, std::max(0.0F, card.bottom() - kPadding - list_top)};
    scroller_.set_extent(content, list_.height);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    renderer.push_clip(list_);

    const float column_width =
        (width - (2.0F * kPadding) - (kColumnGap * static_cast<float>(columns - 1))) /
        static_cast<float>(columns);
    const float keys_width = column_width * kKeysShare;
    for (std::size_t c = 0; c < columns; ++c) {
        const float left =
            card.x + kPadding + (static_cast<float>(c) * (column_width + kColumnGap));
        float y = list_top - offset;
        bool first = true;
        for (const std::string_view group : layout.at(c)) {
            if (group.empty()) {
                continue;
            }
            y += first ? 0.0F : kGroupGap;
            first = false;
            renderer.fill_rect({left, y, column_width, 1.0F / renderer.scale()},
                               palette.outline_variant);
            renderer.draw_text(upper(group), left, y + 10.0F, kGroupSize,
                               palette.on_surface_variant);
            y += kGroupHeight;
            for (const Shortcut& shortcut : shortcuts()) {
                if (shortcut.group != group || shortcut.keys.empty()) {
                    continue;
                }
                renderer.draw_text(shortcut.keys, left, y, kTextSize, palette.primary,
                                   keys_width - 8.0F);
                renderer.draw_text(shortcut.action, left + keys_width, y, kTextSize,
                                   palette.on_surface, column_width - keys_width);
                y += kRowHeight;
            }
        }
    }
    renderer.pop_clip();
    scroll_bar_.place(list_, scroller_);
    scroll_bar_.draw(renderer, palette);
    renderer.pop_clip();
}

}  // namespace nordend::views
