/* The <mesh-graph> element: every node on the mesh as a box, and an arrow from a node to
   each node that consumes what it provides, all names between one pair folded into one
   arrow. Dots run along an arrow while traffic flows, faster and closer as it grows. The
   layout, the view and the selection live here rather than in the element, so a reload
   keeps them. */
#ifndef MESH_HPP
#define MESH_HPP

#include "capture.hpp"

#include <string>
#include <vector>

/* Registers the element. The capture outlives every document. */
void mesh_init(Capture& capture);

/* The funnel's checkboxes, which restrict like the topic tree's: a kind box keeps that
   kind, and with none checked every kind stays. */
enum MeshCategory : unsigned {
    MESH_TOPIC     = 1u << 0,
    MESH_FUNCTION  = 1u << 1,
    MESH_VARIABLE  = 1u << 2,
    MESH_TASK      = 1u << 3,
    MESH_ACTIVE    = 1u << 4,   /* only links with traffic */
    MESH_CONNECTED = 1u << 5,   /* no node without a link */
    MESH_NO_LEAVES = 1u << 6,   /* no node with one neighbour only */
    MESH_NO_SYSTEM = 1u << 7,   /* no link that is only a system name */
};
constexpr unsigned MESH_KINDS = MESH_TOPIC | MESH_FUNCTION | MESH_VARIABLE | MESH_TASK;
/* What the funnel starts with, and what resetting it puts back. */
constexpr unsigned MESH_DEFAULT = MESH_NO_SYSTEM;

struct MeshFilter {
    std::string text;            /* a node's name, or a name one of its links carries */
    unsigned    cats = MESH_DEFAULT;
    int         hops = 1;        /* how far around the selected node stays lit, 0 for all */
    bool        focus = false;   /* past the hops is hidden rather than dimmed */

    bool operator==(const MeshFilter& o) const
    {
        return text == o.text && cats == o.cats && hops == o.hops && focus == o.focus;
    }
};
void mesh_set_filter(const MeshFilter& filter);

/* The selection by a node's key, its name and host, which a restart keeps: a node, or the
   pair of nodes an arrow joins, or neither. */
struct MeshPick {
    std::string node, from, to;

    bool operator==(const MeshPick& o) const { return node == o.node && from == o.from && to == o.to; }
    bool operator!=(const MeshPick& o) const { return !(*this == o); }
};
const MeshPick& mesh_pick();
void mesh_select(const MeshPick& pick);

/* Frames every shown node, and keeps framing them until the view is moved by hand. */
void mesh_fit();
/* Lets go every node the user placed and runs the layout again. */
void mesh_relayout();

/* One line of the sidebar: a name, the nodes it runs from and to where the context does
   not already say, and its traffic. */
struct MeshRow {
    std::string   name;
    Capture::Kind kind = Capture::Kind::Topic;
    std::string   from, to;
    std::string   rate;
    bool          active = false;
    bool          lost = false;   /* sent and not received, the rate is what was sent */

    bool operator==(const MeshRow& o) const
    {
        return name == o.name && kind == o.kind && from == o.from && to == o.to && rate == o.rate &&
               active == o.active && lost == o.lost;
    }
};

/* A titled list of rows, left out while it has none. */
struct MeshGroup {
    std::string          title;
    std::vector<MeshRow> rows;

    bool operator==(const MeshGroup& o) const { return title == o.title && rows == o.rows; }
};

/* What the sidebar shows for the selection. With nothing selected, the mesh in numbers,
   its busiest links and what waits for a provider. For a node, what it sends and receives
   by name and what has nobody on the other side. For a pair, both ways. */
struct MeshDetails {
    int         mode = 0;   /* 0 the mesh, 1 a node, 2 a pair */
    std::string title, from, to, host;
    bool        alive = false, reports = false;
    std::string nodes, links, active, traffic;
    std::vector<MeshGroup> groups;

    bool operator==(const MeshDetails& o) const
    {
        return mode == o.mode && title == o.title && from == o.from && to == o.to && host == o.host &&
               alive == o.alive && reports == o.reports && nodes == o.nodes && links == o.links &&
               active == o.active && traffic == o.traffic && groups == o.groups;
    }
};
MeshDetails mesh_details();

#endif /* MESH_HPP */
