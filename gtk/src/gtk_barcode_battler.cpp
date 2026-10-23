/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_barcode_battler.h"
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

// The folder of the running executable, where a portable build keeps its battler folder.
std::string executable_dir()
{
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.parent_path().string();
}

// One printed card: the run of entries from i sharing its set and photo stem
// (a card with several barcodes); a single entry when it has no photos.
int card_end(const S9xBarcodeCard *cards, int count, int i)
{
    int end = i + 1;
    if (*cards[i].image)
        while (end < count && !strcmp(cards[end].set, cards[i].set) && !strcmp(cards[end].image, cards[i].image))
            end++;
    return end;
}

bool card_matches(const S9xBarcodeCard &card, const Glib::ustring &find)
{
    return find.empty() || Glib::ustring(card.name).casefold().find(find) != Glib::ustring::npos ||
           Glib::ustring(card.set).casefold().find(find) != Glib::ustring::npos ||
           std::string(card.code).find(find.raw()) != std::string::npos;
}

// A card with several barcodes is named after them, less their positions.
std::string card_label(const S9xBarcodeCard *cards, int i, int end)
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

class BarcodeBattlerDialog : public Gtk::Dialog
{
  public:
    BarcodeBattlerDialog(Gtk::Window &parent)
        : Gtk::Dialog(_("Barcode Battler II"), parent, true)
    {
        auto content = get_content_area();
        content->set_spacing(8);
        content->set_border_width(8);

        auto columns = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));
        auto left = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
        columns->pack_start(*left, Gtk::PACK_EXPAND_WIDGET);
        content->pack_start(*columns, Gtk::PACK_EXPAND_WIDGET);

        auto intro = Gtk::manage(new Gtk::Label(
            _("Swipe a card through the Barcode Battler II on controller port 2. "
              "Enter its 13-digit (EAN-13), 12-digit (UPC-A) or 8-digit (EAN-8) "
              "barcode, pick a known card, or read it with a camera or from a picture.")));
        intro->set_line_wrap(true);
        intro->set_max_width_chars(60);
        intro->set_xalign(0.0f);
        left->pack_start(*intro, Gtk::PACK_SHRINK);

        auto code_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        code_box->pack_start(*Gtk::manage(new Gtk::Label(_("Barcode:"))), Gtk::PACK_SHRINK);
        code_entry.set_max_length(20);
        code_entry.set_placeholder_text(_("e.g. 4901234567894"));
        code_entry.set_text(S9xBarcodeBattlerLastCode());
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

        photo_dir = S9xBarcodePhotoDir({ get_config_dir(), executable_dir() });

        preview_frame.set_size_request(PREVIEW_WIDTH, PREVIEW_HEIGHT);
        preview_label.set_text(_("Hold the barcode up to the camera, straight and in focus."));
        preview_label.set_line_wrap(true);
        preview_label.set_max_width_chars(40);
        photo_box.set_halign(Gtk::ALIGN_CENTER);
        photo_box.set_valign(Gtk::ALIGN_CENTER);
        preview_stack.add(preview_label, "label");
        preview_stack.add(preview_image, "image");
        preview_stack.add(photo_box, "photos");
        preview_frame.add(preview_stack);
        if (!names.empty() || !photo_dir.empty())
            left->pack_start(preview_frame, Gtk::PACK_EXPAND_WIDGET);

        auto note = Gtk::manage(new Gtk::Label(
            _("Some games look for the Barcode Battler only when they start; "
              "reset the game if it doesn't respond.")));
        note->set_line_wrap(true);
        note->set_max_width_chars(60);
        note->set_xalign(0.0f);
        left->pack_start(*note, Gtk::PACK_SHRINK);

        // The known cards, one branch per set and one per card with several barcodes.
        auto right = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
        auto find_label = Gtk::manage(new Gtk::Label(_("Fin_d a card:"), true));
        find_label->set_mnemonic_widget(search_entry);
        find_label->set_xalign(0.0f);
        right->pack_start(*find_label, Gtk::PACK_SHRINK);
        search_entry.set_placeholder_text(_("Name, set or barcode"));
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
            char code[14];
            S9xBarcodeRandom(code);
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
        show_photos();
    }

    ~BarcodeBattlerDialog()
    {
        stop_camera();
    }

    bool send()
    {
        return S9xBarcodeBattlerScan(code_entry.get_text().c_str());
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

        // entry: the card entry of a barcode, -1 for a set, -2 - first entry for a card's branch
        Gtk::TreeModelColumn<Glib::ustring> text;
        Gtk::TreeModelColumn<int> entry;
    };

    void code_edited()
    {
        std::string digits = digits_of(code_entry.get_text());
        const size_t n = digits.size();
        const bool all_digits = digits.find_first_not_of("0123456789") == std::string::npos;
        const bool length_ok = all_digits && (n == 8 || n == 12 || n == 13);
        const S9xBarcodeCard *card = length_ok ? S9xBarcodeCardFind(digits.c_str()) : nullptr;

        scan_button->set_sensitive(length_ok);
        if (n == 0)
            status_label.set_text("");
        else if (!all_digits)
            status_label.set_text(_("A barcode holds only digits."));
        else if (!length_ok)
            status_label.set_text(fmt::format(fmt::runtime(_("{} digits; a barcode has 8, 12 or 13.")), n));
        else if (!S9xBarcodeChecksumValid(digits.c_str()))
            status_label.set_text(_("The check digit doesn't add up. A printed barcode would not read like "
                                    "this, but it can still be sent."));
        else if (card)
            status_label.set_text(fmt::format(fmt::runtime(_("{}, from {}.")), card->name, card->set));
        else
            status_label.set_text(fmt::format(fmt::runtime(_("Valid {} barcode.")),
                                              n == 13 ? "EAN-13" : n == 12 ? "UPC-A" : "EAN-8"));

        show_card(card);
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
        msg.set_title(_("Barcode Battler II"));
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
        show_photos();
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

    // The known cards with a set, name or code holding the search text; all
    // folded when there's nothing to search for.
    void fill_cards()
    {
        const Glib::ustring find = Glib::ustring(search_entry.get_text()).casefold();
        card_store->clear();

        int count = 0;
        const S9xBarcodeCard *cards = S9xBarcodeCards(&count);
        const char *set_name = nullptr;
        Gtk::TreeModel::iterator set;
        int in_set = 0;

        auto close_set = [&] {
            if (set_name)
                (*set)[card_columns.text] = fmt::format("{} ({})", set_name, in_set);
        };

        for (int i = 0, end; i < count; i = end)
        {
            end = card_end(cards, count, i);
            bool hit = false;
            for (int k = i; k < end && !hit; k++)
                hit = card_matches(cards[k], find);
            if (!hit)
                continue;

            if (!set_name || strcmp(set_name, cards[i].set))
            {
                close_set();
                set_name = cards[i].set;
                set = card_store->append();
                (*set)[card_columns.text] = set_name;
                (*set)[card_columns.entry] = -1;
                in_set = 0;
            }

            Gtk::TreeModel::iterator parent = set;
            if (end - i > 1)
            {
                parent = card_store->append(set->children());
                (*parent)[card_columns.text] = card_label(cards, i, end);
                (*parent)[card_columns.entry] = -2 - i;
            }

            for (int k = i; k < end; k++)
            {
                auto row = card_store->append(parent->children());
                (*row)[card_columns.text] = fmt::format("{}  {}", cards[k].name, cards[k].code);
                (*row)[card_columns.entry] = k;
            }
            in_set++;
        }
        close_set();

        if (!find.empty())
            card_tree.expand_all();
    }

    // Only a barcode types its code; a set or a card's branch clears it, the
    // branch still showing the card's photos.
    void card_picked()
    {
        auto row = card_tree.get_selection()->get_selected();
        if (!row)
            return;

        int count = 0;
        const S9xBarcodeCard *cards = S9xBarcodeCards(&count);
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

    void show_card(const S9xBarcodeCard *card)
    {
        if (card == photo_card)
            return;

        photo_card = card;
        photos.clear();
        if (card)
            for (auto &photo : S9xBarcodeCardPhotos(photo_dir, *card))
            {
                auto pixbuf = Gdk::Pixbuf::create_from_data(photo.rgba.data(), Gdk::COLORSPACE_RGB, true, 8,
                                                            photo.width, photo.height, photo.width * 4);
                photos.push_back(pixbuf->copy());
            }
        show_photos();
    }

    // The shown card's photos side by side in the preview, while the camera is off.
    void show_photos()
    {
        if (camera_on)
            return;

        for (auto child : photo_box.get_children())
        {
            photo_box.remove(*child);
            delete child;
        }

        if (photos.empty())
        {
            preview_stack.set_visible_child("label");
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

    Gtk::Entry code_entry;
    Gtk::Label status_label;
    Gtk::ComboBoxText camera_combo;
    Gtk::Button camera_button;
    Gtk::Frame preview_frame;
    Gtk::Stack preview_stack;
    Gtk::Label preview_label;
    Gtk::Image preview_image;
    Gtk::Box photo_box;
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
    const S9xBarcodeCard *photo_card = nullptr;
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> photos;
};

} // namespace

bool S9xBarcodeBattlerDialog(Gtk::Window &parent)
{
    BarcodeBattlerDialog dialog(parent);

    while (dialog.run() == Gtk::RESPONSE_ACCEPT)
    {
        if (dialog.send())
        {
            S9xSetInfoString(fmt::format("Barcode Battler: {} scanned", S9xBarcodeBattlerLastCode()).c_str());
            return true;
        }
    }

    return false;
}
