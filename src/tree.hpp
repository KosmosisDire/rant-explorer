/* The topic tree: every name on the mesh split on '/' or '.' into a segment tree, then
   flattened into the rows the document binds, honouring the expanded set. A segment can
   be both a branch and a topic, and a name offered as two kinds is two rows. */
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
    std::string path;        /* the segments joined by '/', the key for expand */
    std::string indent;      /* tree_indent of its depth */
    Capture::Kind kind = Capture::Kind::Topic;   /* the topic's, unset for a namespace */
    std::string topic_name;  /* the name on the mesh, empty for a namespace */
    std::string key;         /* the topic's Capture::key, the key for select */
    int         depth = 0;
    bool        branch = false;      /* has children */
    bool        open = false;        /* a branch showing them */
    bool        has_topic = false;   /* a name on the mesh, not only a namespace */
    bool        odd = false;         /* every other row is striped */
    bool        shared = false;      /* its name is offered as another kind too */
    int         topic = -1;          /* index into the topics the tree was built from */
};

/* hay holds needle, case blind. An empty needle is in everything. */
bool contains_ci(const std::string& hay, const std::string& needle);

/* A name under a namespace the libraries keep for themselves, such as @rant. */
bool is_system(const std::string& name);

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
    CAT_ACTIVE      = 1u << 7,   /* messages arrive here or the nodes report traffic */
    CAT_NO_SYSTEM   = 1u << 8,   /* no system name */
};
constexpr unsigned CAT_KINDS = CAT_TOPIC | CAT_FUNCTION | CAT_VARIABLE | CAT_TASK;
/* What the funnel starts with, and what resetting it puts back. */
constexpr unsigned CAT_DEFAULT = CAT_NO_SYSTEM;
/* The boxes that make a search, which opens the branches holding its matches. Hiding
   system names is where the tree starts, not a search. */
constexpr unsigned CAT_SEARCH = ~CAT_NO_SYSTEM;
constexpr unsigned CAT_QOS   = CAT_RELIABLE | CAT_BEST_EFFORT;

/* A topic passes the text when its name, its type or one of its nodes' names contains
   it, case blind, and passes cats as the enum says. */
bool topic_passes(const Capture& capture, const Capture::TopicRow& topic, const std::string& text,
                  unsigned cats);

/* A name on the mesh as a tree path: its segments joined by '/'. */
std::string tree_path(const std::string& name);

/* The tree path of every branch above a topic that passes. */
std::set<std::string> matched_branches(const Capture& capture, const std::string& text, unsigned cats);

/* The rows in tree order, of the topics that pass, through the expanded branches.
   matched counts the topics kept. */
std::vector<TreeRow> build_tree(const Capture& capture, const std::set<std::string>& expanded,
                                const std::string& text, unsigned cats, int& matched);

#endif /* TREE_HPP */
