/* The <icon name="chevron-right"/> element: one Lucide glyph out of the icon font, so it
   takes the colour of its text and its font-size is the icon's size. */
#ifndef ICONS_HPP
#define ICONS_HPP

/* Load the asset lucide.ttf as the "lucide" family and its codepoint table, then register
   the element. False when either file failed, the element still exists. */
bool icons_init();

#endif /* ICONS_HPP */
