#include "mainwindow.h"
#include "areaoverlay.h"
#include "pathtools.h"

#include <gtkmm.h>

#include <sys/stat.h>

using namespace Cairo;

// 16x16 icons drawn in code (no asset files): a green play triangle and a
// red stop square, on a transparent background.
static Glib::RefPtr<Gdk::Texture>
playIcon()
{
    const int s = 16;
    auto surface = ImageSurface::create(ImageSurface::Format::ARGB32, s, s);
    auto cr = Context::create(surface);
    cr->set_source_rgba(0x4c / 255.0, 0xaf / 255.0, 0x50 / 255.0, 1.0);
    cr->move_to(3, 2);
    cr->line_to(s - 3, s / 2.0);
    cr->line_to(3, s - 2);
    cr->close_path();
    cr->fill();
    surface->flush();
    auto pb = Gdk::Pixbuf::create(surface, 0, 0, s, s);
    return Gdk::Texture::create_for_pixbuf(pb);
}

static Glib::RefPtr<Gdk::Texture>
stopIcon()
{
    const int s = 16;
    auto surface = ImageSurface::create(ImageSurface::Format::ARGB32, s, s);
    auto cr = Context::create(surface);
    cr->set_source_rgba(0xe5 / 255.0, 0x39 / 255.0, 0x35 / 255.0, 1.0);
    cr->rectangle(3, 3, s - 6, s - 6);
    cr->fill();
    surface->flush();
    auto pb = Gdk::Pixbuf::create(surface, 0, 0, s, s);
    return Gdk::Texture::create_for_pixbuf(pb);
}

// A GTK4 button holds a single child, so icon+label go in a small box.
static Gtk::Box *
makeActionButton(const char *label, const Glib::RefPtr<Gdk::Texture> &icon)
{
    auto img = Gtk::make_managed<Gtk::Image>(icon);
    auto lbl = Gtk::make_managed<Gtk::Label>(label);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    box->append(*img);
    box->append(*lbl);
    return box;
}

MainWindow::MainWindow()
{
    set_title("wf-recorder");

    m_area.set_text("full output");
    m_file.set_text(defaultPath());

    // Format items carry a stable id — the logic switches on that, so
    // inserting a new item can't silently shift indices.
    m_format.append("h264", "MP4 — H.264");
    m_format.append("lossless", "MP4 — lossless");
    m_format.append("gif", "GIF");
    m_format.set_active_id("h264");
    m_format.set_tooltip_text(
        "H.264 (default): small, but lossy — colours are slightly shifted "
        "and edges get soft.\n"
        "Lossless: keeps every pixel exactly (H.264 qp=0); the best source "
        "for converting to a GIF. Files are larger.\n"
        "GIF: records losslessly and converts to an animated GIF when you "
        "stop (highest quality). A GIF is always limited to 256 colours per "
        "frame, so it can never be fully pixel-perfect.");
    m_format.signal_changed().connect([this] { onFormatChanged(); });

    m_btnSelect.set_label("Select Area");
    m_btnStart.set_child(*makeActionButton("Start", playIcon()));
    m_btnStop.set_child(*makeActionButton("Stop", stopIcon()));
    m_btnStop.set_sensitive(false);
    m_btnBrowse.set_label("Browse…");

    m_status.set_text("idle");
    m_status.set_ellipsize(Pango::EllipsizeMode::END);
    m_status.set_size_request(240, -1);
    m_status.set_halign(Gtk::Align::END);

    // Row 1: Select Area ......... area readout
    auto row1 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    row1->append(m_btnSelect);
    m_area.set_hexpand(true);
    row1->append(m_area);

    // Row 2 (file): field + Browse
    auto rowFile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_file.set_hexpand(true);
    rowFile->append(m_file);
    rowFile->append(m_btnBrowse);

    // Row 3 (format): label + combo
    auto rowFmt = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto fmtLabel = Gtk::make_managed<Gtk::Label>("Format:");
    rowFmt->append(*fmtLabel);
    m_format.set_hexpand(true);
    rowFmt->append(m_format);

    // Row 4: Start Stop ......... status
    auto row2 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    row2->append(m_btnStart);
    row2->append(m_btnStop);
    m_status.set_hexpand(true);
    row2->append(m_status);

    auto root =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    root->append(*row1);
    root->append(*rowFile);
    root->append(*rowFmt);
    root->append(*row2);
    set_child(*root);

    // Keep the tooltip in sync with the field: it shows the full path on
    // hover.
    m_file.signal_changed().connect([this] { updatePathDisplay(); });
    updatePathDisplay();

    // The window must not resize when the status/area text changes at
    // runtime: pin it to its natural size.
    set_resizable(false);

    m_overlay = new AreaOverlay();
}

MainWindow::Format
MainWindow::currentFormat() const
{
    const Glib::ustring id = m_format.get_active_id();
    if (id == "lossless")
        return Format::Lossless;
    if (id == "gif")
        return Format::Gif;
    return Format::H264;
}

std::string
MainWindow::fileExt() const
{
    return currentFormat() == Format::Gif ? ".gif" : ".mp4";
}

std::string
MainWindow::defaultDir() const
{
    const std::string home = homePath();
    const std::string videos = home + "/Videos";
    struct stat st;
    return (stat(videos.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? videos
                                                                  : home;
}

std::string
MainWindow::defaultPath() const
{
    return defaultDir() + "/recording" + fileExt();
}

// Returns the full output path, with the extension forced to match the
// selected format (so the codec and the file can never disagree).
std::string
MainWindow::outPath() const
{
    return stripKnownExt(normalizeRecordPath(m_file.get_text())) + fileExt();
}

void
MainWindow::updatePathDisplay()
{
    m_file.set_tooltip_text(outPath());
}

void
MainWindow::setStatus(const std::string &text)
{
    // Elide in the fixed-width label; the full text is in the tooltip.
    m_status.set_tooltip_text(text);
    m_status.set_text(text);
}

void
MainWindow::onFormatChanged()
{
    // Keep the user's directory and base name; only swap the extension so
    // the field always shows the full path for the chosen format.
    m_file.set_text(
        stripKnownExt(normalizeRecordPath(m_file.get_text())) + fileExt());
}
