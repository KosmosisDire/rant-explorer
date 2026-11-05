#include "image.hpp"

#include "rant_math.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>

Image image_decode(const std::string& file)
{
    Image image;
    int   channels = 0;
    stbi_uc* pixels = stbi_load_from_memory((const stbi_uc*)file.data(), (int)file.size(),
                                            &image.width, &image.height, &channels, 4);
    if (!pixels) {
        image.width = image.height = 0;
        return image;
    }
    image.rgba.assign(pixels, pixels + (size_t)image.width * image.height * 4);
    stbi_image_free(pixels);
    return image;
}

void image_premultiply(Image& image)
{
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        const unsigned alpha = image.rgba[i + 3];
        image.rgba[i + 0] = (uint8_t)(image.rgba[i + 0] * alpha / 255);
        image.rgba[i + 1] = (uint8_t)(image.rgba[i + 1] * alpha / 255);
        image.rgba[i + 2] = (uint8_t)(image.rgba[i + 2] * alpha / 255);
    }
}

namespace {

uint8_t clamp8(float v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
}

/* How one YUV matrix, range and bit depth turns samples into RGB. */
struct Yuv {
    float y0, ky, c0, kc;       /* the black level and scale of luma, the zero and scale of chroma */
    float rv, gu, gv, bu;
};

Yuv yuv_matrix(int matrix, bool full, int bits)
{
    const float kr = matrix == 709 ? 0.2126f : matrix == 2020 ? 0.2627f : 0.299f;
    const float kb = matrix == 709 ? 0.0722f : matrix == 2020 ? 0.0593f : 0.114f;
    const float kg = 1 - kr - kb, unit = (float)(1 << (bits - 8));
    Yuv m;
    m.y0 = full ? 0 : 16 * unit;
    m.ky = 255.f / ((full ? 255 : 219) * unit);
    m.c0 = 128 * unit;
    m.kc = 255.f / ((full ? 255 : 224) * unit);
    m.rv = 2 * (1 - kr);
    m.gu = -2 * kb * (1 - kb) / kg;
    m.gv = -2 * kr * (1 - kr) / kg;
    m.bu = 2 * (1 - kb);
    return m;
}

void yuv_to_rgb(const Yuv& m, int y, int u, int v, uint8_t* out)
{
    const float c = (y - m.y0) * m.ky, d = (u - m.c0) * m.kc, e = (v - m.c0) * m.kc;
    out[0] = clamp8(c + m.rv * e);
    out[1] = clamp8(c + m.gu * d + m.gv * e);
    out[2] = clamp8(c + m.bu * d);
    out[3] = 255;
}

} /* namespace */

bool image_file_format(const uint8_t* data, size_t size, rant::types::ImageFormat& format)
{
    static const uint8_t PNG[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' }, JPEG[] = { 0xff, 0xd8, 0xff };
    if (size >= sizeof PNG && std::memcmp(data, PNG, sizeof PNG) == 0) format = rant::types::ImageFormat::Png;
    else if (size >= sizeof JPEG && std::memcmp(data, JPEG, sizeof JPEG) == 0) format = rant::types::ImageFormat::Jpeg;
    else return false;
    return true;
}

bool image_levels(const ImageFrame& f, Levels& out, bool zero_is_value)
{
    using rant::types::ImageFormat;
    const size_t bytes = f.format == ImageFormat::Mono16 ? 2 : f.format == ImageFormat::Monof32 ? 4 : 0;
    const size_t row = f.stride ? f.stride : f.width * bytes;
    if (!bytes || !f.data || row < f.width * bytes) return false;
    if (f.height && (size_t)(f.height - 1) * row + f.width * bytes > f.size) return false;
    double lo = INFINITY, hi = -INFINITY;
    for (uint32_t y = 0; y < f.height; y++)
        for (uint32_t x = 0; x < f.width; x++) {
            const uint8_t* at = f.data + y * row + x * bytes;
            double v;
            if (bytes == 2) {
                v = at[0] | at[1] << 8;
            } else {
                float g;
                std::memcpy(&g, at, 4);
                v = g;
            }
            if ((v != 0 || zero_is_value) && std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
    if (!std::isfinite(lo)) lo = 0;
    if (!(hi > lo)) hi = lo + 1;   /* a flat frame keeps its level */
    out = Levels{ lo, hi };
    return true;
}

Image image_from_frame(const ImageFrame& f, const Levels* levels)
{
    using rant::types::ImageFormat;
    Image image;
    if (!f.data) return image;

    if (f.format == ImageFormat::Jpeg || f.format == ImageFormat::Png) {
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(f.data, (int)f.size, &image.width, &image.height, &channels, 4);
        if (!pixels) {
            image.width = image.height = 0;
            return image;
        }
        image.rgba.assign(pixels, pixels + (size_t)image.width * image.height * 4);
        stbi_image_free(pixels);
        return image;
    }

    if (f.width > 16384 || f.height > 16384) return image;
    rant::math::ImageView view;
    view.width  = f.width;
    view.height = f.height;
    view.stride = f.stride;
    view.format = f.format;
    view.data   = f.data;
    view.size   = f.size;
    try {
        rant::math::image_check(view);
        rant::types::Image out;
        Levels own;
        if (!levels) {
            image_levels(f, own);
            levels = &own;
        }
        if (f.format == ImageFormat::Mono16 || f.format == ImageFormat::Monof32)
            out = rant::math::image_colorize(view, levels->lo, levels->hi,
                                             levels->colormap ? rant::math::Colormap::Viridis : rant::math::Colormap::Gray,
                                             ImageFormat::Rgba8);
        else
            out = rant::math::image_convert(view, ImageFormat::Rgba8);
        image.width  = (int)out.width;
        image.height = (int)out.height;
        image.rgba   = std::move(out.data);
    } catch (const rant::math::MathError&) {
        return Image();
    }
    return image;
}

Image image_from_planes(const Planes& p)
{
    Image image;
    if (p.width <= 0 || p.height <= 0 || !p.plane[0]) return image;
    image.width  = p.width;
    image.height = p.height;
    image.rgba.resize((size_t)p.width * p.height * 4);
    const bool wide = p.bits > 8, gray = !p.plane[1] || !p.plane[2];
    const Yuv  m    = yuv_matrix(p.matrix, p.full_range, p.bits);
    const int  down = p.bits - 8;   /* an RGB sample's shift to 8 bits */
    auto at = [&](int c, int x, int y) -> int {
        const uint8_t* row = p.plane[c] + (ptrdiff_t)y * p.stride[c];
        return wide ? (row[x * 2] | row[x * 2 + 1] << 8) : row[x];
    };
    for (int y = 0; y < p.height; y++) {
        uint8_t* dst = image.rgba.data() + (size_t)y * p.width * 4;
        const int cy = y >> p.shift_y;
        for (int x = 0; x < p.width; x++, dst += 4) {
            const int luma = at(0, x, y);
            if (gray) {
                dst[0] = dst[1] = dst[2] = clamp8((luma - m.y0) * m.ky);
                dst[3] = 255;
                continue;
            }
            const int cx = x >> p.shift_x;
            if (p.matrix == 0) {   /* identity: the planes are G, B and R */
                dst[0] = (uint8_t)(at(2, cx, cy) >> down);
                dst[1] = (uint8_t)(luma >> down);
                dst[2] = (uint8_t)(at(1, cx, cy) >> down);
                dst[3] = 255;
                continue;
            }
            yuv_to_rgb(m, luma, at(1, cx, cy), at(2, cx, cy), dst);
        }
    }
    return image;
}

std::vector<uint8_t> image_bmp(const Image& image)
{
    const uint32_t pixels = (uint32_t)image.width * (uint32_t)image.height * 4;
    std::vector<uint8_t> out(54 + pixels);
    auto put16 = [&out](size_t at, uint32_t v) { out[at] = (uint8_t)v; out[at + 1] = (uint8_t)(v >> 8); };
    auto put32 = [&put16](size_t at, uint32_t v) { put16(at, v & 0xFFFF); put16(at + 2, v >> 16); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, (uint32_t)out.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, (uint32_t)image.width);
    put32(22, (uint32_t)image.height);   /* positive: the rows run bottom up */
    put16(26, 1);
    put16(28, 32);
    put32(34, pixels);
    for (int y = 0; y < image.height; y++) {
        const uint8_t* row = image.rgba.data() + (size_t)(image.height - 1 - y) * image.width * 4;
        uint8_t*       to  = out.data() + 54 + (size_t)y * image.width * 4;
        for (int x = 0; x < image.width; x++) {
            to[x * 4 + 0] = row[x * 4 + 2];
            to[x * 4 + 1] = row[x * 4 + 1];
            to[x * 4 + 2] = row[x * 4 + 0];
            to[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    return out;
}

std::vector<uint8_t> image_png(const Image& image)
{
    std::vector<uint8_t> out;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* to = static_cast<std::vector<uint8_t>*>(context);
            to->insert(to->end(), (const uint8_t*)data, (const uint8_t*)data + size);
        },
        &out, image.width, image.height, 4, image.rgba.data(), image.width * 4);
    return out;
}
