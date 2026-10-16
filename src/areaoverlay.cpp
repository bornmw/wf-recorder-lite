#include "areaoverlay.h"

#include <gtkmm.h>

#include <algorithm>
#include <cmath>

#include "pathtools.h"

AreaOverlay::AreaOverlay()
{
    set_title("wf-recorder — select area");
    set_decorated(false); // frameless
    set_resizable(false);

    // Transparent window background on Wayland. The provider is attached to
    // the whole display, so it must be scoped to THIS window via a style
    // class — an unscoped "window { background: transparent; }" would also
    // make the main panel see-through.
    add_css_class("area-select");
    auto css = Gtk::CssProvider::create();
    css->load_from_string(
        ".area-select, .area-select * { background: transparent; }");
    Gtk::StyleProvider::add_provider_for_display(
        Gdk::Display::get_default(), std::move(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    m_area.set_cursor(Gdk::Cursor::create("crosshair"));

    m_area.set_hexpand(true);
    m_area.set_vexpand(true);
    m_area.set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            onDraw(cr, w, h);
        });

    // GTK4's per-type controllers (motion/scroll/keyboard) can't express
    // "press, then motion, then release" as one gesture, so use the legacy
    // combined stream and switch on the event type.
    auto ctrl = Gtk::EventControllerLegacy::create();
    m_area.add_controller(ctrl);
    // SignalProxyNormal::connect has no default for `after`.
    ctrl->signal_event()
        .connect(
            [this](const Glib::RefPtr<const Gdk::Event> &ev) {
                return onEvent(ev);
            },
            false);

    set_child(m_area);
}

void
AreaOverlay::onDraw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int)
{
    cr->set_source_rgba(0, 0, 0, 110.0 / 255.0);
    cr->paint();

    if (m_dragging && (m_x1 != m_x0 || m_y1 != m_y0))
    {
        const int x = std::min(m_x0, m_x1);
        const int y = std::min(m_y0, m_y1);
        const int w = std::abs(m_x1 - m_x0);
        const int h = std::abs(m_y1 - m_y0);

        // Cut the selection out of the dim: CAIRO_OPERATOR_CLEAR makes those
        // pixels fully transparent, so the live screen shows through the hole.
        cr->set_operator(Cairo::Context::Operator::CLEAR);
        cr->rectangle(x, y, w, h);
        cr->fill();
        cr->set_operator(Cairo::Context::Operator::OVER);

        cr->set_line_width(3);
        cr->set_source_rgb(0.0, 220 / 255.0, 130 / 255.0);
        cr->rectangle(x + 1, y + 1, w - 2, h - 2);
        cr->stroke();
    }

    // Hint (same text as the Qt original).
    cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::BOLD);
    cr->set_font_size(18);
    const char *hint = "Drag to select area — Esc to cancel";
    Cairo::TextExtents ext;
    cr->get_text_extents(hint, ext);
    cr->set_source_rgba(1, 1, 1, 220.0 / 255.0);
    cr->move_to((width - ext.width) / 2.0, 36);
    cr->show_text(hint);
}

bool
AreaOverlay::onEvent(const Glib::RefPtr<const Gdk::Event> &event)
{
    // Coordinates arrive as logical (device-independent) window-local pixels.
    // The overlay covers the output from its origin, so physical-local =
    // logical * scale.
    switch (event->get_event_type())
    {
    case Gdk::Event::Type::BUTTON_PRESS:
    {
        if (event->get_button() == 1) // GDK left button
        {
            double x = 0, y = 0;
            if (event->get_position(x, y))
            {
                m_x0 = m_x1 = static_cast<int>(std::lround(x));
                m_y0 = m_y1 = static_cast<int>(std::lround(y));
                m_dragging = true;
                m_area.queue_draw();
            }
        }
        return true;
    }
    case Gdk::Event::Type::MOTION_NOTIFY:
    {
        if (m_dragging)
        {
            double x = 0, y = 0;
            if (event->get_position(x, y))
            {
                m_x1 = static_cast<int>(std::lround(x));
                m_y1 = static_cast<int>(std::lround(y));
                m_area.queue_draw();
            }
        }
        return true;
    }
    case Gdk::Event::Type::BUTTON_RELEASE:
    {
        if (!m_dragging || event->get_button() != 1) // GDK left button
            return true;
        m_dragging = false;

        const int x = std::min(m_x0, m_x1);
        const int y = std::min(m_y0, m_y1);
        const int w = std::abs(m_x1 - m_x0);
        const int h = std::abs(m_y1 - m_y0);
        m_x0 = m_x1 = m_y0 = m_y1 = 0;
        m_area.queue_draw();

        if (w < 8 || h < 8)
            return true; // tiny click — treat as cancel, keep overlay up

        const std::function<void(int, int, int, int)> cb = std::move(m_cb);
        m_cb = nullptr;
        const double sc = (m_scale > 0) ? m_scale : 1.0;
        const int px = static_cast<int>(std::llround(x * sc));
        const int py = static_cast<int>(std::llround(y * sc));
        const int pw = static_cast<int>(std::llround(w * sc));
        const int ph = static_cast<int>(std::llround(h * sc));
        hide();
        if (cb)
            cb(px, py, pw, ph);
        return true;
    }
    case Gdk::Event::Type::KEY_PRESS:
    {
        if (event->get_keyval() == GDK_KEY_Escape)
        {
            m_dragging = false;
            m_x0 = m_x1 = m_y0 = m_y1 = 0;
            m_cb = nullptr;
            hide();
        }
        return true;
    }
    default:
        return false;
    }
}

std::string
AreaOverlay::select(std::function<void(int, int, int, int)> cb)
{
    m_cb = std::move(cb);
    m_dragging = false;
    m_x0 = m_x1 = m_y0 = m_y1 = 0;

    // Size the window to the whole output (logical px) and remember the
    // scale for the logical->physical mapping. Multi-monitor is out of
    // scope, so the first listed output is the one being selected on.
    std::string out;
    m_output.clear();
    m_scale = 1.0;
    int pw = 0, ph = 0, ox = 0, oy = 0;
    double sc = 1.0;
    if (runWlrRandr(out))
    {
        m_output = parseWlrRandrFirstOutput(out);
        if (!m_output.empty())
        {
            parseWlrRandr(out, m_output, ox, oy, sc);
            parseWlrRandrMode(out, m_output, pw, ph);
        }
    }
    if (sc <= 0)
        sc = 1.0;
    m_scale = sc;

    const int lw = (pw > 0) ? static_cast<int>(std::llround(pw / sc)) : 1440;
    const int lh = (ph > 0) ? static_cast<int>(std::llround(ph / sc)) : 900;
    set_default_size(lw, lh);
    present();
    return m_output;
}
