/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_barcodes.h"
#include "gtk_config.h"
#include "gtk_s9x.h"
#include "common/desktop/barcode_card_photos.hpp"
#include "common/video/gb_camera_v4l2.hpp"

#include "snes9x.h"
#include "display.h"
#include "barcodebattler.h"

#include "fmt/format.h"

#include <algorithm>
#include <filesystem>
#include <string.h>
#include <vector>

namespace {

std::string digits_of(const std::string &text)
{
    std::string digits;
    for (char c : text)
        if (c != ' ' && c != '-')
            digits += c;
    return digits;
}

// The folder of the running executable, where a portable build keeps its photo folders.
std::string executable_dir()
{
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.parent_path().string();
}

// One printed card: the run of entries from i sharing its branch and photo
// stem (a card with several barcodes); a single entry when it has no photos.
int card_end(const S9xBarcodeReaderCard *cards, int count, int i)
{
    int end = i + 1;
    if (*cards[i].image)
        while (end < count && !strcmp(cards[end].branch, cards[i].branch) && !strcmp(cards[end].image, cards[i].image))
            end++;
    return end;
}

bool card_matches(const S9xBarcodeReaderCard &card, const Glib::ustring &find)
{
    return find.empty() || Glib::ustring(card.name).casefold().find(find) != Glib::ustring::npos ||
           Glib::ustring(card.kana).casefold().find(find) != Glib::ustring::npos ||
           Glib::ustring(card.branch).casefold().find(find) != Glib::ustring::npos ||
           std::string(card.code).find(find.raw()) != std::string::npos ||
           (card.group && Glib::ustring(card.group).casefold().find(find) != Glib::ustring::npos);
}

// A card with several barcodes is named after them, less their positions.
std::string card_label(const S9xBarcodeReaderCard *cards, int i, int end)
{
    std::vector<std::string> names;
    for (int k = i; k < end; k++)
    {
        std::string name = cards[k].name;
        for (const char *where : { " (Up)", " (Down)", " (Left)", " (Right)", " (Front)", " (Back)" })
        {
            const size_t n = strlen(where);
            if (name.size() > n && !name.compare(name.size() - n, n, where))
            {
                name.erase(name.size() - n);
                break;
            }
        }
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }

    std::string label;
    for (auto &name : names)
        label += (label.empty() ? "" : " / ") + name;
    return label;
}

class BarcodeDialog : public Gtk::Dialog
{
  public:
    BarcodeDialog(Gtk::Window &parent, S9xBarcodeReader reader)
        : Gtk::Dialog(S9xBarcodeReaderName(reader), parent, true), reader(reader)
    {
        const bool battler = reader == S9X_BARCODE_BATTLER;
        auto content = get_content_area();
        content->set_spacing(8);
        content->set_border_width(8);

        auto columns = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));
        auto left = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
        columns->pack_start(*left, Gtk::PACK_EXPAND_WIDGET);
        content->pack_start(*columns, Gtk::PACK_EXPAND_WIDGET);

        auto intro = Gtk::manage(new Gtk::Label(
            battler ? _("Enter a card's 13-digit (EAN-13), 12-digit (UPC-A) or 8-digit (EAN-8) "
                        "barcode, pick a known card, or read it with a camera or from a picture.")
            : reader == S9X_BARCODE_BARCODEBOY
                ? _("Enter a card's 13-digit (JAN/EAN-13) barcode, pick a known card, or read it "
                    "with a camera or from a picture.")
                : _("Enter a card's 13-digit (JAN/EAN-13) or 8-digit (EAN-8) barcode, pick a known "
                    "card, or read it with a camera or from a picture. Shop barcodes work too.")));
        intro->set_line_wrap(true);
        intro->set_max_width_chars(60);
        intro->set_xalign(0.0f);
        left->pack_start(*intro, Gtk::PACK_SHRINK);

        auto code_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        code_box->pack_start(*Gtk::manage(new Gtk::Label(_("Barcode:"))), Gtk::PACK_SHRINK);
        code_entry.set_max_length(20);
        code_entry.set_placeholder_text(_("e.g. 4901234567894"));
        code_entry.set_text(S9xBarcodeReaderLastCode(reader));
        code_entry.set_activates_default(true);
        code_box->pack_start(code_entry, Gtk::PACK_EXPAND_WIDGET);
        auto random_button = Gtk::manage(new Gtk::Button(_("_Random"), true));
        code_box->pack_start(*random_button, Gtk::PACK_SHRINK);
        auto image_button = Gtk::manage(new Gtk::Button(_("From _Image…"), true));
        code_box->pack_start(*image_button, Gtk::PACK_SHRINK);
        left->pack_start(*code_box, Gtk::PACK_SHRINK);

        status_label.set_line_wrap(true);
        status_label.set_max_width_chars(60);
        status_label.set_xalign(0.0f);
        left->pack_start(status_label, Gtk::PACK_SHRINK);

        std::vector<std::string> names;
        S9xGBCameraEnumerate(names);

        auto camera_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        camera_box->pack_start(*Gtk::manage(new Gtk::Label(_("Camera:"))), Gtk::PACK_SHRINK);
        for (auto &name : names)
            camera_combo.append(name);
        if (names.empty())
            camera_combo.append(_("No video camera found"));
        camera_combo.set_active(!names.empty() && Settings.GBVideoCameraIndex < names.size()
                                    ? Settings.GBVideoCameraIndex : 0);
        camera_combo.set_sensitive(!names.empty());
        camera_box->pack_start(camera_combo, Gtk::PACK_EXPAND_WIDGET);
        camera_button.set_label(_("Start _Camera"));
        camera_button.set_use_underline(true);
        camera_button.set_sensitive(!names.empty());
        camera_box->pack_start(camera_button, Gtk::PACK_SHRINK);
        left->pack_start(*camera_box, Gtk::PACK_SHRINK);

        photo_dir = S9xBarcodePhotoDir({ get_config_dir(), executable_dir() }, S9xBarcodeReaderPhotoFolder(reader));

        preview_frame.set_size_request(PREVIEW_WIDTH, PREVIEW_HEIGHT);
        preview_label.set_text(_("Hold the barcode up to the camera, straight and in focus."));
        preview_label.set_line_wrap(true);
        preview_label.set_max_width_chars(40);
        photo_box.set_halign(Gtk::ALIGN_CENTER);
        photo_box.set_valign(Gtk::ALIGN_CENTER);
        barcode_area.signal_draw().connect([this](const Cairo::RefPtr<Cairo::Context> &cr) {
            draw_barcode(cr, barcode_area.get_allocated_width(), barcode_area.get_allocated_height());
            return true;
        });
        preview_stack.add(preview_label, "label");
        preview_stack.add(preview_image, "image");
        preview_stack.add(photo_box, "photos");
        preview_stack.add(barcode_area, "barcode");
        preview_frame.add(preview_stack);
        left->pack_start(preview_frame, Gtk::PACK_EXPAND_WIDGET);

        auto note = Gtk::manage(new Gtk::Label(
            battler ? _("Some games look for the Barcode Battler only when they start; "
                        "reset the game if it doesn't respond.")
                    : _("The game reads the card the next time it asks for one.")));
        note->set_line_wrap(true);
        note->set_max_width_chars(60);
        note->set_xalign(0.0f);
        left->pack_start(*note, Gtk::PACK_SHRINK);

        // The known cards, one branch per card set or game, per group within
        // it and per card with several barcodes.
        auto right = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
        auto find_label = Gtk::manage(new Gtk::Label(_("Fin_d a card:"), true));
        find_label->set_mnemonic_widget(search_entry);
        find_label->set_xalign(0.0f);
        right->pack_start(*find_label, Gtk::PACK_SHRINK);
        search_entry.set_placeholder_text(battler ? _("Name, set or barcode") : _("Name, game or barcode"));
        right->pack_start(search_entry, Gtk::PACK_SHRINK);
        card_store = Gtk::TreeStore::create(card_columns);
        card_tree.set_model(card_store);
        card_tree.append_column("", card_columns.text);
        card_tree.set_headers_visible(false);
        card_tree.set_enable_search(false);
        card_scroll.add(card_tree);
        card_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        card_scroll.set_size_request(320, -1);
        card_scroll.set_shadow_type(Gtk::SHADOW_IN);
        right->pack_start(card_scroll, Gtk::PACK_EXPAND_WIDGET);
        columns->pack_start(*right, Gtk::PACK_SHRINK);

        add_button(Gtk::StockID("gtk-cancel"), Gtk::RESPONSE_CANCEL);
        scan_button = add_button(_("_Scan"), Gtk::RESPONSE_ACCEPT);
        set_default_response(Gtk::RESPONSE_ACCEPT);

        code_entry.signal_changed().connect([this] { code_edited(); });
        random_button->signal_clicked().connect([this] {
            // Any 13 digits work; the Game Boy games' made-up codes carry
            // JAN's old flag 49, as most of their cards do.
            char code[14];
            S9xBarcodeRandom(code);
            if (this->reader != S9X_BARCODE_BATTLER)
            {
                code[0] = '4';
                code[1] = '9';
                int sum = 0;
                for (int i = 0; i < 12; i++)
                    sum += (code[i] - '0') * ((i & 1) ? 3 : 1);
                code[12] = (char)('0' + (10 - sum % 10) % 10);
            }
            code_entry.set_text(code);
        });
        image_button->signal_clicked().connect([this] { from_image(); });
        camera_button.signal_clicked().connect([this] { toggle_camera(); });
        camera_combo.signal_changed().connect([this] {
            if (camera_on)
            {
                stop_camera();
                toggle_camera();
            }
        });
        search_entry.signal_changed().connect([this] { fill_cards(); });
        card_tree.get_selection()->signal_changed().connect([this] { card_picked(); });
        // A double-click (or Enter) on a barcode scans it.
        card_tree.signal_row_activated().connect([this](const Gtk::TreeModel::Path &path, Gtk::TreeViewColumn *) {
            auto row = card_store->get_iter(path);
            if (!row)
                return;
            const int entry = (*row)[card_columns.entry];
            if (entry >= 0)
                response(Gtk::RESPONSE_ACCEPT);
        });

        // A picture dropped anywhere on the dialog reads like one picked with From Image.
        drag_dest_set({ Gtk::TargetEntry("text/uri-list") }, Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
        signal_drag_data_received().connect([this](const Glib::RefPtr<Gdk::DragContext> &, int, int,
                                                   const Gtk::SelectionData &data, guint, guint) {
            auto uris = data.get_uris();
            if (uris.empty())
                return;
            std::string filename;
            try
            {
                filename = Glib::filename_from_uri(uris[0]);
            }
            catch (const Glib::Error &)
            {
                return;
            }
            // After the drop has finished, so a message box doesn't run inside it.
            Glib::signal_idle().connect_once([this, filename] { read_image(filename); });
        });

        code_edited();
        fill_cards();
        show_all();
        show_preview();
    }

    ~BarcodeDialog()
    {
        stop_camera();
    }

    bool send()
    {
        return S9xBarcodeReaderScan(reader, code_entry.get_text().c_str());
    }

  private:
    static const int PREVIEW_WIDTH = 320;
    static const int PREVIEW_HEIGHT = 240;

    struct CardColumns : Gtk::TreeModelColumnRecord
    {
        CardColumns()
        {
            add(text);
            add(entry);
        }

        // entry: the card entry of a barcode, -1 for a branch or group,
        // -2 - first entry for a card's branch
        Gtk::TreeModelColumn<Glib::ustring> text;
        Gtk::TreeModelColumn<int> entry;
    };

    void code_edited()
    {
        std::string digits = digits_of(code_entry.get_text());
        const int n = (int)digits.size();
        const bool all_digits = digits.find_first_not_of("0123456789") == std::string::npos;
        const bool length_ok = all_digits && S9xBarcodeReaderTakes(reader, n);
        const S9xBarcodeReaderCard *card = length_ok ? S9xBarcodeReaderCardFind(reader, digits.c_str()) : nullptr;

        scan_button->set_sensitive(length_ok);
        if (n == 0)
            status_label.set_text("");
        else if (!all_digits)
            status_label.set_text(_("A barcode holds only digits."));
        else if (!length_ok)
            status_label.set_text(fmt::format(fmt::runtime(reader == S9X_BARCODE_BATTLER
                                                               ? _("{} digits; a barcode has 8, 12 or 13.")
                                                           : reader == S9X_BARCODE_BARCODEBOY
                                                               ? _("{} digits; a Barcode Boy card has 13.")
                                                               : _("{} digits; a barcode has 13, or 8 for EAN-8.")),
                                              n));
        else if (!S9xBarcodeChecksumValid(digits.c_str()))
            status_label.set_text(_("The check digit doesn't add up. A printed barcode would not read like "
                                    "this, but it can still be sent."));
        else if (card)
            status_label.set_text(fmt::format(fmt::runtime(_("{}, from {}.")),
                                              *card->kana ? fmt::format("{}  {}", card->name, card->kana) : card->name,
                                              card->branch));
        else if (n == 8 && reader == S9X_BARCODE_BARDIGUN)
            status_label.set_text(_("Valid EAN-8 barcode. It reads, but it is too weak to hatch an egg."));
        else
            status_label.set_text(fmt::format(fmt::runtime(_("Valid {} barcode.")),
                                              n == 13 ? "EAN-13" : n == 12 ? "UPC-A" : "EAN-8"));

        show_card(card);
        const std::string whole = length_ok ? digits : std::string();
        if (whole != whole_code)
        {
            whole_code = whole;
            show_preview();
        }
    }

    void found(const char *code, const char *where)
    {
        code_entry.set_text(code);
        status_label.set_text(fmt::format(fmt::runtime(_("Read {} from the {}.")), code, where));
    }

    void from_image()
    {
        Gtk::FileChooserDialog chooser(*this, _("Read Barcode from Image"), Gtk::FILE_CHOOSER_ACTION_OPEN);
        chooser.add_button(Gtk::StockID("gtk-cancel"), Gtk::RESPONSE_CANCEL);
        chooser.add_button(Gtk::StockID("gtk-open"), Gtk::RESPONSE_ACCEPT);
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Images"));
        filter->add_pixbuf_formats();
        chooser.add_filter(filter);

        if (chooser.run() != Gtk::RESPONSE_ACCEPT)
            return;
        chooser.hide();
        read_image(chooser.get_filename());
    }

    void read_image(const std::string &filename)
    {
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        try
        {
            pixbuf = Gdk::Pixbuf::create_from_file(filename);
        }
        catch (const Glib::Error &)
        {
        }

        if (!pixbuf)
        {
            message(_("This picture could not be opened."), Gtk::MESSAGE_WARNING);
            return;
        }

        const int width = pixbuf->get_width(), height = pixbuf->get_height();
        const int channels = pixbuf->get_n_channels(), stride = pixbuf->get_rowstride();
        const guint8 *pixels = pixbuf->get_pixels();
        std::vector<uint8> luma((size_t)width * height);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                const guint8 *p = pixels + (size_t)y * stride + x * channels;
                luma[(size_t)y * width + x] = (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;
            }

        char code[14];
        if (S9xBarcodeDecode(luma.data(), width, height, width, code))
            found(code, _("picture"));
        else
            message(_("No EAN-13, UPC-A or EAN-8 barcode was found in this picture. "
                      "Crop it closer to the barcode and keep the bars upright."),
                    Gtk::MESSAGE_INFO);
    }

    void message(const char *text, Gtk::MessageType type)
    {
        Gtk::MessageDialog msg(*this, text, false, type, Gtk::BUTTONS_CLOSE, true);
        msg.set_title(S9xBarcodeReaderName(reader));
        msg.run();
    }

    void toggle_camera()
    {
        if (camera_on)
        {
            stop_camera();
            return;
        }

        if (!S9xGBCameraStart(camera_combo.get_active_row_number()))
        {
            status_label.set_text(_("The camera could not be started."));
            return;
        }

        camera_on = true;
        camera_button.set_label(_("Stop _Camera"));
        preview_label.set_text(_("Waiting for the camera…"));
        preview_stack.set_visible_child("label");
        poll_connection = Glib::signal_timeout().connect([this] {
            poll_camera();
            return true;
        }, 100);
    }

    void stop_camera()
    {
        if (!camera_on)
            return;

        camera_on = false;
        poll_connection.disconnect();
        camera_button.set_label(_("Start _Camera"));
        preview_label.set_text(_("Hold the barcode up to the camera, straight and in focus."));
        // Back to what Display Settings > Game Boy Image asks for.
        S9xGBCameraStop();
        S9xGBCameraApply();
        show_preview();
    }

    void poll_camera()
    {
        int width = 0, height = 0;
        if (!S9xGBCameraGetFrame(frame, width, height))
            return;

        // Mirrored, as a webcam preview is expected to look. The decoder
        // reads both directions, so it gets the frame as captured.
        auto pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, width, height);
        guint8 *pixels = pixbuf->get_pixels();
        const int stride = pixbuf->get_rowstride();
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                guint8 *p = pixels + (size_t)y * stride + x * 3;
                p[0] = p[1] = p[2] = frame[(size_t)y * width + (width - 1 - x)];
            }

        double scale = std::min((double)PREVIEW_WIDTH / width, (double)PREVIEW_HEIGHT / height);
        preview_image.set(pixbuf->scale_simple(std::max(1, (int)(width * scale)),
                                               std::max(1, (int)(height * scale)),
                                               Gdk::INTERP_BILINEAR));
        preview_stack.set_visible_child("image");

        char code[14];
        if (S9xBarcodeDecode(frame.data(), width, height, width, code) &&
            digits_of(code_entry.get_text()) != code)
        {
            found(code, _("camera"));
            Gdk::Display::get_default()->beep();
        }
    }

    // The reader's known cards with a branch, group, name or code holding the
    // search text. With nothing to search for, only the loaded game's branch
    // is open, with its groups.
    void fill_cards()
    {
        const Glib::ustring find = Glib::ustring(search_entry.get_text()).casefold();
        const char *open = S9xBarcodeReaderOpenBranch(reader);
        card_store->clear();

        int count = 0;
        const S9xBarcodeReaderCard *cards = S9xBarcodeReaderCards(reader, &count);
        const char *branch_name = nullptr, *group_name = nullptr;
        Gtk::TreeModel::iterator branch, group;
        Gtk::TreeModel::Path open_path;
        int in_branch = 0, in_group = 0;

        auto close_group = [&] {
            if (group_name)
                (*group)[card_columns.text] = fmt::format("{} ({})", group_name, in_group);
            group_name = nullptr;
        };
        auto close_branch = [&] {
            close_group();
            if (branch_name)
                (*branch)[card_columns.text] = fmt::format("{} ({})", branch_name, in_branch);
        };

        for (int i = 0, end; i < count; i = end)
        {
            end = card_end(cards, count, i);
            bool hit = false;
            for (int k = i; k < end && !hit; k++)
                hit = card_matches(cards[k], find);
            if (!hit)
                continue;

            if (!branch_name || strcmp(branch_name, cards[i].branch))
            {
                close_branch();
                branch_name = cards[i].branch;
                branch = card_store->append();
                (*branch)[card_columns.text] = branch_name;
                (*branch)[card_columns.entry] = -1;
                if (open && !strcmp(open, branch_name))
                    open_path = card_store->get_path(branch);
                in_branch = 0;
            }

            Gtk::TreeModel::iterator under = branch;
            if (cards[i].group)
            {
                if (!group_name || strcmp(group_name, cards[i].group))
                {
                    close_group();
                    group_name = cards[i].group;
                    group = card_store->append(branch->children());
                    (*group)[card_columns.text] = group_name;
                    (*group)[card_columns.entry] = -1;
                    in_group = 0;
                }
                under = group;
                in_group++;
            }
            else
                close_group();

            Gtk::TreeModel::iterator parent = under;
            if (end - i > 1)
            {
                parent = card_store->append(under->children());
                (*parent)[card_columns.text] = card_label(cards, i, end);
                (*parent)[card_columns.entry] = -2 - i;
            }

            for (int k = i; k < end; k++)
            {
                auto row = card_store->append(parent->children());
                (*row)[card_columns.text] = fmt::format("{}  {}", cards[k].name, cards[k].code);
                (*row)[card_columns.entry] = k;
            }
            in_branch++;
        }
        close_branch();

        if (!find.empty())
            card_tree.expand_all();
        else if (!open_path.empty())
        {
            card_tree.expand_row(open_path, false);
            auto open_row = card_store->get_iter(open_path);
            for (auto &child : open_row->children())
                if (child[card_columns.entry] == -1)
                    card_tree.expand_row(card_store->get_path(child), false);
        }
    }

    // Only a barcode types its code; a branch, group or card's branch clears
    // it, a card's branch still showing the card's photos.
    void card_picked()
    {
        auto row = card_tree.get_selection()->get_selected();
        if (!row)
            return;

        int count = 0;
        const S9xBarcodeReaderCard *cards = S9xBarcodeReaderCards(reader, &count);
        const int entry = (*row)[card_columns.entry];
        if (entry >= 0 && entry < count)
        {
            code_entry.set_text(cards[entry].code);
            show_card(&cards[entry]);    // this set's printing, not the first card with the code
            return;
        }

        code_entry.set_text("");
        const int first = -2 - entry;
        show_card(first >= 0 && first < count ? &cards[first] : nullptr);
    }

    void show_card(const S9xBarcodeReaderCard *card)
    {
        if (card == photo_card)
            return;

        photo_card = card;
        photos.clear();
        if (card)
            for (auto &photo : S9xBarcodeCardPhotos(photo_dir, card->image, S9xBarcodeReaderPhotoSides(reader)))
            {
                auto pixbuf = Gdk::Pixbuf::create_from_data(photo.rgba.data(), Gdk::COLORSPACE_RGB, true, 8,
                                                            photo.width, photo.height, photo.width * 4);
                photos.push_back(pixbuf->copy());
            }
        show_preview();
    }

    // A code with no photos as its printed barcode: bars on a white label,
    // the guard bars running down between the digits.
    void draw_barcode(const Cairo::RefPtr<Cairo::Context> &cr, int box_w, int box_h)
    {
        bool bars[95];
        const int modules = S9xBarcodeReaderBars(whole_code.c_str(), bars);
        if (!modules)
            return;

        // A UPC-A prints as the EAN-13 it reads as.
        const std::string code = whole_code.size() == 12 ? "0" + whole_code : whole_code;
        const bool ean13 = modules == 95;
        const int quiet_l = ean13 ? 11 : 7, quiet_r = 7, total = quiet_l + modules + quiet_r;
        const int m = std::max(1, box_w * 85 / 100 / total);    // pixels per module
        const int digit_h = 9 * m, pad = 4 * m;
        const int bar_h = std::min(55 * m, box_h - digit_h - 2 * pad - 8);
        const int w = total * m, h = pad + bar_h + digit_h + pad;
        const int x0 = (box_w - w) / 2, y0 = (box_h - h) / 2;
        const int bars_x = x0 + quiet_l * m, bar_top = y0 + pad;

        cr->set_source_rgb(1.0, 1.0, 1.0);
        cr->rectangle(x0, y0, w, h);
        cr->fill();
        cr->set_source_rgb(0.0, 0.0, 0.0);
        const int half = ean13 ? 6 : 4, mid = 3 + 7 * half;
        for (int i = 0; i < modules; i++)
        {
            if (!bars[i])
                continue;
            const bool guard = i < 3 || i >= modules - 3 || (i >= mid && i < mid + 5);
            cr->rectangle(bars_x + i * m, bar_top, m, bar_h + (guard ? digit_h / 2 : 0));
        }
        cr->fill();

        auto layout = barcode_area.create_pango_layout("");
        Pango::FontDescription font("monospace");
        font.set_absolute_size(digit_h * PANGO_SCALE);
        layout->set_font_description(font);
        // Each digit under its own seven modules; an EAN-13's first one in the quiet zone.
        auto digit = [&](char c, int left) {
            layout->set_text(std::string(1, c));
            int tw, th;
            layout->get_pixel_size(tw, th);
            cr->move_to(left + (7 * m - tw) / 2, bar_top + bar_h);
            layout->show_in_cairo_context(cr);
        };
        int k = 0;
        if (ean13)
            digit(code[k++], bars_x - 9 * m);
        for (int i = 0; i < half; i++)
            digit(code[k++], bars_x + (3 + 7 * i) * m);
        for (int i = 0; i < half; i++)
            digit(code[k++], bars_x + (mid + 5 + 7 * i) * m);
    }

    // While the camera is off: the shown card's photos side by side, else the
    // code's barcode, else a hint.
    void show_preview()
    {
        if (camera_on)
            return;

        for (auto child : photo_box.get_children())
        {
            photo_box.remove(*child);
            delete child;
        }

        bool bars[95];
        if (photos.empty())
        {
            const bool drawn = S9xBarcodeReaderBars(whole_code.c_str(), bars) != 0;
            preview_stack.set_visible_child(drawn ? "barcode" : "label");
            barcode_area.queue_draw();
            return;
        }

        const int n = (int)photos.size(), gap = 6;
        const int slot_w = (PREVIEW_WIDTH - gap * (n + 1)) / n, slot_h = PREVIEW_HEIGHT - gap * 2;
        photo_box.set_spacing(gap);
        for (auto &photo : photos)
        {
            const double scale = std::min((double)slot_w / photo->get_width(), (double)slot_h / photo->get_height());
            auto image = new Gtk::Image(photo->scale_simple(std::max(1, (int)(photo->get_width() * scale)),
                                                            std::max(1, (int)(photo->get_height() * scale)),
                                                            Gdk::INTERP_BILINEAR));
            photo_box.pack_start(*image, Gtk::PACK_SHRINK);
        }
        photo_box.show_all();
        preview_stack.set_visible_child("photos");
    }

    S9xBarcodeReader reader;
    Gtk::Entry code_entry;
    Gtk::Label status_label;
    Gtk::ComboBoxText camera_combo;
    Gtk::Button camera_button;
    Gtk::Frame preview_frame;
    Gtk::Stack preview_stack;
    Gtk::Label preview_label;
    Gtk::Image preview_image;
    Gtk::Box photo_box;
    Gtk::DrawingArea barcode_area;
    Gtk::Button *scan_button = nullptr;
    Gtk::Entry search_entry;
    Gtk::ScrolledWindow card_scroll;
    Gtk::TreeView card_tree;
    CardColumns card_columns;
    Glib::RefPtr<Gtk::TreeStore> card_store;
    sigc::connection poll_connection;
    bool camera_on = false;
    std::vector<unsigned char> frame;
    std::string photo_dir;
    const S9xBarcodeReaderCard *photo_card = nullptr;
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> photos;
    std::string whole_code;    // the code typed in when it is whole, drawn with no photos
};

} // namespace

bool S9xBarcodeDialog(Gtk::Window &parent, S9xBarcodeReader reader)
{
    BarcodeDialog dialog(parent, reader);

    while (dialog.run() == Gtk::RESPONSE_ACCEPT)
    {
        if (dialog.send())
        {
            const char *code = S9xBarcodeReaderLastCode(reader);
            const S9xBarcodeReaderCard *card = S9xBarcodeReaderCardFind(reader, code);
            S9xSetInfoString(fmt::format("{}: {} scanned", S9xBarcodeReaderName(reader), card ? card->name : code).c_str());
            return true;
        }
    }

    return false;
}
