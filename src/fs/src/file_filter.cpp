#include "nordend/fs/file_filter.hpp"

#include <algorithm>
#include <fnmatch.h>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace nordend::fs {
namespace {

// The type a chooser knows a file by: from its name, without reading it.
MimeId type_by_name(const Entry& entry) {
    if (entry.mime != kUnknownMime) {
        return entry.mime;
    }
    if (entry.name_guess != kUnknownMime) {
        return entry.name_guess;
    }
    return MimeDatabase::kOctetStream;
}

// mime or one of its ancestors (subclasses file) named media...: "image/x-olympus-orf" is an
// image even if it only says so through a parent. At most 64 types looked at: a loop in a
// broken subclasses file ends there.
bool in_media(const MimeDatabase& database, MimeId mime, std::string_view media) {
    std::vector<MimeId> waiting{mime};
    for (std::size_t looked = 0; !waiting.empty() && looked < 64; ++looked) {
        const MimeId next = waiting.back();
        waiting.pop_back();
        if (database.name(next).starts_with(media)) {
            return true;
        }
        const std::span<const MimeId> parents = database.parents(next);
        waiting.insert(waiting.end(), parents.begin(), parents.end());
    }
    return false;
}

}  // namespace

FileFilterMatch::FileFilterMatch(const FileFilter& filter, std::shared_ptr<const MimeDatabase> mime)
    : globs_{filter.globs}, mime_{std::move(mime)} {
    for (const std::string& type : filter.types) {
        if (type == "*" || type == "*/*") {
            globs_.emplace_back("*");
        } else if (type.ends_with("/*")) {
            media_.push_back(type.substr(0, type.size() - 1));
        } else if (mime_) {
            if (const MimeId id = mime_->find(type); id != kUnknownMime) {
                types_.push_back(id);
            }
        }
    }
}

bool FileFilterMatch::passes(const Entry& entry) const {
    if (std::ranges::any_of(globs_, [&entry](const std::string& glob) {
            return fnmatch(glob.c_str(), entry.name.c_str(), FNM_CASEFOLD) == 0;
        })) {
        return true;
    }
    if (!mime_ || (types_.empty() && media_.empty())) {
        return false;
    }
    const MimeId mime = type_by_name(entry);
    return std::ranges::any_of(types_, [&](MimeId type) { return mime_->is_a(mime, type); }) ||
           std::ranges::any_of(
               media_, [&](const std::string& media) { return in_media(*mime_, mime, media); });
}

}  // namespace nordend::fs
