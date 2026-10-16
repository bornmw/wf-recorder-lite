#include "areaoverlay.h"

#include <gtkmm.h>

#include <gdk/gdk.h>
#include <gtk/gtkeventcontrollerlegacy.h>

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
    // load_from_data (not load_from_string): the latter only exists in newer
    // gtkmm, CI's older one doesn't have it. load_from_data is present in all.
    css->load_from_data(
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
    // combined stream and switch on the event type. Built via the C API
    // (stable across gtkmm versions — the EventControllerLegacy wrapper only
    // exists in newer gtkmm) and the "event" signal wired through a trampoline.
    auto *ctrl = gtk_event_controller_legacy_new();
    gtk_widget_add_controller(GTK_WIDGET(m_area.gobj()),
                              GTK_EVENT_CONTROLLER(ctrl));
    g_object_unref(ctrl); // the widget now owns it
    g_signal_connect(ctrl, "event",
                     G_CALLBACK(&AreaOverlay::eventTramp), this);

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

gboolean
AreaOverlay::eventTramp(GtkEventController *, GdkEvent *ev, gpointer data)
{
    return static_cast<AreaOverlay *>(data)->onEvent(ev);
}

bool
AreaOverlay::onEvent(GdkEvent *event)
{
    // Coordinates arrive as logical (device-independent) window-local pixels.
    // The overlay covers the output from its origin, so physical-local =
    // logical * scale.
    switch (gdk_event_get_event_type(event))
    {
    case GDK_BUTTON_PRESS:
    {
        if (gdk_button_event_get_button(event) == 1) // GDK left button
        {
            double x = 0, y = 0;
            if (gdk_event_get_position(event, &x, &y))
            {
                m_x0 = m_x1 = static_cast<int>(std::lround(x));
                m_y0 = m_y1 = static_cast<int>(std::lround(y));
                m_dragging = true;
                m_area.queue_draw();
            }
        }
        return true;
    }
    case GDK_MOTION_NOTIFY:
    {
        if (m_dragging)
        {
            double x = 0, y = 0;
            if (gdk_event_get_position(event, &x, &y))
            {
                m_x1 = static_cast<int>(std::lround(x));
                m_y1 = static_cast<int>(std::lround(y));
                m_area.queue_draw();
            }
        }
        return true;
    }
    case GDK_BUTTON_RELEASE:
    {
        if (!m_dragging || gdk_button_event_get_button(event) != 1)
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
    case GDK_KEY_PRESS:
    {
        if (gdk_key_event_get_keyval(event) == GDK_KEY_Escape)
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
