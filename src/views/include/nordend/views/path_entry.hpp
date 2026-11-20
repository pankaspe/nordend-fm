#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "nordend/core/worker_pool.hpp"
#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text_field.hpp"

namespace nordend::views {

// What text names, from the folder shown: "~" and "~/..." in the home folder, a relative path
// from shown, a file:// URI, "trash:///". Normalized lexically, as the breadcrumb is, with no
// trailing '/'. nullopt for an empty text, or a relative one while the trash is shown.
[[nodiscard]] std::optional<std::filesystem::path> typed_path(std::string_view text,
                                                              const std::filesystem::path& shown,
                                                              const std::filesystem::path& home);

// The text split at its last '/': the folder part ("~/Pic" -> "~/") and the name being typed
// ("Pic"). The folder part is empty when there is no '/' (a name relative to the folder shown).
// A lone "~" is the home folder, as "~/" is: the folder part, with nothing being typed inside.
struct TypedParts {
    std::string_view folder;
    std::string_view name;
};

[[nodiscard]] TypedParts split_typed(std::string_view text);

// Tab: the names among folders that start with typed, in name order, and the start they all
// share. Hidden names only when typed starts with a dot, or show_hidden.
struct Completion {
    std::vector<std::string> matches;
    std::string common;
};

[[nodiscard]] Completion complete(std::string_view typed, std::span<const std::string> folders,
                                  bool show_hidden);

// The path typed in the header (Ctrl+L): a text field in the breadcrumb's place. Tab completes
// a folder's name from a listing made on a worker, once for each folder typed, so no key waits
// on the disk. The field is drawn red when nothing in its folder starts with the name typed, and
// Enter goes only to a path that exists.
//
// Loop thread only.
class PathEntry {
public:
    struct Listing {                         // one folder, as the worker found it
        std::filesystem::path folder{};      // typed_path() of the folder part
        std::error_code error{};             // the folder cannot be listed
        std::vector<std::string> folders{};  // folders, links to folders and archives, sorted
        std::vector<std::string> files{};    // the other entries (Enter on a file)
    };

    // list(folder, done): lists folder on a worker; done on the loop thread. Returns the token.
    using List = std::function<core::CancelToken(const std::filesystem::path& folder,
                                                 std::function<void(Listing)> done)>;
    // go(path, file): Enter on a folder (file empty) or on a file in path.
    using Go = std::function<void(const std::filesystem::path& path, const std::string& file)>;

    PathEntry(std::function<void()> redraw, List list, Go go,
              std::function<void(std::string problem)> message, std::filesystem::path home);

    // The field on the folder shown, its whole path typed in, selected, so writing replaces it.
    // Once open, only the selection is taken again, as in a browser.
    void open(const std::filesystem::path& shown, bool show_hidden);
    void close();  // cancels the listing and frees it, the breadcrumb back

    [[nodiscard]] bool active() const noexcept { return !shown_.empty(); }

    [[nodiscard]] const std::filesystem::path& shown() const noexcept { return shown_; }

    // While active every key is the field's: true. Enter goes, Escape closes, Tab completes.
    bool key(const ui::KeyEvent& event);
    // Text pasted into the field: its first line, without the line end a terminal adds.
    void paste(std::string_view text);
    // The selected text, taken out of the field.
    [[nodiscard]] std::string cut();
    // A press in the field moves the caret, and the drag that follows is the field's; a press in
    // the row outside the field does nothing.
    void pointer(const ui::PointerEvent& event);

    // The button came up elsewhere: the field's drag ends.
    void end_field_drag() noexcept { field_.end_drag(); }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    [[nodiscard]] bool claims(float x, float y) const;  // the field, as last drawn

    [[nodiscard]] bool invalid() const noexcept { return invalid_; }

    [[nodiscard]] const ui::TextField& field() const noexcept { return field_; }

private:
    // The folder the name being typed would sit in: the part before the last '/', the folder
    // shown when there is none. nullopt: that part names no folder that can be listed (a name
    // relative to the trash, anything after "trash:///", another host, no home).
    [[nodiscard]] std::optional<std::filesystem::path> folder_of(std::string_view part) const;

    // After every change of the text, Tab's writes included: the red state from what it names
    // and from the listing that answers, and a new listing when its folder part moved. Typing
    // inside the same folder costs nothing.
    void text_changed();
    // A change that is not Tab's: Tab's cycle ends with it, and so does a Tab or an Enter
    // waiting for a listing.
    void edited();
    // A folder's listing, from the worker: the red state and Tab's answer come from it, and a
    // Tab or an Enter pressed before it landed is applied now, unless the text moved on.
    void listed(Listing listing);
    // Red from the part before the member and from the listing, when it is there.
    void update_red(const TypedParts& parts);
    [[nodiscard]] bool nothing_starts_with(std::string_view member) const;
    void tab(bool backwards);
    void enter();
    // How many folders Tab found, in the status bar: at the cycle's start, and again at each of
    // its steps, so the count stays in sight while the user goes through the matches.
    void show_matches();
    // The name being typed, replaced by name, as one edit (Ctrl+Z takes it back), the caret at
    // its end.
    void write_member(std::string_view name);

    ui::TextField field_;
    std::function<void()> redraw_;
    List list_;
    Go go_;
    std::function<void(std::string)> message_;
    std::filesystem::path home_;
    std::filesystem::path shown_;
    std::optional<Listing> listing_;
    std::filesystem::path listing_for_;  // the folder asked for (the listing may be on its way)
    core::CancelToken job_;
    bool show_hidden_{false};
    bool invalid_{false};
    bool tab_waiting_{false};         // Tab pressed before the listing came
    bool enter_waiting_{false};       // Enter too
    std::string waiting_text_;        // the text when Tab or Enter waits
    std::string tab_text_;            // the text Tab last wrote: the next Tab cycles from it
    std::vector<std::string> cycle_;  // the matches Tab goes through
    std::string cycle_common_;  // the start the cycle's matches share, for the count's message
    std::size_t cycle_at_{0};
};

}  // namespace nordend::views
