#pragma once

#include <gtkmm.h>

#include <string>

// Small control panel: area selection + file path + start/stop for
// wf-recorder (spawned with -g for the selected region, stopped with
// SIGINT — the clean-stop signal per the wf-recorder man page).
class AreaOverlay;

class MainWindow : public Gtk::Window
{
public:
    // Formats in the drop-down. Stored as item data, so inserting a new
    // item can't silently shift the indices the logic below switches on.
    enum class Format { H264, Lossless, Gif };

    MainWindow();

private:
    // UI helpers (stage 1: layout only; process logic lands in stage 2)
    Format currentFormat() const;
    std::string fileExt() const;
    std::string defaultDir() const;
    std::string defaultPath() const;
    std::string outPath() const;      // full path, extension matched to format
    void updatePathDisplay();         // refresh the field's full-path tooltip
    void setStatus(const std::string &text); // fixed-width status, elided + tooltip
    void onFormatChanged();

    // Widgets (stage 1: layout)
    Gtk::Button m_btnSelect;
    Gtk::Button m_btnStart;
    Gtk::Button m_btnStop;
    Gtk::Button m_btnBrowse;
    Gtk::Entry m_file;
    Gtk::ComboBoxText m_format;
    Gtk::Label m_area;
    Gtk::Label m_status;

    AreaOverlay *m_overlay = nullptr;
};
