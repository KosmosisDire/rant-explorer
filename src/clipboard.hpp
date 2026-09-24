/* What goes on the system clipboard in more than one format at once, so each app pastes
   the one it reads. Plain text goes through RmlUi's system interface and needs nothing here. */
#ifndef CLIPBOARD_HPP
#define CLIPBOARD_HPP

#include "image.hpp"
#include "values.hpp"

struct SDL_Window;

/* A picture as PNG, which keeps transparency and is what Linux apps read, and as a bitmap,
   which every Windows app reads. False when the clipboard refused it. */
bool clipboard_set_image(SDL_Window* window, const Image& image);

/* A table as tab separated text, which a spreadsheet pastes as cells, and as an HTML table,
   which Word, PowerPoint and note apps paste as a table. */
bool clipboard_set_table(SDL_Window* window, const Table& table);

#endif /* CLIPBOARD_HPP */
