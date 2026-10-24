/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_lua_window.h"
#include "gtk_compat.h"
#include "gtk_s9x.h"
#include "gtk_s9xwindow.h"
#include "gtk_sound.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unistd.h>

#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "movie.h"
#include "lua/lua_engine.h"
#ifdef RETROACHIEVEMENTS_SUPPORT
#include "retroachievements.h"
#endif

namespace fs = std::filesystem;

namespace {

const int indent_width = 2; // the LuaRocks style guide's two spaces

bool dark_theme(Gtk::Widget &widget)
{
    auto fg = widget.get_style_context()->get_color(Gtk::STATE_FLAG_NORMAL);
    return (fg.get_red() + fg.get_green() + fg.get_blue()) / 3.0 > 0.5;
}

std::string read_file(const std::string &path, bool &ok)
{
    std::ifstream file(path, std::ios::binary);
    ok = (bool)file;
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string leading_space(const std::string &line)
{
    size_t n = line.find_first_not_of(" \t");
    return line.substr(0, n == std::string::npos ? line.size() : n);
}

bool opens_block(std::string code)
{
    while (!code.empty() && isspace((unsigned char)code.back()))
        code.pop_back();
    size_t start = code.find_first_not_of(" \t");
    if (start == std::string::npos || code.compare(start, 2, "--") == 0)
        return false;
    static const std::regex opener("(\\bthen|\\bdo|\\belse|\\brepeat|[{(]|\\bfunction\\b[^)]*\\))$");
    return std::regex_search(code, opener);
}

/* ---- Editor ------------------------------------------------------------- */

class LuaEditor : public Gtk::TextView
{
  public:
    // A breakpoint rides a mark at its line's start, which follows the text as it's edited.
    struct Breakpoint
    {
        Glib::RefPtr<Gtk::TextMark> anchor;
        bool enabled = true;
        std::string condition, hit, log;
    };

    LuaEditor();
    void set_error_line(int line); // 0 clears it
    void set_debug_lines(int stopped, int frame); // the debugger's stop, the selected frame; 0 clears
    void go_to_line(int line);
    void show_line(int line); // scrolls it into view, leaving the cursor where it is
    int cursor_line();
    void toggle_comment();
    void set_font_size(int points);
    std::string text();
    void undo();
    void redo();
    void clear_history();
    sigc::signal<void, const std::string &> signal_hint;
    sigc::signal<void> signal_cursor_moved;

    std::vector<Breakpoint> breakpoints;
    int breakpoint_line(const Breakpoint &b) { return b.anchor->get_iter().get_line() + 1; }
    Breakpoint *breakpoint_at(int line);
    void toggle_breakpoint(int line);
    void remove_breakpoint(size_t index);
    void clear_breakpoints();
    void merge_breakpoints(); // edits can fold two onto one line: keep the first
    void refresh_gutter();
    sigc::signal<void> signal_breakpoints_changed;
    sigc::signal<void, int, GdkEventButton *> signal_breakpoint_menu;

  protected:
    bool on_draw(const Cairo::RefPtr<Cairo::Context> &cr) override;

  private:
    bool on_key(GdkEventKey *event);
    bool on_gutter_press(GdkEventButton *event);
    int gutter_line(int y);
    int marker_size();
    void schedule_highlight();
    void highlight();
    void mark_lines();
    void update_margin();
    std::string word_before_cursor();
    void update_completion(bool force);
    void accept_completion();
    void show_call_tip();
    void indent_selection(bool outdent);

    // GTK 3's text buffer keeps no history, so the editor does.
    struct EditOp
    {
        bool insert;
        int offset;
        Glib::ustring text;
    };
    void record(bool insert, int offset, const Glib::ustring &text);
    void replay(const std::vector<EditOp> &group, bool forward);
    std::vector<std::vector<EditOp>> undo_stack, redo_stack;
    bool replaying = false;
    bool in_user_action = false, new_group = false;
    gint64 last_edit = 0;
    Glib::RefPtr<Gtk::CssProvider> font_css;

    Glib::RefPtr<Gtk::TextTag> tags[6];
    Glib::RefPtr<Gtk::TextTag> current_tag, error_tag, frame_tag, stopped_tag;
    Glib::RefPtr<Gtk::TextMark> scroll_mark;
    int error_line = 0;
    int stopped_line = 0, frame_line = 0;
    bool highlight_pending = false;
    int margin_digits = 0;

    // Owned by GTK: a popover dies with the widget it points at.
    Gtk::Popover *completion;
    Gtk::TreeView *completion_list;
    Glib::RefPtr<Gtk::ListStore> completion_store;
    Gtk::TreeModelColumnRecord completion_columns;
    Gtk::TreeModelColumn<Glib::ustring> completion_name;
};

LuaEditor::LuaEditor()
{
    set_monospace(true);
    set_wrap_mode(Gtk::WRAP_NONE);
    set_left_margin(6);
    set_top_margin(2);

    auto buffer = get_buffer();
    bool dark = dark_theme(*this);
    struct { S9xLuaTokenKind kind; const char *light, *dark; } colors[] = {
        { S9X_LUA_TOKEN_KEYWORD, "#0033b3", "#7aa6ff" },
        { S9X_LUA_TOKEN_BUILTIN, "#008080", "#4ed6c8" },
        { S9X_LUA_TOKEN_NUMBER, "#b05000", "#f0a050" },
        { S9X_LUA_TOKEN_STRING, "#1a801a", "#98d070" },
        { S9X_LUA_TOKEN_COMMENT, "#707070", "#909090" },
    };
    current_tag = buffer->create_tag();
    current_tag->property_paragraph_background() = dark ? "#2e3440" : "#eef3fb";
    for (auto &c : colors)
    {
        auto tag = buffer->create_tag();
        tag->property_foreground() = dark ? c.dark : c.light;
        tags[c.kind] = tag;
    }
    tags[S9X_LUA_TOKEN_KEYWORD]->property_weight() = Pango::WEIGHT_BOLD;
    tags[S9X_LUA_TOKEN_COMMENT]->property_style() = Pango::STYLE_ITALIC;
    error_tag = buffer->create_tag();
    error_tag->property_paragraph_background() = dark ? "#5a2626" : "#f6c8c8";
    frame_tag = buffer->create_tag();
    frame_tag->property_paragraph_background() = dark ? "#284a28" : "#d6efd6";
    stopped_tag = buffer->create_tag();
    stopped_tag->property_paragraph_background() = dark ? "#5c5317" : "#fff0a0";
    signal_button_press_event().connect(sigc::mem_fun(*this, &LuaEditor::on_gutter_press), false);

    buffer->signal_changed().connect([this] {
        schedule_highlight();
        if (error_line)
            set_error_line(0);
        update_margin();
        // Typing moves the cursor without a mark-set signal.
        Glib::signal_idle().connect_once(sigc::track_obj([this] {
            mark_lines();
            signal_cursor_moved.emit();
        }, *this));
    });
    buffer->signal_mark_set().connect([this](const Gtk::TextIter &, const Glib::RefPtr<Gtk::TextMark> &mark) {
        if (mark != get_buffer()->get_insert())
            return;
        mark_lines();
        signal_cursor_moved.emit();
    });
    buffer->signal_insert().connect([this](const Gtk::TextIter &, const Glib::ustring &text, int) {
        if (replaying)
            return;
        if (text == "(")
            show_call_tip();
        else if (text.size() == 1 && (g_unichar_isalnum(text[0]) || text[0] == '_' || text[0] == '.'))
            Glib::signal_idle().connect_once(sigc::track_obj([this] { update_completion(false); }, *this));
        else
            completion->popdown();
    }, true);

    // History: recorded before the buffer changes, while the offsets hold.
    buffer->signal_insert().connect([this](const Gtk::TextIter &pos, const Glib::ustring &text, int) {
        record(true, pos.get_offset(), text);
    }, false);
    buffer->signal_erase().connect([this](const Gtk::TextIter &start, const Gtk::TextIter &end) {
        record(false, start.get_offset(), get_buffer()->get_text(start, end, true));
    }, false);
    buffer->signal_begin_user_action().connect([this] {
        in_user_action = true;
        new_group = true;
    });
    buffer->signal_end_user_action().connect([this] { in_user_action = false; });
    signal_key_press_event().connect(sigc::mem_fun(*this, &LuaEditor::on_key), false);

    // Completion: a popover list that leaves the keyboard with the editor.
    completion = Gtk::manage(new Gtk::Popover(*this));
    completion_list = Gtk::manage(new Gtk::TreeView);
    auto completion_scroll = Gtk::manage(new Gtk::ScrolledWindow);
    completion_columns.add(completion_name);
    completion_store = Gtk::ListStore::create(completion_columns);
    completion_list->set_model(completion_store);
    completion_list->append_column("", completion_name);
    completion_list->set_headers_visible(false);
    completion_list->set_can_focus(false);
    completion_list->signal_row_activated().connect([this](const Gtk::TreeModel::Path &, Gtk::TreeViewColumn *) {
        accept_completion();
    });
    completion_list->get_selection()->signal_changed().connect([this] {
        auto row = completion_list->get_selection()->get_selected();
        if (!row)
            return;
        Glib::ustring name = (*row)[completion_name];
        if (auto doc = S9xLuaFindApiDoc(name.raw()))
            signal_hint.emit(std::string(doc->name) + doc->args + " - " + doc->summary);
    });
    completion_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    completion_scroll->set_size_request(280, 160);
    completion_scroll->add(*completion_list);
    completion_scroll->show_all();
    completion->add(*completion_scroll);
    completion->set_modal(false);
    completion->set_position(Gtk::POS_BOTTOM);

    update_margin();
}

std::string LuaEditor::text()
{
    return get_buffer()->get_text(true).raw();
}

void LuaEditor::set_font_size(int points)
{
    if (!font_css)
    {
        font_css = Gtk::CssProvider::create();
        get_style_context()->add_provider(font_css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    font_css->load_from_data(points > 0 ? "textview { font-size: " + std::to_string(points) + "pt; }" : "");
    margin_digits = 0;
    update_margin();
}

void LuaEditor::record(bool insert, int offset, const Glib::ustring &text)
{
    if (replaying || text.empty())
        return;
    redo_stack.clear();

    // Typing a word is one step; a space, a newline or a pause starts the next.
    gint64 now = g_get_monotonic_time();
    bool typing = insert && text.size() == 1 && !g_unichar_isspace(text[0]) && !undo_stack.empty() &&
                  undo_stack.back().back().insert && undo_stack.back().back().text.size() >= 1 &&
                  undo_stack.back().back().offset + (int)undo_stack.back().back().text.size() == offset &&
                  now - last_edit < 1500000;
    last_edit = now;

    if (typing && in_user_action && new_group)
    {
        undo_stack.back().back().text += text;
        new_group = false;
        return;
    }
    if (new_group || !in_user_action || undo_stack.empty())
    {
        undo_stack.emplace_back();
        new_group = false;
        if (undo_stack.size() > 500)
            undo_stack.erase(undo_stack.begin());
    }
    undo_stack.back().push_back({ insert, offset, text });
}

void LuaEditor::replay(const std::vector<EditOp> &group, bool forward)
{
    auto buffer = get_buffer();
    replaying = true;
    int cursor = 0;
    auto apply = [&](const EditOp &op, bool insert) {
        auto at = buffer->get_iter_at_offset(op.offset);
        if (insert)
        {
            buffer->insert(at, op.text);
            cursor = op.offset + op.text.size();
        }
        else
        {
            buffer->erase(at, buffer->get_iter_at_offset(op.offset + op.text.size()));
            cursor = op.offset;
        }
    };
    if (forward)
        for (auto &op : group)
            apply(op, op.insert);
    else
        for (auto op = group.rbegin(); op != group.rend(); ++op)
            apply(*op, !op->insert);
    replaying = false;
    buffer->place_cursor(buffer->get_iter_at_offset(cursor));
    scroll_to(buffer->get_insert());
}

void LuaEditor::undo()
{
    if (undo_stack.empty())
        return;
    auto group = std::move(undo_stack.back());
    undo_stack.pop_back();
    replay(group, false);
    redo_stack.push_back(std::move(group));
}

void LuaEditor::redo()
{
    if (redo_stack.empty())
        return;
    auto group = std::move(redo_stack.back());
    redo_stack.pop_back();
    replay(group, true);
    undo_stack.push_back(std::move(group));
}

void LuaEditor::clear_history()
{
    undo_stack.clear();
    redo_stack.clear();
}

void LuaEditor::schedule_highlight()
{
    if (highlight_pending)
        return;
    highlight_pending = true;
    Glib::signal_timeout().connect_once(sigc::track_obj([this] {
        highlight_pending = false;
        highlight();
    }, *this), 60);
}

void LuaEditor::highlight()
{
    auto buffer = get_buffer();
    for (auto &tag : tags)
        if (tag)
            buffer->remove_tag(tag, buffer->begin(), buffer->end());

    std::vector<S9xLuaToken> tokens;
    int state = 0;
    for (int line = 0; line < buffer->get_line_count(); line++)
    {
        auto start = buffer->get_iter_at_line(line);
        auto end = start;
        if (!end.ends_line())
            end.forward_to_line_end();
        std::string bytes = buffer->get_text(start, end, true).raw();
        tokens.clear();
        state = S9xLuaTokenize(bytes.data(), (int)bytes.size(), state, tokens);
        for (auto &token : tokens)
        {
            if (!tags[token.kind])
                continue;
            buffer->apply_tag(tags[token.kind], buffer->get_iter_at_line_index(line, token.start),
                              buffer->get_iter_at_line_index(line, token.start + token.length));
        }
    }
}

void LuaEditor::mark_lines()
{
    auto buffer = get_buffer();
    buffer->remove_tag(current_tag, buffer->begin(), buffer->end());
    auto line = buffer->get_insert()->get_iter();
    line.set_line_offset(0);
    auto end = line;
    end.forward_line();
    buffer->apply_tag(current_tag, line, end);

    auto whole_line = [&](const Glib::RefPtr<Gtk::TextTag> &tag, int number) {
        buffer->remove_tag(tag, buffer->begin(), buffer->end());
        if (number <= 0 || number > buffer->get_line_count())
            return;
        auto start = buffer->get_iter_at_line(number - 1);
        auto stop = start;
        stop.forward_line();
        buffer->apply_tag(tag, start, stop);
    };
    whole_line(error_tag, error_line);
    whole_line(frame_tag, frame_line);
    whole_line(stopped_tag, stopped_line);
    refresh_gutter();
}

void LuaEditor::refresh_gutter()
{
    if (auto window = get_window(Gtk::TEXT_WINDOW_LEFT))
        window->invalidate(false);
}

void LuaEditor::set_error_line(int line)
{
    error_line = line;
    mark_lines();
}

void LuaEditor::set_debug_lines(int stopped, int frame)
{
    if (stopped_line == stopped && frame_line == frame)
        return;
    stopped_line = stopped;
    frame_line = frame;
    mark_lines();
}

void LuaEditor::go_to_line(int line)
{
    auto buffer = get_buffer();
    auto iter = buffer->get_iter_at_line(std::max(line, 1) - 1);
    buffer->place_cursor(iter);
    scroll_to(buffer->get_insert(), 0.2);
    grab_focus();
}

void LuaEditor::show_line(int line)
{
    auto buffer = get_buffer();
    auto iter = buffer->get_iter_at_line(std::max(line, 1) - 1);
    if (!scroll_mark)
        scroll_mark = buffer->create_mark(iter, true);
    else
        buffer->move_mark(scroll_mark, iter);
    scroll_to(scroll_mark, 0.25);
}

int LuaEditor::cursor_line()
{
    return get_buffer()->get_insert()->get_iter().get_line() + 1;
}

LuaEditor::Breakpoint *LuaEditor::breakpoint_at(int line)
{
    for (auto &b : breakpoints)
        if (breakpoint_line(b) == line)
            return &b;
    return nullptr;
}

void LuaEditor::remove_breakpoint(size_t index)
{
    if (index >= breakpoints.size())
        return;
    get_buffer()->delete_mark(breakpoints[index].anchor);
    breakpoints.erase(breakpoints.begin() + index);
}

void LuaEditor::clear_breakpoints()
{
    while (!breakpoints.empty())
        remove_breakpoint(breakpoints.size() - 1);
    refresh_gutter();
}

void LuaEditor::toggle_breakpoint(int line)
{
    for (size_t i = 0; i < breakpoints.size(); i++)
    {
        if (breakpoint_line(breakpoints[i]) == line)
        {
            remove_breakpoint(i);
            refresh_gutter();
            signal_breakpoints_changed.emit();
            return;
        }
    }
    auto buffer = get_buffer();
    if (line < 1 || line > buffer->get_line_count())
        return;
    Breakpoint b;
    // Right gravity: Enter at the line's start takes the breakpoint along with the text.
    b.anchor = buffer->create_mark(buffer->get_iter_at_line(line - 1), false);
    breakpoints.push_back(b);
    refresh_gutter();
    signal_breakpoints_changed.emit();
}

void LuaEditor::merge_breakpoints()
{
    std::vector<int> lines;
    for (size_t i = 0; i < breakpoints.size();)
    {
        int line = breakpoint_line(breakpoints[i]);
        if (std::find(lines.begin(), lines.end(), line) != lines.end())
        {
            remove_breakpoint(i);
            continue;
        }
        lines.push_back(line);
        i++;
    }
}

int LuaEditor::marker_size()
{
    int width = 0, height = 0;
    create_pango_layout("9")->get_pixel_size(width, height);
    return height;
}

// The line under a y in the gutter, 1-based; 0 below the last.
int LuaEditor::gutter_line(int y)
{
    int bx = 0, by = 0;
    window_to_buffer_coords(Gtk::TEXT_WINDOW_LEFT, 0, y, bx, by);
    Gtk::TextIter iter;
    int top = 0;
    get_line_at_y(iter, by, top);
    int line_y = 0, height = 0;
    get_line_yrange(iter, line_y, height);
    return by >= line_y && by < line_y + height ? iter.get_line() + 1 : 0;
}

// A click sets or clears a breakpoint; the right button has the rest.
bool LuaEditor::on_gutter_press(GdkEventButton *event)
{
    auto left = get_window(Gtk::TEXT_WINDOW_LEFT);
    if (!left || event->window != left->gobj() || event->type != GDK_BUTTON_PRESS)
        return false;
    int line = gutter_line((int)event->y);
    if (!line)
        return true;
    if (event->button == 1)
        toggle_breakpoint(line);
    else if (event->button == 3)
        signal_breakpoint_menu.emit(line, event);
    return true;
}

void LuaEditor::update_margin()
{
    int digits = std::max<int>(3, std::to_string(get_buffer()->get_line_count()).size());
    if (digits == margin_digits)
        return;
    margin_digits = digits;
    int width = 0, height = 0;
    create_pango_layout(std::string(digits, '9'))->get_pixel_size(width, height);
    set_border_window_size(Gtk::TEXT_WINDOW_LEFT, marker_size() + 4 + width + 10);
}

bool LuaEditor::on_draw(const Cairo::RefPtr<Cairo::Context> &cr)
{
    bool handled = Gtk::TextView::on_draw(cr);

    auto left = get_window(Gtk::TEXT_WINDOW_LEFT);
    if (!left || !gtk_cairo_should_draw_window(cr->cobj(), left->gobj()))
        return handled;

    cr->save();
    gtk_cairo_transform_to_window(cr->cobj(), (GtkWidget *)gobj(), left->gobj());
    int margin = left->get_width();

    Gdk::Rectangle visible;
    get_visible_rect(visible);
    Gtk::TextIter iter;
    int top = 0;
    get_line_at_y(iter, visible.get_y(), top);
    int current = get_buffer()->get_insert()->get_iter().get_line();

    auto layout = create_pango_layout("");
    auto fg = get_style_context()->get_color(Gtk::STATE_FLAG_NORMAL);
    int text_height = marker_size();
    double size = std::max(6, text_height - 4);
    std::vector<std::pair<int, const Breakpoint *>> marks;
    for (auto &b : breakpoints)
        marks.push_back({ breakpoint_line(b), &b });

    auto draw_breakpoint = [&](const Breakpoint &b, double x, double y) {
        // Red, orange with a condition or hit count, a blue diamond for a log message.
        cr->begin_new_path(); // the line number's text leaves a current point
        if (!b.log.empty())
            cr->set_source_rgb(0x24 / 255.0, 0x70 / 255.0, 0xc8 / 255.0);
        else if (!b.condition.empty() || !b.hit.empty())
            cr->set_source_rgb(0xe2 / 255.0, 0x84 / 255.0, 0.0);
        else
            cr->set_source_rgb(0xc8 / 255.0, 0x24 / 255.0, 0x24 / 255.0);
        if (!b.log.empty())
        {
            cr->move_to(x + size / 2, y);
            cr->line_to(x + size, y + size / 2);
            cr->line_to(x + size / 2, y + size);
            cr->line_to(x, y + size / 2);
            cr->close_path();
        }
        else
            cr->arc(x + size / 2, y + size / 2, size / 2, 0, 2 * G_PI);
        cr->set_line_width(1.5);
        if (b.enabled)
            cr->fill();
        else
            cr->stroke();
    };
    auto draw_arrow = [&](double x, double y, bool stop) {
        // Yellow where the script stopped, green for the selected frame.
        double cy = y + size / 2, mid = x + size / 2, h = size / 5;
        cr->begin_new_path();
        cr->move_to(x, cy - h);
        cr->line_to(mid, cy - h);
        cr->line_to(mid, y);
        cr->line_to(x + size, cy);
        cr->line_to(mid, y + size);
        cr->line_to(mid, cy + h);
        cr->line_to(x, cy + h);
        cr->close_path();
        if (stop)
            cr->set_source_rgb(1.0, 0xd6 / 255.0, 0.0);
        else
            cr->set_source_rgb(0x60 / 255.0, 0xb8 / 255.0, 0x60 / 255.0);
        cr->fill_preserve();
        cr->set_source_rgb(0x60 / 255.0, 0x50 / 255.0, 0.0);
        cr->set_line_width(1.0);
        cr->stroke();
    };

    while (true)
    {
        int y = 0, height = 0, wx = 0, wy = 0;
        get_line_yrange(iter, y, height);
        if (y > visible.get_y() + visible.get_height())
            break;
        buffer_to_window_coords(Gtk::TEXT_WINDOW_LEFT, 0, y, wx, wy);

        int line = iter.get_line();
        double box_y = wy + (std::min(height, text_height) - size) / 2;
        for (auto &mark : marks)
            if (mark.first == line + 1)
                draw_breakpoint(*mark.second, 3, box_y);
        if (line + 1 == stopped_line || line + 1 == frame_line)
            draw_arrow(3, box_y, line + 1 == stopped_line);

        layout->set_text(std::to_string(line + 1));
        int tw = 0, th = 0;
        layout->get_pixel_size(tw, th);
        if (line + 1 == error_line)
            cr->set_source_rgb(0.88, 0.25, 0.25);
        else
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), line == current ? 1.0 : 0.45);
        cr->move_to(margin - tw - 5, wy);
        layout->show_in_cairo_context(cr);

        if (!iter.forward_line())
            break;
    }
    cr->restore();
    return handled;
}

std::string LuaEditor::word_before_cursor()
{
    auto end = get_buffer()->get_insert()->get_iter();
    auto start = end;
    while (!start.starts_line())
    {
        auto previous = start;
        previous.backward_char();
        gunichar c = previous.get_char();
        if (!(g_unichar_isalnum(c) || c == '_' || c == '.'))
            break;
        start = previous;
    }
    return get_buffer()->get_text(start, end, true).raw();
}

void LuaEditor::update_completion(bool force)
{
    std::string prefix = word_before_cursor();
    if (!force && prefix.size() < 2)
    {
        completion->popdown();
        return;
    }

    completion_store->clear();
    int count = 0;
    std::string only;
    for (auto &name : S9xLuaApiNames())
    {
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;
        (*completion_store->append())[completion_name] = name;
        only = name;
        if (++count >= 60)
            break;
    }
    if (count == 0 || (count == 1 && only == prefix))
    {
        completion->popdown();
        return;
    }

    Gdk::Rectangle location;
    get_iter_location(get_buffer()->get_insert()->get_iter(), location);
    int x = 0, y = 0;
    buffer_to_window_coords(Gtk::TEXT_WINDOW_WIDGET, location.get_x(), location.get_y(), x, y);
    completion->set_pointing_to(Gdk::Rectangle(x, y, 1, location.get_height()));
    completion_list->get_selection()->select(completion_store->children().begin());
    completion_list->scroll_to_row(Gtk::TreePath("0"));
    completion->popup();
}

void LuaEditor::accept_completion()
{
    auto row = completion_list->get_selection()->get_selected();
    completion->popdown();
    if (!row)
        return;
    Glib::ustring name = (*row)[completion_name];
    auto buffer = get_buffer();
    auto end = buffer->get_insert()->get_iter();
    auto start = end;
    start.backward_chars(g_utf8_strlen(word_before_cursor().c_str(), -1));
    buffer->begin_user_action();
    buffer->erase(start, end);
    buffer->insert_at_cursor(name);
    buffer->end_user_action();
}

void LuaEditor::show_call_tip()
{
    auto buffer = get_buffer();
    auto end = buffer->get_insert()->get_iter();
    end.backward_char(); // the '(' just typed
    auto start = end;
    while (!start.starts_line())
    {
        auto previous = start;
        previous.backward_char();
        gunichar c = previous.get_char();
        if (!(g_unichar_isalnum(c) || c == '_' || c == '.'))
            break;
        start = previous;
    }
    completion->popdown();
    if (auto doc = S9xLuaFindApiDoc(buffer->get_text(start, end, true).raw()))
    {
        set_tooltip_text(std::string(doc->name) + doc->args + "\n" + doc->summary);
        signal_hint.emit(std::string(doc->name) + doc->args + " - " + doc->summary);
    }
}

void LuaEditor::indent_selection(bool outdent)
{
    auto buffer = get_buffer();
    Gtk::TextIter a, b;
    buffer->get_selection_bounds(a, b);
    int first = a.get_line(), last = b.get_line();
    if (last > first && b.starts_line())
        last--;

    buffer->begin_user_action();
    for (int line = first; line <= last; line++)
    {
        auto start = buffer->get_iter_at_line(line);
        if (outdent)
        {
            auto stop = start;
            int n = 0;
            while (n < indent_width && stop.get_char() == ' ')
            {
                stop.forward_char();
                n++;
            }
            if (n == 0 && stop.get_char() == '\t')
                stop.forward_char();
            buffer->erase(start, stop);
        }
        else if (!start.ends_line())
            buffer->insert(start, std::string(indent_width, ' '));
    }
    buffer->end_user_action();
}

void LuaEditor::toggle_comment()
{
    auto buffer = get_buffer();
    Gtk::TextIter a, b;
    buffer->get_selection_bounds(a, b);
    int first = a.get_line(), last = b.get_line();
    if (last > first && b.starts_line())
        last--;

    auto line_text = [&](int line) {
        auto start = buffer->get_iter_at_line(line);
        auto end = start;
        if (!end.ends_line())
            end.forward_to_line_end();
        return buffer->get_text(start, end, true).raw();
    };

    bool all_commented = true;
    size_t indent = std::string::npos;
    for (int line = first; line <= last; line++)
    {
        std::string text = line_text(line);
        size_t code = text.find_first_not_of(" \t");
        if (code == std::string::npos)
            continue;
        all_commented = all_commented && text.compare(code, 2, "--") == 0;
        indent = std::min(indent, code);
    }
    if (indent == std::string::npos)
        return;

    buffer->begin_user_action();
    for (int line = first; line <= last; line++)
    {
        std::string text = line_text(line);
        size_t code = text.find_first_not_of(" \t");
        if (code == std::string::npos)
            continue;
        if (all_commented)
        {
            int length = text.compare(code, 3, "-- ") == 0 ? 3 : 2;
            auto start = buffer->get_iter_at_line_index(line, code);
            auto stop = buffer->get_iter_at_line_index(line, code + length);
            buffer->erase(start, stop);
        }
        else
            buffer->insert(buffer->get_iter_at_line_index(line, indent), "-- ");
    }
    buffer->end_user_action();
}

bool LuaEditor::on_key(GdkEventKey *event)
{
    unsigned modifiers = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK);

    if (completion->is_visible())
    {
        switch (event->keyval)
        {
        case GDK_KEY_Up:
        case GDK_KEY_Down:
        {
            auto selection = completion_list->get_selection();
            auto row = selection->get_selected();
            auto children = completion_store->children();
            if (!row)
                row = children.begin();
            else if (event->keyval == GDK_KEY_Down)
            {
                auto next = row;
                if (++next != children.end())
                    row = next;
            }
            else if (event->keyval == GDK_KEY_Up && row != children.begin())
                --row;
            selection->select(row);
            completion_list->scroll_to_row(completion_store->get_path(row));
            return true;
        }
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab:
            accept_completion();
            return true;
        case GDK_KEY_Escape:
            completion->popdown();
            return true;
        case GDK_KEY_BackSpace:
            Glib::signal_idle().connect_once(sigc::track_obj([this] { update_completion(false); }, *this));
            return false;
        default:
            break;
        }
    }

    if (event->keyval == GDK_KEY_space && modifiers == GDK_CONTROL_MASK)
    {
        update_completion(true);
        return true;
    }

    auto buffer = get_buffer();
    if (event->keyval == GDK_KEY_Tab && modifiers == 0)
    {
        if (buffer->get_has_selection())
            indent_selection(false);
        else
        {
            int column = buffer->get_insert()->get_iter().get_line_offset();
            buffer->insert_at_cursor(std::string(indent_width - column % indent_width, ' '));
        }
        return true;
    }
    if (event->keyval == GDK_KEY_ISO_Left_Tab)
    {
        indent_selection(true);
        return true;
    }

    if ((event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) && modifiers == 0)
    {
        // Keep the line's indent, one level deeper after a block opener.
        auto cursor = buffer->get_insert()->get_iter();
        auto line_start = cursor;
        line_start.set_line_offset(0);
        std::string before = buffer->get_text(line_start, cursor, true).raw();
        std::string indent = leading_space(before);
        if (opens_block(before))
            indent += std::string(indent_width, ' ');
        buffer->begin_user_action();
        buffer->erase_selection();
        buffer->insert_at_cursor("\n" + indent);
        buffer->end_user_action();
        scroll_to(buffer->get_insert());
        return true;
    }

    return false;
}

/* ---- Window ------------------------------------------------------------- */

class LuaWindow : public Gtk::Window
{
  public:
    LuaWindow();
    void open_script(const std::string &path, bool run);
    bool confirm_close();
    void save_session();
    void stop_all();

    /* The debugger. A stop runs inside the script's hook, on this thread: the
     * engine pumps the main loop while it waits, and the other windows stay off. */
    void hold();
    void release_hold();
    void forget_breakpoints(); // before quitting: nothing may stop any more

  protected:
    bool on_delete_event(GdkEventAny *event) override;

  private:
    struct Document
    {
        LuaEditor *editor = nullptr;
        Gtk::ScrolledWindow *page = nullptr;
        Gtk::Label *label = nullptr;
        std::string path;
        std::string untitled;
        int script = 0;
        bool paused = false;
        bool modified = false;
    };

    void build_menus();
    void build_reference();
    Gtk::MenuItem *add_item(Gtk::Menu &menu, const char *label, std::function<void()> action,
                            guint key = 0, Gdk::ModifierType mods = Gdk::ModifierType(0));
    Document *add_document(const std::string &text, const std::string &path, const std::string &name);
    Document *current();
    Document *for_script(int id);
    std::string display_name(const Document *doc);
    void update_tab(Document *doc);
    void update_actions();
    bool save(Document *doc, bool save_as);
    bool close_document(Document *doc);
    bool ask_save(Document *doc);
    void add_recent(const std::string &path);
    void fill_recent_menu();
    void run(Document *doc);
    void stop(Document *doc);
    void toggle_pause(Document *doc);
    bool poll();
    void append_console(const std::string &text, const Glib::RefPtr<Gtk::TextTag> &tag);
    bool console_click(GdkEventButton *event);
    void find(bool backward);
    void go_to_line();
    void set_status(const std::string &text);
    void zoom(int steps);

    // Debugger
    void build_debug_panels();
    void debug_command(S9xLuaDebugAction action);
    void poll_debug();
    void on_stopped(const S9xLuaDebugEvent &e);
    void on_variables(const S9xLuaDebugEvent &e);
    void on_evaluated(const S9xLuaDebugEvent &e);
    void clear_debug_views();
    void select_frame(int index);
    Document *frame_document(const S9xLuaDebugFrame &f);
    Gtk::TreeModel::iterator add_variable(const Glib::RefPtr<Gtk::TreeStore> &store, const Gtk::TreeModel::iterator *parent,
                                          const std::string &text, int ref);
    void set_children(const Glib::RefPtr<Gtk::TreeStore> &store, const Gtk::TreeModel::iterator &row,
                      const std::vector<S9xLuaDebugVariable> &variables);
    void clear_tree(const Glib::RefPtr<Gtk::TreeStore> &store);
    void request_children(const Glib::RefPtr<Gtk::TreeStore> &store, const Gtk::TreeModel::iterator &row);
    void fill_variables();
    void fill_watches();
    void fill_call_stack();
    void add_watch(const std::string &expression);
    void remove_watch();
    void send_breakpoints(Document *doc);
    void breakpoints_changed(Document *doc);
    void refresh_breakpoint_list();
    LuaEditor::Breakpoint *breakpoint_for_row(int row, Document **owner);
    int selected_breakpoint_row();
    void fill_breakpoint_fields();
    void apply_breakpoint_fields();
    void edit_breakpoint(Document *doc, int line);
    void remove_breakpoint_row(int row);
    void remove_all_breakpoints();
    void go_to_breakpoint(int row);
    void show_breakpoint_menu(Document *doc, int line, GdkEventButton *event);
    void popup(std::vector<std::pair<std::string, std::function<void()>>> items, GdkEventButton *event);

    Gtk::Box root{ Gtk::ORIENTATION_VERTICAL };
    Gtk::MenuBar menubar;
    Gtk::Toolbar toolbar;
    Gtk::Paned main_split{ Gtk::ORIENTATION_HORIZONTAL };
    Gtk::Paned editor_split{ Gtk::ORIENTATION_VERTICAL };
    Gtk::Notebook notebook;
    Gtk::Box console_box{ Gtk::ORIENTATION_VERTICAL };
    Gtk::ScrolledWindow console_scroll;
    Gtk::TextView console;
    Gtk::Box reference_box{ Gtk::ORIENTATION_VERTICAL };
    Gtk::SearchEntry reference_search;
    Gtk::ScrolledWindow reference_scroll;
    Gtk::TreeView reference;
    Glib::RefPtr<Gtk::TreeStore> reference_store;
    Glib::RefPtr<Gtk::TreeModelFilter> reference_filter;
    Gtk::TreeModelColumnRecord reference_columns;
    Gtk::TreeModelColumn<Glib::ustring> ref_label, ref_name, ref_tip;
    Gtk::SearchEntry find_entry;
    Gtk::Box status_box{ Gtk::ORIENTATION_HORIZONTAL };
    Gtk::Label status, position;
    Glib::RefPtr<Gtk::AccelGroup> accels;
    Gtk::Menu *recent_menu = nullptr;
    Gtk::CheckMenuItem *reference_item = nullptr;
    Gtk::ToolButton *run_button = nullptr, *pause_button = nullptr, *stop_button = nullptr;
    Gtk::MenuItem *run_item = nullptr, *pause_item = nullptr, *stop_item = nullptr, *stop_all_item = nullptr;
    Glib::RefPtr<Gtk::TextTag> tag_info, tag_warning, tag_error, tag_quiet;

    std::vector<std::unique_ptr<Document>> documents;
    std::vector<std::pair<int, std::string>> script_names;
    int untitled_count = 0;
    int font_size = 0;

    // Under the editor: the console and the debugger's panels.
    Gtk::Notebook panel_tabs;
    bool panel_restored = false;
    // Variables and watches: "name = value" rows; a stand-in child marks fields not fetched yet.
    Gtk::TreeModelColumnRecord var_columns;
    Gtk::TreeModelColumn<Glib::ustring> var_text;
    Gtk::TreeModelColumn<int> var_ref;
    Gtk::TreeModelColumn<bool> var_stand_in;
    Glib::RefPtr<Gtk::TreeStore> variables_store, watch_store;
    Gtk::TreeView variables_view, watch_view;
    Gtk::ScrolledWindow variables_scroll, watch_scroll;
    Gtk::Box watch_box{ Gtk::ORIENTATION_VERTICAL };
    Gtk::Entry watch_entry;
    Gtk::TreeModelColumnRecord stack_columns;
    Gtk::TreeModelColumn<Glib::ustring> stack_function, stack_location;
    Glib::RefPtr<Gtk::ListStore> stack_store;
    Gtk::TreeView stack_view;
    Gtk::ScrolledWindow stack_scroll;
    Gtk::TreeModelColumnRecord bp_columns;
    Gtk::TreeModelColumn<bool> bp_enabled;
    Gtk::TreeModelColumn<Glib::ustring> bp_location, bp_condition, bp_hit, bp_log;
    Glib::RefPtr<Gtk::ListStore> bp_store;
    Gtk::TreeView bp_view;
    Gtk::ScrolledWindow bp_scroll;
    Gtk::Box bp_box{ Gtk::ORIENTATION_VERTICAL };
    Gtk::Entry bp_fields[3];
    std::unique_ptr<Gtk::Menu> popup_menu;

    Gtk::MenuItem *continue_item = nullptr, *break_item = nullptr;
    Gtk::MenuItem *step_over_item = nullptr, *step_into_item = nullptr, *step_out_item = nullptr;
    Gtk::ToolButton *continue_button = nullptr, *step_over_button = nullptr, *step_into_button = nullptr,
                    *step_out_button = nullptr;

    int stopped = 0; // the script stopped in the debugger
    std::string stop_text;
    std::vector<S9xLuaDebugFrame> frames;
    int frame = 0; // the selected one
    struct Pending
    {
        int ref;
        Glib::RefPtr<Gtk::TreeStore> store;
        Gtk::TreeRowReference row;
    };
    std::vector<Pending> pending; // variable requests, answered by ref
    std::vector<std::string> watches;
    int watch_generation = 0;
    std::vector<std::pair<Document *, size_t>> bp_rows; // the breakpoints list
    bool filling = false; // the call stack or breakpoints list is being refilled
    bool holding = false;
    std::vector<GtkWidget *> held_windows; // referenced while held
};

LuaWindow *lua_window = nullptr;
bool quitting = false; // a stop now ends its script

LuaWindow::LuaWindow()
{
    set_title(_("Lua Scripting"));
    if (top_level && top_level->window)
        set_transient_for(*top_level->window.get());
    set_default_size(gui_config->lua_window_width > 0 ? gui_config->lua_window_width : 900,
                     gui_config->lua_window_height > 0 ? gui_config->lua_window_height : 650);
    font_size = gui_config->lua_font_size;

    accels = Gtk::AccelGroup::create();
    add_accel_group(accels);

    build_menus();
    root.pack_start(menubar, false, false);
    root.pack_start(toolbar, false, false);

    notebook.set_scrollable(true);
    notebook.signal_switch_page().connect([this](Gtk::Widget *, guint) {
        Glib::signal_idle().connect_once(sigc::track_obj([this] { update_actions(); }, *this));
    });

    console.set_editable(false);
    console.set_monospace(true);
    console.set_wrap_mode(Gtk::WRAP_WORD_CHAR);
    console.set_cursor_visible(false);
    console.signal_button_press_event().connect(sigc::mem_fun(*this, &LuaWindow::console_click), false);
    console_scroll.add(console);
    console_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    console_scroll.set_size_request(-1, 120);
    auto console_header = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    auto clear_button = Gtk::manage(new Gtk::Button(_("Clear")));
    clear_button->set_relief(Gtk::RELIEF_NONE);
    clear_button->signal_clicked().connect([this] { console.get_buffer()->set_text(""); });
    console_header->pack_end(*clear_button, false, false);
    console_box.pack_start(*console_header, false, false);
    console_box.pack_start(console_scroll, true, true);
    console_scroll.set_tooltip_text(_("Script output. Double-click an error to jump to its line."));

    bool dark = dark_theme(console);
    auto cbuf = console.get_buffer();
    tag_info = cbuf->create_tag();
    tag_info->property_foreground() = dark ? "#7ab8ff" : "#1a5fb4";
    tag_warning = cbuf->create_tag();
    tag_warning->property_foreground() = dark ? "#ffc050" : "#a06000";
    tag_error = cbuf->create_tag();
    tag_error->property_foreground() = dark ? "#ff7070" : "#c01010";
    tag_quiet = cbuf->create_tag();
    tag_quiet->property_foreground() = dark ? "#909090" : "#707070";

    panel_tabs.append_page(console_box, _("Console"));
    build_debug_panels();
    editor_split.pack1(notebook, true, false);
    editor_split.pack2(panel_tabs, false, false);
    editor_split.set_position(gui_config->lua_window_height > 0 ? gui_config->lua_window_height - 280 : 370);
    // The saved panel height, once the split knows its own.
    editor_split.signal_size_allocate().connect([this](Gtk::Allocation &allocation) {
        if (panel_restored || allocation.get_height() <= 1)
            return;
        panel_restored = true;
        int height = allocation.get_height();
        if (gui_config->lua_panel_height > 0)
            Glib::signal_idle().connect_once(sigc::track_obj([this, height] {
                editor_split.set_position(std::max(120, height - gui_config->lua_panel_height));
            }, *this));
    });

    build_reference();
    main_split.pack1(editor_split, true, false);
    main_split.pack2(reference_box, false, false);
    main_split.set_position((gui_config->lua_window_width > 0 ? gui_config->lua_window_width : 900) - 290);
    root.pack_start(main_split, true, true);

    status.set_xalign(0.0);
    status.set_ellipsize(Pango::ELLIPSIZE_END);
    status_box.set_margin_start(6);
    status_box.set_margin_end(6);
    status_box.set_margin_top(2);
    status_box.set_margin_bottom(2);
    status_box.pack_start(status, true, true);
    status_box.pack_end(position, false, false);
    root.pack_start(status_box, false, false);

    add(root);
    show_all();
    reference_box.set_visible(gui_config->lua_show_reference);
    reference_item->set_active(gui_config->lua_show_reference);

    for (auto &path : gui_config->lua_open_scripts)
        if (fs::exists(path))
            open_script(path, false);

    // Scripts started from the command line get a tab too.
    for (auto &info : S9xLuaScripts())
    {
        if (for_script(info.id) || info.path.empty())
            continue;
        open_script(info.path, false);
        if (auto doc = current(); doc && !doc->script)
        {
            doc->script = info.id;
            doc->paused = info.state == S9X_LUA_PAUSED;
            script_names.push_back({ info.id, info.name });
            update_tab(doc);
        }
    }

    if (documents.empty())
        add_document(S9xLuaNewScriptTemplate(), "", "");

    Glib::signal_timeout().connect(sigc::mem_fun(*this, &LuaWindow::poll), 40);
    update_actions();
}

Gtk::MenuItem *LuaWindow::add_item(Gtk::Menu &menu, const char *label, std::function<void()> action,
                                   guint key, Gdk::ModifierType mods)
{
    auto item = Gtk::manage(new Gtk::MenuItem(label, true));
    item->signal_activate().connect(action);
    if (key)
        item->add_accelerator("activate", accels, key, mods, Gtk::ACCEL_VISIBLE);
    menu.append(*item);
    return item;
}

void LuaWindow::build_menus()
{
    auto add_menu = [this](const char *label) {
        auto item = Gtk::manage(new Gtk::MenuItem(label, true));
        auto menu = Gtk::manage(new Gtk::Menu);
        menu->set_accel_group(accels);
        item->set_submenu(*menu);
        menubar.append(*item);
        return menu;
    };
    auto separator = [](Gtk::Menu *menu) { menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem)); };
    auto editor_do = [this](std::function<void(LuaEditor *)> fn) {
        return [this, fn] {
            if (auto doc = current())
                fn(doc->editor);
        };
    };
    const auto ctrl = Gdk::CONTROL_MASK;
    const auto ctrl_shift = Gdk::CONTROL_MASK | Gdk::SHIFT_MASK;

    auto file = add_menu(_("_File"));
    add_item(*file, _("_New Script"), [this] { add_document(S9xLuaNewScriptTemplate(), "", ""); }, GDK_KEY_n, ctrl);
    auto open_dialog = [this] {
        Gtk::FileChooserDialog dialog(*this, _("Open Lua Script"), Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("_Open"), Gtk::RESPONSE_ACCEPT);
        dialog.set_select_multiple(true);
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Lua scripts"));
        filter->add_pattern("*.lua");
        dialog.add_filter(filter);
        auto all = Gtk::FileFilter::create();
        all->set_name(_("All files"));
        all->add_pattern("*");
        dialog.add_filter(all);
        if (!gui_config->lua_last_directory.empty())
            dialog.set_current_folder(gui_config->lua_last_directory);
        if (dialog.run() != Gtk::RESPONSE_ACCEPT)
            return;
        auto files = dialog.get_filenames();
        dialog.hide();
        for (auto &path : files)
            open_script(path, false);
    };
    add_item(*file, _("_Open..."), open_dialog, GDK_KEY_o, ctrl);
    auto recent_item = Gtk::manage(new Gtk::MenuItem(_("Open _Recent"), true));
    recent_menu = Gtk::manage(new Gtk::Menu);
    recent_item->set_submenu(*recent_menu);
    file->append(*recent_item);
    recent_item->signal_activate().connect([this] { fill_recent_menu(); });
    fill_recent_menu();
    separator(file);
    add_item(*file, _("_Save"), [this] { if (auto doc = current()) save(doc, false); }, GDK_KEY_s, ctrl);
    add_item(*file, _("Save _As..."), [this] { if (auto doc = current()) save(doc, true); }, GDK_KEY_s, ctrl_shift);
    separator(file);
    add_item(*file, _("_Close Script"), [this] { if (auto doc = current()) close_document(doc); }, GDK_KEY_w, ctrl);
    add_item(*file, _("Close _Window"), [this] { hide(); });

    auto edit = add_menu(_("_Edit"));
    add_item(*edit, _("_Undo"), editor_do([](LuaEditor *e) { e->undo(); }), GDK_KEY_z, ctrl);
    add_item(*edit, _("_Redo"), editor_do([](LuaEditor *e) { e->redo(); }), GDK_KEY_y, ctrl);
    separator(edit);
    add_item(*edit, _("Toggle _Comment"), editor_do([](LuaEditor *e) { e->toggle_comment(); }), GDK_KEY_slash, ctrl);
    add_item(*edit, _("_Find..."), [this] { find_entry.grab_focus(); }, GDK_KEY_f, ctrl);
    add_item(*edit, _("Find _Next"), [this] { find(false); }, GDK_KEY_F3);
    add_item(*edit, _("Find Pre_vious"), [this] { find(true); }, GDK_KEY_F3, Gdk::SHIFT_MASK);
    add_item(*edit, _("_Go to Line..."), [this] { go_to_line(); }, GDK_KEY_g, ctrl);

    auto script = add_menu(_("_Script"));
    run_item = add_item(*script, _("_Run"), [this] {
        // While a script is stopped in the debugger, F5 lets it go on.
        if (stopped)
            debug_command(S9X_LUA_DEBUG_CONTINUE);
        else if (auto doc = current())
            run(doc);
    }, GDK_KEY_F5);
    pause_item = add_item(*script, _("_Pause"), [this] { if (auto doc = current()) toggle_pause(doc); }, GDK_KEY_F6);
    stop_item = add_item(*script, _("_Stop"), [this] { if (auto doc = current()) stop(doc); }, GDK_KEY_F5, Gdk::SHIFT_MASK);
    separator(script);
    stop_all_item = add_item(*script, _("Stop _All Scripts"), [this] { stop_all(); });

    auto debug = add_menu(_("_Debug"));
    continue_item = add_item(*debug, _("_Continue"), [this] { debug_command(S9X_LUA_DEBUG_CONTINUE); });
    if (auto label = dynamic_cast<Gtk::AccelLabel *>(continue_item->get_child()))
        label->set_accel(GDK_KEY_F5, Gdk::ModifierType(0)); // Run's key while stopped
    break_item = add_item(*debug, _("_Break"), [this] {
        Document *doc = current();
        if (stopped || !doc || !doc->script)
            return;
        S9xLuaDebugCommand(doc->script, S9X_LUA_DEBUG_BREAK);
        set_status(_("Stopping at the script's next line"));
    }, GDK_KEY_F8);
    step_over_item = add_item(*debug, _("Step _Over"), [this] { debug_command(S9X_LUA_DEBUG_STEP_OVER); }, GDK_KEY_F10);
    step_into_item = add_item(*debug, _("Step _Into"), [this] { debug_command(S9X_LUA_DEBUG_STEP_INTO); }, GDK_KEY_F11);
    step_out_item = add_item(*debug, _("Step O_ut"), [this] { debug_command(S9X_LUA_DEBUG_STEP_OUT); },
                             GDK_KEY_F11, Gdk::SHIFT_MASK);
    separator(debug);
    add_item(*debug, _("_Toggle Breakpoint"), editor_do([](LuaEditor *e) { e->toggle_breakpoint(e->cursor_line()); }),
             GDK_KEY_F9);
    add_item(*debug, _("_Edit Breakpoint..."), [this] {
        if (auto doc = current())
            edit_breakpoint(doc, doc->editor->cursor_line());
    });
    add_item(*debug, _("_Remove All Breakpoints"), [this] { remove_all_breakpoints(); }, GDK_KEY_F9, ctrl_shift);
    add_item(*debug, _("Add _Watch"), [this] {
        Document *doc = current();
        if (!doc)
            return;
        // The selection, or the name under the cursor.
        auto buffer = doc->editor->get_buffer();
        Gtk::TextIter first, last;
        std::string text;
        if (buffer->get_selection_bounds(first, last))
            text = buffer->get_text(first, last, true).raw();
        else
        {
            auto ident = [](gunichar c) { return g_unichar_isalnum(c) || c == '_' || c == '.'; };
            first = last = buffer->get_insert()->get_iter();
            while (!first.starts_line())
            {
                auto previous = first;
                previous.backward_char();
                if (!ident(previous.get_char()))
                    break;
                first = previous;
            }
            while (!last.ends_line() && ident(last.get_char()))
                last.forward_char();
            text = buffer->get_text(first, last, true).raw();
        }
        add_watch(text);
        panel_tabs.set_current_page(panel_tabs.page_num(watch_box));
    }, GDK_KEY_F9, Gdk::SHIFT_MASK);
    separator(debug);
    auto break_on_error = Gtk::manage(new Gtk::CheckMenuItem(_("Break on _Errors"), true));
    break_on_error->set_active(gui_config->lua_break_on_error);
    break_on_error->signal_toggled().connect([break_on_error] {
        gui_config->lua_break_on_error = break_on_error->get_active();
        S9xLuaDebugSetBreakOnError(gui_config->lua_break_on_error);
    });
    debug->append(*break_on_error);

    auto view = add_menu(_("_View"));
    reference_item = Gtk::manage(new Gtk::CheckMenuItem(_("API _Reference"), true));
    reference_item->add_accelerator("activate", accels, GDK_KEY_F1, Gdk::ModifierType(0), Gtk::ACCEL_VISIBLE);
    reference_item->signal_toggled().connect([this] { reference_box.set_visible(reference_item->get_active()); });
    view->append(*reference_item);
    add_item(*view, _("C_lear Console"), [this] { console.get_buffer()->set_text(""); }, GDK_KEY_l, ctrl);
    separator(view);
    add_item(*view, _("Zoom _In"), [this] { zoom(1); }, GDK_KEY_plus, ctrl);
    add_item(*view, _("Zoom _Out"), [this] { zoom(-1); }, GDK_KEY_minus, ctrl);
    add_item(*view, _("_Reset Zoom"), [this] { zoom(0); }, GDK_KEY_0, ctrl);

    auto help = add_menu(_("_Help"));
    auto examples_item = Gtk::manage(new Gtk::MenuItem(_("_Examples"), true));
    auto examples = Gtk::manage(new Gtk::Menu);
    size_t count;
    const S9xLuaExample *list = S9xLuaExamples(&count);
    for (size_t i = 0; i < count; i++)
    {
        const S9xLuaExample *example = &list[i];
        add_item(*examples, example->title, [this, example] { add_document(example->code, "", example->filename); });
    }
    examples_item->set_submenu(*examples);
    help->append(*examples_item);
    separator(help);
    auto open_url = [this](const char *url) {
        return [this, url] {
            GError *error = nullptr;
            gtk_show_uri_on_window(gobj(), url, GDK_CURRENT_TIME, &error);
            if (error)
                g_error_free(error);
        };
    };
    add_item(*help, _("Snes9x _Scripting Guide"), open_url("https://github.com/shanytc/snes9x/blob/master/docs/lua-scripting.md"));
    add_item(*help, _("Lua 5.4 _Manual"), open_url("https://www.lua.org/manual/5.4/"));
    add_item(*help, _("_Programming in Lua"), open_url("https://www.lua.org/pil/contents.html"));

    // Toolbar
    auto tool = [this](const char *icon, const char *label, std::function<void()> action) {
        auto button = Gtk::manage(new Gtk::ToolButton(label));
        button->set_icon_name(icon);
        button->set_is_important(true);
        button->signal_clicked().connect(action);
        toolbar.append(*button);
        return button;
    };
    toolbar.set_toolbar_style(Gtk::TOOLBAR_BOTH_HORIZ);
    tool("document-new", _("New"), [this] { add_document(S9xLuaNewScriptTemplate(), "", ""); });
    tool("document-open", _("Open"), open_dialog);
    tool("document-save", _("Save"), [this] { if (auto doc = current()) save(doc, false); });
    toolbar.append(*Gtk::manage(new Gtk::SeparatorToolItem));
    run_button = tool("media-playback-start", _("Run"), [this] { if (auto doc = current()) run(doc); });
    pause_button = tool("media-playback-pause", _("Pause"), [this] { if (auto doc = current()) toggle_pause(doc); });
    stop_button = tool("media-playback-stop", _("Stop"), [this] { if (auto doc = current()) stop(doc); });
    toolbar.append(*Gtk::manage(new Gtk::SeparatorToolItem));
    continue_button = tool("media-skip-forward", _("Continue"), [this] { debug_command(S9X_LUA_DEBUG_CONTINUE); });
    step_over_button = tool("go-next", _("Step Over"), [this] { debug_command(S9X_LUA_DEBUG_STEP_OVER); });
    step_into_button = tool("go-down", _("Step Into"), [this] { debug_command(S9X_LUA_DEBUG_STEP_INTO); });
    step_out_button = tool("go-up", _("Step Out"), [this] { debug_command(S9X_LUA_DEBUG_STEP_OUT); });

    auto spacer = Gtk::manage(new Gtk::SeparatorToolItem);
    spacer->set_draw(false);
    spacer->set_expand(true);
    toolbar.append(*spacer);
    auto find_item = Gtk::manage(new Gtk::ToolItem);
    find_entry.set_placeholder_text(_("Find"));
    find_entry.signal_activate().connect([this] { find(false); });
    find_item->add(find_entry);
    toolbar.append(*find_item);
}

void LuaWindow::build_reference()
{
    reference_columns.add(ref_label);
    reference_columns.add(ref_name);
    reference_columns.add(ref_tip);
    reference_store = Gtk::TreeStore::create(reference_columns);

    size_t count;
    const S9xLuaApiDoc *docs = S9xLuaApiDocs(&count);
    Gtk::TreeModel::Row module_row;
    std::string module;
    for (size_t i = 0; i < count; i++)
    {
        std::string name = docs[i].name;
        std::string this_module = name.substr(0, name.find('.'));
        if (this_module != module)
        {
            module = this_module;
            module_row = *reference_store->append();
            module_row[ref_label] = "<b>" + module + "</b>";
            module_row[ref_name] = "";
            module_row[ref_tip] = "";
        }
        auto row = *reference_store->append(module_row.children());
        row[ref_label] = Glib::Markup::escape_text(name.substr(module.size() + 1) + docs[i].args);
        row[ref_name] = name;
        row[ref_tip] = Glib::Markup::escape_text(name + docs[i].args) + "\n" + Glib::Markup::escape_text(docs[i].summary);
    }

    reference_filter = Gtk::TreeModelFilter::create(reference_store);
    reference_filter->set_visible_func([this](const Gtk::TreeModel::const_iterator &iter) {
        std::string text = reference_search.get_text().lowercase();
        if (text.empty())
            return true;
        Glib::ustring name = (*iter)[ref_name];
        if (name.empty())
        {
            for (auto &child : iter->children())
            {
                Glib::ustring child_name = child[ref_name];
                Glib::ustring tip = child[ref_tip];
                if (child_name.lowercase().find(text) != Glib::ustring::npos || tip.lowercase().find(text) != Glib::ustring::npos)
                    return true;
            }
            return false;
        }
        Glib::ustring tip = (*iter)[ref_tip];
        return name.lowercase().find(text) != Glib::ustring::npos || tip.lowercase().find(text) != Glib::ustring::npos;
    });
    reference.set_model(reference_filter);
    auto renderer = Gtk::manage(new Gtk::CellRendererText);
    auto column = Gtk::manage(new Gtk::TreeViewColumn(_("API Reference")));
    column->pack_start(*renderer);
    column->add_attribute(renderer->property_markup(), ref_label);
    reference.append_column(*column);
    reference.set_tooltip_column(2);
    reference.signal_row_activated().connect([this](const Gtk::TreeModel::Path &path, Gtk::TreeViewColumn *) {
        auto iter = reference_filter->get_iter(path);
        Glib::ustring name = (*iter)[ref_name];
        if (auto doc = current(); doc && !name.empty())
        {
            doc->editor->get_buffer()->insert_at_cursor(name);
            doc->editor->grab_focus();
        }
    });
    reference.get_selection()->signal_changed().connect([this] {
        auto iter = reference.get_selection()->get_selected();
        if (!iter)
            return;
        Glib::ustring name = (*iter)[ref_name];
        if (auto doc = S9xLuaFindApiDoc(name.raw()))
            set_status(std::string(doc->name) + doc->args + " - " + doc->summary);
    });

    reference_search.set_placeholder_text(_("Search functions"));
    reference_search.signal_search_changed().connect([this] {
        reference_filter->refilter();
        if (reference_search.get_text().empty())
            reference.collapse_all();
        else
            reference.expand_all();
    });

    reference_scroll.add(reference);
    reference_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    reference_box.set_size_request(240, -1);
    reference_box.pack_start(reference_search, false, false);
    reference_box.pack_start(reference_scroll, true, true);
}

void LuaWindow::fill_recent_menu()
{
    for (auto child : recent_menu->get_children())
        recent_menu->remove(*child);

    auto &recent = gui_config->lua_recent_scripts;
    for (size_t i = 0; i < recent.size(); i++)
    {
        std::string path = recent[i];
        auto item = Gtk::manage(new Gtk::MenuItem("_" + std::to_string(i) + ": " + path, true));
        item->signal_activate().connect([this, path] { open_script(path, false); });
        recent_menu->append(*item);
    }
    if (recent.empty())
    {
        auto item = Gtk::manage(new Gtk::MenuItem(_("(none)")));
        item->set_sensitive(false);
        recent_menu->append(*item);
    }
    recent_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem));
    auto clear = Gtk::manage(new Gtk::MenuItem(_("Clear Recent Scripts")));
    clear->signal_activate().connect([this] {
        gui_config->lua_recent_scripts.clear();
        fill_recent_menu();
    });
    recent_menu->append(*clear);
    recent_menu->show_all();
}

void LuaWindow::add_recent(const std::string &path)
{
    auto &recent = gui_config->lua_recent_scripts;
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 10)
        recent.resize(10);
    gui_config->lua_last_directory = fs::path(path).parent_path().string();
    fill_recent_menu();
}

LuaWindow::Document *LuaWindow::add_document(const std::string &text, const std::string &path, const std::string &name)
{
    auto doc = std::make_unique<Document>();
    doc->path = path;
    doc->untitled = name.empty() ? "untitled-" + std::to_string(++untitled_count) + ".lua" : name;
    doc->editor = Gtk::manage(new LuaEditor);
    if (font_size > 0)
        doc->editor->set_font_size(font_size);
    doc->editor->get_buffer()->set_text(text);
    doc->editor->clear_history();
    doc->editor->get_buffer()->place_cursor(doc->editor->get_buffer()->begin());
    doc->editor->get_buffer()->set_modified(false);

    doc->page = Gtk::manage(new Gtk::ScrolledWindow);
    doc->page->add(*doc->editor);

    auto tab = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    doc->label = Gtk::manage(new Gtk::Label);
    auto close = Gtk::manage(new Gtk::Button);
    close->set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
    close->set_relief(Gtk::RELIEF_NONE);
    close->set_focus_on_click(false);
    tab->pack_start(*doc->label);
    tab->pack_start(*close);
    tab->show_all();

    Document *raw = doc.get();
    close->signal_clicked().connect([this, raw] { close_document(raw); });
    raw->editor->get_buffer()->signal_modified_changed().connect([this, raw] { update_tab(raw); });
    raw->editor->signal_cursor_moved.connect([this, raw] {
        if (current() != raw)
            return;
        auto iter = raw->editor->get_buffer()->get_insert()->get_iter();
        position.set_text(Glib::ustring::compose(_("Line %1, Col %2"), iter.get_line() + 1, iter.get_line_offset() + 1));
    });
    raw->editor->signal_hint.connect([this](const std::string &text) { set_status(text); });
    raw->editor->signal_breakpoints_changed.connect([this, raw] { breakpoints_changed(raw); });
    raw->editor->signal_breakpoint_menu.connect([this, raw](int line, GdkEventButton *event) {
        show_breakpoint_menu(raw, line, event);
    });
    raw->editor->get_buffer()->signal_changed().connect([this, raw] {
        if (raw->editor->breakpoints.empty())
            return;
        // Lines moved: the list shows where they are now.
        raw->editor->merge_breakpoints();
        refresh_breakpoint_list();
    });

    documents.push_back(std::move(doc));
    int page = notebook.append_page(*raw->page, *tab);
    notebook.set_tab_reorderable(*raw->page, true);
    raw->page->show_all();
    notebook.set_current_page(page);
    update_tab(raw);
    raw->editor->grab_focus();
    return raw;
}

void LuaWindow::open_script(const std::string &path_in, bool run_it)
{
    std::error_code ec;
    std::string path = fs::weakly_canonical(fs::absolute(path_in, ec), ec).string();
    if (ec || path.empty())
        path = path_in;

    for (auto &doc : documents)
    {
        if (doc->path == path)
        {
            notebook.set_current_page(notebook.page_num(*doc->page));
            if (run_it)
                run(doc.get());
            return;
        }
    }

    bool ok = false;
    std::string text = read_file(path, ok);
    if (!ok)
    {
        Gtk::MessageDialog dialog(*this, Glib::ustring::compose(_("Couldn't open %1."), path), false, Gtk::MESSAGE_ERROR);
        dialog.run();
        return;
    }

    // A lone untouched new script makes way for the opened one.
    if (documents.size() == 1 && documents[0]->path.empty() && !documents[0]->script &&
        !documents[0]->editor->get_buffer()->get_modified() && documents[0]->editor->text() == S9xLuaNewScriptTemplate())
    {
        notebook.remove_page(*documents[0]->page);
        documents.clear();
        refresh_breakpoint_list();
    }

    Document *doc = add_document(text, path, "");
    add_recent(path);
    if (run_it)
        run(doc);
}

LuaWindow::Document *LuaWindow::current()
{
    int page = notebook.get_current_page();
    if (page < 0)
        return nullptr;
    auto widget = notebook.get_nth_page(page);
    for (auto &doc : documents)
        if (doc->page == widget)
            return doc.get();
    return nullptr;
}

LuaWindow::Document *LuaWindow::for_script(int id)
{
    if (!id)
        return nullptr;
    for (auto &doc : documents)
        if (doc->script == id)
            return doc.get();
    return nullptr;
}

std::string LuaWindow::display_name(const Document *doc)
{
    return doc->path.empty() ? doc->untitled : fs::path(doc->path).filename().string();
}

void LuaWindow::update_tab(Document *doc)
{
    std::string title = display_name(doc);
    if (doc->editor->get_buffer()->get_modified())
        title += "*";
    if (doc->script)
        title = (doc->paused ? "\u2016 " : "\u25b6 ") + title;
    doc->label->set_text(title);
    doc->label->set_tooltip_text(doc->path.empty() ? display_name(doc) : doc->path);
    update_actions();
}

void LuaWindow::update_actions()
{
    Document *doc = current();
    bool running = doc && doc->script;
    run_button->set_label(running ? _("Restart") : _("Run"));
    run_button->set_icon_name(running ? "view-refresh" : "media-playback-start");
    run_button->set_sensitive(doc != nullptr);
    run_item->set_sensitive(doc != nullptr);
    pause_button->set_sensitive(running);
    pause_item->set_sensitive(running);
    pause_button->set_label(doc && doc->paused ? _("Resume") : _("Pause"));
    stop_button->set_sensitive(running);
    stop_item->set_sensitive(running);
    stop_all_item->set_sensitive(!script_names.empty());
    for (Gtk::Widget *w : std::initializer_list<Gtk::Widget *>{ continue_item, step_over_item, step_into_item, step_out_item,
                                                               continue_button, step_over_button, step_into_button,
                                                               step_out_button })
        w->set_sensitive(stopped != 0);
    break_item->set_sensitive(!stopped && running);

    size_t count = script_names.size();
    if (stopped)
        set_status(stop_text);
    else if (count == 0)
        set_status(_("No scripts running"));
    else if (running)
        set_status(doc->paused ? _("Paused") : count > 1 ? Glib::ustring::compose(_("Running - %1 scripts running"), count) : _("Running"));
    else
        set_status(count == 1 ? _("1 script running") : Glib::ustring::compose(_("%1 scripts running"), count));
}

void LuaWindow::set_status(const std::string &text)
{
    status.set_text(text);
}

bool LuaWindow::save(Document *doc, bool save_as)
{
    std::string path = doc->path;
    if (path.empty() || save_as)
    {
        Gtk::FileChooserDialog dialog(*this, _("Save Lua Script"), Gtk::FILE_CHOOSER_ACTION_SAVE);
        dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("_Save"), Gtk::RESPONSE_ACCEPT);
        dialog.set_do_overwrite_confirmation(true);
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Lua scripts"));
        filter->add_pattern("*.lua");
        dialog.add_filter(filter);
        if (!doc->path.empty())
            dialog.set_filename(doc->path);
        else
        {
            if (!gui_config->lua_last_directory.empty())
                dialog.set_current_folder(gui_config->lua_last_directory);
            dialog.set_current_name(doc->untitled);
        }
        if (dialog.run() != Gtk::RESPONSE_ACCEPT)
            return false;
        path = dialog.get_filename();
        dialog.hide();
        if (fs::path(path).extension().empty())
            path += ".lua";
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    std::string text = doc->editor->text();
    if (!file || !file.write(text.data(), text.size()))
    {
        Gtk::MessageDialog dialog(*this, Glib::ustring::compose(_("Couldn't write %1."), path), false, Gtk::MESSAGE_ERROR);
        dialog.run();
        return false;
    }

    doc->path = path;
    doc->editor->get_buffer()->set_modified(false);
    add_recent(path);
    update_tab(doc);
    set_status(Glib::ustring::compose(_("Saved %1"), path));
    return true;
}

// True when the document can go: saved, discarded or unchanged.
bool LuaWindow::ask_save(Document *doc)
{
    if (!doc->editor->get_buffer()->get_modified())
        return true;

    present();
    notebook.set_current_page(notebook.page_num(*doc->page));
    Gtk::MessageDialog dialog(*this, Glib::ustring::compose(_("Save changes to %1?"), display_name(doc)), false,
                              Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE);
    dialog.add_button(_("Close _without Saving"), Gtk::RESPONSE_NO);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("_Save"), Gtk::RESPONSE_YES);
    dialog.set_default_response(Gtk::RESPONSE_YES);
    int response = dialog.run();
    dialog.hide();
    if (response == Gtk::RESPONSE_YES)
        return save(doc, false);
    return response == Gtk::RESPONSE_NO;
}

bool LuaWindow::close_document(Document *doc)
{
    if (!doc || !ask_save(doc))
        return false;
    if (doc->script)
        stop(doc);
    notebook.remove_page(*doc->page);
    documents.erase(std::remove_if(documents.begin(), documents.end(),
                                   [doc](const std::unique_ptr<Document> &d) { return d.get() == doc; }),
                    documents.end());
    refresh_breakpoint_list();
    update_actions();
    return true;
}

bool LuaWindow::confirm_close()
{
    for (auto &doc : documents)
        if (!ask_save(doc.get()))
            return false;
    return true;
}

void LuaWindow::save_session()
{
    gui_config->lua_open_scripts.clear();
    for (int i = 0; i < notebook.get_n_pages(); i++)
        for (auto &doc : documents)
            if (doc->page == notebook.get_nth_page(i) && !doc->path.empty())
                gui_config->lua_open_scripts.push_back(doc->path);

    int width = 0, height = 0;
    get_size(width, height);
    gui_config->lua_window_width = width;
    gui_config->lua_window_height = height;
    gui_config->lua_show_reference = reference_item->get_active();
    gui_config->lua_font_size = font_size;
    if (editor_split.get_allocated_height() > 1)
        gui_config->lua_panel_height = editor_split.get_allocated_height() - editor_split.get_position();
}

bool LuaWindow::on_delete_event(GdkEventAny *event)
{
    // Closing only hides the editor; scripts keep running, a stopped one too.
    save_session();
    debug_command(S9X_LUA_DEBUG_CONTINUE);
    hide();
    return true;
}

void LuaWindow::run(Document *doc)
{
    if (doc->script)
        stop(doc);
    doc->script = S9xLuaNewScriptId();
    doc->paused = false;
    doc->editor->set_error_line(0);
    S9xLuaRun(doc->script, doc->editor->text(), display_name(doc), doc->path);
    send_breakpoints(doc);
    update_tab(doc);
}

void LuaWindow::stop(Document *doc)
{
    if (!doc->script)
        return;
    S9xLuaStop(doc->script);
    doc->script = 0;
    doc->paused = false;
    update_tab(doc);
}

void LuaWindow::toggle_pause(Document *doc)
{
    if (doc->script)
        S9xLuaPause(doc->script, !doc->paused);
}

void LuaWindow::stop_all()
{
    S9xLuaStop(0);
    for (auto &doc : documents)
    {
        doc->script = 0;
        doc->paused = false;
        update_tab(doc.get());
    }
}

void LuaWindow::zoom(int steps)
{
    if (steps == 0)
        font_size = 0;
    else
        font_size = std::clamp((font_size > 0 ? font_size : 10) + steps, 6, 40);
    for (auto &doc : documents)
        doc->editor->set_font_size(font_size);
}

void LuaWindow::append_console(const std::string &text, const Glib::RefPtr<Gtk::TextTag> &tag)
{
    auto buffer = console.get_buffer();
    auto adjustment = console_scroll.get_vadjustment();
    bool at_bottom = adjustment->get_value() >= adjustment->get_upper() - adjustment->get_page_size() - 4;

    if (buffer->size() > 0)
        buffer->insert(buffer->end(), "\n");
    if (tag)
        buffer->insert_with_tag(buffer->end(), text, tag);
    else
        buffer->insert(buffer->end(), text);

    // Keep the newest 5000 lines.
    int extra = buffer->get_line_count() - 5000;
    if (extra > 0)
        buffer->erase(buffer->begin(), buffer->get_iter_at_line(extra));

    if (at_bottom)
    {
        auto end = buffer->create_mark(buffer->end());
        console.scroll_to(end);
        buffer->delete_mark(end);
    }
}

bool LuaWindow::poll()
{
    poll_debug();
    std::vector<S9xLuaEvent> events;
    if (!S9xLuaTakeEvents(events))
        return true;

    auto name_of = [this](int id) {
        for (auto &entry : script_names)
            if (entry.first == id)
                return entry.second;
        return std::string();
    };

    for (auto &e : events)
    {
        Document *doc = for_script(e.script);
        std::string prefix = script_names.size() > 1 ? "[" + name_of(e.script) + "] " : "";

        switch (e.kind)
        {
        case S9X_LUA_EVENT_STARTED:
            if (name_of(e.script).empty())
                script_names.push_back({ e.script, e.text });
            append_console("\u25b6 " + Glib::ustring::compose(_("%1 started"), e.text), tag_quiet);
            break;
        case S9X_LUA_EVENT_PRINT:
            append_console(prefix + e.text, {});
            break;
        case S9X_LUA_EVENT_INFO:
            append_console(prefix + e.text, tag_info);
            break;
        case S9X_LUA_EVENT_WARNING:
            append_console(prefix + e.text, tag_warning);
            break;
        case S9X_LUA_EVENT_ERROR:
            append_console(prefix + e.text, tag_error);
            break;
        case S9X_LUA_EVENT_PAUSED:
        case S9X_LUA_EVENT_RESUMED:
            if (doc)
            {
                doc->paused = e.kind == S9X_LUA_EVENT_PAUSED;
                update_tab(doc);
            }
            append_console(e.kind == S9X_LUA_EVENT_PAUSED ? "\u2016 " + Glib::ustring::compose(_("%1 paused"), e.text)
                                                          : "\u25b6 " + Glib::ustring::compose(_("%1 resumed"), e.text),
                           tag_quiet);
            break;
        case S9X_LUA_EVENT_ENDED:
        {
            std::string name = name_of(e.script);
            script_names.erase(std::remove_if(script_names.begin(), script_names.end(),
                                              [&](const std::pair<int, std::string> &p) { return p.first == e.script; }),
                               script_names.end());
            if (doc)
            {
                doc->script = 0;
                doc->paused = false;
                if (e.line > 0)
                    doc->editor->set_error_line(e.line);
                update_tab(doc);
            }
            if (e.text == "failed")
                append_console("\u25a0 " + Glib::ustring::compose(_("%1 stopped with an error"), name), tag_error);
            else if (e.text == "finished")
                append_console("\u25a0 " + Glib::ustring::compose(_("%1 finished"), name), tag_quiet);
            else
                append_console("\u25a0 " + Glib::ustring::compose(_("%1 stopped"), name), tag_quiet);
            break;
        }
        case S9X_LUA_EVENT_CLEAR:
            console.get_buffer()->set_text("");
            break;
        }
    }
    update_actions();
    return true;
}

bool LuaWindow::console_click(GdkEventButton *event)
{
    if (event->type != GDK_2BUTTON_PRESS || event->button != 1)
        return false;

    int bx = 0, by = 0;
    console.window_to_buffer_coords(Gtk::TEXT_WINDOW_WIDGET, (int)event->x, (int)event->y, bx, by);
    Gtk::TextIter iter;
    console.get_iter_at_location(iter, bx, by);
    auto start = iter;
    start.set_line_offset(0);
    auto end = start;
    if (!end.ends_line())
        end.forward_to_line_end();
    std::string line = console.get_buffer()->get_text(start, end, true).raw();

    static const std::regex location("([^\\s:\"'\\[\\]]+\\.lua):(\\d+):");
    std::smatch match;
    if (!std::regex_search(line, match, location))
        return false;
    for (auto &doc : documents)
    {
        if (display_name(doc.get()) == match[1].str())
        {
            notebook.set_current_page(notebook.page_num(*doc->page));
            doc->editor->go_to_line(std::stoi(match[2].str()));
            return true;
        }
    }
    return false;
}

void LuaWindow::find(bool backward)
{
    Document *doc = current();
    std::string needle = find_entry.get_text().raw();
    if (!doc || needle.empty())
        return;

    auto buffer = doc->editor->get_buffer();
    Gtk::TextIter from, to, match_start, match_end;
    buffer->get_selection_bounds(from, to);
    auto flags = Gtk::TEXT_SEARCH_TEXT_ONLY;
    bool found = backward ? from.backward_search(needle, flags, match_start, match_end)
                          : to.forward_search(needle, flags, match_start, match_end);
    if (!found) // wrap around once
        found = backward ? buffer->end().backward_search(needle, flags, match_start, match_end)
                         : buffer->begin().forward_search(needle, flags, match_start, match_end);
    if (!found)
    {
        set_status(Glib::ustring::compose(_("\"%1\" not found"), needle));
        return;
    }
    buffer->select_range(match_start, match_end);
    doc->editor->scroll_to(buffer->get_insert(), 0.2);
}

void LuaWindow::go_to_line()
{
    Document *doc = current();
    if (!doc)
        return;
    Gtk::Dialog dialog(_("Go to Line"), *this, true);
    Gtk::SpinButton spin;
    int lines = doc->editor->get_buffer()->get_line_count();
    spin.set_range(1, lines);
    spin.set_increments(1, 10);
    spin.set_value(doc->editor->get_buffer()->get_insert()->get_iter().get_line() + 1);
    spin.set_activates_default(true);
    dialog.get_content_area()->pack_start(spin, true, true, 8);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("_Go"), Gtk::RESPONSE_OK);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    dialog.show_all();
    if (dialog.run() == Gtk::RESPONSE_OK)
        doc->editor->go_to_line(spin.get_value_as_int());
}

/* ---- Debugger ----------------------------------------------------------- */

// Where the script is: the innermost frame with a line.
int top_frame(const std::vector<S9xLuaDebugFrame> &frames)
{
    for (size_t i = 0; i < frames.size(); i++)
        if (frames[i].line > 0)
            return (int)i;
    return 0;
}

Gtk::TreeModel::Path row_path(int row)
{
    return Gtk::TreeModel::Path(Glib::ustring(std::to_string(row)));
}

void LuaWindow::build_debug_panels()
{
    auto scrolled = [](Gtk::ScrolledWindow &scroll, Gtk::Widget &child) {
        scroll.add(child);
        scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    };

    var_columns.add(var_text);
    var_columns.add(var_ref);
    var_columns.add(var_stand_in);
    variables_store = Gtk::TreeStore::create(var_columns);
    watch_store = Gtk::TreeStore::create(var_columns);
    for (Gtk::TreeView *view : { &variables_view, &watch_view })
    {
        auto store = view == &variables_view ? variables_store : watch_store;
        view->set_model(store);
        view->append_column("", var_text);
        view->set_headers_visible(false);
        view->set_enable_search(false);
        view->signal_row_expanded().connect([this, store](const Gtk::TreeModel::iterator &row, const Gtk::TreeModel::Path &) {
            bool stand_in = false;
            for (auto &child : row->children())
            {
                stand_in = child[var_stand_in];
                break;
            }
            if (stand_in)
                request_children(store, row);
        });
        // Long values are cut off in the tree; the status bar has room.
        view->get_selection()->signal_changed().connect([this, view] {
            if (auto row = view->get_selection()->get_selected())
                set_status(Glib::ustring((*row)[var_text]).raw());
        });
    }
    scrolled(variables_scroll, variables_view);
    panel_tabs.append_page(variables_scroll, _("Variables"));

    watch_entry.set_placeholder_text(_("Add a watch: type an expression and press Enter"));
    watch_entry.signal_activate().connect([this] {
        add_watch(watch_entry.get_text().raw());
        watch_entry.set_text("");
    });
    watch_view.signal_key_press_event().connect([this](GdkEventKey *event) {
        if (event->keyval != GDK_KEY_Delete)
            return false;
        remove_watch();
        return true;
    }, false);
    watch_view.signal_button_press_event().connect([this](GdkEventButton *event) {
        if (event->type != GDK_BUTTON_PRESS || event->button != 3)
            return false;
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn *column = nullptr;
        int cell_x = 0, cell_y = 0;
        if (watch_view.get_path_at_pos((int)event->x, (int)event->y, path, column, cell_x, cell_y))
            watch_view.get_selection()->select(path);
        popup({ { _("Remove Watch"), [this] { remove_watch(); } } }, event);
        return true;
    }, false);
    scrolled(watch_scroll, watch_view);
    watch_box.pack_start(watch_entry, false, false);
    watch_box.pack_start(watch_scroll, true, true);
    panel_tabs.append_page(watch_box, _("Watch"));

    stack_columns.add(stack_function);
    stack_columns.add(stack_location);
    stack_store = Gtk::ListStore::create(stack_columns);
    stack_view.set_model(stack_store);
    stack_view.append_column(_("Function"), stack_function);
    stack_view.append_column(_("Location"), stack_location);
    stack_view.get_column(0)->set_resizable(true);
    stack_view.get_column(0)->set_min_width(220);
    stack_view.get_selection()->signal_changed().connect([this] {
        if (filling)
            return;
        if (auto row = stack_view.get_selection()->get_selected())
        {
            int index = stack_store->get_path(row)[0];
            if (index != frame)
                select_frame(index);
        }
    });
    scrolled(stack_scroll, stack_view);
    panel_tabs.append_page(stack_scroll, _("Call Stack"));

    bp_columns.add(bp_enabled);
    bp_columns.add(bp_location);
    bp_columns.add(bp_condition);
    bp_columns.add(bp_hit);
    bp_columns.add(bp_log);
    bp_store = Gtk::ListStore::create(bp_columns);
    bp_view.set_model(bp_store);
    auto toggle = Gtk::manage(new Gtk::CellRendererToggle);
    toggle->signal_toggled().connect([this](const Glib::ustring &path) {
        Document *doc = nullptr;
        LuaEditor::Breakpoint *b = breakpoint_for_row(Gtk::TreeModel::Path(path)[0], &doc);
        if (!b)
            return;
        b->enabled = !b->enabled;
        (*bp_store->get_iter(path))[bp_enabled] = b->enabled;
        send_breakpoints(doc);
        doc->editor->refresh_gutter();
    });
    auto enabled_column = Gtk::manage(new Gtk::TreeViewColumn(""));
    enabled_column->pack_start(*toggle, false);
    enabled_column->add_attribute(toggle->property_active(), bp_enabled);
    bp_view.append_column(*enabled_column);
    bp_view.append_column(_("Location"), bp_location);
    bp_view.append_column(_("Condition"), bp_condition);
    bp_view.append_column(_("Hit Count"), bp_hit);
    bp_view.append_column(_("Log Message"), bp_log);
    for (int i = 1; i < 5; i++)
        bp_view.get_column(i)->set_resizable(true);
    bp_view.get_column(1)->set_min_width(160);
    bp_view.get_column(2)->set_min_width(160);
    bp_view.get_selection()->signal_changed().connect([this] {
        if (!filling)
            fill_breakpoint_fields();
    });
    bp_view.signal_row_activated().connect([this](const Gtk::TreeModel::Path &path, Gtk::TreeViewColumn *) {
        go_to_breakpoint(path[0]);
    });
    bp_view.signal_key_press_event().connect([this](GdkEventKey *event) {
        if (event->keyval != GDK_KEY_Delete)
            return false;
        remove_breakpoint_row(selected_breakpoint_row());
        return true;
    }, false);
    bp_view.signal_button_press_event().connect([this](GdkEventButton *event) {
        if (event->type != GDK_BUTTON_PRESS || event->button != 3)
            return false;
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn *column = nullptr;
        int cell_x = 0, cell_y = 0;
        if (bp_view.get_path_at_pos((int)event->x, (int)event->y, path, column, cell_x, cell_y))
            bp_view.get_selection()->select(path);
        int row = selected_breakpoint_row();
        std::function<void()> remove, remove_all;
        if (row >= 0)
            remove = [this, row] { remove_breakpoint_row(row); };
        if (!bp_rows.empty())
            remove_all = [this] { remove_all_breakpoints(); };
        popup({ { _("Remove Breakpoint"), remove }, { _("Remove All Breakpoints"), remove_all } }, event);
        return true;
    }, false);
    scrolled(bp_scroll, bp_view);

    auto fields = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    fields->set_margin_start(4);
    fields->set_margin_end(4);
    fields->set_margin_top(2);
    fields->set_margin_bottom(2);
    const char *labels[3] = { _("Condition:"), _("Hit count:"), _("Log message:") };
    const char *cues[3] = { "lives < 3", "5, >= 5 or % 5", "x is {x}" };
    for (int i = 0; i < 3; i++)
    {
        fields->pack_start(*Gtk::manage(new Gtk::Label(labels[i])), false, false);
        bp_fields[i].set_placeholder_text(cues[i]);
        bp_fields[i].set_sensitive(false);
        // Enter or leaving the field applies it.
        bp_fields[i].signal_activate().connect([this] { apply_breakpoint_fields(); });
        bp_fields[i].signal_focus_out_event().connect([this](GdkEventFocus *) {
            apply_breakpoint_fields();
            return false;
        });
        if (i == 1)
            bp_fields[i].set_width_chars(12);
        fields->pack_start(bp_fields[i], i != 1, i != 1);
    }
    bp_box.pack_start(bp_scroll, true, true);
    bp_box.pack_start(*fields, false, false);
    panel_tabs.append_page(bp_box, _("Breakpoints"));
}

// A right-click menu; an item without an action shows greyed, one without a label is a line.
void LuaWindow::popup(std::vector<std::pair<std::string, std::function<void()>>> items, GdkEventButton *event)
{
    popup_menu = std::make_unique<Gtk::Menu>();
    for (auto &item : items)
    {
        if (item.first.empty())
        {
            popup_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem));
            continue;
        }
        auto menu_item = Gtk::manage(new Gtk::MenuItem(item.first, true));
        if (item.second)
            menu_item->signal_activate().connect(item.second);
        else
            menu_item->set_sensitive(false);
        popup_menu->append(*menu_item);
    }
    popup_menu->show_all();
    popup_menu->popup_at_pointer((GdkEvent *)event);
}

void LuaWindow::debug_command(S9xLuaDebugAction action)
{
    if (int script = S9xLuaDebugStoppedScript())
        S9xLuaDebugCommand(script, action);
}

void LuaWindow::hold()
{
    if (!holding)
    {
        // The script holds the frame: the other windows wait with it.
        holding = true;
        for (Gtk::Window *w : Gtk::Window::list_toplevels())
        {
            if (w == this || !w->get_visible() || !w->get_sensitive() || w->get_transient_for() == this ||
                gtk_window_get_window_type(w->gobj()) == GTK_WINDOW_POPUP)
                continue;
            w->set_sensitive(false);
            held_windows.push_back(GTK_WIDGET(g_object_ref(w->gobj())));
        }
        S9xSoundStop();
    }
    present();
    poll();
}

void LuaWindow::release_hold()
{
    if (!holding || S9xLuaDebugStoppedScript())
        return; // a step that stopped again keeps the hold
    holding = false;
    for (GtkWidget *w : held_windows)
    {
        gtk_widget_set_sensitive(w, TRUE);
        g_object_unref(w);
    }
    held_windows.clear();
    if (gui_config->rom_loaded && !Settings.Paused)
        S9xSoundStart();
    poll();
}

void LuaWindow::forget_breakpoints()
{
    for (auto &doc : documents)
        if (doc->script)
            S9xLuaDebugSetBreakpoints(doc->script, {});
}

void LuaWindow::poll_debug()
{
    std::vector<S9xLuaDebugEvent> events;
    if (!S9xLuaDebugTakeEvents(events))
        return;
    bool resumed = false;
    for (auto &e : events)
    {
        switch (e.kind)
        {
        case S9X_LUA_DEBUG_STOPPED:
            resumed = false;
            on_stopped(e);
            break;
        case S9X_LUA_DEBUG_RESUMED:
            stopped = 0;
            resumed = true;
            break;
        case S9X_LUA_DEBUG_VARIABLES:
            on_variables(e);
            break;
        case S9X_LUA_DEBUG_EVALUATED:
            on_evaluated(e);
            break;
        }
    }
    // A step stops again at once: only clear the views if it didn't.
    if (resumed && !stopped)
        clear_debug_views();
}

void LuaWindow::on_stopped(const S9xLuaDebugEvent &e)
{
    stopped = e.script;
    frames = e.frames;
    int top = top_frame(frames);
    if (e.reason == "error")
        stop_text = _("Stopped on an error: ") + e.message;
    else
    {
        const char *why = e.reason == "breakpoint" ? _("breakpoint") : e.reason == "step" ? _("step") : _("break");
        stop_text = Glib::ustring::compose(_("Paused at line %1 (%2)"), frames.empty() ? 0 : frames[top].line, why);
    }
    fill_call_stack();
    if (frames.empty())
    {
        fill_variables();
        fill_watches();
    }
    else
        select_frame(top);
    if (panel_tabs.get_current_page() == 0)
        panel_tabs.set_current_page(panel_tabs.page_num(variables_scroll));
    update_actions();
}

Gtk::TreeModel::iterator LuaWindow::add_variable(const Glib::RefPtr<Gtk::TreeStore> &store,
                                                 const Gtk::TreeModel::iterator *parent, const std::string &text, int ref)
{
    auto row = parent ? store->append((*parent)->children()) : store->append();
    (*row)[var_text] = text;
    (*row)[var_ref] = ref;
    (*row)[var_stand_in] = false;
    if (ref)
    {
        auto stand_in = store->append(row->children());
        (*stand_in)[var_text] = "...";
        (*stand_in)[var_ref] = 0;
        (*stand_in)[var_stand_in] = true;
    }
    return row;
}

void LuaWindow::set_children(const Glib::RefPtr<Gtk::TreeStore> &store, const Gtk::TreeModel::iterator &row,
                             const std::vector<S9xLuaDebugVariable> &variables)
{
    GtkTreeIter child;
    auto parent = const_cast<GtkTreeIter *>(row.gobj());
    while (gtk_tree_model_iter_children(GTK_TREE_MODEL(store->gobj()), &child, parent))
        gtk_tree_store_remove(store->gobj(), &child);
    for (auto &v : variables)
        add_variable(store, &row, v.name + " = " + v.value, v.ref);
}

void LuaWindow::on_variables(const S9xLuaDebugEvent &e)
{
    for (size_t i = 0; i < pending.size();)
    {
        if (pending[i].ref != e.ref)
        {
            i++;
            continue;
        }
        Pending p = pending[i];
        pending.erase(pending.begin() + i);
        if (!p.row.is_valid())
            continue;
        auto path = p.row.get_path();
        set_children(p.store, p.store->get_iter(path), e.variables);
        auto &view = p.store == variables_store ? variables_view : watch_view;
        if (!e.variables.empty())
            view.expand_row(path, false);
    }
}

void LuaWindow::on_evaluated(const S9xLuaDebugEvent &e)
{
    int generation = e.request / 1000, index = e.request % 1000;
    if (generation != watch_generation || index >= (int)watches.size() || e.variables.empty())
        return;
    auto row = watch_store->get_iter(Glib::ustring(std::to_string(index)));
    if (!row)
        return;
    const S9xLuaDebugVariable &v = e.variables[0];
    int ref = e.failed ? 0 : v.ref;
    set_children(watch_store, row, {});
    (*row)[var_text] = watches[index] + " = " + (e.failed ? std::string(_("error: ")) : std::string()) + v.value;
    (*row)[var_ref] = ref;
    if (ref)
    {
        auto stand_in = watch_store->append(row->children());
        (*stand_in)[var_text] = "...";
        (*stand_in)[var_stand_in] = true;
    }
}

void LuaWindow::clear_debug_views()
{
    frames.clear();
    pending.clear();
    for (auto &doc : documents)
        doc->editor->set_debug_lines(0, 0);
    filling = true;
    stack_store->clear();
    filling = false;
    clear_tree(variables_store);
    fill_watches();
    update_actions();
}

void LuaWindow::select_frame(int index)
{
    if (index < 0 || index >= (int)frames.size())
        return;
    frame = index;

    int top = top_frame(frames);
    Document *stop_doc = frame_document(frames[top]);
    Document *shown = frame_document(frames[index]);
    for (auto &doc : documents)
        doc->editor->set_debug_lines(doc.get() == stop_doc ? frames[top].line : 0,
                                     doc.get() == shown && index != top ? frames[index].line : 0);
    if (shown)
    {
        notebook.set_current_page(notebook.page_num(*shown->page));
        shown->editor->show_line(frames[index].line);
    }

    filling = true;
    stack_view.get_selection()->select(row_path(index));
    filling = false;
    fill_variables();
    fill_watches();
}

// The document a frame's code is in, opening its file if it has to.
LuaWindow::Document *LuaWindow::frame_document(const S9xLuaDebugFrame &f)
{
    if (f.line <= 0)
        return nullptr;
    if (f.own)
        return for_script(stopped);
    if (f.path.empty())
        return nullptr;
    std::error_code ec;
    std::string path = fs::weakly_canonical(fs::absolute(f.path, ec), ec).string();
    if (ec || path.empty())
        path = f.path;
    auto find = [&]() -> Document * {
        for (auto &doc : documents)
            if (doc->path == path)
                return doc.get();
        return nullptr;
    };
    if (Document *doc = find())
        return doc;
    if (!fs::is_regular_file(path, ec))
        return nullptr;
    open_script(path, false);
    return find();
}

void LuaWindow::clear_tree(const Glib::RefPtr<Gtk::TreeStore> &store)
{
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&store](const Pending &p) { return p.store == store; }),
                  pending.end());
    store->clear();
}

// A variable request whose answer fills the row's children.
void LuaWindow::request_children(const Glib::RefPtr<Gtk::TreeStore> &store, const Gtk::TreeModel::iterator &row)
{
    int ref = (*row)[var_ref];
    if (!stopped || !ref)
        return;
    pending.push_back({ ref, store, Gtk::TreeRowReference(store, store->get_path(row)) });
    S9xLuaDebugRequestVariables(stopped, ref);
}

void LuaWindow::fill_variables()
{
    clear_tree(variables_store);
    if (!stopped || frame >= (int)frames.size())
        return;
    const char *scopes[3] = { _("Locals"), _("Upvalues"), _("Globals") };
    for (int scope = 0; scope < 3; scope++)
    {
        auto row = add_variable(variables_store, nullptr, scopes[scope], S9xLuaDebugScopeRef(frame, scope));
        if (scope == 0)
            request_children(variables_store, row);
    }
}

// Every watch, evaluated again in the selected frame.
void LuaWindow::fill_watches()
{
    clear_tree(watch_store);
    watch_generation++;
    for (size_t i = 0; i < watches.size(); i++)
    {
        add_variable(watch_store, nullptr, watches[i], 0);
        if (stopped)
            S9xLuaDebugEvaluate(stopped, frame, watches[i], watch_generation * 1000 + (int)i);
    }
}

void LuaWindow::fill_call_stack()
{
    filling = true;
    stack_store->clear();
    for (auto &f : frames)
    {
        auto row = stack_store->append();
        (*row)[stack_function] = f.function;
        (*row)[stack_location] = f.line ? f.source + ":" + std::to_string(f.line) : f.source.empty() ? "[C]" : f.source;
    }
    filling = false;
}

void LuaWindow::add_watch(const std::string &expression)
{
    size_t first = expression.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || watches.size() >= 999)
        return;
    watches.push_back(expression.substr(first, expression.find_last_not_of(" \t\r\n") - first + 1));
    fill_watches();
}

void LuaWindow::remove_watch()
{
    auto row = watch_view.get_selection()->get_selected();
    if (!row || row->parent())
        return;
    size_t index = watch_store->get_path(row)[0];
    if (index >= watches.size())
        return;
    watches.erase(watches.begin() + index);
    fill_watches();
}

// To the running script; its code is what was run, so lines are as they are now.
void LuaWindow::send_breakpoints(Document *doc)
{
    if (!doc->script)
        return;
    std::vector<S9xLuaBreakpoint> list;
    for (auto &b : doc->editor->breakpoints)
    {
        S9xLuaBreakpoint bp;
        bp.line = doc->editor->breakpoint_line(b);
        bp.enabled = b.enabled;
        bp.condition = b.condition;
        bp.hit_condition = b.hit;
        bp.log_message = b.log;
        list.push_back(bp);
    }
    S9xLuaDebugSetBreakpoints(doc->script, list);
}

void LuaWindow::breakpoints_changed(Document *doc)
{
    doc->editor->merge_breakpoints();
    send_breakpoints(doc);
    refresh_breakpoint_list();
    doc->editor->refresh_gutter();
}

void LuaWindow::refresh_breakpoint_list()
{
    if (!bp_store)
        return;
    filling = true;
    int selected = selected_breakpoint_row();
    bp_store->clear();
    bp_rows.clear();
    for (auto &doc : documents)
    {
        auto &list = doc->editor->breakpoints;
        std::vector<std::pair<int, size_t>> order;
        for (size_t i = 0; i < list.size(); i++)
            order.push_back({ doc->editor->breakpoint_line(list[i]), i });
        std::sort(order.begin(), order.end());
        for (auto &entry : order)
        {
            const LuaEditor::Breakpoint &b = list[entry.second];
            bp_rows.push_back({ doc.get(), entry.second });
            auto row = bp_store->append();
            (*row)[bp_enabled] = b.enabled;
            (*row)[bp_location] = display_name(doc.get()) + ":" + std::to_string(entry.first);
            (*row)[bp_condition] = b.condition;
            (*row)[bp_hit] = b.hit;
            (*row)[bp_log] = b.log;
        }
    }
    if (selected >= 0 && selected < (int)bp_rows.size())
        bp_view.get_selection()->select(row_path(selected));
    filling = false;
    fill_breakpoint_fields();
}

LuaEditor::Breakpoint *LuaWindow::breakpoint_for_row(int row, Document **owner)
{
    if (row < 0 || row >= (int)bp_rows.size())
        return nullptr;
    Document *doc = bp_rows[row].first;
    size_t index = bp_rows[row].second;
    bool open = std::any_of(documents.begin(), documents.end(),
                            [doc](const std::unique_ptr<Document> &d) { return d.get() == doc; });
    if (!open || index >= doc->editor->breakpoints.size())
        return nullptr;
    if (owner)
        *owner = doc;
    return &doc->editor->breakpoints[index];
}

int LuaWindow::selected_breakpoint_row()
{
    auto row = bp_view.get_selection()->get_selected();
    return row ? bp_store->get_path(row)[0] : -1;
}

void LuaWindow::fill_breakpoint_fields()
{
    LuaEditor::Breakpoint *b = breakpoint_for_row(selected_breakpoint_row(), nullptr);
    const std::string *values[3] = { b ? &b->condition : nullptr, b ? &b->hit : nullptr, b ? &b->log : nullptr };
    for (int i = 0; i < 3; i++)
    {
        bp_fields[i].set_text(values[i] ? *values[i] : std::string());
        bp_fields[i].set_sensitive(b != nullptr);
    }
}

void LuaWindow::apply_breakpoint_fields()
{
    int row = selected_breakpoint_row();
    Document *doc = nullptr;
    LuaEditor::Breakpoint *b = breakpoint_for_row(row, &doc);
    if (!b)
        return;
    auto trim = [](const std::string &s) {
        size_t first = s.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : s.substr(first, s.find_last_not_of(" \t") - first + 1);
    };
    std::string condition = trim(bp_fields[0].get_text().raw()), hit = trim(bp_fields[1].get_text().raw());
    std::string log = bp_fields[2].get_text().raw();
    if (condition == b->condition && hit == b->hit && log == b->log)
        return;
    b->condition = condition;
    b->hit = hit;
    b->log = log;
    send_breakpoints(doc);

    // The row in place: a rebuild here could pull the list out from under a click.
    if (auto iter = bp_store->get_iter(Glib::ustring(std::to_string(row))))
    {
        (*iter)[bp_condition] = condition;
        (*iter)[bp_hit] = hit;
        (*iter)[bp_log] = log;
    }
    doc->editor->refresh_gutter();
}

void LuaWindow::edit_breakpoint(Document *doc, int line)
{
    if (!doc->editor->breakpoint_at(line))
        doc->editor->toggle_breakpoint(line);
    panel_tabs.set_current_page(panel_tabs.page_num(bp_box));
    for (size_t row = 0; row < bp_rows.size(); row++)
    {
        if (bp_rows[row].first != doc ||
            doc->editor->breakpoint_line(doc->editor->breakpoints[bp_rows[row].second]) != line)
            continue;
        bp_view.get_selection()->select(row_path((int)row));
        bp_view.scroll_to_row(row_path((int)row));
        break;
    }
    bp_fields[0].grab_focus();
}

void LuaWindow::remove_breakpoint_row(int row)
{
    Document *doc = nullptr;
    if (LuaEditor::Breakpoint *b = breakpoint_for_row(row, &doc))
        doc->editor->toggle_breakpoint(doc->editor->breakpoint_line(*b));
}

void LuaWindow::remove_all_breakpoints()
{
    for (auto &doc : documents)
    {
        doc->editor->clear_breakpoints();
        send_breakpoints(doc.get());
    }
    refresh_breakpoint_list();
}

void LuaWindow::go_to_breakpoint(int row)
{
    Document *doc = nullptr;
    if (LuaEditor::Breakpoint *b = breakpoint_for_row(row, &doc))
    {
        notebook.set_current_page(notebook.page_num(*doc->page));
        doc->editor->go_to_line(doc->editor->breakpoint_line(*b));
    }
}

void LuaWindow::show_breakpoint_menu(Document *doc, int line, GdkEventButton *event)
{
    LuaEditor *editor = doc->editor;
    LuaEditor::Breakpoint *b = editor->breakpoint_at(line);
    std::vector<std::pair<std::string, std::function<void()>>> items;
    items.push_back({ b ? _("Remove Breakpoint") : _("Add Breakpoint"), [editor, line] { editor->toggle_breakpoint(line); } });
    items.push_back({ b ? _("Edit Breakpoint...") : _("Add Conditional Breakpoint..."),
                      [this, doc, line] { edit_breakpoint(doc, line); } });
    if (b)
    {
        items.push_back({ b->enabled ? _("Disable Breakpoint") : _("Enable Breakpoint"), [this, doc, line] {
            if (auto bp = doc->editor->breakpoint_at(line))
            {
                bp->enabled = !bp->enabled;
                breakpoints_changed(doc);
            }
        } });
    }
    items.push_back({ "", nullptr });
    std::function<void()> remove_all;
    if (!bp_rows.empty())
        remove_all = [this] { remove_all_breakpoints(); };
    items.push_back({ _("Remove All Breakpoints"), remove_all });
    popup(items, event);
}

// The engine's stop callback, inside the script's hook.
void debugger_stopped(int script, bool stopped)
{
    if (stopped)
    {
        if (quitting)
        {
            S9xLuaDebugCommand(script, S9X_LUA_DEBUG_STOP);
            return;
        }
        if (!lua_window)
            lua_window = new LuaWindow;
        lua_window->hold();
        return;
    }
    // Later, from the main loop: a step that stops again at once keeps the hold.
    Glib::signal_timeout().connect_once([] {
        if (lua_window)
            lua_window->release_hold();
    }, 0);
}

} // namespace

void S9xLuaInstallFrontend()
{
    S9xLuaFrontend frontend;
    frontend.set_paused = [](bool paused) {
        if (paused)
            top_level->pause_from_user();
        else
            top_level->unpause_from_user();
    };
    frontend.reset = [](bool power) {
        // As Emulation > Reset / Hard Reset.
        S9xMovieUpdateOnReset();
        if (S9xMoviePlaying())
            S9xMovieStop(true);
        if (power)
            S9xReset();
        else
            S9xSoftReset();
#ifdef RETROACHIEVEMENTS_SUPPORT
        RA_OnReset();
#endif
        S9xLuaNotifyReset(power);
    };
    frontend.default_dir = [] {
        // The emulator's own folder; the config folder when that's read-only.
        std::error_code ec;
        fs::path dir = fs::read_symlink("/proc/self/exe", ec).parent_path();
        if (ec || dir.empty() || access(dir.c_str(), W_OK) != 0)
            dir = get_config_dir();
        std::string path = dir.string();
        if (!path.empty() && path.back() != '/')
            path += '/';
        return path;
    };
    // A stop waits inside the hook, running the main loop so the editor works.
    frontend.debug_break = debugger_stopped;
    frontend.debug_wait = [] { gtk_main_iteration_do(TRUE); };
    S9xLuaSetFrontend(frontend);
    S9xLuaDebugSetBreakOnError(gui_config->lua_break_on_error);
}

void S9xShowLuaWindow()
{
    if (!lua_window)
        lua_window = new LuaWindow;
    lua_window->present();
}

void S9xRunLuaScriptFile(const std::string &path)
{
    if (lua_window)
    {
        lua_window->open_script(path, true);
        return;
    }
    S9xLuaRunFile(S9xLuaNewScriptId(), path);
}

bool S9xLuaWindowConfirmClose()
{
    if (lua_window && !lua_window->confirm_close())
        return false;
    // A stopped script is inside the frame: end it, then quit from the main loop.
    if (int script = S9xLuaDebugStoppedScript())
    {
        quitting = true;
        S9xLuaDebugSetBreakOnError(false);
        if (lua_window)
            lua_window->forget_breakpoints();
        S9xLuaDebugCommand(script, S9X_LUA_DEBUG_STOP);
        Glib::signal_idle().connect_once([] { S9xExit(); });
        return false;
    }
    return true;
}

void S9xLuaWindowShutdown()
{
    // No more stops: the scripts' on_stop handlers run next.
    quitting = true;
    S9xLuaDebugSetBreakOnError(false);
    if (lua_window)
    {
        lua_window->forget_breakpoints();
        lua_window->save_session();
        delete lua_window;
        lua_window = nullptr;
    }
    S9xLuaShutdown();
}

bool S9xLuaWindowOpen()
{
    return lua_window && lua_window->get_visible();
}
