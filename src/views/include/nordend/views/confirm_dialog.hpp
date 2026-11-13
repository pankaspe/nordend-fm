#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"

namespace nordend::views {

// The question before something that cannot be undone (deleting for good, replacing a file
// when saving), on a card over the window: Enter does it, Escape gives up; so do its two buttons.
// While it is open it takes every key and click.
//
// Loop thread only.
class ConfirmDialog {
public:
    // confirmed: Enter or the action's button; false: Escape or Cancel.
    using Answer = std::function<void(bool confirmed)>;

    explicit ConfirmDialog(std::function<void()> redraw);

    // title: "Delete “P1.ORF” permanently?"; detail: the line under it ("Adding up the size…",
    // changed later by set_detail); action: the button that does it ("Delete"); note: the dimmed
    // line under the detail.
    void open(std::string title, std::string detail, std::string action, Answer answer,
              std::string note = "Nothing goes to the trash: this cannot be undone.");

    void set_detail(std::string detail);

    // Closes without answering.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return answer_ != nullptr; }

    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

    // Every key while open: true.
    bool key(const ui::KeyEvent& event);

    void pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    // One of its buttons is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const {
        return std::ranges::any_of(
            buttons_, [x, y](const ui::Rect& button) { return button.contains(x, y); });
    }

private:
    // The buttons, left to right.
    static constexpr std::size_t kCancel = 0;
    static constexpr std::size_t kAction = 1;
    static constexpr std::size_t kButtons = 2;

    void choose(bool confirmed);

    std::function<void()> redraw_;
    std::string title_;
    std::string detail_;
    std::string action_;
    std::string note_;
    Answer answer_;
    std::array<ui::Rect, kButtons> buttons_{};  // as last drawn
    std::optional<std::size_t> hovered_;
};

}  // namespace nordend::views
