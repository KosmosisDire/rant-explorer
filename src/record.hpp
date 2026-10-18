/* A card saved as a picture or recorded as a video or a GIF, and the file handed to the user:
   a save dialog on a desktop, a download in a page. A recording encodes off the UI thread,
   or as frames are pushed in a page without threads. */
#ifndef RECORD_HPP
#define RECORD_HPP

#include "image.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

enum class FileKind { Png, Mp4, Gif };

/* The extension, without its dot. */
const char* file_extension(FileKind kind);

class Recording {
public:
    /* Every frame is fitted to the first one's size. */
    Recording(FileKind kind, int width, int height);
    ~Recording();

    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

    /* Whether a frame read at seconds would be taken: the format's pace, and none while the
       encoder is behind. */
    bool due(double seconds) const;
    /* A frame shown from seconds on, which the next one ends. */
    void push(Image frame, double seconds);
    /* The file, its last frame shown until seconds. Empty when the encoder failed. */
    std::vector<uint8_t> finish(double seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* The picture of the window's pixels in a rectangle, top row first and opaque. The frame must
   be drawn and not yet presented. */
Image read_window(int x, int y, int width, int height, int window_height);

/* Hands the file to the user under a suggested name. A desktop asks where through a dialog
   that answers later, which save_note reports. */
void save_file(SDL_Window* window, std::vector<uint8_t> bytes, FileKind kind, const std::string& name);

/* What became of the latest save, once, for the status bar. False while there is no news. */
bool save_note(std::string& note);

#endif /* RECORD_HPP */
