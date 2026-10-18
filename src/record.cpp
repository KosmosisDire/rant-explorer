#include "record.hpp"

#include <cgif.h>
#include <codec_api.h>
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
#include <unordered_map>

namespace {

/* A page without threads encodes on the UI thread, as frames are pushed. */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
constexpr bool THREADS = false;
#else
constexpr bool THREADS = true;
#endif

/* Frames a recording takes a second. A GIF counts in hundredths, so 25 keeps its delays even. */
constexpr double MP4_RATE = 30, GIF_RATE = 25;

/* An MP4 keeps one quality throughout, low enough a QP that text and thin lines stay intact. */
constexpr int MP4_QP = 18;

/* H.264 keeps one colour per 2 by 2 pixels, so a pixel is coded as 2 by 2 while that still
   encodes at 20 frames a second. A page encodes on its UI thread, so it never doubles. */
constexpr int MP4_DOUBLED_MOST = THREADS ? 2560 * 1600 : 0;

class Encoder {
public:
    virtual ~Encoder() = default;
    /* One frame shown for duration seconds. False once encoding failed. */
    virtual bool frame(const Image& frame, double duration) = 0;
    virtual std::vector<uint8_t> end() = 0;
};

/* A frame's own colours as a GIF palette, never dithered, so a card shows exactly. A frame
   with more, a camera picture, is cut by median cut weighted by pixel count. */
class Palette {
public:
    /* One entry is left free, for the transparency that skips unchanged pixels. */
    static constexpr size_t MOST = 255;

    void take(const Image& frame, std::vector<uint8_t>& indices)
    {
        entries_.clear();
        slot_.clear();
        const uint8_t* px = frame.rgba.data();
        const size_t   n  = (size_t)frame.width * frame.height;
        indices.resize(n);
        uint32_t last = ~0u;
        uint8_t  last_slot = 0;
        bool     exact = true;
        for (size_t i = 0; i < n && exact; i++) {
            const uint32_t c = colour_at(px, i);
            if (c != last) {
                auto found = slot_.find(c);
                if (found == slot_.end()) {
                    if (entries_.size() == MOST) {
                        exact = false;
                        break;
                    }
                    found = slot_.emplace(c, (uint8_t)entries_.size()).first;
                    entries_.push_back(c);
                }
                last      = c;
                last_slot = found->second;
            }
            indices[i] = last_slot;
        }
        if (exact) return;
        entries_.clear();
        slot_.clear();
        cut(px, n);
        last = ~0u;
        for (size_t i = 0; i < n; i++) {
            const uint32_t c = colour_at(px, i);
            if (c != last) {
                last      = c;
                last_slot = slot_.at(c);
            }
            indices[i] = last_slot;
        }
    }

    /* The entries as the RGB triples a GIF stores. */
    void rgb(std::vector<uint8_t>& out) const
    {
        out.clear();
        for (const uint32_t c : entries_) out.insert(out.end(), { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) });
    }

    size_t size() const { return entries_.size(); }

private:
    static uint32_t colour_at(const uint8_t* px, size_t i)
    {
        return px[4 * i] | (uint32_t)px[4 * i + 1] << 8 | (uint32_t)px[4 * i + 2] << 16;
    }

    struct Swatch {
        uint32_t colour, count;
        int channel(int k) const { return (colour >> (8 * k)) & 0xFF; }
    };
    struct Box {
        size_t begin, end;
        int    widest = 0, range = 0;
    };

    void cut(const uint8_t* px, size_t n)
    {
        std::unordered_map<uint32_t, uint32_t> counts;
        for (size_t i = 0; i < n; i++) counts[colour_at(px, i)]++;
        std::vector<Swatch> swatches;
        swatches.reserve(counts.size());
        for (const auto& entry : counts) swatches.push_back(Swatch{ entry.first, entry.second });
        auto measure = [&swatches](Box& box) {
            int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
            for (size_t i = box.begin; i < box.end; i++)
                for (int k = 0; k < 3; k++) {
                    lo[k] = std::min(lo[k], swatches[i].channel(k));
                    hi[k] = std::max(hi[k], swatches[i].channel(k));
                }
            box.range = -1;
            for (int k = 0; k < 3; k++)
                if (hi[k] - lo[k] > box.range) {
                    box.range  = hi[k] - lo[k];
                    box.widest = k;
                }
        };
        std::vector<Box> boxes{ Box{ 0, swatches.size() } };
        measure(boxes[0]);
        while (boxes.size() < MOST) {
            /* the widest box splits at the colour that halves its pixels */
            Box* box = nullptr;
            for (Box& b : boxes)
                if (b.end - b.begin > 1 && (!box || b.range > box->range)) box = &b;
            if (!box || box->range == 0) break;
            const int k = box->widest;
            std::sort(swatches.begin() + box->begin, swatches.begin() + box->end,
                      [k](const Swatch& a, const Swatch& b) { return a.channel(k) < b.channel(k); });
            uint64_t total = 0, half = 0;
            for (size_t i = box->begin; i < box->end; i++) total += swatches[i].count;
            size_t split = box->begin + 1;
            for (size_t i = box->begin; i + 1 < box->end; i++) {
                half += swatches[i].count;
                split = i + 1;
                if (half * 2 >= total) break;
            }
            Box upper{ split, box->end };
            box->end = split;
            measure(*box);
            measure(upper);
            boxes.push_back(upper);
        }
        for (const Box& box : boxes) {
            uint64_t sum[3] = {}, weight = 0;
            for (size_t i = box.begin; i < box.end; i++) {
                for (int k = 0; k < 3; k++) sum[k] += (uint64_t)swatches[i].channel(k) * swatches[i].count;
                weight += swatches[i].count;
                slot_[swatches[i].colour] = (uint8_t)entries_.size();
            }
            uint32_t mean = 0;
            for (int k = 0; k < 3; k++) mean |= (uint32_t)((sum[k] + weight / 2) / weight) << (8 * k);
            entries_.push_back(mean);
        }
    }

    std::vector<uint32_t>                 entries_;   /* 0xBBGGRR */
    std::unordered_map<uint32_t, uint8_t> slot_;
};

class Gif : public Encoder {
public:
    Gif(int width, int height)
    {
        CGIF_Config config{};
        config.attrFlags = CGIF_ATTR_IS_ANIMATED | CGIF_ATTR_NO_GLOBAL_TABLE;
        config.width     = (uint16_t)width;
        config.height    = (uint16_t)height;
        config.numLoops  = CGIF_INFINITE_LOOP;
        config.pWriteFn  = write;
        config.pContext  = &file_;
        gif_ = cgif_newgif(&config);
    }

    ~Gif() override
    {
        if (gif_) cgif_close(gif_);
    }

    bool frame(const Image& frame, double duration) override
    {
        if (!gif_) return false;
        /* Delays round from the running total, so their error never adds up. */
        shown_ += duration;
        const int cs = std::max(2, (int)std::lround(shown_ * 100) - shown_cs_);
        shown_cs_ += cs;
        palette_.take(frame, indices_);
        palette_.rgb(rgb_);
        CGIF_FrameConfig config{};
        config.pLocalPalette          = rgb_.data();
        config.numLocalPaletteEntries = (uint16_t)palette_.size();
        config.pImageData             = indices_.data();
        config.attrFlags              = CGIF_FRAME_ATTR_USE_LOCAL_TABLE;
        config.genFlags               = CGIF_FRAME_GEN_USE_TRANSPARENCY | CGIF_FRAME_GEN_USE_DIFF_WINDOW;
        config.delay                  = (uint16_t)std::min(cs, 0xFFFF);
        return cgif_addframe(gif_, &config) == CGIF_OK;
    }

    std::vector<uint8_t> end() override
    {
        if (!gif_) return {};
        const bool ok = cgif_close(gif_) == CGIF_OK;
        gif_ = nullptr;
        return ok ? std::move(file_) : std::vector<uint8_t>();
    }

private:
    static int write(void* context, const uint8_t* data, const size_t size)
    {
        std::vector<uint8_t>& file = *static_cast<std::vector<uint8_t>*>(context);
        file.insert(file.end(), data, data + size);
        return CGIF_OK;
    }

    CGIF*                gif_ = nullptr;
    Palette              palette_;
    std::vector<uint8_t> indices_, rgb_, file_;
    double               shown_ = 0;
    int                  shown_cs_ = 0;
};

/* H.264 in an MP4, BT.709 at limited range, which every player assumes for a stream that
   states it. A frame comes at the card's size and is doubled while it may be. */
class Mp4 : public Encoder {
public:
    Mp4(int width, int height)
        : scale_(4 * width * height <= MP4_DOUBLED_MOST ? 2 : 1),
          width_(scale_ == 2 ? 2 * width : width & ~1), height_(scale_ == 2 ? 2 * height : height & ~1)
    {
        if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) return;
        SEncParamExt p;
        encoder_->GetDefaultParams(&p);
        p.iUsageType       = SCREEN_CONTENT_REAL_TIME;   /* sharp lines and text over flat fills */
        p.iPicWidth        = width_;
        p.iPicHeight       = height_;
        p.iRCMode          = RC_OFF_MODE;
        p.iTargetBitrate   = std::max(1000000, width_ * height_ * 4);
        p.iMaxBitrate      = UNSPECIFIED_BIT_RATE;
        p.bEnableAdaptiveQuant = false;
        p.fMaxFrameRate    = (float)MP4_RATE;
        p.uiIntraPeriod    = (unsigned)(2 * MP4_RATE);   /* a seek point every two seconds */
        p.bEnableFrameSkip = false;
        p.iMultipleThreadIdc = 1;
        p.iSpatialLayerNum = 1;
        SSpatialLayerConfig& layer = p.sSpatialLayers[0];
        layer.iVideoWidth       = width_;
        layer.iVideoHeight      = height_;
        layer.fFrameRate        = (float)MP4_RATE;
        layer.iDLayerQp         = MP4_QP;
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
        mp4_h26x_write_init(&writer_, mux_, width_, height_, 0);
        yuv_.resize((size_t)width_ * height_ * 3 / 2);
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

    /* BT.709 at limited range in 8 bit fixed point. Chroma takes four times a pixel, so its
       shift is two more, and the offset keeps it positive. */
    void to_i420(const Image& frame, uint8_t* y, uint8_t* u, uint8_t* v) const
    {
        auto luma = [](const uint8_t* p) { return (uint8_t)(16 + ((47 * p[0] + 157 * p[1] + 16 * p[2] + 128) >> 8)); };
        auto cb   = [](int r, int g, int b) { return (uint8_t)((-26 * r - 87 * g + 112 * b + (128 << 10) + 512) >> 10); };
        auto cr   = [](int r, int g, int b) { return (uint8_t)((112 * r - 102 * g - 10 * b + (128 << 10) + 512) >> 10); };
        const int      cw  = width_ / 2;
        const uint8_t* src = frame.rgba.data();
        if (scale_ == 2) {
            for (int row = 0; row < frame.height; row++)
                for (int col = 0; col < frame.width; col++) {
                    const uint8_t* p = src + ((size_t)row * frame.width + col) * 4;
                    const uint8_t  l = luma(p);
                    uint8_t* top = y + (size_t)row * 2 * width_ + col * 2;
                    top[0] = top[1] = top[width_] = top[width_ + 1] = l;
                    u[(size_t)row * cw + col] = cb(4 * p[0], 4 * p[1], 4 * p[2]);
                    v[(size_t)row * cw + col] = cr(4 * p[0], 4 * p[1], 4 * p[2]);
                }
            return;
        }
        for (int row = 0; row < height_; row++)
            for (int col = 0; col < width_; col++) y[(size_t)row * width_ + col] = luma(src + ((size_t)row * frame.width + col) * 4);
        for (int row = 0; row < height_ / 2; row++)
            for (int col = 0; col < cw; col++) {
                int r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    const uint8_t* p = src + ((size_t)(row * 2 + k / 2) * frame.width + col * 2 + k % 2) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
                u[(size_t)row * cw + col] = cb(r, g, b);
                v[(size_t)row * cw + col] = cr(r, g, b);
            }
    }

    int                  scale_, width_, height_;
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
    im.kind   = kind;
    im.width  = width;
    im.height = height;
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
