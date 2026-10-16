/* A VideoFrame stream decoded to pictures off the UI thread, or as pushed in a page without
   threads. MJPEG, H.264, H.265 and AV1, and a codec left unstated is told by its bitstream. */
#ifndef VIDEO_HPP
#define VIDEO_HPP

#include "capture.hpp"
#include "image.hpp"

#include <memory>
#include <string>
#include <vector>

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /* Frames in stream order. After a loss decoding starts over at the next keyframe. */
    void push(std::vector<Capture::Packet> packets, bool lost);

    /* The newest picture when it is newer than seq, which then moves up to it. */
    bool picture(Image& out, uint64_t& seq);

    /* What the stream is, for the card's foot: the codec, the picture's size, and why no
       picture shows, empty while one does. */
    struct Info {
        std::string codec;
        int         width = 0, height = 0;
        std::string problem;
    };
    Info info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif /* VIDEO_HPP */
