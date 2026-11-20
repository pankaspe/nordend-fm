#include "nordend/views/path_entry.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "nordend/core/file.hpp"
#include "nordend/core/memory.hpp"
#include "nordend/fs/trash.hpp"

namespace nordend::views {
namespace {

// As the breadcrumb's row: the height of a step's box and of its text.
constexpr float kBoxHeight = 26.0F;
constexpr float kTextSize = 14.0F;
// Either side of the field, inside the breadcrumb's row (its padding less a step's inset).
constexpr float kSideInset = 10.0F;

// Whether text is valid UTF-8: a name that is not cannot be offered to a field that walks
// characters (fs::Entry::name is bytes from the disk, usually UTF-8, not guaranteed).
[[nodiscard]] bool is_utf8(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto byte = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        if (byte < 0x80U) {
            length = 1;
        } else if ((byte & 0xE0U) == 0xC0U) {
            length = 2;
        } else if ((byte & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((byte & 0xF8U) == 0xF0U) {
            length = 4;
        } else {
            return false;
        }
        if (at + length > text.size()) {
            return false;
        }
        for (std::size_t i = 1; i < length; ++i) {
            if ((static_cast<unsigned char>(text[at + i]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        at += length;
    }
    return true;
}

// lexically_normal() keeps a trailing '/', which no folder's path here has but the root's.
[[nodiscard]] std::filesystem::path without_trailing_slash(std::filesystem::path path) {
    while (path != path.root_path() && !path.empty() &&
           path.native().ends_with(path.preferred_separator)) {
        path = path.parent_path();
    }
    return path;
}

}  // namespace

std::optional<std::filesystem::path> typed_path(std::string_view text,
                                                const std::filesystem::path& shown,
                                                const std::filesystem::path& home) {
    if (text.empty()) {
        return std::nullopt;
    }
    // The trash before the normalizing below, which would turn "trash:///" into "trash:/".
    if (fs::is_trash(std::filesystem::path{text})) {
        return fs::trash_path();
    }
    if (text.starts_with("trash:")) {
        return std::nullopt;  // the trash has no folders of its own in Nordend
    }
    if (text.starts_with("file:")) {
        return core::path_from_uri(text);
    }
    std::filesystem::path out;
    if (text == "~" || text.starts_with("~/")) {
        if (home.empty()) {
            return std::nullopt;
        }
        std::string_view rest = text.substr(1);
        if (!rest.empty() && rest.front() == '/') {
            rest.remove_prefix(1);
        }
        out = home;
        if (!rest.empty()) {
            out /= std::filesystem::path{rest};
        }
    } else if (text.starts_with('/')) {
        out = std::filesystem::path{text};
    } else {
        if (shown.empty() || fs::is_trash(shown)) {
            return std::nullopt;  // nothing relative to the trash names anything
        }
        out = shown / std::filesystem::path{text};
    }
    return without_trailing_slash(out.lexically_normal());
}

TypedParts split_typed(std::string_view text) {
    // A lone "~" is the home folder, as "~/" is: the folder part, nothing being typed inside.
    if (text == "~") {
        return {.folder = text, .name = {}};
    }
    const std::size_t at = text.rfind('/');
    if (at == std::string_view::npos) {
        return {.folder = {}, .name = text};
    }
    return {.folder = text.substr(0, at + 1), .name = text.substr(at + 1)};
}

Completion complete(std::string_view typed, std::span<const std::string> folders,
                    bool show_hidden) {
    Completion out;
    for (const std::string& name : folders) {
        // Hidden names come up only when the typed part starts with a dot, or when hidden
        // files are shown; a name that is not UTF-8 cannot go into the field.
        if (name.empty()) {
            continue;
        }
        const bool hidden = name.front() == '.';
        const bool wants_hidden = (!typed.empty() && typed.front() == '.') || show_hidden;
        if (hidden && !wants_hidden) {
            continue;
        }
        if (!name.starts_with(typed) || !is_utf8(name)) {
            continue;
        }
        out.matches.push_back(name);
    }
    std::ranges::sort(out.matches);
    if (!out.matches.empty()) {
        out.common = out.matches.front();
        for (std::size_t i = 1; i < out.matches.size() && !out.common.empty(); ++i) {
            const std::string& name = out.matches[i];
            std::size_t at = 0;
            while (at < out.common.size() && at < name.size() && out.common[at] == name[at]) {
                ++at;
            }
            out.common.resize(at);
        }
    }
    return out;
}

PathEntry::PathEntry(std::function<void()> redraw, List list, Go go,
                     std::function<void(std::string problem)> message, std::filesystem::path home)
    : redraw_{std::move(redraw)},
      list_{std::move(list)},
      go_{std::move(go)},
      message_{std::move(message)},
      home_{std::move(home)} {}

void PathEntry::open(const std::filesystem::path& shown, bool show_hidden) {
    if (active()) {
        field_.select_all();  // already open: the whole text is taken again
        redraw_();
        return;
    }
    shown_ = shown;
    show_hidden_ = show_hidden;
    invalid_ = false;
    cycle_.clear();
    cycle_common_.clear();
    tab_text_.clear();
    listing_.reset();
    listing_for_.clear();
    job_ = {};
    tab_waiting_ = false;
    enter_waiting_ = false;
    // The folder's path, with the '/' a completed name would follow; the root and the trash
    // have none to spare.
    std::string text = shown.string();
    if (shown != shown.root_path() && !fs::is_trash(shown)) {
        text += '/';
    }
    field_.set_text(std::move(text));
    field_.select_all();
    text_changed();
    redraw_();
}

void PathEntry::close() {
    if (!active()) {
        return;
    }
    shown_.clear();
    job_.cancel();
    job_ = {};
    if (listing_) {
        listing_.reset();
        core::give_back_free_memory();  // a big folder's names, back to the system
    }
    cycle_.clear();
    cycle_common_.clear();
    cycle_at_ = 0;
    tab_text_.clear();
    tab_waiting_ = false;
    enter_waiting_ = false;
    waiting_text_.clear();
    invalid_ = false;
    redraw_();
}

std::optional<std::filesystem::path> PathEntry::folder_of(std::string_view part) const {
    if (part.empty()) {
        if (fs::is_trash(shown_)) {
            return std::nullopt;  // a name typed in the trash goes nowhere
        }
        return shown_;
    }
    std::optional<std::filesystem::path> folder = typed_path(part, shown_, home_);
    if (folder && fs::is_trash(*folder)) {
        folder.reset();  // the trash is never listed: it is not a folder on the disk
    }
    return folder;
}

void PathEntry::text_changed() {
    const TypedParts parts = split_typed(field_.text());
    const std::optional<std::filesystem::path> folder = folder_of(parts.folder);

    // A listing of another folder does not answer this text.
    if (folder && listing_ && listing_->folder != *folder) {
        listing_.reset();
    }
    if (!folder || !listing_) {
        // Tab's cycle belongs to the folder it was started in.
        cycle_.clear();
        cycle_common_.clear();
        cycle_at_ = 0;
        tab_text_.clear();
    }
    update_red(parts);
    if (folder) {
        if (*folder != listing_for_) {
            job_.cancel();
            listing_for_ = *folder;
            job_ = list_(*folder, [this](Listing listing) { listed(std::move(listing)); });
        }
    } else {
        // Nothing to list: the trash, or a part that names no folder.
        job_.cancel();
        job_ = {};
        listing_for_.clear();
    }
}

void PathEntry::edited() {
    cycle_.clear();
    cycle_common_.clear();
    cycle_at_ = 0;
    tab_text_.clear();
    tab_waiting_ = false;
    enter_waiting_ = false;
    text_changed();
}

void PathEntry::update_red(const TypedParts& parts) {
    const std::string& text = field_.text();
    const std::optional<std::filesystem::path> folder = folder_of(parts.folder);
    if (text.empty()) {
        invalid_ = false;  // nothing typed: no colour, and Enter does nothing
    } else if (!folder) {
        // The part before the member names the trash or nothing at all: the trash's own path
        // goes somewhere ("trash:///"), anything after it or instead of it does not.
        const std::optional<std::filesystem::path> typed = typed_path(parts.folder, shown_, home_);
        invalid_ = !(typed && fs::is_trash(*typed) && parts.name.empty());
    } else if (listing_) {
        invalid_ = nothing_starts_with(parts.name);
    }
    // A real folder, its listing still on its way: the colour stays as it was, so fast typing
    // does not flicker.
}

bool PathEntry::nothing_starts_with(std::string_view member) const {
    const Listing& listing = *listing_;
    if (listing.error) {
        return true;
    }
    if (member.empty()) {
        return false;  // the folder's own name needs nothing to start with
    }
    const auto starts = [member](const std::string& name) { return name.starts_with(member); };
    return std::ranges::none_of(listing.folders, starts) &&
           std::ranges::none_of(listing.files, starts);
}

void PathEntry::listed(Listing listing) {
    if (!active() || listing.folder != listing_for_) {
        return;  // the text moved to another folder, or the field closed, while it was read
    }
    listing_ = std::move(listing);
    update_red(split_typed(field_.text()));
    if (tab_waiting_ || enter_waiting_) {
        if (field_.text() != waiting_text_) {
            tab_waiting_ = false;  // the text moved on: what was pressed is forgotten
            enter_waiting_ = false;
        } else {
            if (enter_waiting_) {
                enter_waiting_ = false;
                enter();
            }
            if (tab_waiting_ && active()) {
                tab_waiting_ = false;
                tab(false);
            }
        }
    }
    redraw_();
}

bool PathEntry::key(const ui::KeyEvent& event) {
    if (!active()) {
        return false;
    }
    const bool plain = !event.modifiers.ctrl && !event.modifiers.alt;
    if (plain && (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter)) {
        enter();
    } else if (plain && event.keysym == XKB_KEY_Escape) {
        close();
    } else if (plain && event.keysym == XKB_KEY_Tab) {
        tab(false);
    } else if (plain && event.keysym == XKB_KEY_ISO_Left_Tab) {
        tab(true);
    } else if (event.modifiers == ui::Modifiers{.ctrl = true} &&
               (event.keysym == XKB_KEY_l || event.keysym == XKB_KEY_L)) {
        field_.select_all();  // once open, Ctrl+L takes the whole text again
        redraw_();
    } else {
        const std::string before = field_.text();
        if (field_.key(event)) {
            if (field_.text() != before) {
                edited();
            }
            redraw_();
        }
    }
    return true;
}

void PathEntry::paste(std::string_view text) {
    if (!active()) {
        return;
    }
    std::string line{text.substr(0, text.find_first_of("\r\n"))};
    while (!line.empty() && line.back() == '\t') {
        line.pop_back();  // a tab a terminal lined up behind the path
    }
    const std::string before = field_.text();
    field_.insert(line);
    if (field_.text() != before) {
        edited();
    }
    redraw_();
}

std::string PathEntry::cut() {
    if (!active()) {
        return {};
    }
    const std::string before = field_.text();
    std::string text = field_.cut();
    if (field_.text() != before) {
        edited();
    }
    if (!text.empty()) {
        redraw_();
    }
    return text;
}

void PathEntry::pointer(const ui::PointerEvent& event) {
    if (!active()) {
        return;
    }
    if (field_.drag(event)) {
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left ||
        !field_.bounds().contains(event.x, event.y)) {
        return;  // the row, away from the field: nothing to do, the field stays
    }
    const std::string before = field_.text();
    field_.press(event);
    if (field_.text() != before) {
        edited();
    }
    redraw_();
}

void PathEntry::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                     const theme::Palette& palette) {
    if (!active()) {
        return;
    }
    const ui::Rect box{bounds.x + kSideInset, bounds.y + ((kTitleHeight - kBoxHeight) / 2.0F),
                       std::max(0.0F, bounds.width - (2.0F * kSideInset)), kBoxHeight};
    field_.draw(renderer, box, kTextSize,
                {.background = palette.surface_container_highest,
                 .border = invalid_ ? palette.error : palette.primary,
                 .text = invalid_ ? palette.error : palette.on_surface,
                 .selection = palette.secondary_container,
                 .caret = palette.on_surface});
    // The line under the row, which the breadcrumb drew and now does not.
    renderer.fill_rect(
        {bounds.x, bounds.y + kTitleHeight - 1.0F, bounds.width, 1.0F / renderer.scale()},
        palette.outline_variant);
}

bool PathEntry::claims(float x, float y) const {
    return active() && field_.bounds().contains(x, y);
}

void PathEntry::write_member(std::string_view name) {
    const TypedParts parts = split_typed(field_.text());
    std::string written{name};
    if (!parts.folder.empty() && !parts.folder.ends_with('/')) {
        written.insert(written.begin(), '/');  // a lone "~": the member goes behind the slash
    }
    field_.select(parts.folder.size(), field_.text().size());
    field_.insert(written);  // replaces the member: one edit, so Ctrl+Z takes it back
}

void PathEntry::tab(bool backwards) {
    if (!active()) {
        return;
    }
    // A selection goes first: Ctrl+L left the whole path selected, and Tab completes what is
    // typed, all of it, not the selected part only.
    const auto [first, last] = field_.selection();
    if (first != last) {
        field_.select(field_.text().size(), field_.text().size());
    }
    // The matches of the last Tab, one by one, when nothing else was written since. Only their
    // common start may be in the text yet: the cycle begins at its first match, or its last one
    // when going back.
    if (field_.text() == tab_text_ && !cycle_.empty()) {
        if (cycle_at_ >= cycle_.size()) {
            cycle_at_ = backwards ? cycle_.size() - 1 : 0;
        } else {
            cycle_at_ = backwards ? (cycle_at_ + cycle_.size() - 1) % cycle_.size()
                                  : (cycle_at_ + 1) % cycle_.size();
        }
        write_member(cycle_[cycle_at_]);
        show_matches();  // said again, so the count does not time out while the cycle goes on
        tab_text_ = field_.text();
        text_changed();
        redraw_();
        return;
    }
    const TypedParts parts = split_typed(field_.text());
    const std::optional<std::filesystem::path> folder = folder_of(parts.folder);
    if (!folder) {
        return;  // the trash, or a part that names no folder: nothing to complete
    }
    if (!listing_) {
        tab_waiting_ = true;  // the folder is still being listed: Tab is applied when it lands
        waiting_text_ = field_.text();
        return;
    }
    const Completion completion = complete(parts.name, listing_->folders, show_hidden_);
    if (completion.matches.empty()) {
        return;  // red already says it, unless a file starts with the member
    }
    if (completion.matches.size() == 1) {
        write_member(completion.matches.front() + "/");  // the whole name, and into it
        text_changed();                                  // a new folder: its listing starts
        redraw_();
        return;
    }
    cycle_ = std::move(completion.matches);
    cycle_common_ = completion.common;
    if (completion.common.size() > parts.name.size()) {
        write_member(completion.common);  // as far as they agree
        show_matches();
        cycle_at_ = cycle_.size();  // nothing of the cycle is in the text yet
    } else {
        // Nothing new to write: the cycle starts at once, with the count said here too.
        cycle_at_ = backwards ? cycle_.size() - 1 : 0;
        write_member(cycle_[cycle_at_]);
        show_matches();
    }
    tab_text_ = field_.text();
    text_changed();
    redraw_();
}

void PathEntry::show_matches() {
    if (cycle_common_.empty()) {
        message_(std::format("Tab goes through {} folders", cycle_.size()));
        return;
    }
    message_(std::format("{} folders start with “{}”", cycle_.size(), cycle_common_));
}

void PathEntry::enter() {
    if (!active()) {
        return;
    }
    const std::string text = field_.text();
    if (text.empty()) {
        return;
    }
    const TypedParts parts = split_typed(text);
    const std::optional<std::filesystem::path> typed = typed_path(text, shown_, home_);
    // The trash is never listed, and it is always there.
    if (typed && fs::is_trash(*typed)) {
        close();
        go_(*typed, {});
        return;
    }
    if (!typed) {
        return;
    }
    if (!listing_) {
        // The listing that would say what the member names is still on its way; Enter waits
        // for it, and is forgotten if the text moves on.
        if (folder_of(parts.folder)) {
            enter_waiting_ = true;
            waiting_text_ = text;
        }
        return;
    }
    const auto among = [member = parts.name](const std::vector<std::string>& names) {
        return std::ranges::find(names, member) != names.end();
    };
    if (parts.name.empty() && !listing_->error) {
        close();
        go_(*typed, {});  // the folder part is the whole text, and it is a folder
    } else if (among(listing_->folders)) {
        close();
        go_(*typed, {});
    } else if (among(listing_->files)) {
        close();
        go_(typed->parent_path(), std::string{parts.name});  // its folder, the cursor on the file
    }
    // A member that names nothing yet: Enter does nothing (the field would be red, or the name
    // is half typed).
}

}  // namespace nordend::views
