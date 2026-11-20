#include "nordend/views/sidebar.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "nordend/fs/trash.hpp"

namespace nordend::views {
namespace {

constexpr float kTop = 8.0F;  // above the first section
constexpr float kSectionHeight = 32.0F;
constexpr float kSectionGap = 8.0F;  // before every section but the first
constexpr float kInset = 8.0F;       // the highlight box inside the sidebar
constexpr float kIconSize = 18.0F;
constexpr float kIconLeft = 18.0F;
constexpr float kTextLeft = 46.0F;
constexpr float kTextSize = 13.0F;
constexpr float kSectionSize = 11.0F;
constexpr float kEjectSize = 16.0F;
// With a folder tree: the chevron before the icon, and each level of the tree further in.
constexpr float kChevronRoom = 14.0F;
constexpr float kChevronSize = 12.0F;
constexpr float kIndent = 14.0F;

std::string_view title_of(fs::Place::Section section) {
    switch (section) {
        case fs::Place::Section::places:
            return "PLACES";
        case fs::Place::Section::bookmarks:
            return "BOOKMARKS";
        case fs::Place::Section::devices:
            return "DEVICES";
    }
    return {};
}

// path is inside (or is) folder, comparing whole components.
bool inside(const std::filesystem::path& path, const std::filesystem::path& folder) {
    auto p = path.begin();
    for (auto f = folder.begin(); f != folder.end(); ++f, ++p) {
        if (f->empty()) {
            continue;  // a trailing slash
        }
        if (p == path.end() || *p != *f) {
            return false;
        }
    }
    return true;
}

// A place with a tree under it: a folder (not the trash, nor a disk not mounted).
bool has_tree(const fs::Place& place) { return !place.path.empty() && !fs::is_trash(place.path); }

}  // namespace

std::optional<std::size_t> place_holding(std::span<const fs::Place> places,
                                         const std::filesystem::path& path) {
    const std::filesystem::path normal = path.lexically_normal();
    std::optional<std::size_t> best;
    std::size_t best_depth = 0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const std::filesystem::path folder = places[i].path.lexically_normal();
        if (folder.empty() || !inside(normal, folder)) {  // empty: a disk not mounted
            continue;
        }
        const auto depth = static_cast<std::size_t>(std::distance(folder.begin(), folder.end()));
        if (!best || depth > best_depth) {
            best = i;
            best_depth = depth;
        }
    }
    return best;
}

Sidebar::Sidebar(std::function<void()> redraw, std::function<void(ui::CursorShape)> set_cursor,
                 std::function<void(const fs::Place&)> open)
    : redraw_{std::move(redraw)}, set_cursor_{std::move(set_cursor)}, open_{std::move(open)} {}

void Sidebar::set_places(std::vector<fs::Place> places) {
    // The cursor stays on its place when that is still listed.
    std::optional<std::filesystem::path> at;
    if (cursor_ && *cursor_ < places_.size()) {
        at = places_[*cursor_].path;
    }
    places_ = std::move(places);
    cursor_.reset();
    if (at) {
        const auto it = std::ranges::find(places_, *at, &fs::Place::path);
        if (it != places_.end()) {
            cursor_ = static_cast<std::size_t>(it - places_.begin());
        }
    }
    if (!cursor_) {
        cursor_folder_.clear();
    }
    if (focused_ && !cursor_ && !places_.empty()) {
        cursor_ = 0;
    }
    if (renaming_ && std::ranges::none_of(places_, [&](const fs::Place& place) {
            return place.section == fs::Place::Section::bookmarks && place.path == *renaming_;
        })) {
        renaming_.reset();  // gone meanwhile (another program changed the bookmarks)
    }
    hovered_.reset();
    current_.reset();
    current_folder_.clear();
    if (tree_) {
        tree_->keep_places(places_);
    }
    layout();
    redraw_();
}

void Sidebar::set_tree(std::unique_ptr<FolderTree> tree) {
    tree_ = std::move(tree);
    if (tree_) {
        tree_->set_change_handler([this] {
            layout();
            redraw_();
        });
    }
    cursor_folder_.clear();
    current_folder_.clear();
    hovered_.reset();
    layout();
    redraw_();
}

void Sidebar::set_show_hidden(bool show) {
    if (show != show_hidden_) {
        show_hidden_ = show;
        if (tree_) {
            layout();
            redraw_();
        }
    }
}

void Sidebar::release_under(const std::filesystem::path& mount_point) {
    if (tree_) {
        tree_->release_under(mount_point);
    }
}

void Sidebar::layout() {
    rows_.clear();
    float y = kTop;
    std::vector<FolderTree::Row> folders;
    for (std::size_t i = 0; i < places_.size(); ++i) {
        const fs::Place::Section section = places_[i].section;
        if (i == 0 || section != places_[i - 1].section) {
            if (i > 0) {
                y += kSectionGap;
            }
            rows_.push_back({.top = y, .place = std::nullopt, .section = section});
            y += kSectionHeight;
        }
        rows_.push_back({.top = y, .place = i, .section = section});
        y += kRowHeight;
        if (!tree_ || !has_tree(places_[i])) {
            continue;
        }
        folders.clear();
        tree_->rows_under(places_[i].path, show_hidden_, folders);
        for (FolderTree::Row& folder : folders) {
            rows_.push_back({.top = y,
                             .place = i,
                             .section = section,
                             .folder = std::move(folder.folder),
                             .depth = folder.depth,
                             .more = folder.more});
            y += kRowHeight;
        }
    }
    content_height_ = y + kTop;
    // A folder folded away (or gone) takes the cursor to the nearest one shown above it.
    if (cursor_ && *cursor_ < places_.size()) {
        const std::filesystem::path& place = places_[*cursor_].path;
        while (!cursor_folder_.empty() && !row_of(*cursor_, cursor_folder_)) {
            const std::filesystem::path parent = cursor_folder_.parent_path();
            cursor_folder_ =
                parent == place || parent == cursor_folder_ ? std::filesystem::path{} : parent;
        }
    }
    if (hovered_ && *hovered_ >= rows_.size()) {
        hovered_.reset();
    }
}

std::optional<std::size_t> Sidebar::row_of(std::size_t place,
                                           const std::filesystem::path& folder) const {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].place == place && rows_[i].more == 0 && rows_[i].folder == folder) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<FolderTree::Fold> Sidebar::fold_at(const Row& row) const {
    if (!tree_ || !row.place || row.more > 0 || !has_tree(places_[*row.place])) {
        return std::nullopt;
    }
    const std::filesystem::path& place = places_[*row.place].path;
    return tree_->fold_of(place, row.depth == 0 ? place : row.folder, show_hidden_);
}

float Sidebar::shift(const Row& row) const noexcept {
    return tree_ ? kChevronRoom + (static_cast<float>(row.depth) * kIndent) : 0.0F;
}

bool Sidebar::on_chevron(const Row& row, float x) const {
    const std::optional<FolderTree::Fold> fold = fold_at(row);
    if (!fold || (*fold != FolderTree::Fold::folded && *fold != FolderTree::Fold::unfolded)) {
        return false;
    }
    const float from = bounds_.x + kInset + (static_cast<float>(row.depth) * kIndent);
    return x >= from && x < bounds_.x + kIconLeft + shift(row);
}

void Sidebar::toggle(const Row& row) {
    const std::optional<FolderTree::Fold> fold = fold_at(row);
    if (!fold) {
        return;
    }
    const std::filesystem::path place = places_[*row.place].path;  // the tree lays rows_ out again
    const std::filesystem::path folder = row.depth == 0 ? place : row.folder;
    if (*fold == FolderTree::Fold::folded) {
        tree_->unfold(place, folder);
    } else if (*fold == FolderTree::Fold::unfolded) {
        tree_->fold(place, folder);
    }
}

float Sidebar::width_in(float window_width) const {
    const float room = window_width - kMinContent;
    return room < kMinWidth ? 0.0F : std::min(width_, room);
}

void Sidebar::set_width(float width) {
    width = std::clamp(width, kMinWidth, kMaxWidth);
    if (width != width_) {
        width_ = width;
        redraw_();
    }
}

void Sidebar::set_focused(bool focused) {
    if (focused == focused_) {
        return;
    }
    focused_ = focused;
    if (focused_) {
        cursor_ = current_;
        cursor_folder_ = current_ ? current_folder_ : std::filesystem::path{};
        if (!cursor_ && !places_.empty()) {
            cursor_ = 0;
        }
    }
    redraw_();
}

void Sidebar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons, const std::filesystem::path& current,
                   Clock::time_point now) {
    bounds_ = bounds;
    current_ = place_holding(places_, current);
    // In the tree, the deepest row shown that holds the current folder is marked instead.
    current_folder_.clear();
    if (current_ && tree_) {
        const std::filesystem::path normal = current.lexically_normal();
        std::size_t deepest = 0;
        for (const Row& row : rows_) {
            if (row.place == current_ && row.depth > deepest && row.more == 0 &&
                inside(normal, row.folder)) {
                deepest = row.depth;
                current_folder_ = row.folder;
            }
        }
    }
    renderer.fill_background(bounds, palette.surface_container_low);
    scroller_.set_extent(content_height_, bounds.height);
    scroller_.advance(now);
    const float offset = scroller_.offset();

    renderer.push_clip(bounds);
    eject_rects_.assign(places_.size(), ui::Rect{});
    // The tree may hold thousands of rows: start at the first one in sight.
    const auto first =
        std::ranges::lower_bound(rows_, offset - kSectionHeight, std::ranges::less{}, &Row::top);
    for (auto it = first; it != rows_.end(); ++it) {
        const Row& row = *it;
        const float y = bounds.y + row.top - offset;
        if (y > bounds.bottom()) {
            break;
        }
        if (y + kRowHeight < bounds.y) {
            continue;
        }
        if (!row.place) {
            renderer.draw_text(title_of(row.section),
                               bounds.x + kIconLeft + (tree_ ? kChevronRoom : 0.0F), y + 14.0F,
                               kSectionSize, palette.on_surface_variant);
            continue;
        }
        const float left = shift(row);
        if (row.more > 0) {
            renderer.draw_text(
                std::format("{} more folder{}", row.more, row.more == 1 ? "" : "s"),
                bounds.x + kTextLeft + left, y + 8.0F, kTextSize, palette.on_surface_variant,
                std::max(1.0F, bounds.width - kTextLeft - left - kInset - 4.0F), ui::Ellipsis::end);
            continue;
        }
        const std::size_t i = *row.place;
        const fs::Place& place = places_[i];
        const bool own = row.depth == 0;  // the place's own row, not a folder in its tree
        const auto at = static_cast<std::size_t>(it - rows_.begin());
        const std::optional<FolderTree::Fold> fold = fold_at(row);
        // The label being edited: its field on a quiet row, not on the cursor's colour.
        const bool editing = own && renaming_ && place.section == fs::Place::Section::bookmarks &&
                             place.path == *renaming_;
        const bool is_cursor = focused_ && cursor_ == i && cursor_folder_ == row.folder && !editing;
        const bool is_current = current_ == i && current_folder_ == row.folder;
        const bool drop = own && drop_target_ == place.path;
        // A disk not mounted is quieter until a click mounts it; so is a folder that could not
        // be read.
        const bool quiet = !place.disk.mounted || (!own && fold == FolderTree::Fold::failed);
        std::optional<ui::Color> fill;
        ui::Color icon = quiet ? palette.on_surface_variant : palette.primary;
        ui::Color text = quiet ? palette.on_surface_variant : palette.on_surface;
        if (drop) {
            fill = palette.primary_container;
            icon = palette.on_primary_container;
            text = palette.on_primary_container;
        } else if (is_cursor) {
            fill = palette.primary;
            icon = palette.on_primary;
            text = palette.on_primary;
        } else if (is_current) {
            fill = palette.surface_container_highest;
        } else if (hovered_ == at) {
            fill = palette.surface_container_high;
        }
        if (fill) {
            renderer.draw_box(
                {bounds.x + kInset, y + 1.0F, bounds.width - (2.0F * kInset), kRowHeight - 2.0F},
                {.fill = *fill,
                 .radius = ui::Corners::all(8.0F),
                 .border_width = drop ? 1.5F : 0.0F,
                 .border = palette.primary});
        }
        if (fold == FolderTree::Fold::folded || fold == FolderTree::Fold::unfolded) {
            icons.draw(
                renderer, fold == FolderTree::Fold::folded ? "chevron-right" : "chevron-down",
                bounds.x + kInset + 4.0F + left - kChevronRoom,
                y + ((kRowHeight - kChevronSize) / 2.0F), kChevronSize, text.with_alpha(0.7F));
        }
        icons.draw(renderer, own ? place.icon : "folder", bounds.x + kIconLeft + left,
                   y + ((kRowHeight - kIconSize) / 2.0F), kIconSize, icon);
        float text_room = bounds.width - kTextLeft - left - kInset - 4.0F;
        if (own && place.disk.ejectable) {
            const ui::Rect eject{bounds.right() - kInset - 6.0F - kEjectSize,
                                 y + ((kRowHeight - kEjectSize) / 2.0F), kEjectSize, kEjectSize};
            eject_rects_[i] = eject;
            icons.draw(renderer, "player-eject", eject.x, eject.y, kEjectSize,
                       hovered_eject_ == i ? icon : text.with_alpha(0.7F));
            text_room -= kEjectSize + 8.0F;
        }
        if (editing) {
            field_.draw(renderer,
                        {bounds.x + kTextLeft + left - 6.0F, y + 3.0F,
                         std::max(1.0F, text_room + 6.0F), kRowHeight - 6.0F},
                        kTextSize,
                        {.background = palette.surface_container_highest,
                         .border = palette.primary,
                         .text = palette.on_surface,
                         .selection = palette.secondary_container,
                         .caret = palette.on_surface});
            continue;
        }
        renderer.draw_text(own ? place.label : row.folder.filename().string(),
                           bounds.x + kTextLeft + left, y + 8.0F, kTextSize, text,
                           std::max(1.0F, text_room), ui::Ellipsis::middle);
    }
    if (bookmark_drop_) {
        // After the last bookmark, or where the first one would go: after the places.
        float line = kTop;
        for (const Row& row : rows_) {
            if (row.place && row.section != fs::Place::Section::devices) {
                line = row.top + kRowHeight;
            }
        }
        renderer.draw_box({bounds.x + kInset, bounds.y + line - offset - 1.0F,
                           bounds.width - (2.0F * kInset), 2.0F},
                          {.fill = palette.primary, .radius = ui::Corners::all(1.0F)});
    }
    renderer.pop_clip();

    const bool grabbed = drag_ || over_edge_;
    renderer.fill_rect({bounds.right() - (grabbed ? 1.0F : 1.0F / renderer.scale()), bounds.y,
                        grabbed ? 2.0F : 1.0F / renderer.scale(), bounds.height},
                       grabbed ? palette.primary : palette.outline_variant);
    update_hover();  // content may have moved under a still pointer
}

const fs::Place* Sidebar::place_under(float x, float y) const {
    const std::optional<std::size_t> place = place_at(x, y);
    return place ? &places_[*place] : nullptr;
}

void Sidebar::set_drop_target(std::optional<std::filesystem::path> path) {
    if (path != drop_target_) {
        drop_target_ = std::move(path);
        redraw_();
    }
}

bool Sidebar::in_bookmark_zone(float x, float y) const {
    if (bounds_.empty() || x < bounds_.x || x >= bounds_.right() - kEdgeGrab || y < bounds_.y ||
        y >= bounds_.bottom()) {
        return false;
    }
    const float content_y = y - bounds_.y + scroller_.offset();
    if (content_y >= content_height_ - kTop) {
        return true;  // below the last place
    }
    return std::ranges::any_of(rows_, [content_y](const Row& row) {
        return !row.place && row.section == fs::Place::Section::bookmarks && content_y >= row.top &&
               content_y < row.top + kSectionHeight;
    });
}

void Sidebar::set_bookmark_drop(bool shown) {
    if (shown != bookmark_drop_) {
        bookmark_drop_ = shown;
        redraw_();
    }
}

std::optional<std::size_t> Sidebar::eject_at(float x, float y) const {
    const std::optional<std::size_t> place = place_at(x, y);
    if (place && *place < eject_rects_.size() && !eject_rects_[*place].empty() &&
        x >= eject_rects_[*place].x - 4.0F && x < eject_rects_[*place].right() + 4.0F) {
        return place;
    }
    return std::nullopt;
}

bool Sidebar::clickable_at(float x, float y) const { return eject_at(x, y).has_value(); }

std::optional<std::size_t> Sidebar::row_at(float x, float y) const {
    if (bounds_.empty() || x < bounds_.x || x >= bounds_.right() || y < bounds_.y ||
        y >= bounds_.bottom()) {
        return std::nullopt;
    }
    const float content_y = y - bounds_.y + scroller_.offset();
    // The last row that starts above the point: rows are laid out top to bottom.
    const auto after = std::ranges::upper_bound(rows_, content_y, std::ranges::less{}, &Row::top);
    if (after == rows_.begin()) {
        return std::nullopt;
    }
    const Row& row = *std::prev(after);
    if (!row.place || content_y >= row.top + kRowHeight) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::prev(after) - rows_.begin());
}

std::optional<std::size_t> Sidebar::place_at(float x, float y) const {
    const std::optional<std::size_t> row = row_at(x, y);
    if (!row || rows_[*row].depth > 0) {
        return std::nullopt;
    }
    return rows_[*row].place;
}

bool Sidebar::on_edge(float x, float y) const {
    return !bounds_.empty() && std::abs(x - bounds_.right()) <= kEdgeGrab && y >= bounds_.y &&
           y < bounds_.bottom();
}

bool Sidebar::claims(float x, float y) const {
    if (drag_) {
        return true;
    }
    if (bounds_.empty() || y < bounds_.y || y >= bounds_.bottom()) {
        return false;
    }
    return (x >= bounds_.x && x < bounds_.right()) || on_edge(x, y);
}

void Sidebar::update_hover() {
    const std::optional<std::size_t> row =
        pointer_inside_ && !drag_ && !on_edge(pointer_x_, pointer_y_)
            ? row_at(pointer_x_, pointer_y_)
            : std::nullopt;
    if (row != hovered_) {
        hovered_ = row;
        redraw_();
    }
    const std::optional<std::size_t> eject = row ? eject_at(pointer_x_, pointer_y_) : std::nullopt;
    if (eject != hovered_eject_) {
        hovered_eject_ = eject;
        redraw_();
    }
    const bool over = drag_ || (pointer_inside_ && on_edge(pointer_x_, pointer_y_));
    if (over != over_edge_) {
        over_edge_ = over;
        set_cursor_(over ? ui::CursorShape::ew_resize : ui::CursorShape::standard);
        redraw_();
    }
}

void Sidebar::choose(std::size_t place) {
    const fs::Place chosen = places_[place];  // open_ may replace the places
    focused_ = false;
    redraw_();
    if (open_) {
        open_(chosen);
    }
}

void Sidebar::choose(const Row& row) {
    if (row.depth == 0) {
        choose(*row.place);
        return;
    }
    // A folder in the tree opens as a place of its own would.
    const fs::Place chosen{.section = places_[*row.place].section,
                           .label = row.folder.filename().string(),
                           .path = row.folder,
                           .icon = "folder",
                           .disk = {}};
    focused_ = false;
    redraw_();
    if (open_) {
        open_(chosen);
    }
}

std::optional<ui::Rect> Sidebar::eject_icon(const std::filesystem::path& path) const {
    for (std::size_t i = 0; i < places_.size() && i < eject_rects_.size(); ++i) {
        if (places_[i].path == path && !eject_rects_[i].empty()) {
            return eject_rects_[i];
        }
    }
    return std::nullopt;
}

void Sidebar::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::enter:
        case Kind::motion:
            pointer_inside_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            if (drag_) {
                set_width(drag_->start_width + (event.x - drag_->start_x));
            }
            update_hover();
            break;
        case Kind::leave:
            pointer_inside_ = false;
            update_hover();
            break;
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
            break;
        case Kind::press: {
            if (event.button == ui::PointerButton::right) {
                if (const std::optional<std::size_t> place = place_at(event.x, event.y);
                    place && menu_) {
                    const fs::Place chosen = places_[*place];  // menu_ may replace the places
                    menu_(chosen, event.x, event.y);
                }
                break;
            }
            if (event.button != ui::PointerButton::left) {
                break;
            }
            if (on_edge(event.x, event.y)) {
                if (event.click_count == 2) {
                    set_width(kWidth);
                } else {
                    drag_ = Drag{.start_x = event.x, .start_width = width_};
                }
                update_hover();
                break;
            }
            const std::optional<std::size_t> row = row_at(event.x, event.y);
            if (!row || rows_[*row].more > 0) {
                break;
            }
            if (on_chevron(rows_[*row], event.x)) {
                toggle(rows_[*row]);
                break;
            }
            if (rows_[*row].depth > 0) {
                choose(rows_[*row]);
                break;
            }
            if (const std::optional<std::size_t> place = rows_[*row].place) {
                const bool on_eject = *place < eject_rects_.size() &&
                                      !eject_rects_[*place].empty() &&
                                      event.x >= eject_rects_[*place].x - 4.0F &&
                                      event.x < eject_rects_[*place].right() + 4.0F;
                if (on_eject && eject_) {
                    const fs::Place chosen = places_[*place];  // eject_ may replace the places
                    eject_(chosen);
                } else {
                    choose(*place);
                }
            }
            break;
        }
        case Kind::release:
            if (drag_ && event.button == ui::PointerButton::left) {
                drag_.reset();
                update_hover();
            }
            break;
    }
}

void Sidebar::start_rename(const std::filesystem::path& folder) {
    const auto it = std::ranges::find_if(places_, [&](const fs::Place& place) {
        return place.section == fs::Place::Section::bookmarks && place.path == folder;
    });
    if (it == places_.end()) {
        return;
    }
    renaming_ = folder;
    renaming_from_ = it->label;
    field_.set_text(it->label);
    field_.select_all();
    redraw_();
}

bool Sidebar::rename_key(const ui::KeyEvent& event) {
    if (!renaming_) {
        return false;
    }
    const bool plain = !event.modifiers.ctrl && !event.modifiers.alt;
    if (plain && (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter)) {
        confirm_rename();
    } else if (plain && event.keysym == XKB_KEY_Escape) {
        cancel_rename();
    } else if (field_.key(event)) {
        redraw_();
    }
    return true;
}

bool Sidebar::rename_pointer(const ui::PointerEvent& event) {
    if (!renaming_) {
        return false;
    }
    if (field_.drag(event)) {
        redraw_();
        return true;
    }
    if (event.kind != ui::PointerEvent::Kind::press) {
        return false;
    }
    const ui::Rect& box = field_.bounds();
    const bool inside =
        event.x >= box.x && event.x < box.right() && event.y >= box.y && event.y < box.bottom();
    if (inside && event.button == ui::PointerButton::left) {
        field_.press(event);
        redraw_();
        return true;
    }
    confirm_rename();
    return false;
}

void Sidebar::paste(std::string_view text) {
    if (!renaming_) {
        return;
    }
    std::string line;
    for (const char c : text.substr(0, text.find_first_of("\r\n"))) {
        if (static_cast<unsigned char>(c) >= 0x20U && c != 0x7F) {
            line.push_back(c);
        }
    }
    field_.insert(line);
    redraw_();
}

std::string Sidebar::cut() {
    if (!renaming_) {
        return {};
    }
    std::string text = field_.cut();
    if (!text.empty()) {
        redraw_();
    }
    return text;
}

void Sidebar::confirm_rename() {
    if (!renaming_) {
        return;
    }
    const std::filesystem::path folder = *renaming_;
    const std::string label = field_.text();
    renaming_.reset();
    redraw_();
    if (label != renaming_from_ && rename_) {
        rename_(folder, label);
    }
}

void Sidebar::cancel_rename() {
    if (renaming_) {
        renaming_.reset();
        redraw_();
    }
}

bool Sidebar::edit_key(const ui::KeyEvent& event) {
    const bool rename = event.keysym == XKB_KEY_F2 && event.modifiers == ui::Modifiers{};
    const bool move = (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down) &&
                      event.modifiers == ui::Modifiers{.alt = true};
    if (!focused_ || (!rename && !move)) {
        return false;
    }
    if (!cursor_ || *cursor_ >= places_.size() || !cursor_folder_.empty() ||
        places_[*cursor_].section != fs::Place::Section::bookmarks) {
        return true;  // nothing to edit here, and nothing for the view either
    }
    const std::filesystem::path folder = places_[*cursor_].path;
    if (rename) {
        start_rename(folder);
    } else if (move_) {
        move_(folder, event.keysym == XKB_KEY_Up ? -1 : 1);
    }
    return true;
}

void Sidebar::set_cursor_to(const std::filesystem::path& path) {
    const auto it = std::ranges::find(places_, path, &fs::Place::path);
    if (it != places_.end()) {
        cursor_ = static_cast<std::size_t>(it - places_.begin());
        cursor_folder_.clear();
        redraw_();
    }
}

void Sidebar::put_cursor(const Row& row) {
    cursor_ = row.place;
    cursor_folder_ = row.folder;
}

bool Sidebar::key(const ui::KeyEvent& event) {
    if (places_.empty()) {
        if (event.keysym == XKB_KEY_Escape) {
            set_focused(false);
            return true;
        }
        return false;
    }
    // The rows the cursor stops on: the places, and the folders shown in their trees.
    std::vector<std::size_t> stops;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].place && rows_[i].more == 0) {
            stops.push_back(i);
        }
    }
    if (stops.empty()) {
        return false;
    }
    const std::optional<std::size_t> row = row_of(cursor_.value_or(0), cursor_folder_);
    const auto found = row ? std::ranges::find(stops, *row) : stops.end();
    const std::size_t at =
        found == stops.end() ? 0 : static_cast<std::size_t>(found - stops.begin());
    const std::size_t last = stops.size() - 1;
    const Row& here = rows_[stops[at]];
    switch (event.keysym) {
        case XKB_KEY_Up:
            put_cursor(rows_[stops[at > 0 ? at - 1 : 0]]);
            break;
        case XKB_KEY_Down:
            put_cursor(rows_[stops[std::min(at + 1, last)]]);
            break;
        case XKB_KEY_Home:
            put_cursor(rows_[stops.front()]);
            break;
        case XKB_KEY_End:
            put_cursor(rows_[stops.back()]);
            break;
        case XKB_KEY_Right:
            // Unfolds; unfolded already, goes into its first folder.
            if (fold_at(here) == FolderTree::Fold::folded) {
                put_cursor(here);
                toggle(here);
            } else if (at < last && rows_[stops[at + 1]].place == here.place &&
                       rows_[stops[at + 1]].depth > here.depth) {
                put_cursor(rows_[stops[at + 1]]);
            }
            break;
        case XKB_KEY_Left:
            // Folds; folded (or nothing in it), goes up to the folder that holds it.
            if (fold_at(here) == FolderTree::Fold::unfolded) {
                put_cursor(here);
                toggle(here);
            } else if (here.depth > 0) {
                cursor_ = here.place;
                cursor_folder_ =
                    here.depth > 1 ? here.folder.parent_path() : std::filesystem::path{};
            }
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            choose(here);
            return true;
        case XKB_KEY_Escape:
            set_focused(false);
            return true;
        default:
            return false;
    }
    // Keep the cursor's row in sight.
    if (cursor_) {
        if (const std::optional<std::size_t> shown = row_of(*cursor_, cursor_folder_)) {
            const float top = rows_[*shown].top;
            const float from = scroller_.destination();
            if (top < from) {
                scroller_.glide_to(top - kTop, Clock::now());
            } else if (top + kRowHeight > from + bounds_.height) {
                scroller_.glide_to(top + kRowHeight + kTop - bounds_.height, Clock::now());
            }
        }
    }
    redraw_();
    return true;
}

}  // namespace nordend::views
