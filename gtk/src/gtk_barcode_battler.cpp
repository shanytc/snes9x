/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_barcode_battler.h"
#include "gtk_s9x.h"
#include "common/video/gb_camera_v4l2.hpp"

#include "snes9x.h"
#include "display.h"
#include "barcodebattler.h"

#include "fmt/format.h"

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

class BarcodeBattlerDialog : public Gtk::Dialog
{
  public:
    BarcodeBattlerDialog(Gtk::Window &parent)
        : Gtk::Dialog(_("Barcode Battler II"), parent, true)
    {
        auto content = get_content_area();
        content->set_spacing(8);
        content->set_border_width(8);

        auto intro = Gtk::manage(new Gtk::Label(
            _("Swipe a card through the Barcode Battler II on controller port 2. "
              "Enter its 13-digit (EAN-13), 12-digit (UPC-A) or 8-digit (EAN-8) "
              "barcode, or read it with a camera or from a picture.")));
        intro->set_line_wrap(true);
        intro->set_max_width_chars(60);
        intro->set_xalign(0.0f);
        content->pack_start(*intro, Gtk::PACK_SHRINK);

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
        content->pack_start(*code_box, Gtk::PACK_SHRINK);

        status_label.set_line_wrap(true);
        status_label.set_max_width_chars(60);
        status_label.set_xalign(0.0f);
        content->pack_start(status_label, Gtk::PACK_SHRINK);

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
        content->pack_start(*camera_box, Gtk::PACK_SHRINK);

        preview_frame.set_size_request(PREVIEW_WIDTH, PREVIEW_HEIGHT);
        preview_label.set_text(_("Hold the barcode up to the camera, straight and in focus."));
        preview_label.set_line_wrap(true);
        preview_label.set_max_width_chars(40);
        preview_stack.add(preview_label, "label");
        preview_stack.add(preview_image, "image");
        preview_frame.add(preview_stack);
        if (!names.empty())
            content->pack_start(preview_frame, Gtk::PACK_EXPAND_WIDGET);

        auto note = Gtk::manage(new Gtk::Label(
            _("Some games look for the Barcode Battler only when they start; "
              "reset the game if it doesn't respond.")));
        note->set_line_wrap(true);
        note->set_max_width_chars(60);
        note->set_xalign(0.0f);
        content->pack_start(*note, Gtk::PACK_SHRINK);

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

        code_edited();
        show_all();
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

    void code_edited()
    {
        std::string digits = digits_of(code_entry.get_text());
        const size_t n = digits.size();
        const bool all_digits = digits.find_first_not_of("0123456789") == std::string::npos;
        const bool length_ok = all_digits && (n == 8 || n == 12 || n == 13);

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
        else
            status_label.set_text(fmt::format(fmt::runtime(_("Valid {} barcode.")),
                                              n == 13 ? "EAN-13" : n == 12 ? "UPC-A" : "EAN-8"));
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

        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        try
        {
            pixbuf = Gdk::Pixbuf::create_from_file(chooser.get_filename());
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
        preview_stack.set_visible_child("label");
        // Back to what Display Settings > Game Boy Image asks for.
        S9xGBCameraStop();
        S9xGBCameraApply();
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

    Gtk::Entry code_entry;
    Gtk::Label status_label;
    Gtk::ComboBoxText camera_combo;
    Gtk::Button camera_button;
    Gtk::Frame preview_frame;
    Gtk::Stack preview_stack;
    Gtk::Label preview_label;
    Gtk::Image preview_image;
    Gtk::Button *scan_button = nullptr;
    sigc::connection poll_connection;
    bool camera_on = false;
    std::vector<unsigned char> frame;
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
