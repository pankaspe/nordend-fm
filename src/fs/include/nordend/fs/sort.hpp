#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "nordend/fs/entry.hpp"

namespace nordend::fs {

// Natural name order, as people read file names: runs of digits compare by value
// ("IMG_2" < "IMG_10"), letters ignore ASCII case. Bytes outside ASCII compare as bytes, so
// accented letters sort after z (no locale collation yet). Ties fall back to plain byte order, so
// the order is total and stable across runs.
[[nodiscard]] int natural_compare(std::string_view a, std::string_view b) noexcept;

enum class SortKey : std::uint8_t { name, size, modified, type };

struct SortOrder {
    SortKey key{SortKey::name};
    bool descending{false};
    bool directories_first{true};  // directories (and links to them) before everything else
    // For SortKey::type: the type names, alphabetically ("image/jpeg" < "text/plain"); entries
    // whose name does not tell their type come last. Without it, types group in no set order.
    const MimeDatabase* mime{nullptr};

    // Strict weak order on entries; ties on size, date or type fall back to the name. By size,
    // directories compare by name among themselves.
    [[nodiscard]] bool operator()(const Entry& a, const Entry& b) const noexcept;

    [[nodiscard]] bool operator==(const SortOrder&) const = default;
};

// A directory's entries kept sorted while they arrive in chunks. Hidden entries (dotfiles) can be
// left out: they are kept aside, unsorted, and come back when they are shown again.
class Listing {
public:
    explicit Listing(SortOrder order = {}) : order_{order} {}

    // Sorts the chunk and merges it in: O(chunk log chunk + size).
    void add(std::vector<Entry> chunk);

    // Removes the entries named in remove and those named like an entry of insert, then merges
    // insert in: one pass for a whole batch of changes.
    void replace(const std::unordered_set<std::string>& remove, std::vector<Entry> insert);

    // Re-sorts everything with a new order.
    void set_order(SortOrder order);

    // Shows or leaves out the hidden entries (shown at first); keep stays shown either way
    // (empty: none). Returns the names that left entries() and the names that came into it.
    struct Moved {
        std::vector<std::string> hidden;
        std::vector<std::string> shown;
    };

    Moved set_filter(bool show_hidden, std::string keep);

    // Only the entries `only` passes, besides the rule for hidden ones (a file chooser's filter,
    // nordend --pick); empty: every entry. keep stays shown either way here too.
    using Only = std::function<bool(const Entry&)>;
    Moved set_only(Only only);

    [[nodiscard]] bool has_only() const noexcept { return static_cast<bool>(only_); }

    [[nodiscard]] bool shows_hidden() const noexcept { return show_hidden_; }

    void clear() {
        entries_.clear();
        hidden_.clear();
    }

    [[nodiscard]] const SortOrder& order() const noexcept { return order_; }

    // What is shown, sorted.
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    // What is left out, in no order.
    [[nodiscard]] const std::vector<Entry>& hidden() const noexcept { return hidden_; }

private:
    [[nodiscard]] bool left_out(const Entry& entry) const {
        if (!keep_.empty() && entry.name == keep_) {
            return false;
        }
        return (!show_hidden_ && entry.hidden()) || (only_ && !only_(entry));
    }

    Moved refilter();

    void merge(std::vector<Entry> chunk);

    SortOrder order_;
    std::vector<Entry> entries_;
    std::vector<Entry> hidden_;
    bool show_hidden_{true};
    std::string keep_;
    Only only_;
};

}  // namespace nordend::fs
