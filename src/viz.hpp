/* The <viz path="..."/> element: one field's visual inside a card. It reads the watched
   value and its trace from the capture every frame, so no value passes through the data
   model. Text visuals are child elements written in place, drawn ones use RmlUi geometry. */
#ifndef VIZ_HPP
#define VIZ_HPP

#include "capture.hpp"
#include "image.hpp"
#include "tiling.hpp"
#include "values.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Rml { class Element; }

/* Registers the element. The capture outlives every document. */
void viz_init(Capture& capture);

/* What the shown visuals of one topic need from the capture: the scalar fields they draw
   over time, a plot's value or a vector's or geo point's components for its trail, and the
   VideoFrame fields they decode, whose every frame the capture queues. */
struct VizFeeds {
    std::vector<Capture::TraceSpec> traces;
    std::vector<std::string>        streams;
};
/* A path whose card hides its history shows only the newest value, so it is not traced. */
VizFeeds viz_feeds(const Capture::WatchView& watch, const std::set<std::string>& shown,
                   const std::map<std::string, ShowChoices>& shows);

/* A part of a card's visual the card can show or hide: what came before, the labels, the
   grid. Each visual offers the few it draws, and each part has its own default. */
struct VizShow {
    std::string key;     /* what the card's choice is kept under */
    std::string label;   /* its menu line */
    bool        on = true;
};
/* What a card's visual offers now, in menu order. None for any other element. */
std::vector<VizShow> viz_shows(Rml::Element* element);

/* A card's choices as its viz element's shows attribute. */
std::string viz_shows_text(const ShowChoices& choices);

/* What a card's visual wants from the tiling. The default for any other element. */
Tile viz_tile(Rml::Element* element);

/* The picture a video card shows now. Invalid for any other element. */
Image viz_picture(Rml::Element* element);

#endif /* VIZ_HPP */
