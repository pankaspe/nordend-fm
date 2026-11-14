#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "nordend/picture/image.hpp"
#include "nordend/picture/photo_info.hpp"

// How Nordend asks for a picture: what it asks (LoadRequest), what comes back (Loaded), why it
// may fail (Failure), and where the asking goes (the loader: in Nordend, the isolated helpers).
// Making the picture is the formats' job (formats/), never done here.
namespace nordend::picture {

// Why a file gave no picture.
enum class Failure : std::uint8_t {
    none,
    unknown_format,       // not an image format Nordend reads
    no_readable_preview,  // a camera raw file without a preview Nordend can read
    too_large,            // more pixels than DecodeOptions::max_pixels
    damaged,              // the format is known but the data is broken or cut short
    unreadable,           // the file could not be read (permissions, I/O)
    unavailable,          // the isolated helper stopped, took too long, or could not start
};

// The sentence shown for a failure; empty for none.
[[nodiscard]] std::string_view failure_message(Failure failure) noexcept;

struct LoadRequest {
    std::filesystem::path path;
    // The picture fits in max_width x max_height pixels (scaled down, never up).
    std::uint32_t max_width{0};
    std::uint32_t max_height{0};
};

struct Loaded {
    Image image;  // upright, fits in the asked size; empty on failure
    // The size of the whole image, upright, when known (0 when only a preview was read and the
    // file does not say).
    std::uint32_t full_width{0};
    std::uint32_t full_height{0};
    bool from_preview{false};  // made from a preview stored in the file, not the whole image
    Failure failure{Failure::none};
    std::string detail;  // the technical reason of a failure, for the log

    // Where the time went, for the trace log: set where the picture is made, never sent between
    // processes.
    struct Stages {
        float preview_ms{0};  // finding and reading a stored preview
        float decode_ms{0};   // decoding (or developing) the picture used
        float scale_ms{0};
        float turn_ms{0};
        std::uint32_t decoded_width{0};  // the picture as decoded, before scaling
        std::uint32_t decoded_height{0};
    } stages;

    [[nodiscard]] std::string_view error() const noexcept { return failure_message(failure); }
};

// Where Nordend's pictures are made. Nordend sets this once at startup to its isolated helpers
// (ROADMAP 2b); tests and development tools set the formats' own load, in their process.
using Loader = std::function<Loaded(const LoadRequest&)>;
void set_loader(Loader loader);

// A picture for thumbnails, Quick Look and the info pane, through the loader set; "unavailable"
// when none is. Thread-safe.
[[nodiscard]] Loaded load_picture(const LoadRequest& request);

// Where a photo's information is read (the info pane): set like the loader. Nordend: its
// isolated helpers; tests and tools: the image family in their process.
using InfoLoader = std::function<PhotoInfo(const std::filesystem::path&)>;
void set_info_loader(InfoLoader loader);

// What path says about itself (EXIF), through the information loader;
// empty when none is set or the file says nothing. Thread-safe.
[[nodiscard]] PhotoInfo load_photo_info(const std::filesystem::path& path);

}  // namespace nordend::picture
