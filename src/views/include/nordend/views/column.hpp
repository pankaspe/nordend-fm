#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/scroller.hpp"
#include "nordend/views/cut_marks.hpp"
#include "nordend/views/drop_mark.hpp"
#include "nordend/views/edit_slot.hpp"
#include "nordend/views/folder.hpp"
#include "nordend/views/list_size.hpp"
#include "nordend/views/scroll_bar.hpp"

namespace nordend::views {

// One folder in the column view, drawn as a virtualized, scrolling list of icon and name. Each
// frame draws only the rows inside the viewport and asks the folder to sniff their types. The
// folder (cursor, selection, listing) is shared: the grid can show the same one.
//
// Loop thread only.
class Column {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kRowHeight = kListSizes[kUsualListSize].row;  // at the usual size

    explicit Column(std::shared_ptr<Folder> folder);

    [[nodiscard]] Folder& folder() noexcept { return *folder_; }

    [[nodiscard]] const Folder& folder() const noexcept { return *folder_; }

    [[nodiscard]] const std::shared_ptr<Folder>& shared_folder() const noexcept { return folder_; }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return folder_->path(); }

    // The rows' size (Ctrl+plus/minus in the lists); the cursor's row stays where it is on screen.
    void set_size(const ListSize& size);

    [[nodiscard]] const ListSize& size() const noexcept { return size_; }

    // Rows a page key moves: what fits in the last drawn viewport, less one.
    [[nodiscard]] std::size_t page_rows() const;

    // Lays out and draws into bounds at time now (advancing the scroll animation). The cursor is
    // strong in the focused column, quiet in the others (the path that leads to it). A name
    // being edited (edit) is left out and its place noted there; cut entries are faded.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, bool focused, Clock::time_point now, EditSlot* edit = nullptr,
              const CutMarks* cut = nullptr, const DropMark* drop = nullptr);

    // A scroll animation is running: the window should keep drawing frames.
    [[nodiscard]] bool animating() const noexcept { return scroller_.animating(); }

    [[nodiscard]] ui::Scroller& scroller() noexcept { return scroller_; }

    [[nodiscard]] const ui::Scroller& scroller() const noexcept { return scroller_; }

    [[nodiscard]] ScrollBar& scroll_bar() noexcept { return scroll_bar_; }

    [[nodiscard]] const ScrollBar& scroll_bar() const noexcept { return scroll_bar_; }

    // Row at a window point, from the last drawn bounds; nullopt outside the rows.
    [[nodiscard]] std::optional<std::size_t> row_at(float x, float y) const;

    // The point is on row's icon or name as last drawn (not the empty rest of the row).
    [[nodiscard]] bool on_item(std::size_t row, float x) const;

    // Where the rows were last drawn, scrolled by the scroller's offset.
    [[nodiscard]] const ui::Rect& rows_area() const noexcept { return rows_area_; }

    // The rows a band touches, in content points (BandLane).
    [[nodiscard]] std::vector<std::size_t> rows_in(const ui::Rect& content) const;

    // Below the cursor's row, near its left: where a menu opened from the keyboard goes; nullopt
    // when the row is not shown.
    [[nodiscard]] std::optional<std::pair<float, float>> cursor_anchor() const;

    // Hover highlight; returns true if it changed.
    bool set_hovered(std::optional<std::size_t> row);

private:
    void reveal_cursor(Folder::Reveal reveal, Clock::time_point now);

    std::shared_ptr<Folder> folder_;
    ui::Scroller scroller_;
    ScrollBar scroll_bar_{ScrollBar::Axis::vertical, 5.0F};  // clear of the divider's grab
    ui::Rect rows_area_{};  // where the rows were last drawn, for hit testing
    // Where the icon and name of each drawn row end (window x), from item_first_ on.
    std::vector<float> item_right_;
    std::size_t item_first_{0};
    std::optional<std::size_t> hovered_;
    ListSize size_{kListSizes[kUsualListSize]};
};

}  // namespace nordend::views
