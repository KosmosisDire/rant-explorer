/* What the user does to each topic's values, kept while the explorer runs: folds, shown
   visuals, pins and edits. The watched topic is the one edited, and its edits go back to
   the mesh as a variable's set or force, a composed topic's send or a function's or task's call. */
#ifndef EDITOR_HPP
#define EDITOR_HPP

#include "capture.hpp"
#include "values.hpp"

#include <map>
#include <string>
#include <vector>

class Editor {
public:
    explicit Editor(Capture& capture) : capture_(capture) {}

    /* The watched topic's view, and any topic's by its Capture::key. */
    ValueView& view() { return views_[capture_.watched().key]; }
    ValueView& view(const std::string& key) { return views_[key]; }
    const std::map<std::string, ValueView>& views() const { return views_; }

    /* The nodes the tree shows: a composed topic's frozen draft, else the newest value. */
    const std::vector<Capture::ValueNode>& shown();
    /* The shown node at path, null for none. */
    const Capture::ValueNode* node(const std::string& path);

    /* Every editor lands here. An edit is kept only while it differs from the shown value,
       so typing a value back to what it was clears it. */
    void apply(const std::string& path, const std::string& text);
    /* Back to the mesh's value as it is now. */
    void reset(const std::string& path);
    /* What a field's editor holds: the typed text, else the shown value. */
    std::string text(const std::string& path);
    /* A number moved by whole steps from a text, applied like any edit. */
    void nudge(const std::string& path, const std::string& from, int steps, bool big);
    void flip(const std::string& path);
    void undo();

    /* What Enter and the head's main button do: Set, Send, or Force while the variable is
       forced. Nothing while live, since the edits are out already. */
    void commit();
    /* On writes what is edited at once. Off ends a topic's sends and settles a variable's edits. */
    void toggle_live();
    /* Forces the shown value with every edit in it, and releases a force, whoever made it. */
    void force();
    void release();
    /* A topic's draft: frozen from the newest message, or the schema's default before one. */
    void compose(bool on);
    bool can_compose() const;
    /* The watched value as the tree shows it, every element and every edit in it, for copying.
       False when that is the newest value as it came. */
    bool shown_value(Capture::WatchView& out);
    /* Whether pasting text over the field at path works, and pasting it, which starts a draft
       first where one can start. The fields it changed, or -1 with the reason in error. */
    bool fits(const std::string& path, const std::string& text);
    int  paste(const std::string& path, const std::string& text, std::string& error);
    /* A draft's variable array grows by one element at its end, or loses the element at
       path. Edits and folds past a removed element move up with it. */
    void add_element(const std::string& array);
    void remove_element(const std::string& path);
    /* A draft and live belong to the watch, so leaving the topic ends them. */
    void end_draft();
    void call();
    void cancel();

    /* Once a frame, a live composed topic sends on its clock. */
    void repeat();
    /* Once a frame, a written field stops waiting once the owner's value carries it, or
       after a few seconds so a refused write does not hang there. */
    void settle(bool new_message);

    /* A live composed topic's rate as typed, which the document binds. */
    std::string rate = "10";
    /* Why the last write, send or call failed, until the next edit. */
    const std::string& error() const { return error_; }

    /* Whether the rows or only the value cells must be rebuilt since the last ask. */
    bool take_rows() { return take(rows_dirty_); }
    bool take_cells() { return take(cells_dirty_); }

private:
    /* What a paste does: the draft it starts or resizes, and its edits. */
    struct Pasting {
        bool                   start = false, resized = false;
        Capture::Draft         draft;
        std::vector<FieldEdit> edits;
    };
    bool plan(const std::string& path, const Pasted& value, Pasting& out, std::string& error);
    bool edited(std::vector<Capture::ValueNode>& out);
    void push();
    void send_draft();
    void write(bool force);
    bool take_draft(bool ok, Capture::Draft& draft, const std::string& error);
    static bool take(bool& flag)
    {
        const bool was = flag;
        flag = false;
        return was;
    }

    Capture&                         capture_;
    std::map<std::string, ValueView> views_;
    std::string                      error_;
    bool                             rows_dirty_ = false, cells_dirty_ = false;
};

#endif /* EDITOR_HPP */
