#include "editor.hpp"

#include <algorithm>
#include <cstdlib>

using ValueNode = Capture::ValueNode;

const std::vector<ValueNode>& Editor::shown()
{
    const ValueView& v = view();
    return v.composing ? v.frozen : capture_.watched().nodes;
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
    if (on) {
        if (!capture_.draft(draft)) return;
        v.frozen         = draft.nodes;
        v.frozen_message = draft.message;
        v.frozen_schema  = draft.schema;
    }
    v.composing = on;
    capture_.compose(on, draft.schema);
    rows_dirty_ = true;
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
    const std::vector<uint8_t> bytes = capture_.encode(v.frozen_message, v.frozen_schema, nodes, error);
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
