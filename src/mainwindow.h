#pragma once

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

#include "proc.h"

// Small control panel: area selection + file path + start/stop for
// wf-recorder (spawned with -g for the selected region, stopped with
// SIGINT — the clean-stop signal per the wf-recorder man page).
class AreaOverlay;

class MainWindow : public Gtk::Window
{
public:
    // Formats in the drop-down. Each combo item carries a stable id, so
    // inserting a new item can't silently shift the logic below.
    enum class Format { H264, Lossless, Gif };

    MainWindow();

protected:
    // Synchronous teardown of live processes before the window (and the
    // application) go away.
    bool onWindowClose();

private:
    // UI
    void onSelectArea();
    void onAreaSelected(int x, int y, int w, int h);
    void onBrowse();
    void onFormatChanged();
    void onStart();
    void onStop();
    void onStarted();
    void onTick();
    void onStderrReady(const std::string &chunk);
    void onFinished(int code, bool normalExit);
    void onGifConverted(int code, bool normalExit);

    // Path/format helpers
    Format currentFormat() const;
    std::string defaultDir() const;
    std::string defaultPath() const;
    std::string fileExt() const;
    std::vector<std::string> codecArgs() const; // extra args for the format
    std::string outPath() const;      // full path, extension matched to format
    std::string recPath() const;      // file currently being written (temp for GIF)
    void updatePathDisplay();         // refresh the field's full-path tooltip
    void setStatus(const std::string &text); // fixed-width status, elided + tooltip
    std::string fmtSize(long long bytes) const;

    // Process layer
    void stopEscalation();
    void launchFailed();           // restore idle state after a failed launch
    void convertGif();             // start the async temp .mp4 -> final .gif
    void reownClipboard();         // keep copied text alive via wl-copy

    // Widgets
    Gtk::Button m_btnSelect;
    Gtk::Button m_btnStart;
    Gtk::Button m_btnStop;
    Gtk::Button m_btnBrowse;
    Gtk::Entry m_file;
    Gtk::ComboBoxText m_format;
    Gtk::Label m_area;
    Gtk::Label m_status;

    Glib::RefPtr<AreaOverlay> m_overlay;

    // Selection state (PHYSICAL device pixels; m_haveArea == false means
    // record the full output).
    bool m_haveArea = false;
    int m_areaX = 0, m_areaY = 0, m_areaW = 0, m_areaH = 0;
    std::string m_output; // output name the area was selected on ("" = default)

    // Recording
    std::unique_ptr<Child> m_proc;
    Timer m_tick;      // 1 s status tick
    Timer m_stopTerm;  // SIGTERM if wf-recorder hangs after SIGINT
    Timer m_stopKill;  // last resort: SIGKILL
    int m_elapsed = 0;
    std::string m_stderr;   // captured wf-recorder stderr (failure diagnostics)
    pid_t m_stopPid = 0;    // wf-recorder PID the stop escalation targets

    // GIF session (via lossless temp .mp4)
    bool m_isGif = false;
    std::string m_gifTempPath;     // lossless .mp4 being recorded
    // The async conversion; its 120 s cap timer is owned by it (removed in
    // onGifConverted / close path), so a second conversion can't extend
    // the first one's cap.
    std::unique_ptr<Child> m_ffmpeg;
    Timer m_gifCap;  // 120 s conversion cap (owned by the conversion)
    std::string m_ffmpegErr;   // captured ffmpeg stderr (conversion diagnostics)
    std::string m_gifFinalPath;  // where m_ffmpeg is writing the .gif
    std::string m_gifTempSource; // the .mp4 this conversion reads (owned by the
                                 // conversion — a new recording may have
                                 // replaced m_gifTempPath)
};
