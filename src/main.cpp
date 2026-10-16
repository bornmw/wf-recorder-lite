#include <gtkmm.h>

#include "mainwindow.h"

int main(int argc, char **argv)
{
    const auto app =
        Gtk::Application::create("org.bornmw.wfrecorderlite");

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
