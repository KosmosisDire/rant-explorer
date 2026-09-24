/* The topic tree: every name on the mesh split on '/' or '.' into a segment tree, then
   flattened into the rows the document binds, honouring the expanded set. A segment can
   be both a branch and a topic. */
#ifndef TREE_HPP
#define TREE_HPP

#include "capture.hpp"

#include <set>
#include <string>
#include <vector>

/* The room before a tree table row's caret at a depth, as an RCSS length. */
std::string tree_indent(int depth);

struct TreeRow {
    std::string name;        /* the last segment */
    std::string path;        /* the segments joined by '/', the key for expand and select */
    std::string indent;      /* tree_indent of its depth */
    Capture::Kind kind = Capture::Kind::Topic;   /* the topic's, unset for a namespace */
    std::string topic_name;  /* the name on the mesh, empty for a namespace */
    int         depth = 0;
    bool        branch = false;      /* has children */
    bool        open = false;        /* a branch showing them */
    bool        has_topic = false;   /* a name on the mesh, not only a namespace */
    bool        odd = false;         /* every other row is striped */
    int         topic = -1;          /* index into the topics the tree was built from */
};

/* The Lucide glyph for a kind, by the word the document holds it as. */
const char* kind_icon(const std::string& word);

/* The funnel's checkboxes. Within a group any checked box passes, every group must pass,
   and a group with nothing checked passes all. The two state boxes are each their own. */
enum TreeCategory : unsigned {
    CAT_TOPIC       = 1u << 0,
    CAT_FUNCTION    = 1u << 1,
    CAT_VARIABLE    = 1u << 2,
    CAT_TASK        = 1u << 3,
    CAT_RELIABLE    = 1u << 4,
    CAT_BEST_EFFORT = 1u << 5,
    CAT_SUBSCRIBED  = 1u << 6,   /* this explorer holds a subscription, kept or a preview */
    CAT_ACTIVE      = 1u << 7,   /* messages are arriving, so only seen while subscribed */
};
constexpr unsigned CAT_KINDS = CAT_TOPIC | CAT_FUNCTION | CAT_VARIABLE | CAT_TASK;
constexpr unsigned CAT_QOS   = CAT_RELIABLE | CAT_BEST_EFFORT;

/* A topic passes the text when its name, its type or one of its nodes' names contains
   it, case blind, and passes cats as the enum says. */
bool topic_passes(const Capture& capture, const Capture::TopicRow& topic, const std::string& text,
                  unsigned cats);

/* A name on the mesh as a tree path: its segments joined by '/'. */
std::string tree_path(const std::string& name);

/* The rows in tree order, of the topics that pass. While anything filters, every branch
   opens so a match is never hidden. matched counts the topics kept. */
std::vector<TreeRow> build_tree(const Capture& capture, const std::set<std::string>& expanded,
                                const std::string& text, unsigned cats, int& matched);

#endif /* TREE_HPP */
