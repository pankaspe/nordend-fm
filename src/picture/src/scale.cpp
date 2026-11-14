#include "nordend/picture/scale.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#if defined(__SSE2__)
#  include <emmintrin.h>
#endif

namespace nordend::picture {
namespace {

// The input pixels each output pixel covers, with how much of each, in one table: output o reads
// count(o) input pixels from first[o], their weights (summing to 1) from start[o] on. Contiguous,
// so the loops below walk it with pointers (no vector of vectors, no index checks per weight).
struct Taps {
    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> start;  // out + 1 entries: start[o + 1] - start[o] = count(o)
    std::vector<float> weights;
};

Taps taps(std::size_t in, std::size_t out) {
    Taps result;
    result.first.resize(out);
    result.start.resize(out + 1);
    result.weights.reserve((in / out + 2) * out);
    const double ratio = static_cast<double>(in) / static_cast<double>(out);
    for (std::size_t o = 0; o < out; ++o) {
        const double from = static_cast<double>(o) * ratio;
        const double to = static_cast<double>(o + 1) * ratio;
        const auto first = static_cast<std::size_t>(from);
        result.first[o] = static_cast<std::uint32_t>(first);
        result.start[o] = static_cast<std::uint32_t>(result.weights.size());
        const auto last = std::min(in, static_cast<std::size_t>(std::ceil(to)));
        for (std::size_t i = first; i < last; ++i) {
            const double covered =
                std::min(to, static_cast<double>(i + 1)) - std::max(from, static_cast<double>(i));
            result.weights.push_back(static_cast<float>(covered / ratio));
        }
    }
    result.start[out] = static_cast<std::uint32_t>(result.weights.size());
    return result;
}

// Half the size, each pixel the alpha-weighted average of a 2x2 block (an odd last row or
// column is dropped: less than a pixel of the result).
Image halve(const Image& image) {
    Image out;
    out.width = image.width / 2;
    out.height = image.height / 2;
    out.rgba.resize(std::size_t{out.width} * out.height * 4);
    const std::size_t in_stride = std::size_t{image.width} * 4;
    const std::uint8_t* const pixels = image.rgba.data();
    std::uint8_t* const result = out.rgba.data();
    for (std::size_t y = 0; y < out.height; ++y) {
        const std::uint8_t* top = pixels + ((y * 2) * in_stride);
        const std::uint8_t* bottom = top + in_stride;
        std::uint8_t* to = result + (y * std::size_t{out.width} * 4);
        for (std::size_t x = 0; x < out.width; ++x, top += 8, bottom += 8, to += 4) {
            const std::uint32_t a0 = top[3];
            const std::uint32_t a1 = top[7];
            const std::uint32_t a2 = bottom[3];
            const std::uint32_t a3 = bottom[7];
            const std::uint32_t alpha = a0 + a1 + a2 + a3;
            if (alpha == 4 * 255) {
                // Opaque (every photo): (255 s + 510) / 1020 is exactly (s + 2) / 4.
                for (std::size_t c = 0; c < 3; ++c) {
                    const std::uint32_t sum =
                        std::uint32_t{top[c]} + top[4 + c] + bottom[c] + bottom[4 + c];
                    to[c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
                to[3] = 255;
                continue;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                const std::uint32_t sum =
                    (top[c] * a0) + (top[4 + c] * a1) + (bottom[c] * a2) + (bottom[4 + c] * a3);
                to[c] = static_cast<std::uint8_t>(alpha > 0 ? (sum + (alpha / 2)) / alpha : 0);
            }
            to[3] = static_cast<std::uint8_t>((alpha + 2) / 4);
        }
    }
    return out;
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::clamp(value + 0.5F, 0.0F, 255.0F));
}

Image area_scale(const Image& image, std::size_t out_width, std::size_t out_height) {
    const std::size_t in_width = image.width;
    const Taps columns = taps(in_width, out_width);
    const Taps rows = taps(image.height, out_height);
    const std::uint8_t* const pixels = image.rgba.data();
    // Photos are opaque: then there is no alpha to weight by or to divide by.
    bool opaque = true;
    for (std::size_t i = 3; i < image.rgba.size() && opaque; i += 4) {
        opaque = pixels[i] == 255;
    }

    // One input row averaged across to out_width pixels (premultiplied floats, 0-255). An opaque
    // row is read as it is; one with alpha is premultiplied into premultiplied first.
    std::vector<float> premultiplied(opaque ? 0 : in_width * 4);
    std::vector<float> across(out_width * 4);
    std::size_t across_row = SIZE_MAX;  // the input row in across
    const auto across_pixels = [&]<class Pixel>(const Pixel* const row) {
        const std::uint32_t* const first = columns.first.data();
        const std::uint32_t* const start = columns.start.data();
        const float* const weights = columns.weights.data();
        float* to = across.data();
#if defined(__SSE2__)
        // NOLINTBEGIN(portability-simd-intrinsics): with a portable version below
        // The four channels of a pixel at once: the same multiplications and additions, in the
        // same order, as the loop below (no fused multiply-add), so the same results.
        for (std::size_t x = 0; x < out_width; ++x, to += 4) {
            const Pixel* pixel = row + (std::size_t{first[x]} * 4);
            __m128 sum = _mm_setzero_ps();
            for (const float* weight = weights + start[x]; weight != weights + start[x + 1];
                 ++weight, pixel += 4) {
                __m128 channels{};
                if constexpr (std::is_same_v<Pixel, float>) {
                    channels = _mm_loadu_ps(pixel);
                } else {
                    std::int32_t bytes = 0;
                    std::memcpy(&bytes, pixel, sizeof(bytes));
                    const __m128i zero = _mm_setzero_si128();
                    channels = _mm_cvtepi32_ps(_mm_unpacklo_epi16(
                        _mm_unpacklo_epi8(_mm_cvtsi32_si128(bytes), zero), zero));
                }
                sum = _mm_add_ps(sum, _mm_mul_ps(_mm_set1_ps(*weight), channels));
            }
            _mm_storeu_ps(to, sum);
        }
        // NOLINTEND(portability-simd-intrinsics)
#else
        for (std::size_t x = 0; x < out_width; ++x, to += 4) {
            const Pixel* pixel = row + (std::size_t{first[x]} * 4);
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            float a = 0.0F;
            for (const float* weight = weights + start[x]; weight != weights + start[x + 1];
                 ++weight, pixel += 4) {
                r += *weight * static_cast<float>(pixel[0]);
                g += *weight * static_cast<float>(pixel[1]);
                b += *weight * static_cast<float>(pixel[2]);
                a += *weight * static_cast<float>(pixel[3]);
            }
            to[0] = r;
            to[1] = g;
            to[2] = b;
            to[3] = a;
        }
#endif
    };
    const auto scale_row = [&](std::size_t y) {
        const std::uint8_t* const row = pixels + (y * in_width * 4);
        if (opaque) {
            across_pixels(row);
        } else {
            float* to = premultiplied.data();
            for (std::size_t x = 0; x < in_width; ++x, to += 4) {
                const std::uint8_t* const pixel = row + (x * 4);
                const auto alpha = static_cast<float>(pixel[3]);
                for (std::size_t c = 0; c < 3; ++c) {
                    to[c] = static_cast<float>(pixel[c]) * alpha / 255.0F;
                }
                to[3] = alpha;
            }
            const float* const premultiplied_row = premultiplied.data();
            across_pixels(premultiplied_row);
        }
        across_row = y;
    };

    // Down, one output row at a time: the input rows it covers, each scaled across once (a row
    // shared by two output rows is the last of one and the first of the next).
    Image out;
    out.width = static_cast<std::uint32_t>(out_width);
    out.height = static_cast<std::uint32_t>(out_height);
    out.rgba.resize(out_width * out_height * 4);
    const std::size_t row_floats = out_width * 4;
    std::vector<float> sum(row_floats);
    float* const sums = sum.data();
    const float* const scaled = across.data();
    for (std::size_t y = 0; y < out_height; ++y) {
        std::ranges::fill(sum, 0.0F);
        const std::size_t first = rows.first[y];
        const float* const weights = rows.weights.data();
        for (std::uint32_t k = rows.start[y]; k < rows.start[y + 1]; ++k) {
            const std::size_t input_row = first + (k - rows.start[y]);
            if (across_row != input_row) {
                scale_row(input_row);
            }
            const float weight = weights[k];
            for (std::size_t i = 0; i < row_floats; ++i) {
                sums[i] += weight * scaled[i];
            }
        }
        std::uint8_t* const to = out.rgba.data() + (y * row_floats);
        if (opaque) {
            std::size_t i = 0;
#if defined(__SSE2__)
            // NOLINTBEGIN(portability-simd-intrinsics): the loop below does the rest, or all
            // Sixteen at a time, as to_byte does each: + 0.5, clamped, truncated.
            const __m128 half = _mm_set1_ps(0.5F);
            const __m128 low = _mm_setzero_ps();
            const __m128 high = _mm_set1_ps(255.0F);
            const auto bytes = [&](std::size_t at) {
                const __m128 value = _mm_add_ps(_mm_loadu_ps(sums + at), half);
                return _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(value, low), high));
            };
            for (; i + 16 <= row_floats; i += 16) {
                const __m128i low_half = _mm_packs_epi32(bytes(i), bytes(i + 4));
                const __m128i high_half = _mm_packs_epi32(bytes(i + 8), bytes(i + 12));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(to + i),  // NOLINT: an unaligned store
                                 _mm_packus_epi16(low_half, high_half));
            }
            // NOLINTEND(portability-simd-intrinsics)
#endif
            for (; i < row_floats; ++i) {
                to[i] = to_byte(sums[i]);
            }
            continue;
        }
        for (std::size_t x = 0; x < out_width; ++x) {
            const float* const pixel = sums + (x * 4);
            const float alpha = pixel[3];
            const float unpremultiply = alpha > 0.0F ? 255.0F / alpha : 0.0F;
            for (std::size_t c = 0; c < 3; ++c) {
                to[(x * 4) + c] = to_byte(pixel[c] * unpremultiply);
            }
            to[(x * 4) + 3] = to_byte(alpha);
        }
    }
    return out;
}

}  // namespace

Image scale_to_fit(const Image& image, std::uint32_t max_width, std::uint32_t max_height) {
    if (image.empty() || max_width == 0 || max_height == 0 ||
        (image.width <= max_width && image.height <= max_height)) {
        return image;
    }
    const double factor = std::min(static_cast<double>(max_width) / image.width,
                                   static_cast<double>(max_height) / image.height);
    const auto out_width =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(image.width * factor)));
    const auto out_height =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(image.height * factor)));
    // Halving while the half is still at least as big as the result costs a fraction of the area
    // filter, which then does what is left.
    if (image.width >= out_width * 2 && image.height >= out_height * 2) {
        Image half = halve(image);
        while (half.width >= out_width * 2 && half.height >= out_height * 2) {
            half = halve(half);
        }
        return area_scale(half, out_width, out_height);
    }
    return area_scale(image, out_width, out_height);
}

}  // namespace nordend::picture
