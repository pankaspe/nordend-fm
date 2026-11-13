#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text_field.hpp"

namespace nordend::views {

// The file chooser's bar (nordend --pick), over the status bar. At the left, when saving, the
// file's name; then the filter in use and the application's own choices ("Encoding: UTF-8", a
// checkbox "Open read-only"). At the right, when saving, New Folder; Cancel; and the
// application's word for choosing ("Open", "Save"). Loop thread only.
class PickBar {
public:
    static constexpr float kHeight = 48.0F;

    // A button at the left: a filter or a choice. Not a checkbox: a list opens from it.
    struct Item {
        std::string text;
        bool checkbox{false};
        bool checked{false};

        [[nodiscard]] bool operator==(const Item&) const = default;
    };

    struct Actions {
        std::function<void()> accept;
        std::function<void()> cancel;
        std::function<void()> new_folder;
        // An item was pressed; (x, y): its top left corner, where its list goes.
        std::function<void(std::size_t item, float x, float y)> item;
    };

    PickBar(std::function<void()> redraw, Actions actions);

    // accept: the button's word ("_Save" loses its mnemonic underscore); empty: "Open".
    void set_accept_label(const std::string& accept);

    void set_items(std::vector<Item> items);

    // Saving: the name field, with name in it, the part before its extension selected, and the
    // keyboard. nullopt: no field (choosing files to open).
    void set_save(std::optional<std::string> name);

    [[nodiscard]] bool saving() const noexcept { return saving_; }

    // The name typed; set_name replaces it as set_save does (the stem selected).
    [[nodiscard]] const std::string& name() const noexcept { return field_.text(); }

    void set_name(std::string name);

    // The name field has the keyboard (saving only); Tab moves it between the field and the view.
    [[nodiscard]] bool name_focused() const noexcept { return saving_ && name_focused_; }

    void set_name_focused(bool focused);

    // A key for the name field (typing, editing, moving): true when it took it.
    bool name_key(const ui::KeyEvent& event);

    // Text pasted into the name field (the first line).
    void paste(std::string_view text);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons);

    [[nodiscard]] bool claims(float x, float y) const;
    [[nodiscard]] bool clickable_at(float x, float y) const;
    [[nodiscard]] bool editable_at(float x, float y) const;  // the name field: a text cursor
    void pointer(const ui::PointerEvent& event);

    [[nodiscard]] const std::string& accept_label() const noexcept { return accept_; }

    [[nodiscard]] const std::vector<Item>& items() const noexcept { return items_; }

private:
    // What the pointer can be over: an item (its index), or one of these.
    enum class Button : std::uint8_t { item, new_folder, cancel, accept };

    struct Hit {
        Button button{Button::item};
        std::size_t item{0};

        [[nodiscard]] bool operator==(const Hit&) const = default;
    };

    [[nodiscard]] std::optional<Hit> hit(float x, float y) const;

    std::function<void()> redraw_;
    Actions actions_;
    std::string accept_{"Open"};
    std::vector<Item> items_;
    bool saving_{false};
    bool name_focused_{false};
    ui::TextField field_;
    // As last drawn.
    ui::Rect bounds_{};
    std::vector<ui::Rect> item_rects_;
    ui::Rect new_folder_button_{};
    ui::Rect cancel_button_{};
    ui::Rect accept_button_{};
    std::optional<Hit> hovered_;
};

// "_Open" → "Open": GTK's mnemonic underscores out, "__" kept as one.
[[nodiscard]] std::string without_mnemonic(std::string_view label);

}  // namespace nordend::views
