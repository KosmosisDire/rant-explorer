#include "editor.hpp"

#include <algorithm>
#include <cstdlib>

using ValueNode = Capture::ValueNode;

const std::vector<ValueNode>& Editor::shown()
{
    const ValueView& v = view();
    return v.composing ? v.draft.nodes : capture_.watched().nodes;
}

const ValueNode* Editor::node(const std::string& path)
{
    for (const ValueNode& n : shown())
        if (n.path == path) return &n;
    return nullptr;
}

void Editor::apply(const std::string& path, const std::string& text)
{
    /* Every row holds every editor, hidden but for its own, and a hidden or half built
       dropdown still reports a value. Only a real value of an editable field counts. */
    const ValueNode* n = node(path);
    if (!n) return;
    ValueView& v = view();
    if (!(capture_.watched().writable || v.composing || n->input) || editor_for(*n).empty()) return;
    if (n->kind == ValueNode::Enum && !n->options.empty() &&
        std::find(n->options.begin(), n->options.end(), text) == n->options.end())
        return;
    /* An editor reports the text it was given too, which would round a pasted value. */
    const auto now = v.edits.find(path);
    if (now != v.edits.end() && now->second.text == text) return;
    FieldEdit edit;
    edit.text = text;
    edit.bad  = !parse_value(*n, text, edit.value);
    if (!edit.bad && text == edit_text(*n)) v.edits.erase(path);
    else v.edits[path] = edit;
    error_.clear();
    cells_dirty_ = true;
    /* While repeating an edit goes out at once, not on the next tick. */
    if (v.repeating && !edit.bad) {
        v.next_send = Capture::now_s();
        push();
    }
}

void Editor::reset(const std::string& path)
{
    /* A draft froze an older message, so a field the mesh has moved since takes the newer
       value as an edit that is not the user's. */
    ValueView& v = view();
    v.edits.erase(path);
    cells_dirty_ = true;
    const ValueNode* frozen = v.composing ? node(path) : nullptr;
    if (!frozen) return;
    for (const ValueNode& live : capture_.watched().nodes)
        if (live.path == path && edit_text(live) != edit_text(*frozen)) {
            FieldEdit edit;
            edit.text  = edit_text(live);
            edit.value = live;
            edit.mine  = false;
            v.edits[path] = edit;
        }
}

std::string Editor::text(const std::string& path)
{
    const ValueView& v = view();
    const auto edit = v.edits.find(path);
    if (edit != v.edits.end()) return edit->second.text;
    const ValueNode* n = node(path);
    return n ? edit_text(*n) : std::string();
}

void Editor::nudge(const std::string& path, const std::string& from, int steps, bool big)
{
    const ValueNode* n = node(path);
    if (!n || steps == 0) return;
    const std::string moved = step_value(*n, from, steps, big);
    if (!moved.empty()) apply(path, moved);
}

void Editor::flip(const std::string& path)
{
    const ValueNode* n = node(path);
    if (!n) return;
    const ValueView& v = view();
    const auto edit = v.edits.find(path);
    const bool now = edit != v.edits.end() ? edit->second.value.number != 0 : n->number != 0;
    apply(path, now ? "false" : "true");
}

void Editor::undo()
{
    view().edits.clear();
    cells_dirty_ = true;
}

void Editor::commit()
{
    if (Capture::is_call(capture_.watched().kind)) call();
    else if (!view().repeating) push();
}

void Editor::send()
{
    ValueView& v = view();
    if (!v.repeat) {
        push();
        return;
    }
    v.repeating = true;
    v.next_send = Capture::now_s();
    if (!v.composing) push();
    rows_dirty_ = true;
}

/* A variable's edits are on the mesh by now, so stopping settles them as a Set would. */
void Editor::stop()
{
    ValueView& v = view();
    v.repeating = false;
    if (!v.composing) set_edits();
    rows_dirty_ = true;
}

/* The flag only arms Send or Set, so it is locked while repeating. */
void Editor::toggle_repeat()
{
    ValueView& v = view();
    if (v.repeating) return;
    v.repeat    = !v.repeat;
    rows_dirty_ = true;
}

void Editor::compose(bool on)
{
    ValueView& v = view();
    v.repeating = false;
    v.edits.clear();
    v.sent = 0;
    error_.clear();
    Capture::Draft draft;
    if (on && !capture_.draft(draft)) return;
    v.draft     = std::move(draft);
    v.composing = on;
    capture_.compose(on, v.draft.schema);
    rows_dirty_ = true;
}

bool Editor::can_compose() const
{
    const Capture::WatchView& watch = capture_.watched();
    return !watch.writable && watch.kind == Capture::Kind::Topic && watch.draftable;
}

bool Editor::shown_value(Capture::WatchView& out)
{
    const ValueView& v = view();
    if (!v.composing && v.edits.empty()) return false;
    const Capture::WatchView& watch = capture_.watched();
    out = watch;
    if (v.composing) {
        out.nodes       = v.draft.nodes;
        out.message     = v.draft.message;
        out.root_std    = v.draft.root_std;
        out.root_struct = v.draft.root_struct;
    } else if (!capture_.whole(watch.key).empty()) {
        out.nodes = capture_.whole(watch.key);
    }
    for (ValueNode& n : out.nodes) {
        const auto edit = v.edits.find(n.path);
        if (edit == v.edits.end() || edit->second.bad) continue;
        n.number  = edit->second.value.number;
        n.integer = edit->second.value.integer;
        n.text    = edit->second.value.text;
    }
    return true;
}

/* A draft takes arrays of any length, so it plans twice: once for the lengths, then again
   over the resized draft for the values. */
bool Editor::plan(const std::string& path, const Pasted& value, Pasting& out, std::string& error)
{
    const Capture::WatchView& watch = capture_.watched();
    const ValueView& v = view();
    out = Pasting();
    PastePlan planned;
    if (v.composing || can_compose()) {
        out.start = !v.composing;
        if (v.composing) out.draft = v.draft;
        else if (!capture_.draft(out.draft)) {
            error = "nothing to compose from yet";
            return false;
        }
        Capture::WatchView root;   /* only its root is read */
        root.root_std    = out.draft.root_std;
        root.root_struct = out.draft.root_struct;
        if (!plan_paste(out.draft.nodes, root, path, value, true, planned, error)) return false;
        for (const auto& length : planned.lengths)
            if (!capture_.resize(out.draft, length.first, length.second, error)) return false;
        out.resized = !planned.lengths.empty();
        if (out.resized && !plan_paste(out.draft.nodes, root, path, value, false, planned, error)) return false;
    } else {
        /* the whole value, so a paste reaches past a long array's gap */
        const std::vector<ValueNode>& whole = capture_.whole(watch.key);
        const std::vector<ValueNode>& nodes = whole.empty() ? watch.nodes : whole;
        const int at = find_node(nodes, watch, path);
        if (!watch.writable && !(at >= 0 && nodes[at].input)) {
            error = "only a request or a goal takes a paste";
            return false;
        }
        if (!plan_paste(nodes, watch, path, value, false, planned, error)) return false;
    }
    out.edits = std::move(planned.edits);
    return true;
}

bool Editor::fits(const std::string& path, const std::string& text)
{
    Pasting p;
    std::string error;
    return plan(path, read_pasted(text), p, error);
}

namespace {

/* Whether the field at key lies in the one at path, a part's heading being its prefix and an @. */
bool covers(const std::string& path, const std::string& key)
{
    const std::string at = !path.empty() && path[0] == '@' ? path.substr(1) : path;
    if (at.empty()) return true;
    if (key.compare(0, at.size(), at) != 0) return false;
    return key.size() == at.size() || key[at.size()] == '.' || key[at.size()] == '[';
}

} /* namespace */

/* A paste replaces every edit under its field. A draft is planned again once started, since
   it may freeze a newer message than the one planned on. */
int Editor::paste(const std::string& path, const std::string& text, std::string& error)
{
    const Pasted value = read_pasted(text);
    Pasting p;
    if (!plan(path, value, p, error)) return -1;
    if (p.start) {
        compose(true);
        if (!plan(path, value, p, error)) return -1;
    }
    ValueView& v = view();
    if (p.resized) {
        v.draft     = std::move(p.draft);
        rows_dirty_ = true;
    }
    for (auto it = v.edits.begin(); it != v.edits.end();) it = covers(path, it->first) ? v.edits.erase(it) : std::next(it);
    for (const FieldEdit& edit : p.edits) v.edits[edit.value.path] = edit;
    error_.clear();
    cells_dirty_ = true;
    if (v.repeating) {
        v.next_send = Capture::now_s();
        push();
    }
    return (int)p.edits.size();
}

namespace {

/* Where a key under element k of array goes when k is removed: -1 gone with it, 0 kept as
   it is, 1 moved to to, one element up. j is the element the key was under. */
int shift(const std::string& key, const std::string& array, int k, std::string& to, int& j)
{
    to = key;
    const std::string head = array + "[";
    const size_t close = key.find(']', head.size());
    if (key.compare(0, head.size(), head) != 0 || close == std::string::npos) return 0;
    j = std::atoi(key.c_str() + head.size());
    if (j == k) return -1;
    if (j < k) return 0;
    to = head + std::to_string(j - 1) + key.substr(close);
    return 1;
}

} /* namespace */

void Editor::add_element(const std::string& array)
{
    const ValueNode* n = node(array);
    if (!n || !view().composing) return;
    Capture::Draft draft = view().draft;
    std::string error;
    take_draft(capture_.resize(draft, n->field, n->count + 1, error), draft, error);
}

void Editor::remove_element(const std::string& path)
{
    const ValueNode* n = node(path);
    const size_t open = path.rfind('[');
    if (!n || !view().composing || open == std::string::npos) return;
    const std::string array = path.substr(0, open);
    const ValueNode* a = node(array);
    if (!a) return;
    const int    k     = std::atoi(path.c_str() + open + 1);
    const size_t level = a->elems.size();   /* where a struct array's index sits in a member's elems */
    Capture::Draft draft = view().draft;
    std::string error;
    if (!take_draft(capture_.remove(draft, n->field, (uint32_t)k, error), draft, error)) return;

    ValueView& v = view();
    std::map<std::string, FieldEdit> edits;
    for (auto& entry : v.edits) {
        std::string to;
        int j = 0;
        const int moved = shift(entry.first, array, k, to, j);
        if (moved < 0) continue;
        FieldEdit edit = entry.second;
        if (moved > 0) {
            edit.value.path = to;
            if (to == array + "[" + std::to_string(j - 1) + "]") {
                edit.value.name = "[" + std::to_string(j - 1) + "]";
                if (edit.value.element >= 0) edit.value.element = j - 1;
            }
            if (edit.value.elems.size() > level) edit.value.elems[level] = (uint32_t)(j - 1);
        }
        edits[to] = edit;
    }
    v.edits.swap(edits);
    std::map<std::string, bool> folds;
    for (const auto& entry : v.open) {
        std::string to;
        int j = 0;
        if (shift(entry.first, array, k, to, j) >= 0) folds[to] = entry.second;
    }
    v.open.swap(folds);
}

bool Editor::take_draft(bool ok, Capture::Draft& draft, const std::string& error)
{
    cells_dirty_ = true;
    error_       = ok ? std::string() : error;
    if (!ok) return false;
    view().draft = std::move(draft);
    rows_dirty_  = true;
    return true;
}

void Editor::end_draft()
{
    ValueView& v = view();
    v.composing = false;
    v.repeating = false;
    v.edits.clear();
}

/* The default request with every edit in it. The edits stay, so the next call sends the
   same form. */
void Editor::call()
{
    const Capture::WatchView& watch = capture_.watched();
    std::vector<ValueNode> nodes;
    if (watch.calling || !edited(nodes)) return;
    std::string error;
    const std::vector<uint8_t> bytes = watch.request_schema
                                     ? capture_.encode(watch.request, watch.request_schema, nodes, error)
                                     : std::vector<uint8_t>();
    if (error.empty()) error = capture_.call(bytes);
    error_       = error;
    cells_dirty_ = true;
}

void Editor::cancel()
{
    error_       = capture_.cancel();
    cells_dirty_ = true;
}

/* A topic repeats on the clock, and falls back to now rather than bursting to catch up. A
   variable has no clock: it is written only as it is edited. */
void Editor::repeat()
{
    const ValueView& v = view();
    if (!v.repeating || !v.composing || Capture::now_s() < v.next_send) return;
    push();
    if (!v.repeating) rows_dirty_ = true;
}

void Editor::settle(bool new_message)
{
    ValueView& v = view();
    const bool late = Capture::now_s() - v.writing_since > 3;
    if (v.writing.empty() || !(new_message || late)) return;
    for (auto it = v.writing.begin(); it != v.writing.end();) {
        const ValueNode* n = node(it->first);
        /* compared as values, so 2.5 written is 2.500 echoed */
        ValueNode wrote;
        const bool echoed = n && parse_value(*n, it->second, wrote) && edit_text(wrote) == edit_text(*n);
        if (!n || echoed || late) it = v.writing.erase(it);
        else ++it;
    }
    cells_dirty_ = true;
}

bool Editor::edited(std::vector<ValueNode>& out)
{
    out.clear();
    for (const auto& entry : view().edits) {
        if (entry.second.bad) return false;
        out.push_back(entry.second.value);
    }
    return true;
}

/* One write of the watch as it is edited. A repeat restarts its clock from here, so a
   live edit is not sent twice. */
void Editor::push()
{
    ValueView& v = view();
    if (v.composing) send_draft();
    else set_edits();
    const double hz = std::min(1000.0, std::max(0.1, std::atof(rate.c_str())));
    v.next_send     = std::max(v.next_send + 1.0 / hz, Capture::now_s());
}

/* The frozen message with every edit in it, encoded fresh each time, so an edit made while
   repeating goes out on the next send. */
void Editor::send_draft()
{
    ValueView& v = view();
    std::vector<ValueNode> nodes;
    if (!v.composing || !edited(nodes)) return;
    std::string error;
    const std::vector<uint8_t> bytes = capture_.encode(v.draft.message, v.draft.schema, nodes, error);
    if (error.empty()) error = capture_.publish(bytes);
    error_ = error;
    if (error.empty()) v.sent++;
    else v.repeating = false;
    cells_dirty_ = true;
}

/* The owner's newest value with every edit in it. While repeating the edits stay for the
   next write, else they wait outlined for the echo. */
void Editor::set_edits()
{
    ValueView& v = view();
    std::vector<ValueNode> nodes;
    if ((v.edits.empty() && !v.repeating) || !edited(nodes)) return;
    std::string error;
    const Capture::WatchView& watch = capture_.watched();
    const std::vector<uint8_t> bytes = capture_.encode(watch.message, watch.schema, nodes, error);
    if (error.empty()) error = capture_.set(bytes);
    error_ = error;
    if (!error.empty()) {
        v.repeating = false;
    } else if (!v.repeating) {
        for (const auto& entry : v.edits) v.writing[entry.first] = entry.second.text;
        v.writing_since = Capture::now_s();
        v.edits.clear();
    }
    cells_dirty_ = true;
}
