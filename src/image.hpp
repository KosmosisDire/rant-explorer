/* Pictures to and from RGBA, the one place stb is compiled: the logo, since RmlUi's OpenGL
   3 renderer reads only TGA and SDL wants raw pixels for the window icon, the frames of the
   standard Image type a topic carries, decoded video pictures, and those encoded for the
   clipboard. */
#ifndef IMAGE_HPP
#define IMAGE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;   /* width * height * 4, straight alpha */

    bool valid() const { return width > 0 && height > 0; }
};

/* Decode a PNG or JPEG file's bytes to straight alpha RGBA. Invalid on failure. */
Image image_decode(const std::string& file);

/* RmlUi wants premultiplied alpha for generated textures, SDL wants straight. */
void image_premultiply(Image& image);

/* One frame of the standard Image type, by its format number, to straight alpha RGBA.
   A stride of 0 means packed rows. Invalid when the format is unknown or the data is too
   short for the size. */
Image image_from_frame(uint32_t width, uint32_t height, uint32_t stride, int format,
                       const uint8_t* data, size_t size);

/* A decoded video picture: a luma plane and two chroma planes, or luma alone for gray.
   Chroma is subsampled by the shifts. Above 8 bits a sample is two bytes, little endian.
   matrix is 601, 709 or 2020, or 0 for identity, where the planes hold G, B and R. */
struct Planes {
    const uint8_t* plane[3] = {};
    ptrdiff_t      stride[3] = {};   /* bytes per row */
    int            width = 0, height = 0;
    int            shift_x = 1, shift_y = 1;
    int            bits = 8;
    int            matrix = 601;
    bool           full_range = false;
};
Image image_from_planes(const Planes& planes);

/* The picture as a PNG file, and as a BMP file of 32 bit BGRA rows. */
std::vector<uint8_t> image_png(const Image& image);
std::vector<uint8_t> image_bmp(const Image& image);

/* The format numbers of the standard Image type's enum. */
struct ImageFormat {
    enum : int { Mono8 = 0, Mono16 = 1, Rgb8 = 2, Rgba8 = 3, Bgr8 = 4, Yuyv = 5, Nv12 = 6, Monof32 = 7,
                 Jpeg = 16, Png = 17 };
};

/* The name of an Image format number, for a message when it cannot be shown. */
const char* image_format_name(int format);

#endif /* IMAGE_HPP */
