#include "record.hpp"

#include <codec_api.h>
#define MSF_GIF_IMPL
#include <msf_gif.h>
#define MINIMP4_IMPLEMENTATION
#include <minimp4.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten.h>
#else
#include <RmlUi_Include_GL3.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

/* A page without threads encodes on the UI thread, as frames are pushed. */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
constexpr bool THREADS = false;
#else
constexpr bool THREADS = true;
#endif

/* Frames a recording takes a second. A GIF counts in hundredths, so 25 keeps its delays even. */
constexpr double MP4_RATE = 30, GIF_RATE = 25;

class Encoder {
public:
    virtual ~Encoder() = default;
    /* One frame shown for duration seconds. False once encoding failed. */
    virtual bool frame(const Image& frame, double duration) = 0;
    virtual std::vector<uint8_t> end() = 0;
};

class Gif : public Encoder {
public:
    Gif(int width, int height) { ok_ = msf_gif_begin(&state_, width, height) != 0; }
    ~Gif() override
    {
        if (ok_) msf_gif_free(msf_gif_end(&state_));
    }

    bool frame(const Image& frame, double duration) override
    {
        /* Delays round from the running total, so their error never adds up. */
        shown_ += duration;
        const int cs = std::max(2, (int)std::lround(shown_ * 100) - shown_cs_);
        shown_cs_ += cs;
        ok_ = ok_ && msf_gif_frame(&state_, const_cast<uint8_t*>(frame.rgba.data()), cs, 16, frame.width * 4);
        return ok_;
    }

    std::vector<uint8_t> end() override
    {
        if (!ok_) return {};
        ok_ = false;
        const MsfGifResult result = msf_gif_end(&state_);
        const uint8_t* data = static_cast<const uint8_t*>(result.data);
        std::vector<uint8_t> file(data, data + (data ? result.dataSize : 0));
        msf_gif_free(result);
        return file;
    }

private:
    MsfGifState state_{};
    bool        ok_ = false;
    double      shown_ = 0;
    int         shown_cs_ = 0;
};

/* H.264 in an MP4, BT.709 at limited range, which every player assumes for a stream that
   states it. */
class Mp4 : public Encoder {
public:
    Mp4(int width, int height) : width_(width), height_(height)
    {
        if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) return;
        SEncParamExt p;
        encoder_->GetDefaultParams(&p);
        p.iUsageType       = SCREEN_CONTENT_REAL_TIME;   /* sharp lines and text over flat fills */
        p.iPicWidth        = width;
        p.iPicHeight       = height;
        p.iRCMode          = RC_QUALITY_MODE;
        p.iTargetBitrate   = std::max(1000000, width * height * 4);
        p.iMaxBitrate      = UNSPECIFIED_BIT_RATE;
        p.fMaxFrameRate    = (float)MP4_RATE;
        p.uiIntraPeriod    = (unsigned)(2 * MP4_RATE);   /* a seek point every two seconds */
        p.bEnableFrameSkip = false;
        p.iMultipleThreadIdc = 1;
        p.iSpatialLayerNum = 1;
        SSpatialLayerConfig& layer = p.sSpatialLayers[0];
        layer.iVideoWidth       = width;
        layer.iVideoHeight      = height;
        layer.fFrameRate        = (float)MP4_RATE;
        layer.iSpatialBitrate   = p.iTargetBitrate;
        layer.iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
        layer.bVideoSignalTypePresent  = true;
        layer.uiVideoFormat            = VF_UNDEF;
        layer.bFullRange               = false;
        layer.bColorDescriptionPresent = true;
        layer.uiColorPrimaries         = CP_BT709;
        layer.uiTransferCharacteristics = TRC_BT709;
        layer.uiColorMatrix            = CM_BT709;
        if (encoder_->InitializeExt(&p) != 0) return;
        int format = videoFormatI420;
        encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &format);
        mux_ = MP4E_open(0, 0, &file_, write);
        if (!mux_) return;
        mp4_h26x_write_init(&writer_, mux_, width, height, 0);
        yuv_.resize((size_t)width * height * 3 / 2);
        ok_ = true;
    }

    ~Mp4() override
    {
        if (mux_) {
            MP4E_close(mux_);
            mp4_h26x_write_close(&writer_);
        }
        if (encoder_) {
            encoder_->Uninitialize();
            WelsDestroySVCEncoder(encoder_);
        }
    }

    bool frame(const Image& frame, double duration) override
    {
        if (!ok_) return false;
        uint8_t* y = yuv_.data();
        uint8_t* u = y + width_ * height_;
        uint8_t* v = u + width_ * height_ / 4;
        to_i420(frame, y, u, v);
        SSourcePicture picture{};
        picture.iColorFormat = videoFormatI420;
        picture.iPicWidth    = width_;
        picture.iPicHeight   = height_;
        picture.iStride[0]   = width_;
        picture.iStride[1] = picture.iStride[2] = width_ / 2;
        picture.pData[0] = y;
        picture.pData[1] = u;
        picture.pData[2] = v;
        picture.uiTimeStamp = std::llround(shown_ * 1000);
        SFrameBSInfo info{};
        if (encoder_->EncodeFrame(&picture, &info) != cmResultSuccess) return ok_ = false;
        /* Durations count from the running total, and a skipped frame's time goes to the next. */
        shown_ += duration;
        if (info.eFrameType == videoFrameTypeSkip) return true;
        const int64_t  total = std::llround(shown_ * 90000);
        const unsigned ticks = (unsigned)std::max<int64_t>(1, total - ticks_);
        ticks_ = total;
        for (int i = 0; i < info.iLayerNum; i++) {
            const SLayerBSInfo& layer = info.sLayerInfo[i];
            int size = 0;
            for (int n = 0; n < layer.iNalCount; n++) size += layer.pNalLengthInByte[n];
            if (mp4_h26x_write_nal(&writer_, layer.pBsBuf, size, ticks) != MP4E_STATUS_OK) return ok_ = false;
        }
        return true;
    }

    std::vector<uint8_t> end() override
    {
        if (!ok_ || !mux_) return {};
        ok_ = MP4E_close(mux_) == MP4E_STATUS_OK;
        mp4_h26x_write_close(&writer_);
        mux_ = nullptr;
        return ok_ ? std::move(file_) : std::vector<uint8_t>();
    }

private:
    static int write(int64_t offset, const void* buffer, size_t size, void* token)
    {
        std::vector<uint8_t>& file = *static_cast<std::vector<uint8_t>*>(token);
        if (file.size() < (size_t)offset + size) file.resize((size_t)offset + size);
        std::memcpy(file.data() + offset, buffer, size);
        return 0;
    }

    /* BT.709 at limited range in 8 bit fixed point, chroma from each 2 by 2 block's mean. */
    void to_i420(const Image& frame, uint8_t* y, uint8_t* u, uint8_t* v) const
    {
        const uint8_t* rgba = frame.rgba.data();
        for (int i = 0; i < width_ * height_; i++, rgba += 4)
            y[i] = (uint8_t)(16 + ((47 * rgba[0] + 157 * rgba[1] + 16 * rgba[2] + 128) >> 8));
        for (int row = 0; row < height_ / 2; row++)
            for (int col = 0; col < width_ / 2; col++) {
                int r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    const uint8_t* p = frame.rgba.data() + (((size_t)(row * 2 + k / 2) * width_ + col * 2 + k % 2) * 4);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
                /* The sums are four times the mean, so the shift is two more. The offset keeps
                   the sum positive before it shifts. */
                const size_t at = (size_t)row * (width_ / 2) + col;
                u[at] = (uint8_t)((-26 * r - 87 * g + 112 * b + (128 << 10) + 512) >> 10);
                v[at] = (uint8_t)((112 * r - 102 * g - 10 * b + (128 << 10) + 512) >> 10);
            }
    }

    int                  width_, height_;
    ISVCEncoder*         encoder_ = nullptr;
    MP4E_mux_t*          mux_ = nullptr;
    mp4_h26x_writer_t    writer_{};
    std::vector<uint8_t> file_, yuv_;
    bool                 ok_ = false;
    double               shown_ = 0;
    int64_t              ticks_ = 0;
};

/* The frame cut or padded to the size around its centre. Padding takes its top left colour,
   which is the card's fill. */
Image fit(Image frame, int width, int height)
{
    if (frame.width == width && frame.height == height) return frame;
    Image out;
    out.width  = width;
    out.height = height;
    out.rgba.resize((size_t)width * height * 4);
    for (size_t i = 0; i < out.rgba.size(); i += 4) std::memcpy(&out.rgba[i], frame.rgba.data(), 4);
    const int dx = (width - frame.width) / 2, dy = (height - frame.height) / 2;
    for (int row = std::max(0, dy); row < std::min(height, dy + frame.height); row++) {
        const int from = std::max(0, dx), to = std::min(width, dx + frame.width);
        std::memcpy(&out.rgba[((size_t)row * width + from) * 4],
                    &frame.rgba[((size_t)(row - dy) * frame.width + from - dx) * 4], (size_t)(to - from) * 4);
    }
    return out;
}

double now_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::mutex  note_lock;
std::string note_text;
bool        note_new = false;

void post_note(const std::string& text)
{
    std::lock_guard<std::mutex> hold(note_lock);
    note_text = text;
    note_new  = true;
}

} /* namespace */

const char* file_extension(FileKind kind)
{
    switch (kind) {
    case FileKind::Png: return "png";
    case FileKind::Mp4: return "mp4";
    case FileKind::Gif: return "gif";
    }
    return "";
}

struct Recording::Impl {
    struct Item {
        Image  frame;
        double seconds = 0;
        bool   last = false;
    };

    FileKind                 kind;
    int                      width, height;
    double                   pace;
    std::unique_ptr<Encoder> encoder;

    /* The encoding side: the frame waiting for the next to say how long it shows. */
    Image  held;
    double held_at = 0;
    bool   holding = false, failed = false;

    mutable std::mutex      lock;
    std::condition_variable wake;
    std::deque<Item>        queue;
    bool                    busy = false;
    double                  last_at = 0, next_at = 0;
    double                  lag = 0;   /* how long the latest frame took to encode */
    std::vector<uint8_t>    file;
    std::thread             worker;    /* last, so it starts after what it reads */

    void take(Item& item)
    {
        if (holding && !failed) failed = !encoder->frame(held, std::max(0.0, item.seconds - held_at));
        if (item.last) {
            file = failed ? std::vector<uint8_t>() : encoder->end();
            return;
        }
        held    = std::move(item.frame);
        held_at = item.seconds;
        holding = true;
    }

    /* Encodes one item, timed, so due can slow the pace to what the encoder keeps up with. */
    void timed(Item& item)
    {
        const double start = now_seconds();
        take(item);
        std::lock_guard<std::mutex> hold(lock);
        lag  = now_seconds() - start;
        busy = false;
    }

    void run()
    {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> hold(lock);
                wake.wait(hold, [this] { return !queue.empty(); });
                item = std::move(queue.front());
                queue.pop_front();
                busy = true;
            }
            const bool last = item.last;
            timed(item);
            if (last) return;
        }
    }

    void push(Item item)
    {
        if (!THREADS) {
            timed(item);
            return;
        }
        std::lock_guard<std::mutex> hold(lock);
        queue.push_back(std::move(item));
        wake.notify_one();
    }
};

Recording::Recording(FileKind kind, int width, int height) : impl_(new Impl)
{
    Impl& im = *impl_;
    im.kind = kind;
    /* H.264 subsamples chroma by two both ways. */
    im.width  = kind == FileKind::Mp4 ? width & ~1 : width;
    im.height = kind == FileKind::Mp4 ? height & ~1 : height;
    im.pace   = 1.0 / (kind == FileKind::Gif ? GIF_RATE : MP4_RATE);
    if (kind == FileKind::Gif) im.encoder.reset(new Gif(im.width, im.height));
    else im.encoder.reset(new Mp4(im.width, im.height));
    if (THREADS) im.worker = std::thread([&im] { im.run(); });
}

Recording::~Recording()
{
    if (impl_->worker.joinable()) {
        impl_->push(Impl::Item{ Image(), impl_->last_at, true });
        impl_->worker.join();
    }
}

bool Recording::due(double seconds) const
{
    const Impl& im = *impl_;
    std::lock_guard<std::mutex> hold(im.lock);
    /* A quarter frame early still counts, so the jitter of the frames drawn never skips one. */
    return im.queue.size() + (im.busy ? 1 : 0) < 2 && seconds >= im.next_at - im.pace / 4;
}

void Recording::push(Image frame, double seconds)
{
    Impl& im = *impl_;
    {
        /* Frames keep to a grid, which starts over after a fall behind. A page encodes on the UI
           thread, so it leaves at least as long again for the UI. */
        std::lock_guard<std::mutex> hold(im.lock);
        const double gap = std::max(im.pace, THREADS ? im.lag : 2 * im.lag);
        im.next_at = im.next_at + gap < seconds ? seconds + gap : im.next_at + gap;
        im.last_at = seconds;
    }
    im.push(Impl::Item{ fit(std::move(frame), im.width, im.height), seconds, false });
}

std::vector<uint8_t> Recording::finish(double seconds)
{
    Impl& im = *impl_;
    /* The last frame shows a frame's time at least, so a still recording is not empty. */
    im.push(Impl::Item{ Image(), std::max(seconds, im.last_at + im.pace), true });
    if (im.worker.joinable()) im.worker.join();
    return std::move(im.file);
}

Image read_window(int x, int y, int width, int height, int window_height)
{
    Image image;
    if (width <= 0 || height <= 0) return image;
    image.width  = width;
    image.height = height;
    image.rgba.resize((size_t)width * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(x, window_height - y - height, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    /* GL counts rows from the bottom. */
    const size_t row = (size_t)width * 4;
    std::vector<uint8_t> swap(row);
    for (int top = 0, bottom = height - 1; top < bottom; top++, bottom--) {
        std::memcpy(swap.data(), &image.rgba[top * row], row);
        std::memcpy(&image.rgba[top * row], &image.rgba[bottom * row], row);
        std::memcpy(&image.rgba[bottom * row], swap.data(), row);
    }
    for (size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
    return image;
}

#ifdef __EMSCRIPTEN__

EM_JS_DEPS(record, "$UTF8ToString");

EM_JS(void, js_download, (const uint8_t* data, int size, const char* name, const char* type), {
    const blob = new Blob([HEAPU8.slice(data, data + size)], { type: UTF8ToString(type) });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = UTF8ToString(name);
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 60000);
});

void save_file(SDL_Window*, std::vector<uint8_t> bytes, FileKind kind, const std::string& name)
{
    static const char* const TYPES[] = { "image/png", "video/mp4", "image/gif" };
    const std::string file = name + "." + file_extension(kind);
    js_download(bytes.data(), (int)bytes.size(), file.c_str(), TYPES[(int)kind]);
    post_note("downloaded " + file);
}

#else

namespace {

struct Pending {
    std::vector<uint8_t> bytes;
    std::string          extension;
};

/* Called once the dialog closes, maybe on another thread. */
void SDLCALL saved(void* userdata, const char* const* files, int)
{
    std::unique_ptr<Pending> pending(static_cast<Pending*>(userdata));
    if (!files) {
        post_note(std::string("save failed: ") + SDL_GetError());
        return;
    }
    if (!files[0]) return;   /* canceled */
    std::string path = files[0];
    const std::string dot = "." + pending->extension;
    std::string tail = path.size() >= dot.size() ? path.substr(path.size() - dot.size()) : std::string();
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (tail != dot) path += dot;
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    out.write(reinterpret_cast<const char*>(pending->bytes.data()), (std::streamsize)pending->bytes.size());
    const std::string name = std::filesystem::u8path(path).filename().u8string();
    post_note(out.good() ? "saved " + name : "could not write " + path);
}

} /* namespace */

void save_file(SDL_Window* window, std::vector<uint8_t> bytes, FileKind kind, const std::string& name)
{
    static const SDL_DialogFileFilter FILTERS[] = {
        { "PNG image", "png" }, { "MP4 video", "mp4" }, { "GIF animation", "gif" } };
    const SDL_DialogFileFilter& filter = FILTERS[(int)kind];
    Pending* pending = new Pending{ std::move(bytes), file_extension(kind) };
    const std::string file = name + "." + pending->extension;
    SDL_ShowSaveFileDialog(saved, pending, window, &filter, 1, file.c_str());
}

#endif

bool save_note(std::string& note)
{
    std::lock_guard<std::mutex> hold(note_lock);
    if (!note_new) return false;
    note     = note_text;
    note_new = false;
    return true;
}
