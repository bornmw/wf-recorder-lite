#include <gtkmm.h>

#include "mainwindow.h"

int main(int argc, char **argv)
{
    // NON_UNIQUE: no D-Bus name ownership, so each launch is a standalone
    // instance (the default GApplication is single-instance and a second
    // launch would just activate the first one and exit 0).
    const auto app =
        Gtk::Application::create("org.bornmw.wfrecorderlite",
                                 Gio::Application::Flags::NON_UNIQUE);

    // The window must be created after the GApplication::startup signal
    // has been emitted — i.e. inside the activate handler.
    MainWindow *win = nullptr;
    app->signal_activate().connect([&win, &app] {
        if (!win)
        {
            win = new MainWindow();
            win->set_application(app); // keep the app alive while it's open
            win->present();
        }
    });

    return app->run(argc, argv);
}
