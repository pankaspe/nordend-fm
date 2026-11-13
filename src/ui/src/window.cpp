#include "nordend/ui/window.hpp"

#include <GLES3/gl3.h>
#include <algorithm>
#include <format>
#include <span>
#include <stdexcept>
#include <utility>

#include "nordend/core/log.hpp"

namespace nordend::ui {
namespace {

// fractional-scale-v1 sends scales as numerator over 120.
constexpr std::int32_t kScaleDenominator = 120;

// Physical size of a logical length, rounded half away from zero as the protocol asks.
std::int32_t to_physical(std::int32_t logical, std::int32_t scale_120) {
    return ((logical * scale_120) + (kScaleDenominator / 2)) / kScaleDenominator;
}

// The uint32_t values of a wl_array (configure's states, wm_capabilities).
std::span<const std::uint32_t> values_of(const wl_array* array) {
    if (array == nullptr || array->data == nullptr) {
        return {};
    }
    return {static_cast<const std::uint32_t*>(array->data), array->size / sizeof(std::uint32_t)};
}

std::uint32_t to_xdg_edge(Edge edge) {
    switch (edge) {
        case Edge::top:
            return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
        case Edge::bottom:
            return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        case Edge::left:
            return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        case Edge::right:
            return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        case Edge::top_left:
            return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
        case Edge::top_right:
            return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
        case Edge::bottom_left:
            return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
        case Edge::bottom_right:
            return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    }
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

}  // namespace

std::int32_t WindowFrame::margin(const WindowState& state, bool client_decorations,
                                 bool wanted) noexcept {
    const bool tiled =
        state.tiled_left || state.tiled_right || state.tiled_top || state.tiled_bottom;
    if (!wanted || !client_decorations || state.maximized || state.fullscreen || tiled) {
        return 0;
    }
    return kMargin;
}

void WindowFrame::draw(Renderer& renderer, bool activated) {
    // Close to GTK's: a near shadow and a wide soft one, lighter for a window in the back.
    static constexpr Color kBlack = Color::rgb(0x000000);
    static constexpr std::array<WindowShadow, 2> kActive{{
        {.blur = 2.0F, .offset_y = 1.0F, .color = kBlack.with_alpha(0.22F)},
        {.blur = 8.0F, .offset_y = 3.0F, .color = kBlack.with_alpha(0.30F)},
    }};
    static constexpr std::array<WindowShadow, 2> kInactive{{
        {.blur = 1.5F, .offset_y = 1.0F, .color = kBlack.with_alpha(0.16F)},
        {.blur = 5.0F, .offset_y = 2.0F, .color = kBlack.with_alpha(0.18F)},
    }};
    renderer.frame_window(kRadius, activated ? kActive : kInactive, kBlack.with_alpha(0.18F));
}

Window::Window(Display& display, EglContext& egl, FontSystem& fonts, const WindowOptions& options)
    : display_{display},
      egl_{egl},
      width_{options.width},
      height_{options.height},
      frame_wanted_{options.frame},
      title_{options.title},
      background_{options.background},
      background_opacity_{options.background_opacity} {
    surface_.reset(wl_compositor_create_surface(display_.compositor()));

    static constexpr xdg_surface_listener kSurfaceListener{
        .configure = &Window::on_surface_configure,
    };
    xdg_surface_.reset(xdg_wm_base_get_xdg_surface(display_.wm_base(), surface_.get()));
    xdg_surface_add_listener(xdg_surface_.get(), &kSurfaceListener, this);

    static constexpr xdg_toplevel_listener kToplevelListener{
        .configure = &Window::on_toplevel_configure,
        .close = &Window::on_toplevel_close,
        .configure_bounds = &Window::on_toplevel_configure_bounds,
        .wm_capabilities = &Window::on_toplevel_wm_capabilities,
    };
    toplevel_.reset(xdg_surface_get_toplevel(xdg_surface_.get()));
    xdg_toplevel_add_listener(toplevel_.get(), &kToplevelListener, this);
    xdg_toplevel_set_title(toplevel_.get(), options.title.c_str());
    xdg_toplevel_set_app_id(toplevel_.get(), options.app_id.c_str());
    xdg_toplevel_set_min_size(toplevel_.get(), options.min_width, options.min_height);
    if (options.maximized) {
        xdg_toplevel_set_maximized(toplevel_.get());  // before the first commit: opens maximized
    }

    // Server-side asked for, or client-side when forced; the compositor's configure has the
    // last word, so the window never gets two title bars.
    auto* manager = display_.decoration_manager();
    client_decorations_ = manager == nullptr || options.force_client_decorations;
    if (manager == nullptr) {
        log::info("decorations: client-side, the compositor draws none");
    } else {
        static constexpr zxdg_toplevel_decoration_v1_listener kDecorationListener{
            .configure = &Window::on_decoration_configure,
        };
        decoration_.reset(
            zxdg_decoration_manager_v1_get_toplevel_decoration(manager, toplevel_.get()));
        zxdg_toplevel_decoration_v1_add_listener(decoration_.get(), &kDecorationListener, this);
        zxdg_toplevel_decoration_v1_set_mode(decoration_.get(),
                                             options.force_client_decorations
                                                 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                                 : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        if (options.force_client_decorations) {
            log::info("decorations: client-side asked for (Nordend's own header)");
        }
    }

    if (display_.viewporter() != nullptr && display_.fractional_scale_manager() != nullptr) {
        static constexpr wp_fractional_scale_v1_listener kScaleListener{
            .preferred_scale = &Window::on_preferred_scale,
        };
        viewport_.reset(wp_viewporter_get_viewport(display_.viewporter(), surface_.get()));
        fractional_scale_.reset(wp_fractional_scale_manager_v1_get_fractional_scale(
            display_.fractional_scale_manager(), surface_.get()));
        wp_fractional_scale_v1_add_listener(fractional_scale_.get(), &kScaleListener, this);
    }

    buffer_width_ = to_physical(width_, scale_120_);
    buffer_height_ = to_physical(height_, scale_120_);
    egl_window_.reset(wl_egl_window_create(surface_.get(), buffer_width_, buffer_height_));
    egl_surface_ =
        eglCreatePlatformWindowSurface(egl_.display(), egl_.config(), egl_window_.get(), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        throw std::runtime_error{
            std::format("eglCreatePlatformWindowSurface failed (0x{:04x})", eglGetError())};
    }
    eglMakeCurrent(egl_.display(), egl_surface_, egl_surface_, egl_.context());
    // Never block in eglSwapBuffers: frame callbacks pace us, the event loop stays responsive.
    eglSwapInterval(egl_.display(), 0);
    // glGetString returns GLubyte*; the cast to char* is the documented way to print it.
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));  // NOLINT
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));    // NOLINT
    log::info("GL: {} | {}", renderer, version);
    renderer_ = std::make_unique<Renderer>(fonts);

    // Initial commit without a buffer: the compositor answers with the first configure.
    wl_surface_commit(surface_.get());
}

Window::~Window() {
    drop_drag_icon();
    // GL objects must be deleted with their context current.
    eglMakeCurrent(egl_.display(), egl_surface_, egl_surface_, egl_.context());
    renderer_.reset();
    eglMakeCurrent(egl_.display(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_.display(), egl_surface_);
}

void Window::set_draw_handler(DrawHandler handler) {
    draw_handler_ = std::move(handler);
    request_redraw();
}

void Window::request_redraw() {
    dirty_ = true;
    if (configured_ && !frame_callback_) {
        draw();
    }
}

void Window::draw() {
    dirty_ = false;

    // Ask for the next frame callback before the swap commits this frame.
    static constexpr wl_callback_listener kFrameListener{.done = &Window::on_frame_done};
    frame_callback_.reset(wl_surface_frame(surface_.get()));
    wl_callback_add_listener(frame_callback_.get(), &kFrameListener, this);

    // The buffer is in physical pixels; the viewport maps it back onto the logical size.
    // Both are double-buffered state applied by the commit inside eglSwapBuffers.
    drawn_margin_ = margin_;
    if (viewport_) {
        wp_viewport_set_destination(viewport_.get(), width_ + (2 * margin_),
                                    height_ + (2 * margin_));
    }
    update_surface_regions();

    eglMakeCurrent(egl_.display(), egl_surface_, egl_surface_, egl_.context());
    renderer_->begin_frame(buffer_width_, buffer_height_, static_cast<float>(scale()), background_,
                           static_cast<float>(margin_), background_opacity_);
    if (draw_handler_) {
        draw_handler_(*renderer_,
                      Rect{0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_)});
    }
    if (margin_ > 0) {
        WindowFrame::draw(*renderer_, state_.activated);
    }
    renderer_->end_frame();
    if (eglSwapBuffers(egl_.display(), egl_surface_) != EGL_TRUE) {
        log::error("eglSwapBuffers failed (0x{:04x})", eglGetError());
    }
    log::trace("frame {}x{} logical, {}x{} buffer", width_, height_, buffer_width_, buffer_height_);
}

wl_surface* Window::make_drag_icon(std::int32_t width, std::int32_t height,
                                   const DrawHandler& draw) {
    drop_drag_icon();
    DragIcon icon;
    icon.surface.reset(wl_compositor_create_surface(display_.compositor()));
    // Sharp at a fractional scale through a viewport; else at scale 1.
    const std::int32_t scale_120 =
        display_.viewporter() != nullptr ? scale_120_ : kScaleDenominator;
    const std::int32_t buffer_width = std::max(1, to_physical(width, scale_120));
    const std::int32_t buffer_height = std::max(1, to_physical(height, scale_120));
    if (display_.viewporter() != nullptr) {
        icon.viewport.reset(wp_viewporter_get_viewport(display_.viewporter(), icon.surface.get()));
        wp_viewport_set_destination(icon.viewport.get(), width, height);
    }
    icon.egl_window.reset(wl_egl_window_create(icon.surface.get(), buffer_width, buffer_height));
    icon.egl_surface = eglCreatePlatformWindowSurface(egl_.display(), egl_.config(),
                                                      icon.egl_window.get(), nullptr);
    if (icon.egl_surface == EGL_NO_SURFACE) {
        log::warn("drag image: eglCreatePlatformWindowSurface failed (0x{:04x})", eglGetError());
        return nullptr;
    }
    eglMakeCurrent(egl_.display(), icon.egl_surface, icon.egl_surface, egl_.context());
    eglSwapInterval(egl_.display(), 0);
    renderer_->begin_frame(buffer_width, buffer_height,
                           static_cast<float>(scale_120) / kScaleDenominator, background_);
    draw(*renderer_, Rect{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)});
    renderer_->end_frame();
    if (eglSwapBuffers(egl_.display(), icon.egl_surface) != EGL_TRUE) {
        log::warn("drag image: eglSwapBuffers failed (0x{:04x})", eglGetError());
    }
    eglMakeCurrent(egl_.display(), egl_surface_, egl_surface_, egl_.context());
    drag_icon_ = std::move(icon);
    return drag_icon_.surface.get();
}

void Window::drop_drag_icon() {
    if (drag_icon_.egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_.display(), drag_icon_.egl_surface);
    }
    drag_icon_ = DragIcon{};
}

void Window::update_buffer_size() {
    margin_ = WindowFrame::margin(state_, client_decorations_, frame_wanted_);
    const std::int32_t width = to_physical(width_ + (2 * margin_), scale_120_);
    const std::int32_t height = to_physical(height_ + (2 * margin_), scale_120_);
    if (width != buffer_width_ || height != buffer_height_) {
        buffer_width_ = width;
        buffer_height_ = height;
        wl_egl_window_resize(egl_window_.get(), buffer_width_, buffer_height_, 0, 0);
    }
}

void Window::update_surface_regions() {
    const bool is_opaque = background_opacity_ >= 1.0F;
    const std::array<std::int32_t, 4> wanted{width_, height_, margin_, is_opaque ? 1 : 0};
    if (wanted == regions_for_) {
        return;
    }
    regions_for_ = wanted;
    const std::int32_t m = margin_;
    xdg_surface_set_window_geometry(xdg_surface_.get(), m, m, width_, height_);

    // The pointer reaches the window and the frame's resize band; the shadow beyond it lets
    // clicks through to what is below.
    if (m > 0) {
        const auto band = static_cast<std::int32_t>(WindowFrame::kResizeBand);
        wl_region* input = wl_compositor_create_region(display_.compositor());
        wl_region_add(input, m - band, m - band, width_ + (2 * band), height_ + (2 * band));
        wl_surface_set_input_region(surface_.get(), input);
        wl_region_destroy(input);
    } else {
        wl_surface_set_input_region(surface_.get(), nullptr);  // the whole surface
    }

    // Transparency can change on config reload without changing the window geometry.
    if (!is_opaque) {
        wl_surface_set_opaque_region(surface_.get(), nullptr);
        return;
    }

    // Opaque: the window but its rounded corners, so the compositor need not blend it.
    const auto r = m > 0 ? static_cast<std::int32_t>(WindowFrame::kRadius) : 0;
    wl_region* opaque = wl_compositor_create_region(display_.compositor());
    wl_region_add(opaque, m, m + r, width_, std::max(0, height_ - (2 * r)));
    if (r > 0) {
        wl_region_add(opaque, m + r, m, std::max(0, width_ - (2 * r)), height_);
    }
    wl_surface_set_opaque_region(surface_.get(), opaque);
    wl_region_destroy(opaque);
}

void Window::on_surface_configure(void* data, xdg_surface* surface, std::uint32_t serial) {
    auto& self = *static_cast<Window*>(data);
    xdg_surface_ack_configure(surface, serial);
    self.configured_ = true;
    self.state_ = self.pending_state_;
    if (self.pending_width_ > 0 && self.pending_height_ > 0) {
        self.width_ = self.pending_width_;
        self.height_ = self.pending_height_;
    }
    self.update_buffer_size();  // the size, or the state that sets the frame, may have changed
    // Every configure needs a commit to apply its ack; with a frame in flight the commit
    // happens on the next frame callback, which keeps interactive resizing at display rate.
    self.request_redraw();
}

void Window::on_toplevel_configure(void* data, xdg_toplevel* /*toplevel*/, std::int32_t width,
                                   std::int32_t height, wl_array* states) {
    auto& self = *static_cast<Window*>(data);
    self.pending_width_ = width;
    self.pending_height_ = height;
    WindowState& state = self.pending_state_;
    state.maximized = state.fullscreen = state.activated = false;
    state.tiled_left = state.tiled_right = state.tiled_top = state.tiled_bottom = false;
    for (const std::uint32_t value : values_of(states)) {
        switch (value) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED:
                state.maximized = true;
                break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:
                state.fullscreen = true;
                break;
            case XDG_TOPLEVEL_STATE_ACTIVATED:
                state.activated = true;
                break;
            case XDG_TOPLEVEL_STATE_TILED_LEFT:
                state.tiled_left = true;
                break;
            case XDG_TOPLEVEL_STATE_TILED_RIGHT:
                state.tiled_right = true;
                break;
            case XDG_TOPLEVEL_STATE_TILED_TOP:
                state.tiled_top = true;
                break;
            case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
                state.tiled_bottom = true;
                break;
            default:
                break;
        }
    }
}

void Window::on_toplevel_close(void* data, xdg_toplevel* /*toplevel*/) {
    auto& self = *static_cast<Window*>(data);
    if (self.close_handler_) {
        self.close_handler_();
    }
}

void Window::on_toplevel_configure_bounds(void* /*data*/, xdg_toplevel* /*toplevel*/,
                                          std::int32_t width, std::int32_t height) {
    log::debug("configure bounds {}x{}", width, height);
}

void Window::on_toplevel_wm_capabilities(void* data, xdg_toplevel* /*toplevel*/,
                                         wl_array* capabilities) {
    // Sent before a configure; replaces the "everything" assumed without it.
    auto& self = *static_cast<Window*>(data);
    WindowState& state = self.pending_state_;
    state.can_minimize = state.can_maximize = state.has_window_menu = false;
    for (const std::uint32_t value : values_of(capabilities)) {
        state.can_minimize = state.can_minimize || value == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
        state.can_maximize = state.can_maximize || value == XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
        state.has_window_menu =
            state.has_window_menu || value == XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU;
    }
}

void Window::move(wl_seat* seat, std::uint32_t serial) {
    if (seat != nullptr) {
        xdg_toplevel_move(toplevel_.get(), seat, serial);
    }
}

void Window::resize(wl_seat* seat, std::uint32_t serial, Edge edge) {
    if (seat != nullptr) {
        xdg_toplevel_resize(toplevel_.get(), seat, serial, to_xdg_edge(edge));
    }
}

void Window::minimize() { xdg_toplevel_set_minimized(toplevel_.get()); }

void Window::toggle_maximized() {
    if (state_.maximized) {
        xdg_toplevel_unset_maximized(toplevel_.get());
    } else {
        xdg_toplevel_set_maximized(toplevel_.get());
    }
}

void Window::show_window_menu(wl_seat* seat, std::uint32_t serial, float x, float y) {
    if (seat != nullptr) {
        xdg_toplevel_show_window_menu(toplevel_.get(), seat, serial, static_cast<std::int32_t>(x),
                                      static_cast<std::int32_t>(y));
    }
}

void Window::close() {
    if (close_handler_) {
        close_handler_();
    }
}

void Window::on_decoration_configure(void* data, zxdg_toplevel_decoration_v1* /*decoration*/,
                                     std::uint32_t mode) {
    auto& self = *static_cast<Window*>(data);
    const bool client = mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    if (client != self.client_decorations_ || !self.decoration_answered_) {
        // The compositor's answer has the last word: one title bar, never two.
        log::info("decorations: {}", client ? "Nordend's own (client-side)"
                                            : "the compositor's title bar (server-side)");
    }
    self.decoration_answered_ = true;
    self.client_decorations_ = client;
    self.update_buffer_size();
    self.request_redraw();
}

void Window::on_preferred_scale(void* data, wp_fractional_scale_v1* /*scale*/,
                                std::uint32_t scale_120) {
    auto& self = *static_cast<Window*>(data);
    const auto scale = static_cast<std::int32_t>(scale_120);
    if (scale == self.scale_120_) {
        return;
    }
    log::debug("preferred scale {:.3f}", static_cast<double>(scale) / kScaleDenominator);
    self.scale_120_ = scale;
    self.update_buffer_size();
    self.request_redraw();
}

void Window::on_frame_done(void* data, wl_callback* /*callback*/, std::uint32_t /*time_ms*/) {
    auto& self = *static_cast<Window*>(data);
    self.frame_callback_.reset();
    if (self.frame_shown_handler_) {
        self.frame_shown_handler_();
    }
    if (self.dirty_) {
        self.draw();
    }
}

}  // namespace nordend::ui
