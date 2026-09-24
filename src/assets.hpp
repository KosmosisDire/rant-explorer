/* The RML, RCSS, fonts and images of the UI, by file name. They come from a directory on
   disk while one is found, else from the copy the build embedded in the exe. */
#ifndef ASSETS_HPP
#define ASSETS_HPP

#include <RmlUi/Core/FileInterface.h>

#include <string>

/* Pick the source, in this order:

     RANT_UI_ASSETS        an explicit override
     the source tree       the assets/ this exe was built from, when it is still there, so
                           editing one reloads in the running window with nothing to set up
     the exe               the copy embedded at build time, which is what a release has */
void assets_init();

/* The directory the assets come from, with a trailing slash. Empty when embedded. */
const std::string& assets_dir();

/* The whole file. A relative name is an asset, an absolute path is read from disk. */
bool asset_read(const std::string& name, std::string& out);

/* Serves RmlUi every file through asset_read. */
Rml::FileInterface& assets_file_interface();

#endif /* ASSETS_HPP */
