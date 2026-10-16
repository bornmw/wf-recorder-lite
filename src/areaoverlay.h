#pragma once

#include <gtkmm.h>

#include <functional>
#include <string>

// Fullscreen transparent overlay for selecting a screen region:
// drag a rectangle, Esc cancels. Emits the rectangle in PHYSICAL
// (device) pixels local to the output — the space wf-recorder's -g
// geometry consumes, independent of the fractional scale factor.
class AreaOverlay : public Gtk::Window
{
public:
    explicit AreaOverlay();

    // Shows the overlay (sized to the whole output, frameless) and arms the
    // selection. `cb` fires once with the physical-px rectangle when the
    // user releases a drag of >= 8 logical px, then the overlay hides;
    // Esc cancels (no callback). Tiny clicks just clear the selection and
    // keep the overlay up. Returns the output name the selection is local
    // to ("" if it couldn't be determined).
    std::string select(std::function<void(int x, int y, int w, int h)> cb);

private:
    // draw the dim + selection
    void onDraw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int height);
    // legacy event controller: one "event" signal, switch on type
    bool onEvent(const Glib::RefPtr<const Gdk::Event> &event);

    double m_scale = 1.0;
    std::string m_output;
    bool m_dragging = false;
    int m_x0 = 0, m_y0 = 0, m_x1 = 0, m_y1 = 0; // logical, window-local
    std::function<void(int, int, int, int)> m_cb;

    Gtk::DrawingArea m_area;
};
