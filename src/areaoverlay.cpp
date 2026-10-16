#include "areaoverlay.h"

#include <gtkmm.h>

AreaOverlay::AreaOverlay()
{
    set_title("area select");
    set_decorated(false); // frameless

    // Transparent window background on Wayland.
    auto css = Gtk::CssProvider::create();
    css->load_from_string("window, window > * { background: transparent; }");
    Gtk::StyleProvider::add_provider_for_display(
        Gdk::Display::get_default(), std::move(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    m_area.set_hexpand(true);
    m_area.set_vexpand(true);
    m_area.set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            onDraw(cr, w, h);
        });
    set_child(m_area);
}

void
AreaOverlay::onDraw(const Cairo::RefPtr<Cairo::Context> &cr, int, int)
{
    cr->set_source_rgba(0, 0, 0, 110.0 / 255.0);
    cr->paint();
}

bool
AreaOverlay::onEvent(const Glib::RefPtr<const Gdk::Event> &event)
{
    (void)event;
    return false;
}

void
AreaOverlay::select(std::function<void(int, int, int, int)> cb)
{
    m_cb = std::move(cb);
}

void
AreaOverlay::setScale(double scale)
{
    m_scale = scale;
}
