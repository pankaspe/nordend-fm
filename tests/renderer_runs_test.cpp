#include <GLES3/gl3.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/ui/color.hpp"
#include "nordend/ui/offscreen.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/text.hpp"

namespace ui = nordend::ui;

namespace {

constexpr std::int32_t kWidth = 240;
constexpr std::int32_t kHeight = 40;

std::unique_ptr<ui::Offscreen> offscreen(std::int32_t width = kWidth,
                                         std::int32_t height = kHeight) {
    try {
        return std::make_unique<ui::Offscreen>(width, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

}  // namespace

TEST_CASE("renderer: a line in several colours, by the bytes each glyph comes from") {
    const std::unique_ptr<ui::Offscreen> gl = offscreen();
    if (!gl) {
        return;
    }
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    renderer.begin_frame(kWidth, kHeight, 1.0F, ui::Color::rgb(0x000000));
    // "MMMM" white, "éééé" (two bytes each: bytes 4 to 12) red.
    const std::array runs{ui::ColorRun{.start = 4, .end = 12, .color = ui::Color::rgb(0xFF0000)}};
    const float width =
        renderer.draw_text_runs("MMMMéééé", 4.0F, 4.0F, 24.0F, ui::Color::rgb(0xFFFFFF), runs, 0.0F,
                                ui::Ellipsis::end, ui::Typeface::monospace);
    renderer.end_frame();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // Monospace: the colour changes halfway along the text.
    const float half = 4.0F + (width / 2.0F);
    int white_left = 0;
    int white_right = 0;
    int red_left = 0;
    int red_right = 0;
    for (std::int32_t y = 0; y < kHeight; ++y) {
        for (std::int32_t x = 0; x < kWidth; ++x) {
            const std::size_t at =
                ((static_cast<std::size_t>(y) * kWidth) + static_cast<std::size_t>(x)) * 4;
            const bool white = pixels[at] > 128 && pixels[at + 1] > 128;
            const bool red = pixels[at] > 128 && pixels[at + 1] < 64;
            const bool left = static_cast<float>(x) < half - 2.0F;
            const bool right = static_cast<float>(x) > half + 2.0F;
            white_left += white && left ? 1 : 0;
            white_right += white && right ? 1 : 0;
            red_left += red && left ? 1 : 0;
            red_right += red && right ? 1 : 0;
        }
    }
    CHECK(white_left > 50);
    CHECK(red_right > 50);
    CHECK(white_right == 0);
    CHECK(red_left == 0);
}

TEST_CASE("renderer: background alpha does not accumulate or fade foreground content") {
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        CAPTURE(scale);
        const auto physical = [scale](float value) {
            return static_cast<int>(std::lround(value * scale));
        };
        const auto gl = offscreen(physical(kWidth), physical(kHeight));
        if (!gl) {
            return;
        }
        ui::FontSystem fonts;
        ui::Renderer renderer{fonts};
        const std::array<std::uint8_t, 4> texel{255, 0, 0, 255};
        ui::Texture texture{1, 1, texel};
        for (const float opacity : {0.0F, 0.7F, 1.0F}) {
            CAPTURE(opacity);
            renderer.begin_frame(physical(kWidth), physical(kHeight), scale,
                                 ui::Color::rgb(0xFFFFFF), 0.0F, opacity);
            renderer.fill_background({0, 0, 80, 40}, ui::Color::rgb(0x00FF00));
            renderer.push_clip({0, 0, 40, 40});
            renderer.fill_background({0, 0, 80, 40}, ui::Color::rgb(0x0000FF));
            renderer.pop_clip();
            renderer.draw_image(texture, {80, 0, 40, 40});
            renderer.fill_rect({120, 0, 40, 40}, ui::Color::rgb(0xFF0000));
            renderer.draw_text("M", 165, 0, 32, ui::Color::rgb(0xFFFFFF));
            renderer.end_frame();
            const auto pixel = [&](float x) {
                std::array<std::uint8_t, 4> rgba{};
                glReadPixels(physical(x), physical(20), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                             rgba.data());
                return rgba;
            };
            const auto blue = pixel(20);
            const auto green = pixel(60);
            const auto clear = pixel(230);
            for (const auto rgba : {blue, green, clear}) {
                CHECK(static_cast<float>(rgba[3]) ==
                      doctest::Approx(static_cast<double>(opacity) * 255).epsilon(0.005));
            }
            CHECK(blue[0] == 0);
            CHECK(blue[1] == 0);
            CHECK(blue[2] == blue[3]);
            CHECK(green[0] == 0);
            CHECK(green[1] == green[3]);
            CHECK(green[2] == 0);
            CHECK(clear[0] == clear[3]);
            CHECK(clear[1] == clear[3]);
            CHECK(clear[2] == clear[3]);
            CHECK(pixel(100) == texel);
            CHECK(pixel(140) == texel);
            std::vector<std::uint8_t> glyph(static_cast<std::size_t>(physical(40) * physical(40)) *
                                            4);
            glReadPixels(physical(160), 0, physical(40), physical(40), GL_RGBA, GL_UNSIGNED_BYTE,
                         glyph.data());
            int solid_glyph_pixels = 0;
            for (std::size_t i = 0; i < glyph.size(); i += 4) {
                if (glyph[i] == 255 && glyph[i + 3] == 255) {
                    ++solid_glyph_pixels;
                }
            }
            CHECK(solid_glyph_pixels > 20);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
}

TEST_CASE("renderer: adjacent translucent panels meet at fractional pixel boundaries") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    for (const float scale : {1.25F, 1.5F}) {
        CAPTURE(scale);
        renderer.begin_frame(kWidth, kHeight, scale, ui::Color::rgb(0xFF0000), 0.0F, 0.7F);
        renderer.fill_background({0, 0, 39, 30}, ui::Color::rgb(0x00FF00));
        renderer.fill_background({39, 0, 40, 30}, ui::Color::rgb(0x0000FF));
        renderer.end_frame();
        const int boundary = static_cast<int>(std::lround(39 * scale));
        for (int x = boundary - 2; x <= boundary + 2; ++x) {
            std::array<std::uint8_t, 4> rgba{};
            glReadPixels(x, kHeight - 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            CHECK(rgba[0] == 0);  // no uncovered red seam
            CHECK(rgba[3] == doctest::Approx(0.7 * 255).epsilon(0.005));
            // Green left of the boundary, blue from it on.
            const bool left = x < boundary;
            CHECK((left ? rgba[1] : rgba[2]) == rgba[3]);
            CHECK((left ? rgba[2] : rgba[1]) == 0);
        }
    }
}
