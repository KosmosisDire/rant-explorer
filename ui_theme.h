/* Design tokens: the two color palettes, as Clay_Color (rgba floats 0-255).
   Values mirror the HTML mockup's CSS variables. Swap the active palette to
   toggle theme. Requires clay.h included first (for Clay_Color). */
#ifndef UI_THEME_H
#define UI_THEME_H

typedef struct {
    Clay_Color bg, panel, panel2, panel3, border, border2;
    Clay_Color text, dim, faint;
    Clay_Color accent, accent_bg;
    Clay_Color green, amber, red, gray;
    Clay_Color green_bg, amber_bg, gray_bg, red_bg;
    Clay_Color code_bg;
} Palette;

/* the "_bg" tints keep real alpha: they are blended over the panel at draw time. */
static const Palette UI_DARK = {
    .bg={20,20,22,255}, .panel={27,27,30,255}, .panel2={33,33,37,255},
    .panel3={40,40,45,255}, .border={42,42,47,255}, .border2={56,56,64,255},
    .text={237,237,238,255}, .dim={154,154,160,255}, .faint={100,100,107,255},
    .accent={77,182,172,255}, .accent_bg={77,182,172,38},
    .green={70,174,100,255}, .amber={207,154,58,255}, .red={210,87,75,255}, .gray={116,116,124,255},
    .green_bg={70,174,100,36}, .amber_bg={207,154,58,38}, .gray_bg={116,116,124,46}, .red_bg={210,87,75,41},
    .code_bg={19,19,21,255},
};

static const Palette UI_LIGHT = {
    .bg={244,244,245,255}, .panel={255,255,255,255}, .panel2={244,244,245,255},
    .panel3={236,236,238,255}, .border={230,230,232,255}, .border2={214,214,218,255},
    .text={25,25,27,255}, .dim={94,94,100,255}, .faint={154,154,160,255},
    .accent={46,139,130,255}, .accent_bg={46,139,130,26},
    .green={47,157,87,255}, .amber={169,121,31,255}, .red={207,59,45,255}, .gray={154,154,160,255},
    .green_bg={47,157,87,28}, .amber_bg={169,121,31,31}, .gray_bg={154,154,160,36}, .red_bg={207,59,45,26},
    .code_bg={244,244,245,255},
};

/* a fully transparent color, for "no fill" / "no border" slots */
static const Clay_Color UI_NONE = {0, 0, 0, 0};

#endif /* UI_THEME_H */
