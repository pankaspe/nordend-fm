#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/transfer.hpp"

// Symbolic links the user asks for: "Create Link" in the menu. Blocking: TransferQueue runs them
// on its thread, after the transfers queued before them.
namespace nordend::fs {

// The two ends of a link are in one tree: on one file system (same_device), and their real paths
// share a folder below "/". A relative link then keeps working when that folder moves, or when
// the disk is mounted under another path.
[[nodiscard]] bool same_tree(const std::filesystem::path& link_folder,
                             const std::filesystem::path& item_folder, bool same_device);

// What a link in link_folder to item holds when its target is relative: item relative to
// link_folder ("../a/P1.ORF"), else item itself. Both are real paths.
[[nodiscard]] std::filesystem::path link_contents(const std::filesystem::path& item,
                                                  const std::filesystem::path& link_folder);

// The name of a link to name: "Link to name" in the item's own folder, else name. From 2 up a
// number goes before the item's extension, as free_name() puts it ("Link to P1 2.ORF"), or at
// the end for a folder or a name that starts with a dot ("Link to Trip.2023 2",
// "Link to .bashrc 2").
[[nodiscard]] std::string link_name(std::string_view name, bool folder, bool own_folder,
                                    int number = 1);

// Makes a link in request.destination to each of request.names in request.source.
//
// - The destination is opened once, following a link to it as a copy does, and every name is
//   looked up, made and renamed relative to that descriptor: the links land in the folder that
//   was opened, even if its path changes meanwhile.
// - A free name: symlinkat(), which never replaces anything, so a link is made whole or not at
//   all. Replace: a hidden temporary (".nordend-part-") renamed over the old entry. A link never
//   replaces a folder.
// - Taken names go to callbacks.resolve, except in the items' own folder, where each link is
//   "Link to X" and takes the next number free by itself. Keep both counts up until symlinkat()
//   finds a number free in the folder opened.
// - In the items' own folder the target is absolute: a link made next to its item usually gets
//   moved, and a relative one would break. Elsewhere it is relative when both ends are in one
//   tree (same_tree), else absolute.
// - The items are never opened, and their sidecars are not linked.
//
// The result: placed = the links' names, items = each item and its link, files_done = links made.
[[nodiscard]] TransferResult run_link(const TransferRequest& request,
                                      const core::CancelToken& token,
                                      const TransferCallbacks& callbacks);

}  // namespace nordend::fs
