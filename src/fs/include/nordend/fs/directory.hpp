#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/entry.hpp"
#include "nordend/fs/mime.hpp"

namespace nordend::fs {

struct ReadOptions {
    // Entries per chunk: the first chunk is small so a view can draw at once, then chunks double
    // up to the maximum to keep the number of hand-offs low.
    std::size_t first_chunk{128};
    std::size_t max_chunk{4096};
    // When set, Entry::mime is filled from the kind and the name (no file is opened).
    std::shared_ptr<const MimeDatabase>
        mime{};  // NOLINT(readability-redundant-member-init): GCC's -Wmissing-field-initializers
};

struct ReadResult {
    std::error_code error;  // opening or reading the directory failed (EACCES, ENOENT, ...)
    std::size_t count{0};   // entries delivered
};

// Reads a directory on the calling thread (blocking: workers only), "." and ".." excluded, in
// on-disk order. Each entry is stat'ed; one that vanishes meanwhile is skipped. Stops early,
// without delivering the partial chunk, when token is cancelled.
ReadResult read_directory(const std::filesystem::path& path, const core::CancelToken& token,
                          const ReadOptions& options,
                          const std::function<void(std::vector<Entry> chunk)>& on_chunk);

// Reads the named entries of a directory (blocking: workers only), same fields as
// read_directory. An entry that does not exist (any more) is nullopt; so is every entry when the
// directory itself cannot be opened.
std::vector<std::optional<Entry>> read_entries(const std::filesystem::path& path,
                                               const std::vector<std::string>& names,
                                               const ReadOptions& options);

// The folders in a folder, as the sidebar's tree shows them.
struct Subfolders {
    std::error_code error;           // the folder could not be opened or read
    std::vector<std::string> names;  // natural order (natural_compare), hidden ones too
    std::size_t more{0};             // folders left out past the limit
    std::uint64_t device{0};         // the folder itself, as opened (links followed)
    std::uint64_t inode{0};
};

// Reads the folders in a folder (blocking: workers only), links to folders included. readdir's
// type decides when the file system gives one; only links and entries of unknown type are
// stat'ed. Keeps the first limit names in natural order and counts the rest. Stops early, with
// nothing, when token is cancelled.
Subfolders read_subfolders(const std::filesystem::path& path, const core::CancelToken& token,
                           std::size_t limit);

// read_directory on a worker: on_chunk and on_done run on the loop thread, in order. Cancel the
// returned token to stop; nothing more is delivered after that.
core::CancelToken load_directory(core::WorkerPool& pool, std::filesystem::path path,
                                 std::function<void(std::vector<Entry> chunk)> on_chunk,
                                 std::function<void(const ReadResult& result)> on_done,
                                 ReadOptions options = {});

}  // namespace nordend::fs
