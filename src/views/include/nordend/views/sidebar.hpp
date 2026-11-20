#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nordend/fs/places.hpp"
#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/scroller.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text_field.hpp"
#include "nordend/views/folder_tree.hpp"

namespace nordend::views {

// The place that holds path: the one with the longest path that path is in (Pictures before
// Home, Home before the file system); nullopt when none does.
[[nodiscard]] std::optional<std::size_t> place_holding(std::span<const fs::Place> places,
                                                       const std::filesystem::path& path);

// The places at the left of the window, in sections (places, bookmarks, devices), the one that
// holds the current folder marked. A click opens a place; so does Enter when the keyboard is in
// the sidebar (the browser moves it there and back), where Up/Down, Home/End move a cursor and
// Escape leaves; a right click asks for a place's menu. Its right edge drags to resize it; a double
// click there restores the width.
//
// With a folder tree, a chevron before each folder place unfolds the folders in it, and theirs in
// turn; Right and Left do the same from the keyboard, and Enter or a click on a name opens one.
// Folders in the tree take no drops and have no menu.
//
// Loop thread only.
class Sidebar {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kWidth = 200.0F;  // at start, and after a double click on its edge
    static constexpr float kMinWidth = 140.0F;
    static constexpr float kMaxWidth = 400.0F;
    // The view at its right keeps at least this: the sidebar narrows, then is left out.
    static constexpr float kMinContent = 480.0F;
    static constexpr float kEdgeGrab = 4.0F;  // either side of the edge, where the pointer grabs
    static constexpr float kRowHeight = 30.0F;

    // open: a place was chosen (click or Enter); a disk not mounted has no path yet.
    Sidebar(std::function<void()> redraw, std::function<void(ui::CursorShape)> set_cursor,
            std::function<void(const fs::Place&)> open);

    // The eject icon of an external disk was clicked.
    void set_eject_handler(std::function<void(const fs::Place&)> eject) {
        eject_ = std::move(eject);
    }

    // Where the eject icon of the place at a window point is, as last drawn; nullopt when it
    // has none (tests).
    [[nodiscard]] std::optional<ui::Rect> eject_icon(const std::filesystem::path& path) const;

    void set_places(std::vector<fs::Place> places);

    // The folder tree under the places ([sidebar] tree); nullptr: none.
    void set_tree(std::unique_ptr<FolderTree> tree);

    [[nodiscard]] FolderTree* tree() const noexcept { return tree_.get(); }

    // Hidden folders are shown in the tree (Ctrl+H).
    void set_show_hidden(bool show);

    // A disk is about to be unmounted or ejected: its folders in the tree are folded.
    void release_under(const std::filesystem::path& mount_point);

    // A right click on a place: menu(place, x, y).
    using MenuHandler = std::function<void(const fs::Place& place, float x, float y)>;

    void set_menu_handler(MenuHandler menu) { menu_ = std::move(menu); }

    [[nodiscard]] const std::vector<fs::Place>& places() const noexcept { return places_; }

    // current: the folder shown, whose place is marked.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const std::filesystem::path& current, Clock::time_point now);

    // Left out of this frame (no room, or hidden): it takes no pointer events.
    void not_drawn() noexcept { bounds_ = {}; }

    // Its width in a window this wide: narrowed to leave the view kMinContent, 0 when even
    // kMinWidth does not fit.
    [[nodiscard]] float width_in(float window_width) const;

    [[nodiscard]] float width() const noexcept { return width_; }

    // Clamped to [kMinWidth, kMaxWidth].
    void set_width(float width);

    // The pointer at (x, y) is the sidebar's: over it or its edge, or dragging the edge.
    [[nodiscard]] bool claims(float x, float y) const;

    // An eject icon is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    void pointer(const ui::PointerEvent& event, Clock::time_point now);

    // A key while the keyboard is in the sidebar: false when it is not one of its keys. Enter
    // and Escape hand the keyboard back (focused() turns false).
    bool key(const ui::KeyEvent& event);

    // The keyboard is in the sidebar: its cursor shows, starting on the current place.
    void set_focused(bool focused);

    [[nodiscard]] bool focused() const noexcept { return focused_; }

    // The place under the cursor, or the one whose tree holds it.
    [[nodiscard]] std::optional<std::size_t> cursor() const noexcept { return cursor_; }

    // The folder under the cursor in cursor()'s tree; empty on the place itself.
    [[nodiscard]] const std::filesystem::path& cursor_folder() const noexcept {
        return cursor_folder_;
    }

    // The row marked as the current folder's, as last drawn: the deepest one shown that holds
    // it, in the tree of the current place; empty for the place itself.
    [[nodiscard]] const std::filesystem::path& current_folder() const noexcept {
        return current_folder_;
    }

    [[nodiscard]] bool animating() const noexcept { return scroller_.animating(); }

    // The place at a window point, as last drawn.
    [[nodiscard]] const fs::Place* place_under(float x, float y) const;

    // A drag over the window would drop into the place at this path (nullopt: none), drawn
    // marked.
    void set_drop_target(std::optional<std::filesystem::path> path);

    // Where a drop adds bookmarks: the "BOOKMARKS" title, or the free space below the places.
    [[nodiscard]] bool in_bookmark_zone(float x, float y) const;

    // A drag over that zone: a line shows where the bookmarks would go (after the last one).
    void set_bookmark_drop(bool shown);

    // Bookmarks edited from the sidebar: renamed (an empty label gives the folder's name back),
    // moved one shown bookmark up (step -1) or down (+1).
    using RenameHandler =
        std::function<void(const std::filesystem::path& folder, const std::string& label)>;
    using MoveHandler = std::function<void(const std::filesystem::path& folder, int step)>;

    void set_rename_handler(RenameHandler rename) { rename_ = std::move(rename); }

    void set_move_handler(MoveHandler move) { move_ = std::move(move); }

    // A text field over the label of folder's bookmark, its label selected. Enter or a press
    // elsewhere confirms, Escape cancels. Nothing when folder is not a bookmark.
    void start_rename(const std::filesystem::path& folder);

    [[nodiscard]] bool renaming() const noexcept { return renaming_.has_value(); }

    // The label's field, while renaming.
    [[nodiscard]] const ui::TextField& field() const noexcept { return field_; }

    // While renaming every key is the field's (true).
    bool rename_key(const ui::KeyEvent& event);

    // While renaming: a press in the field is the field's (true), and so is the drag that follows
    // until the button is up; elsewhere it confirms and returns false, so the press goes on to
    // what is under it.
    bool rename_pointer(const ui::PointerEvent& event);

    // The button came up elsewhere: the field's drag ends.
    void end_field_drag() noexcept { field_.end_drag(); }

    // Text pasted into the field (its first line, control characters left out).
    void paste(std::string_view text);

    // While renaming: the selected text, taken out of the label.
    std::string cut();

    void confirm_rename();
    void cancel_rename();

    // F2 (rename) and Alt+Up, Alt+Down (move) on the bookmark under the cursor, while focused.
    // True for these keys even off a bookmark: they are the sidebar's, never the view's.
    bool edit_key(const ui::KeyEvent& event);

    // The cursor on the place at path, when it is listed (a bookmark pointed elsewhere).
    void set_cursor_to(const std::filesystem::path& path);

private:
    struct Row {
        float top{0.0F};                   // content coordinates
        std::optional<std::size_t> place;  // nullopt: the title of section
        fs::Place::Section section{fs::Place::Section::places};
        // In place's tree: the folder, at depth 1 right under the place; empty for the place.
        std::filesystem::path folder{};  // NOLINT(readability-redundant-member-init): GCC's
                                         // -Wmissing-field-initializers
        std::size_t depth{0};
        std::size_t more{0};  // the row that says how many more folders the level has
    };

    void layout();
    // The row of a place (folder empty) or of a folder in its tree, as laid out.
    [[nodiscard]] std::optional<std::size_t> row_of(std::size_t place,
                                                    const std::filesystem::path& folder) const;
    // The row of a place or a folder at a window point (titles left out).
    [[nodiscard]] std::optional<std::size_t> row_at(float x, float y) const;
    // A place's own row only.
    [[nodiscard]] std::optional<std::size_t> place_at(float x, float y) const;
    // How the row's folder stands in the tree; nullopt without a tree, or for a place that has
    // none (the trash, a disk not mounted).
    [[nodiscard]] std::optional<FolderTree::Fold> fold_at(const Row& row) const;
    // A click at x on the row hits its chevron.
    [[nodiscard]] bool on_chevron(const Row& row, float x) const;
    void toggle(const Row& row);
    // How far the row's chevron, icon and name move right.
    [[nodiscard]] float shift(const Row& row) const noexcept;
    void put_cursor(const Row& row);
    // The place whose eject icon is at a point (a little wider than the icon).
    [[nodiscard]] std::optional<std::size_t> eject_at(float x, float y) const;
    [[nodiscard]] bool on_edge(float x, float y) const;
    void update_hover();
    void choose(std::size_t place);
    void choose(const Row& row);

    std::function<void()> redraw_;
    std::function<void(ui::CursorShape)> set_cursor_;
    std::function<void(const fs::Place&)> open_;
    std::function<void(const fs::Place&)> eject_;
    // A place's eject icon, by place, as last drawn (empty for the others).
    std::vector<ui::Rect> eject_rects_;
    std::optional<std::size_t> hovered_eject_;
    MenuHandler menu_;
    std::vector<fs::Place> places_;
    std::vector<Row> rows_;
    float content_height_{0.0F};
    std::optional<std::size_t> current_;  // the place holding the folder shown, as last drawn
    std::filesystem::path current_folder_;
    std::optional<std::size_t> cursor_;  // while focused
    std::filesystem::path cursor_folder_;
    std::optional<std::size_t> hovered_;  // a row
    std::unique_ptr<FolderTree> tree_;
    bool show_hidden_{false};
    std::optional<std::filesystem::path> drop_target_;
    bool bookmark_drop_{false};
    bool focused_{false};
    float width_{kWidth};
    ui::Rect bounds_{};  // as last drawn; empty when not drawn
    ui::Scroller scroller_;

    struct Drag {
        float start_x;
        float start_width;
    };

    std::optional<Drag> drag_;
    RenameHandler rename_;
    MoveHandler move_;
    ui::TextField field_;                            // the label being edited
    std::optional<std::filesystem::path> renaming_;  // whose bookmark
    std::string renaming_from_;                      // its label when the edit began
    bool over_edge_{false};
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
};

}  // namespace nordend::views
