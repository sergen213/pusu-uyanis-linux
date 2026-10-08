#pragma once

#include <gtk/gtk.h>
#include <png.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace pusu {

// Decode packaged original artwork directly, without an external GTK image-loader process.
inline GdkPixbuf* load_setup_artwork(const std::filesystem::path& path, int max_width, int max_height) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    std::unique_ptr<png_image, decltype(&png_image_free)> owner(&image, png_image_free);
    if (!png_image_begin_read_from_file(&image, path.c_str()))
        throw std::runtime_error("Cannot load original Pusu artwork: " + path.string() + ": " + image.message);
    if (!image.width || !image.height || image.width > 8192 || image.height > 8192 ||
        std::uint64_t(image.width) * image.height * 4 > 256ull * 1024 * 1024 ||
        max_width < 1 || max_height < 1)
        throw std::runtime_error("Invalid packaged Pusu artwork dimensions: " + path.string());
    image.format = PNG_FORMAT_RGBA;
    std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)> pixels(
        gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, static_cast<int>(image.width),
                       static_cast<int>(image.height)), g_object_unref);
    if (!pixels) throw std::runtime_error("Cannot allocate original Pusu artwork.");
    if (!png_image_finish_read(&image, nullptr, gdk_pixbuf_get_pixels(pixels.get()),
                               gdk_pixbuf_get_rowstride(pixels.get()), nullptr))
        throw std::runtime_error("Cannot decode original Pusu artwork: " + path.string() + ": " + image.message);
    const double scale = std::min({1.0, double(max_width) / image.width, double(max_height) / image.height});
    if (scale == 1.0) return pixels.release();
    auto* scaled = gdk_pixbuf_scale_simple(pixels.get(), std::max(1, int(image.width * scale)),
        std::max(1, int(image.height * scale)), GDK_INTERP_BILINEAR);
    if (!scaled) throw std::runtime_error("Cannot scale original Pusu artwork.");
    return scaled;
}

} // namespace pusu
