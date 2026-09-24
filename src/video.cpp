#include "video.hpp"

#include <codec_api.h>
#include <dav1d/dav1d.h>
#include <libde265/de265.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>

namespace {

using Packet = Capture::Packet;

/* The VideoFrame codec enum's values. */
enum Codec { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 };

const char* codec_name(int codec)
{
    switch (codec) {
    case Mjpeg: return "MJPEG";
    case H264:  return "H.264";
    case H265:  return "H.265";
    case Av1:   return "AV1";
    default:    return "unknown codec";
    }
}

/* A decoder holding more frames than this skips ahead to the newest keyframe among them. */
constexpr size_t BEHIND = 8;
/* One holding more than this drops them all and waits for the next keyframe. */
constexpr size_t OVERRUN = 240;

/* ------------------------------------------------------------------ bitstreams */

bool annex_b(const uint8_t* d, size_t n)
{
    return n >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1));
}

/* Each NAL unit of an Annex B stream, after its start code. */
template <class Visit>
void each_nal(const uint8_t* d, size_t n, Visit visit)
{
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 3 <= n;) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) visit(d + start, i - start);
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < n) visit(d + start, n - start);
}

/* NAL units with a four byte length each, as MP4 carries them, rewritten with start codes.
   Anything else passes as it is. */
const uint8_t* as_annex_b(const uint8_t* d, size_t& n, std::vector<uint8_t>& buf)
{
    if (annex_b(d, n)) return d;
    auto length = [d](size_t at) { return (size_t)d[at] << 24 | (size_t)d[at + 1] << 16 | (size_t)d[at + 2] << 8 | d[at + 3]; };
    size_t at = 0;
    while (at + 4 <= n) {
        const size_t len = length(at);
        if (!len || len > n - at - 4) return d;
        at += 4 + len;
    }
    if (at != n || !n) return d;
    buf.clear();
    for (at = 0; at < n; at += 4 + length(at)) {
        const uint8_t code[4] = { 0, 0, 0, 1 };
        buf.insert(buf.end(), code, code + 4);
        buf.insert(buf.end(), d + at + 4, d + at + 4 + length(at));
    }
    n = buf.size();
    return buf.data();
}

/* An AV1 temporal unit's OBUs by type, each with its size field. False once one lacks it. */
template <class Visit>
bool each_obu(const uint8_t* d, size_t n, Visit visit)
{
    for (size_t at = 0; at < n;) {
        const uint8_t head = d[at];
        if (head & 0x81 || !(head & 0x02)) return false;
        size_t p = at + 1 + ((head & 0x04) ? 1 : 0);
        uint64_t size = 0;
        for (int i = 0; i < 8 && p < n; i++) {
            size |= (uint64_t)(d[p] & 0x7F) << (7 * i);
            if (!(d[p++] & 0x80)) break;
        }
        if (size > n - std::min(p, n)) return false;
        visit((head >> 3) & 0x0F);
        at = p + (size_t)size;
    }
    return true;
}

/* The codec of a frame that does not state one, from how its bitstream starts. */
int sniff(const uint8_t* d, size_t n)
{
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return Mjpeg;
    int first = -1;
    if (n && !annex_b(d, n) && each_obu(d, n, [&first](int type) { if (first < 0) first = type; }) &&
        (first == 1 || first == 2 || first == 6))   /* a sequence header, a temporal delimiter, a frame */
        return Av1;
    std::vector<uint8_t> buf;
    const uint8_t* s = as_annex_b(d, n, buf);
    if (!annex_b(s, n)) return Unknown;
    /* H.265 has a two byte header whose second byte is almost always 1. */
    int h265 = 0, h264 = 0;
    each_nal(s, n, [&](const uint8_t* nal, size_t len) {
        if (len < 2 || nal[0] & 0x80) return;
        const int type = (nal[0] >> 1) & 0x3F;
        if (nal[1] == 1 && (type <= 21 || (type >= 32 && type <= 40))) h265++;
        else h264++;
    });
    return h265 > h264 ? H265 : h264 ? H264 : Unknown;
}

/* Whether decoding can start at this frame: an intra picture or the parameters before one. */
bool opens(int codec, const uint8_t* d, size_t n)
{
    bool key = false;
    switch (codec) {
    case Mjpeg:
        return true;
    case H264:
        each_nal(d, n, [&key](const uint8_t* nal, size_t len) {
            if (len && ((nal[0] & 0x1F) == 5 || (nal[0] & 0x1F) == 7)) key = true;
        });
        return key;
    case H265:
        each_nal(d, n, [&key](const uint8_t* nal, size_t len) {
            const int type = len ? (nal[0] >> 1) & 0x3F : -1;
            if ((type >= 16 && type <= 23) || type == 32 || type == 33) key = true;
        });
        return key;
    case Av1:
        each_obu(d, n, [&key](int type) { if (type == 1) key = true; });
        return key;
    default:
        return false;
    }
}

/* The Huffman tables of JPEG's Annex K. MJPEG cameras leave them out of every frame and a
   decoder is expected to assume them. */
const uint8_t DEFAULT_DHT[] = {
    0xFF, 0xC4, 0x01, 0xA2,
    0x00, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    0x10, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
    0x01, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    0x11, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};

/* A JPEG with no Huffman table before its scan gets the default ones put in front of it. */
const uint8_t* with_tables(const uint8_t* d, size_t& n, std::vector<uint8_t>& buf)
{
    for (size_t at = 2; at + 4 <= n && d[at] == 0xFF;) {
        const uint8_t marker = d[at + 1];
        if (marker == 0xFF) {   /* a fill byte */
            at++;
            continue;
        }
        if (marker == 0xC4) return d;
        if (marker == 0xDA) {
            buf.assign(d, d + at);
            buf.insert(buf.end(), std::begin(DEFAULT_DHT), std::end(DEFAULT_DHT));
            buf.insert(buf.end(), d + at, d + n);
            n = buf.size();
            return buf.data();
        }
        at += 2 + ((size_t)d[at + 2] << 8 | d[at + 3]);
    }
    return d;
}

/* A picture's matrix from its H.273 code, which all three codecs share. Unstated reads as
   the size suggests: BT.709 from HD up, BT.601 below. */
int matrix_of(int code, int height)
{
    switch (code) {
    case 0:          return 0;
    case 1:          return 709;
    case 5: case 6:  return 601;
    case 9: case 10: return 2020;
    default:         return height >= 720 ? 709 : 601;
    }
}

/* ------------------------------------------------------------------ decoders */

/* One codec's decoder. A frame goes in and out takes the picture that came out of it, when
   one did and want is set, which only the newest frame of a batch asks. */
class Backend {
public:
    virtual ~Backend() = default;
    /* False with the reason in error when the frame cannot be decoded. */
    virtual bool decode(const uint8_t* data, size_t size, bool want, Image& out, std::string& error) = 0;
};

class MjpegDecoder : public Backend {
public:
    bool decode(const uint8_t* data, size_t size, bool want, Image& out, std::string& error) override
    {
        if (!want) return true;   /* every frame stands alone */
        std::vector<uint8_t> buf;
        const uint8_t* d = with_tables(data, size, buf);
        out = image_from_frame(0, 0, 0, ImageFormat::Jpeg, d, size);
        if (out.valid()) return true;
        error = "MJPEG frame not readable";
        return false;
    }
};

class H264Decoder : public Backend {
public:
    H264Decoder()
    {
        if (WelsCreateDecoder(&dec_) != 0 || !dec_) {
            dec_ = nullptr;
            return;
        }
        SDecodingParam param = {};
        param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        param.eEcActiveIdc = ERROR_CON_DISABLE;   /* a broken frame shows nothing rather than smears */
        if (dec_->Initialize(&param) != 0) {
            WelsDestroyDecoder(dec_);
            dec_ = nullptr;
        }
    }

    ~H264Decoder() override
    {
        if (!dec_) return;
        dec_->Uninitialize();
        WelsDestroyDecoder(dec_);
    }

    bool decode(const uint8_t* data, size_t size, bool want, Image& out, std::string& error) override
    {
        if (!dec_) {
            error = "the H.264 decoder did not start";
            return false;
        }
        unsigned char* planes[3] = {};
        SBufferInfo info = {};
        const DECODING_STATE state = dec_->DecodeFrameNoDelay(data, (int)size, planes, &info);
        if (info.iBufferStatus != 1) {
            if (state == dsErrorFree) return true;
            error = "H.264 frame not decodable";
            return false;
        }
        if (!want) return true;
        const SSysMEMBuffer& buffer = info.UsrData.sSystemBuffer;
        Planes p;
        for (int i = 0; i < 3; i++) {
            p.plane[i]  = planes[i];
            p.stride[i] = buffer.iStride[i ? 1 : 0];
        }
        p.width  = buffer.iWidth;
        p.height = buffer.iHeight;
        p.matrix = matrix_of(2, buffer.iHeight);   /* openh264 does not pass the VUI on */
        out = image_from_planes(p);
        return true;
    }

private:
    ISVCDecoder* dec_ = nullptr;
};

class H265Decoder : public Backend {
public:
    H265Decoder()
    {
        ctx_ = de265_new_decoder();
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        if (ctx_) de265_start_worker_threads(ctx_, (int)std::min(4u, cores));
    }

    ~H265Decoder() override
    {
        if (ctx_) de265_free_decoder(ctx_);
    }

    bool decode(const uint8_t* data, size_t size, bool want, Image& out, std::string& error) override
    {
        if (!ctx_) {
            error = "the H.265 decoder did not start";
            return false;
        }
        de265_error e = de265_push_data(ctx_, data, (int)size, 0, nullptr);
        if (!de265_isOK(e)) {
            error = std::string("H.265: ") + de265_get_error_text(e);
            return false;
        }
        de265_push_end_of_frame(ctx_);
        for (int more = 1; more;) {
            more = 0;
            e = de265_decode(ctx_, &more);
            drain(want, out);   /* a full picture buffer only empties this way */
            if (e == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
            if (!de265_isOK(e) && !more) {
                error = std::string("H.265: ") + de265_get_error_text(e);
                return false;
            }
        }
        drain(want, out);
        return true;
    }

private:
    void drain(bool want, Image& out)
    {
        while (const de265_image* img = de265_get_next_picture(ctx_)) {
            if (!want) continue;
            const de265_chroma chroma = de265_get_chroma_format(img);
            Planes p;
            p.width  = de265_get_image_width(img, 0);
            p.height = de265_get_image_height(img, 0);
            p.bits   = de265_get_bits_per_pixel(img, 0);
            for (int i = 0; i < (chroma == de265_chroma_mono ? 1 : 3); i++) {
                int stride = 0;
                p.plane[i]  = de265_get_image_plane(img, i, &stride);
                p.stride[i] = stride;
            }
            p.shift_x    = chroma == de265_chroma_444 ? 0 : 1;
            p.shift_y    = chroma == de265_chroma_420 ? 1 : 0;
            p.matrix     = matrix_of(de265_get_image_matrix_coefficients(img), p.height);
            p.full_range = de265_get_image_full_range_flag(img) != 0;
            out = image_from_planes(p);
        }
    }

    de265_decoder_context* ctx_ = nullptr;
};

class Av1Decoder : public Backend {
public:
    Av1Decoder()
    {
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.max_frame_delay = 1;   /* a picture per frame in, as a live view wants */
        if (dav1d_open(&ctx_, &settings) < 0) ctx_ = nullptr;
    }

    ~Av1Decoder() override
    {
        if (ctx_) dav1d_close(&ctx_);
    }

    bool decode(const uint8_t* data, size_t size, bool want, Image& out, std::string& error) override
    {
        if (!ctx_) {
            error = "the AV1 decoder did not start";
            return false;
        }
        Dav1dData in = {};
        uint8_t* buf = dav1d_data_create(&in, size);
        if (!buf) {
            error = "out of memory";
            return false;
        }
        std::memcpy(buf, data, size);
        /* Sending refuses while a picture waits, so each turn takes one out. */
        bool ok = true;
        do {
            const int sent = dav1d_send_data(ctx_, &in);
            if (sent < 0 && sent != DAV1D_ERR(EAGAIN)) {
                ok = false;
                break;
            }
            if (!take(want, out)) {
                ok = false;
                break;
            }
        } while (in.sz > 0);
        if (in.sz > 0) dav1d_data_unref(&in);
        while (ok && take(want, out, true)) {}
        if (!ok) error = "AV1 frame not decodable";
        return ok;
    }

private:
    /* One picture out if there is one. False on a decode error, or with until_empty set
       once there is none left. */
    bool take(bool want, Image& out, bool until_empty = false)
    {
        Dav1dPicture pic = {};
        const int got = dav1d_get_picture(ctx_, &pic);
        if (got == DAV1D_ERR(EAGAIN)) return !until_empty;
        if (got < 0) return false;
        if (want) {
            Planes p;
            p.width  = pic.p.w;
            p.height = pic.p.h;
            p.bits   = pic.p.bpc;
            p.plane[0]  = (const uint8_t*)pic.data[0];
            p.stride[0] = pic.stride[0];
            if (pic.p.layout != DAV1D_PIXEL_LAYOUT_I400)
                for (int i = 1; i < 3; i++) {
                    p.plane[i]  = (const uint8_t*)pic.data[i];
                    p.stride[i] = pic.stride[1];
                }
            p.shift_x    = pic.p.layout == DAV1D_PIXEL_LAYOUT_I444 ? 0 : 1;
            p.shift_y    = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 ? 1 : 0;
            p.matrix     = matrix_of(pic.seq_hdr ? (int)pic.seq_hdr->mtrx : 2, pic.p.h);
            p.full_range = pic.seq_hdr && pic.seq_hdr->color_range;
            out = image_from_planes(p);
        }
        dav1d_picture_unref(&pic);
        return true;
    }

    Dav1dContext* ctx_ = nullptr;
};

std::unique_ptr<Backend> make_backend(int codec)
{
    switch (codec) {
    case Mjpeg: return std::make_unique<MjpegDecoder>();
    case H264:  return std::make_unique<H264Decoder>();
    case H265:  return std::make_unique<H265Decoder>();
    case Av1:   return std::make_unique<Av1Decoder>();
    default:    return nullptr;
    }
}

/* One packet ready to decode: its codec settled, its bitstream in the form the decoder
   takes, and whether decoding can start there. */
struct Frame {
    int                  codec = Unknown;
    const uint8_t*       data = nullptr;
    size_t               size = 0;
    std::vector<uint8_t> buf;
    bool                 key = false;
};

void prepare(const Packet& packet, Frame& f)
{
    f.data  = packet.data.data();
    f.size  = packet.data.size();
    f.codec = packet.codec >= Mjpeg && packet.codec <= Av1 ? packet.codec : sniff(f.data, f.size);
    if (f.codec == H264 || f.codec == H265) f.data = as_annex_b(f.data, f.size, f.buf);
    f.key = packet.keyframe || opens(f.codec, f.data, f.size);
}

} /* namespace */

struct VideoDecoder::Impl {
    std::mutex              mutex;
    std::condition_variable wake;
    std::deque<Packet>      queue;
    bool                    lost = false, quit = false;
    Image                   latest;
    uint64_t                latest_seq = 0;
    Info                    info;

    /* Only the worker touches these. */
    std::unique_ptr<Backend> backend;
    int                      codec = Unknown;
    bool                     need_key = true;

    std::thread              worker;   /* last, so it starts after what it reads */

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return quit || lost || !queue.empty(); });
            if (quit) return;
            std::deque<Packet> batch;
            batch.swap(queue);
            const bool restart = lost;
            lost = false;
            lock.unlock();
            decode(batch, restart);
            lock.lock();
        }
    }

    void decode(const std::deque<Packet>& batch, bool restart)
    {
        if (restart) {
            backend.reset();
            need_key = true;
        }
        std::vector<Frame> frames(batch.size());
        for (size_t i = 0; i < batch.size(); i++) prepare(batch[i], frames[i]);
        size_t first = 0;
        if (frames.size() > BEHIND)
            for (size_t i = frames.size(); i-- > 1;)
                if (frames[i].key) {
                    first = i;
                    backend.reset();
                    need_key = true;
                    break;
                }

        Image image;
        std::string problem;
        for (size_t i = first; i < frames.size(); i++) {
            const Frame& f = frames[i];
            if (f.codec == Unknown) {
                problem = "the codec is not stated and not recognised";
                continue;
            }
            if (f.codec != codec || !backend) {
                backend  = make_backend(f.codec);
                codec    = f.codec;
                need_key = true;
            }
            if (need_key && !f.key) {
                problem = "waiting for a keyframe";
                continue;
            }
            need_key = false;
            Image out;
            std::string error;
            if (!backend->decode(f.data, f.size, i + 1 == frames.size(), out, error)) {
                problem = error;
                continue;
            }
            if (out.valid()) {
                image = std::move(out);
                problem.clear();
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (!batch.empty()) {
            info.codec = codec_name(frames.back().codec);
            if (!info.width) {   /* the stated size, until a picture gives the real one */
                info.width  = (int)batch.back().width;
                info.height = (int)batch.back().height;
            }
        }
        if (image.valid()) {
            info.width  = image.width;
            info.height = image.height;
            latest = std::move(image);
            latest_seq++;
        }
        if (latest_seq || !problem.empty()) info.problem = problem;
    }
};

VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>())
{
    impl_->info.problem = "waiting for a frame";
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
}

VideoDecoder::~VideoDecoder()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->wake.notify_one();
    impl_->worker.join();
}

void VideoDecoder::push(std::vector<Packet> packets, bool lost)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (lost) {
        impl_->queue.clear();
        impl_->lost = true;
    }
    for (Packet& p : packets) impl_->queue.push_back(std::move(p));
    if (impl_->queue.size() > OVERRUN) {
        impl_->queue.clear();
        impl_->lost = true;
    }
    impl_->wake.notify_one();
}

bool VideoDecoder::picture(Image& out, uint64_t& seq)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->latest_seq == seq || !impl_->latest.valid()) return false;
    seq = impl_->latest_seq;
    out = std::move(impl_->latest);
    impl_->latest = Image();
    return true;
}

VideoDecoder::Info VideoDecoder::info() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->info;
}
