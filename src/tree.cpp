#include "tree.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

std::string tree_indent(int depth)
{
    return std::to_string(depth * 15) + "dp";
}

namespace {

bool contains_ci(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    const auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
                                });
    return it != hay.end();
}

std::vector<std::string> split_segments(const std::string& name)
{
    std::vector<std::string> out;
    std::string segment;
    for (const char c : name) {
        if (c == '/' || c == '.') {
            if (!segment.empty()) out.push_back(segment);
            segment.clear();
        } else {
            segment += c;
        }
    }
    if (!segment.empty()) out.push_back(segment);
    return out;
}

struct Node {
    std::string      name, path;
    int              topic = -1;
    std::vector<int> children;   /* in the order they were made, which is sorted */
};

/* One topic split and ready to sort. Sorting by segments, not by the raw name, keeps every
   subtree contiguous, so a segment is always its parent's newest child when it repeats. */
struct Split {
    std::vector<std::string> segments;
    int                      topic;
};

void flatten(const std::vector<Node>& nodes, int at, int depth, const std::set<std::string>& expanded,
             bool filtering, std::vector<TreeRow>& out)
{
    for (const int child : nodes[at].children) {
        const Node& node = nodes[child];
        TreeRow row;
        row.name      = node.name;
        row.path      = node.path;
        row.indent    = tree_indent(depth);
        row.depth     = depth;
        row.branch    = !node.children.empty();
        row.open      = row.branch && (filtering || expanded.count(node.path));
        row.has_topic = node.topic >= 0;
        row.topic     = node.topic;
        out.push_back(row);
        if (row.open) flatten(nodes, child, depth + 1, expanded, filtering, out);
    }
}

} /* namespace */

bool topic_passes(const Capture& capture, const Capture::TopicRow& topic, const std::string& text,
                  unsigned cats)
{
    if (cats & CAT_KINDS) {
        const unsigned bit = topic.kind == Capture::Kind::Function ? CAT_FUNCTION
                           : topic.kind == Capture::Kind::Variable ? CAT_VARIABLE
                           : topic.kind == Capture::Kind::Task     ? CAT_TASK
                           :                                         CAT_TOPIC;
        if (!(cats & bit)) return false;
    }
    if ((cats & CAT_QOS) && !(cats & (topic.reliable ? CAT_RELIABLE : CAT_BEST_EFFORT))) return false;
    if ((cats & CAT_SUBSCRIBED) && !capture.subscribed(topic.name) && !capture.previewing(topic.name))
        return false;
    if (cats & CAT_ACTIVE) {
        const Capture::WatchView* view = capture.view(topic.name);
        if (!view || !(view->rate_hz > 0)) return false;
    }
    if (text.empty() || contains_ci(topic.name, text) || contains_ci(topic.type, text)) return true;
    for (const std::string& node : topic.nodes)
        if (contains_ci(node, text)) return true;
    return false;
}

std::string tree_path(const std::string& name)
{
    std::string out;
    for (const std::string& segment : split_segments(name)) out += (out.empty() ? "" : "/") + segment;
    return out;
}

const char* kind_icon(const std::string& word)
{
    using Kind = Capture::Kind;
    if (word == Capture::kind_word(Kind::Function)) return "square-function";
    if (word == Capture::kind_word(Kind::Variable)) return "variable";
    if (word == Capture::kind_word(Kind::Task))     return "list-clock";
    return "rss";
}

std::vector<TreeRow> build_tree(const Capture& capture, const std::set<std::string>& expanded,
                                const std::string& text, unsigned cats, int& matched)
{
    const std::vector<Capture::TopicRow>& topics = capture.topics();
    std::vector<Split> splits;
    for (size_t i = 0; i < topics.size(); i++) {
        if (!topic_passes(capture, topics[i], text, cats)) continue;
        Split split{ split_segments(topics[i].name), (int)i };
        if (!split.segments.empty()) splits.push_back(std::move(split));
    }
    matched = (int)splits.size();
    std::sort(splits.begin(), splits.end(),
              [](const Split& a, const Split& b) { return a.segments < b.segments; });

    std::vector<Node> nodes(1);   /* node 0 is the root */
    for (const Split& split : splits) {
        int at = 0;
        for (const std::string& segment : split.segments) {
            const std::vector<int>& siblings = nodes[at].children;
            if (!siblings.empty() && nodes[siblings.back()].name == segment) {
                at = siblings.back();
                continue;
            }
            Node node;
            node.name = segment;
            node.path = at == 0 ? segment : nodes[at].path + "/" + segment;
            nodes.push_back(std::move(node));
            nodes[at].children.push_back((int)nodes.size() - 1);
            at = (int)nodes.size() - 1;
        }
        nodes[at].topic = split.topic;
    }

    std::vector<TreeRow> rows;
    flatten(nodes, 0, 0, expanded, !text.empty() || cats, rows);
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].has_topic) {
            rows[i].kind       = topics[rows[i].topic].kind;
            rows[i].topic_name = topics[rows[i].topic].name;
        }
        rows[i].odd = i % 2 == 1;
    }
    return rows;
}
