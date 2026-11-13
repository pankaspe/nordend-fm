#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nordend/fs/entry.hpp"
#include "nordend/fs/mime.hpp"

namespace nordend::fs {

// A file chooser's filter, as an application asks for it through the portal (a label and a list
// of glob patterns and MIME types): "Images", {"*.jpg", "image/*"}.
struct FileFilter {
    std::string label;
    std::vector<std::string> globs;  // "*.jpg"
    std::vector<std::string> types;  // "image/jpeg", "image/*"

    [[nodiscard]] bool operator==(const FileFilter&) const = default;
};

// Whether a file passes a filter: its name matches one of the globs (case aside, as in GTK 4: a
// "*.jpg" filter shows PHOTO.JPG), or its type, from the name as a file chooser knows it, is
// one of the types or a subtype of one ("image/*": every image type; "text/plain": source code
// too). A name that says nothing is application/octet-stream, as for GLib. Folders are not
// asked: what to do with them is the chooser's.
class FileFilterMatch {
public:
    FileFilterMatch(const FileFilter& filter, std::shared_ptr<const MimeDatabase> mime);

    [[nodiscard]] bool passes(const Entry& entry) const;

private:
    std::vector<std::string> globs_;
    std::vector<MimeId> types_;       // exact types and their subtypes
    std::vector<std::string> media_;  // "image/" for "image/*"
    std::shared_ptr<const MimeDatabase> mime_;
};

}  // namespace nordend::fs
