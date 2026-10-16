#pragma once

#include <gtkmm.h>

#include <functional>
#include <string>

// Fullscreen transparent overlay for selecting a screen region:
// drag a rectangle, Esc cancels. Emits the rectangle in PHYSICAL
// (device) pixels — the space wf-recorder's -g geometry consumes,
// independent of the fractional scale factor.
class AreaOverlay : public Gtk::Window
{
public:
    explicit AreaOverlay();

    void select(std::function<void(int x, int y, int w, int h)> cb);
    // Logical (device-independent) pixel position of a selection within
    // the overlay; the caller converts to physical via the monitor scale.
    void setScale(double scale);

private:
    // draw the dim + selection
    void onDraw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int height);
    // legacy event controller: one "event" signal, switch on type
    bool onEvent(const Glib::RefPtr<const Gdk::Event> &event);

    double m_scale = 1.0;
    bool m_dragging = false;
    int m_x0 = 0, m_y0 = 0, m_x1 = 0, m_y1 = 0;
    std::function<void(int, int, int, int)> m_cb;

    Gtk::DrawingArea m_area;
};
