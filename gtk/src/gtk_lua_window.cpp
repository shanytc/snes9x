/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_lua_window.h"
#include "gtk_compat.h"
#include "gtk_s9x.h"
#include "gtk_s9xwindow.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

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
    LuaEditor();
    void set_error_line(int line); // 0 clears it
    void go_to_line(int line);
    void toggle_comment();
    void set_font_size(int points);
    std::string text();
    void undo();
    void redo();
    void clear_history();
    sigc::signal<void, const std::string &> signal_hint;
    sigc::signal<void> signal_cursor_moved;

  protected:
    bool on_draw(const Cairo::RefPtr<Cairo::Context> &cr) override;

  private:
    bool on_key(GdkEventKey *event);
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
    Glib::RefPtr<Gtk::TextTag> current_tag, error_tag;
    int error_line = 0;
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

    buffer->remove_tag(error_tag, buffer->begin(), buffer->end());
    if (error_line > 0 && error_line <= buffer->get_line_count())
    {
        auto start = buffer->get_iter_at_line(error_line - 1);
        auto stop = start;
        stop.forward_line();
        buffer->apply_tag(error_tag, start, stop);
    }
    if (auto window = get_window(Gtk::TEXT_WINDOW_LEFT))
        window->invalidate(false);
}

void LuaEditor::set_error_line(int line)
{
    error_line = line;
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

void LuaEditor::update_margin()
{
    int digits = std::max<int>(3, std::to_string(get_buffer()->get_line_count()).size());
    if (digits == margin_digits)
        return;
    margin_digits = digits;
    int width = 0, height = 0;
    create_pango_layout(std::string(digits, '9'))->get_pixel_size(width, height);
    set_border_window_size(Gtk::TEXT_WINDOW_LEFT, width + 10);
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
    while (true)
    {
        int y = 0, height = 0, wx = 0, wy = 0;
        get_line_yrange(iter, y, height);
        if (y > visible.get_y() + visible.get_height())
            break;
        buffer_to_window_coords(Gtk::TEXT_WINDOW_LEFT, 0, y, wx, wy);

        int line = iter.get_line();
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
};

LuaWindow *lua_window = nullptr;

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
    auto console_label = Gtk::manage(new Gtk::Label(_("Console")));
    console_label->set_margin_start(6);
    auto clear_button = Gtk::manage(new Gtk::Button(_("Clear")));
    clear_button->set_relief(Gtk::RELIEF_NONE);
    clear_button->signal_clicked().connect([this] { console.get_buffer()->set_text(""); });
    console_header->pack_start(*console_label, false, false);
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

    editor_split.pack1(notebook, true, false);
    editor_split.pack2(console_box, false, false);
    editor_split.set_position(gui_config->lua_window_height > 0 ? gui_config->lua_window_height - 280 : 370);

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
    run_item = add_item(*script, _("_Run"), [this] { if (auto doc = current()) run(doc); }, GDK_KEY_F5);
    pause_item = add_item(*script, _("_Pause"), [this] { if (auto doc = current()) toggle_pause(doc); }, GDK_KEY_F6);
    stop_item = add_item(*script, _("_Stop"), [this] { if (auto doc = current()) stop(doc); }, GDK_KEY_F5, Gdk::SHIFT_MASK);
    separator(script);
    stop_all_item = add_item(*script, _("Stop _All Scripts"), [this] { stop_all(); });

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

    size_t count = script_names.size();
    if (count == 0)
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
}

bool LuaWindow::on_delete_event(GdkEventAny *event)
{
    // Closing only hides the editor; scripts keep running.
    save_session();
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
    S9xLuaSetFrontend(frontend);
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
    return !lua_window || lua_window->confirm_close();
}

void S9xLuaWindowShutdown()
{
    if (lua_window)
    {
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
