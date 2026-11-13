#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"

namespace nordend::views {

// The file chooser's bar (nordend --pick), over the status bar: at the left the filter the
// application offers, a button that lists the others; at the right Cancel and the application's
// own word for choosing ("Open", "Select"). Loop thread only.
class PickBar {
public:
    static constexpr float kHeight = 48.0F;

    struct Actions {
        std::function<void()> accept;
        std::function<void()> cancel;
        // The filter button was pressed: list the filters under (x, y), the button's corner.
        std::function<void(float x, float y)> filters;
    };

    PickBar(std::function<void()> redraw, Actions actions);

    // accept: the button's word ("_Open" loses its mnemonic underscore); empty: "Open".
    void set_accept_label(const std::string& accept);

    // The filter in use; empty: no filter button (the application offers none).
    void set_filter_label(std::string filter);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons);

    [[nodiscard]] bool claims(float x, float y) const;
    [[nodiscard]] bool clickable_at(float x, float y) const;
    void pointer(const ui::PointerEvent& event);

    [[nodiscard]] const std::string& accept_label() const noexcept { return accept_; }

    [[nodiscard]] const std::string& filter_label() const noexcept { return filter_; }

private:
    enum class Button : std::uint8_t { filter, cancel, accept };
    [[nodiscard]] std::optional<Button> button_at(float x, float y) const;
    [[nodiscard]] const ui::Rect& rect(Button button) const;

    std::function<void()> redraw_;
    Actions actions_;
    std::string accept_{"Open"};
    std::string filter_;
    ui::Rect bounds_{};
    ui::Rect filter_button_{};  // as last drawn; empty without filters
    ui::Rect cancel_button_{};
    ui::Rect accept_button_{};
    std::optional<Button> hovered_;
};

// "_Open" → "Open": GTK's mnemonic underscores out, "__" kept as one.
[[nodiscard]] std::string without_mnemonic(std::string_view label);

}  // namespace nordend::views
