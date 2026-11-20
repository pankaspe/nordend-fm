#include "nordend/fs/links.hpp"

#include <cerrno>
#include <fcntl.h>
#include <format>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "nordend/fs/operations.hpp"

namespace nordend::fs {
namespace {

constexpr std::string_view kTemporary = ".nordend-part-";  // as a copy's
constexpr int kTries = 10'000;                             // numbers tried, as make_folder()

std::error_code last_error() { return {errno, std::generic_category()}; }

// A file descriptor closed on every path.
class Fd {
public:
    explicit Fd(int fd) : fd_{fd} {}

    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

// The real path of the open folder, as /proc/self/fd says it: the folder the links go into, not
// whatever its path names by now. canonical() when /proc is not mounted.
std::filesystem::path real_path_of(int folder, const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path real =
        std::filesystem::read_symlink(std::format("/proc/self/fd/{}", folder), error);
    if (error || !real.is_absolute()) {
        real = std::filesystem::canonical(path, error);
    }
    return error ? std::filesystem::path{} : real;
}

// A link holding contents, made in folder under name's link names from number up: the first one
// free. Its name; empty, and error set, when none could be made.
std::string make_free(int folder, const std::filesystem::path& contents, std::string_view name,
                      bool directory, bool own_folder, int number, std::error_code& error) {
    for (; number <= kTries; ++number) {
        std::string candidate = link_name(name, directory, own_folder, number);
        if (symlinkat(contents.c_str(), folder, candidate.c_str()) == 0) {
            return candidate;
        }
        if (errno != EEXIST) {
            error = last_error();
            return {};
        }
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

// A link holding contents put in place of name in folder: made under a hidden name first, then
// renamed over the old entry, so name is never missing. Nothing is left behind on failure.
std::error_code replace_with_link(int folder, const std::filesystem::path& contents,
                                  const std::string& name) {
    std::string temporary;
    for (int number = 1;; ++number) {
        if (number > kTries) {
            return std::make_error_code(std::errc::file_exists);
        }
        temporary = std::format("{}{}-{}", kTemporary, getpid(), number);
        if (symlinkat(contents.c_str(), folder, temporary.c_str()) == 0) {
            break;
        }
        if (errno != EEXIST) {
            return last_error();
        }
    }
    if (renameat(folder, temporary.c_str(), folder, name.c_str()) != 0) {
        const std::error_code error = last_error();
        unlinkat(folder, temporary.c_str(), 0);
        return error;
    }
    return {};
}

}  // namespace

bool same_tree(const std::filesystem::path& link_folder, const std::filesystem::path& item_folder,
               bool same_device) {
    if (!same_device || !link_folder.is_absolute() || !item_folder.is_absolute()) {
        return false;
    }
    const std::filesystem::path link_below = link_folder.relative_path();
    const std::filesystem::path item_below = item_folder.relative_path();
    return !link_below.empty() && !item_below.empty() && *link_below.begin() == *item_below.begin();
}

std::filesystem::path link_contents(const std::filesystem::path& item,
                                    const std::filesystem::path& link_folder) {
    std::filesystem::path relative = item.lexically_relative(link_folder);
    return relative.empty() ? item : relative;
}

std::string link_name(std::string_view name, bool folder, bool own_folder, int number) {
    std::string base = own_folder ? std::format("Link to {}", name) : std::string{name};
    if (number <= 1) {
        return base;
    }
    const std::size_t dot = name.rfind('.');
    if (folder || dot == std::string_view::npos || dot == 0) {
        return std::format("{} {}", base, number);
    }
    const std::size_t at = base.size() - (name.size() - dot);  // the item's extension
    return std::format("{} {}{}", base.substr(0, at), number, base.substr(at));
}

TransferResult run_link(const TransferRequest& request, const core::CancelToken& token,
                        const TransferCallbacks& callbacks) {
    TransferResult result;
    // The folder chosen is followed as a copy follows it (a link to another disk); the links made
    // under it never are.
    const Fd folder{::open(request.destination.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    struct stat to{};
    if (folder.get() < 0 || fstat(folder.get(), &to) != 0) {
        result.refused =
            std::format("{}: {}", request.destination.string(), last_error().message());
        return result;
    }
    struct stat from{};
    const bool source_found = stat(request.source.c_str(), &from) == 0;
    const bool own_folder = source_found && from.st_dev == to.st_dev && from.st_ino == to.st_ino;

    // A relative target is worked out from the real paths: the kernel resolves it from where the
    // link really is (a ~/Pictures that is a link to another disk).
    std::filesystem::path real_source;
    std::filesystem::path real_destination;
    bool relative = false;
    if (source_found && !own_folder) {
        std::error_code ignored;
        real_source = std::filesystem::canonical(request.source, ignored);
        real_destination = real_path_of(folder.get(), request.destination);
        relative = !real_source.empty() && !real_destination.empty() &&
                   same_tree(real_destination, real_source, from.st_dev == to.st_dev);
    }

    TransferProgress progress;
    progress.files_total = request.names.size();
    const auto report = [&] {
        if (callbacks.progress) {
            callbacks.progress(progress);
        }
    };
    report();

    for (const std::string& name : request.names) {
        if (token.cancelled()) {
            result.cancelled = true;
            break;
        }
        progress.current = name;
        std::error_code ignored;
        const std::filesystem::path item =
            std::filesystem::absolute(request.source / name, ignored).lexically_normal();
        struct stat info{};
        if (lstat(item.c_str(), &info) != 0) {
            result.errors.push_back(std::format("{}: {}", name, last_error().message()));
            continue;
        }
        // A link to a folder is numbered as a folder is.
        struct stat followed{};
        const bool directory = S_ISDIR(info.st_mode) ||
                               (stat(item.c_str(), &followed) == 0 && S_ISDIR(followed.st_mode));
        const std::filesystem::path contents =
            relative ? link_contents(real_source / name, real_destination) : item;
        const std::string base = link_name(name, directory, own_folder);
        if (const std::optional<std::string> problem = name_problem(base)) {
            result.errors.push_back(std::format("{}: {}", name, *problem));
            continue;
        }

        struct stat there{};
        const bool taken = fstatat(folder.get(), base.c_str(), &there, AT_SYMLINK_NOFOLLOW) == 0;
        Resolution resolution = Resolution::keep_both;
        if (taken && !own_folder) {
            resolution = callbacks.resolve
                             ? callbacks.resolve({.name = base,
                                                  .destination = request.destination,
                                                  .directory = false,
                                                  .target_directory = S_ISDIR(there.st_mode),
                                                  .merge = false})
                             : Resolution::skip;
        }
        if (resolution == Resolution::cancel) {
            result.cancelled = true;
            break;
        }
        if (resolution == Resolution::skip) {
            continue;
        }

        std::string made;
        if (resolution == Resolution::replace) {
            if (S_ISDIR(there.st_mode)) {
                result.errors.push_back(std::format(
                    "{}: a folder has its name; a link does not replace a folder", base));
                continue;
            }
            if (const std::error_code error = replace_with_link(folder.get(), contents, base)) {
                result.errors.push_back(std::format("{}: {}", base, error.message()));
                continue;
            }
            made = base;
            ++result.replaced;  // the old one is gone: an undo cannot bring it back
        } else {
            // The name itself, or for Keep both the numbers from 2 up; a number taken meanwhile is
            // skipped.
            std::error_code error;
            made = make_free(folder.get(), contents, name, directory, own_folder, taken ? 2 : 1,
                             error);
            if (made.empty()) {
                result.errors.push_back(std::format("{}: {}", base, error.message()));
                continue;
            }
            result.items.push_back({.from = item, .to = request.destination / made});
        }
        result.placed.push_back(std::move(made));
        ++result.files_done;
        ++progress.files_done;
        report();
    }
    return result;
}

}  // namespace nordend::fs
