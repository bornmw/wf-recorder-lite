#include "mainwindow.h"
#include "areaoverlay.h"
#include "pathtools.h"
#include "proc.h"

#include <gtkmm.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Max source width for the async GIF conversion; wider sources are scaled
// down, smaller ones pass through unchanged (see convertGif()).
static constexpr int kGifMaxWidth = 1280;

// 16x16 icons drawn in code (no asset files, no icon-theme dependency):
// a green play triangle and a red stop square.
static Glib::RefPtr<Gdk::Texture>
playIcon()
{
    const int s = 16;
    auto surface =
        Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, s, s);
    auto cr = Cairo::Context::create(surface);
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
    auto surface =
        Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, s, s);
    auto cr = Cairo::Context::create(surface);
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

// wf-recorder builds its output list from the xdg-output protocol
// (logical_position/logical_size) and passes "-g minus output position" to
// wlroots, which interprets the region in the output's LOGICAL space (then
// scales it to the buffer). On this wayfire build the reported position is
// the PHYSICAL one while the size is logical, so the -g value that lands on
// a selection (px,py,W,H) of PHYSICAL local pixels is:
//   x = physPos.x + px/scale,  y = physPos.y + py/scale
//   w = W/scale,               h = H/scale
// (physPos from wlr-randr). One wlr-randr query yields both, so Start blocks
// at most once (~2 s) instead of parsing the same output twice. Parsing
// itself lives in pathtools.h (unit-tested).
static bool
outputGeom(const std::string &name, int &x, int &y, double &scale)
{
    std::string out;
    if (!runWlrRandr(out))
        return false;
    return parseWlrRandr(out, name, x, y, scale);
}

// Collision-proof temp name (random suffix) in $TMPDIR. An empty placeholder
// is created; wf-recorder overwrites it (we pass -y, so no "Overwrite?"
// prompt). The file's lifecycle belongs to the recording session and is
// removed by onGifConverted() (or its no-output branch).
static std::string
makeGifTemp()
{
    const char *tmpdir = std::getenv("TMPDIR");
    const std::string dir = (tmpdir && tmpdir[0]) ? tmpdir : "/tmp";
    // mkstemp() wants its X's at the very END of the template, which would
    // yield a name without the .mp4 suffix — generate the suffix manually
    // and create the file with O_EXCL (same collision protection).
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t rnd[6] = {0, 0, 0, 0, 0, 0};
    const ssize_t got = getrandom(rnd, sizeof rnd, 0);
    if (got != sizeof rnd)
        return "";
    char suffix[7];
    for (int i = 0; i < 6; ++i)
        suffix[i] = charset[rnd[i] % (sizeof charset - 1)];
    suffix[6] = '\0';
    const std::string name = dir + "/wfrec-" + suffix + ".mp4";
    const int fd = open(name.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        return "";
    ::close(fd);
    return name;
}

static std::string
lastStderrLine(const std::string &stderrText)
{
    // The most relevant line of the (often verbose) stderr, for the status.
    const std::string t = trimSpaces(stderrText);
    if (t.empty())
        return "";
    const auto pos = t.rfind('\n');
    return (pos == std::string::npos) ? t : trimSpaces(t.substr(pos + 1));
}

static std::string
trimmed(const std::string &s)
{
    return trimSpaces(s);
}

// True if `name` is an executable on $PATH.
static bool
executableOnPath(const std::string &name)
{
    const char *path = std::getenv("PATH");
    if (!path)
        return false;
    std::string p = path;
    size_t start = 0;
    for (;;)
    {
        const size_t c = p.find(':', start);
        const std::string dir =
            (c == std::string::npos) ? p.substr(start)
                                     : p.substr(start, c - start);
        start = (c == std::string::npos) ? p.size() : c + 1;
        if (dir.empty())
            continue;
        struct stat st;
        const std::string full = dir + "/" + name;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & S_IXUSR))
            return true;
    }
    return false;
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
    auto rowFile =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_file.set_hexpand(true);
    rowFile->append(m_file);
    rowFile->append(m_btnBrowse);

    // Row 3 (format): label + combo
    auto rowFmt =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
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

    auto root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    root->append(*row1);
    root->append(*rowFile);
    root->append(*rowFmt);
    root->append(*row2);
    set_child(*root);

    // Keep the tooltip in sync with the field: the tooltip shows the full
    // path on hover.
    m_file.signal_changed().connect([this] { updatePathDisplay(); });
    updatePathDisplay();

    m_btnSelect.signal_clicked().connect([this] { onSelectArea(); });
    m_btnBrowse.signal_clicked().connect([this] { onBrowse(); });
    m_btnStart.signal_clicked().connect([this] { onStart(); });
    m_btnStop.signal_clicked().connect([this] { onStop(); });

    // Fixed window size: the status changes length at runtime (and the
    // area readout appears after a selection), so the window must not resize
    // with it. The status label has its own fixed width, so the rows drive
    // the size.
    set_resizable(false);

    m_overlay = new AreaOverlay();
    m_overlay->set_application(get_application());

    // Stop escalation: SIGINT is the clean-stop. wf-recorder can hang after
    // it finalizes the output file (seen with GIF), leaving the process
    // alive so the UI stays on "recording…". Escalate so it always exits.
    // The timers are created per stop (in onStop) and guarded by m_stopPid so
    // a stale timer from a previous session can never signal a freshly
    // started recording.

    // Synchronous teardown of live processes when the window closes (the
    // async escalation timers can no longer fire once the event loop exits).
    // Run before the default handler (which does the actual close).
    signal_close_request().connect([this] { return onWindowClose(); }, false);
}

void
MainWindow::setStatus(const std::string &text)
{
    // Elide in the fixed-width label; the full text is in the tooltip.
    m_status.set_tooltip_text(text);
    m_status.set_text(text);
}

std::string
MainWindow::fmtSize(long long bytes) const
{
    return formatSize(bytes);
}

void
MainWindow::onSelectArea()
{
    // Do NOT use showFullScreen(): the compositor's fullscreen path is
    // unstable on this box and kills the session. The overlay is a frameless
    // top-level sized to the whole output (it sizes itself to it). The
    // returned name is where the selection lives, so the -o flag and the
    // -g output-offset share one source of truth.
    m_output = m_overlay->select(
        [this](int x, int y, int w, int h) { onAreaSelected(x, y, w, h); });
}

void
MainWindow::onAreaSelected(int x, int y, int w, int h)
{
    m_haveArea = true;
    m_areaX = x;
    m_areaY = y;
    m_areaW = w;
    m_areaH = h;
    m_area.set_text(std::to_string(x) + "," + std::to_string(y) + " " +
                    std::to_string(w) + "x" + std::to_string(h));
}

// Wayland clipboard/selection content is owned by the process that set it, so
// it dies with us on exit. Re-own it via wl-copy (which keeps a background
// daemon) so copied text survives the window closing. `primary` re-owns the
// primary selection (wl-copy -p) instead of the regular clipboard — copying in
// an Entry sets both, so both are re-owned to keep the copied text available
// via either Ctrl+V (clipboard) or middle-click paste (primary).
static void
reownClipboardText(const Glib::RefPtr<Gdk::Clipboard> &clip, bool primary)
{
    if (!clip)
        return;

    // The GTK4 clipboard read is async-only; spin the context until it
    // completes (500 ms cap, mirroring the old waitForFinished(500)).
    std::promise<std::string> prom;
    std::future<std::string> fut = prom.get_future();
    clip->read_text_async(
        [clip, &prom](const Glib::RefPtr<Gio::AsyncResult> &r) {
            std::string text;
            try
            {
                // Glib::ustring derives from std::string, so this slices to
                // one.
                text = std::string(clip->read_text_finish(r));
            }
            catch (const Glib::Error &)
            {
                // The selection holds no text (empty, or a non-text format
                // such as an image): nothing to re-own. Without this the
                // Glib::Error would escape the async callback as an unhandled
                // exception in a signal handler.
            }
            prom.set_value(text);
        });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;)
    {
        if (fut.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready)
            break;
        if (std::chrono::steady_clock::now() > deadline)
            break;
        Glib::MainContext::get_default()->iteration(false);
    }
    std::string text;
    if (fut.valid() &&
        fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        text = fut.get();
    if (text.empty())
        return;

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
        return;
    const pid_t pid = fork();
    if (pid < 0)
    {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return;
    }
    if (pid == 0)
    {
        // Child: stdin <- pipe, the rest away.
        dup2(pipefd[0], STDIN_FILENO);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                ::close(devnull);
        }
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        // execvp: the bare name must be resolved through $PATH (execl would
        // look for ./wl-copy relative to the app's cwd and silently fail,
        // leaving the clipboard to die with us).
        char *const argvPlain[] = {const_cast<char *>("wl-copy"), nullptr};
        char *const argvPrimary[] = {const_cast<char *>("wl-copy"),
                                     const_cast<char *>("-p"),
                                     nullptr};
        execvp("wl-copy", primary ? argvPrimary : argvPlain);
        _exit(127);
    }
    ::close(pipefd[0]);
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(text.size()))
    {
        const ssize_t n =
            write(pipefd[1], text.data() + off, text.size() - off);
        if (n <= 0)
            break;
        off += n;
    }
    ::close(pipefd[1]);
    int status = 0;
    int waited = 0;
    while (waited < 500)
    {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0)
            break;
        usleep(10 * 1000);
        waited += 10;
    }
    if (waited >= 500)
    {
        ::kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

void
MainWindow::reownClipboard()
{
    if (!g_getenv("WAYLAND_DISPLAY"))
        return;
    if (!executableOnPath("wl-copy"))
        return;
    const auto display = Gdk::Display::get_default();
    reownClipboardText(display->get_clipboard(), false);
    reownClipboardText(display->get_primary_clipboard(), true);
}

bool
MainWindow::onWindowClose()
{
    // Finalize live processes when the window closes. The async escalation
    // timers can no longer fire once the event loop exits, so escalate
    // synchronously here, mirroring onStop(). Detaching the process's GLib
    // sources first keeps its callbacks from running mid-teardown (they
    // would touch members that are already destroyed by then).
    if (m_proc && m_proc->running())
    {
        m_proc->detach();
        m_proc->signal(SIGINT);
        if (!m_proc->waitSync(2000))
        {
            m_proc->signal(SIGTERM);
            if (!m_proc->waitSync(2000))
            {
                m_proc->signal(SIGKILL); // last resort, same as the escalation
                m_proc->waitSync(1000);
            }
        }
        stopEscalation();
        g_warning(
            "wf-recorder-lite: window closed while recording — "
            "recording stopped, output at %s",
            recPath().c_str());
        if (m_isGif)
            g_warning(
                "wf-recorder-lite: GIF conversion skipped on close; "
                "lossless temp file kept for manual conversion: %s",
                m_gifTempPath.c_str());
    }

    // Same teardown for a conversion that is still running: kill it and
    // drop the incomplete .gif (the lossless source is kept).
    if (m_ffmpeg && m_ffmpeg->running())
    {
        m_gifCap.stop();
        m_ffmpeg->detach();
        m_ffmpeg->signal(SIGKILL);
        m_ffmpeg->waitSync(5000);
        std::remove(m_gifFinalPath.c_str());
        g_warning(
            "wf-recorder-lite: GIF conversion aborted on close; "
            "source kept for manual conversion: %s",
            m_gifTempSource.c_str());
    }

    reownClipboard();
    return false; // allow the close
}

void
MainWindow::onBrowse()
{
    // GTK 4.10+ save dialog: async save() on Gtk::FileDialog, modal on this
    // window. It opens at the current directory with the current base name,
    // and the extension is filtered to the selected format — the field is
    // re-normalized on accept, so the codec and the file can't disagree.
    const std::string path = outPath();
    const size_t slash = path.rfind('/');
    const std::string dir = (slash == std::string::npos) ? "" : path.substr(0, slash);
    const std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);

    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Save recording as…");
    dlg->set_accept_label("Save");
    dlg->set_modal(true);
    if (!dir.empty())
        dlg->set_initial_folder(Gio::File::create_for_path(dir));
    dlg->set_initial_name(name);

    auto filter = Gtk::FileFilter::create();
    filter->add_pattern("*" + fileExt());
    dlg->set_default_filter(filter);

    // dlg is captured by value (RefPtr copy) so it stays alive until the
    // async finish; cancel/close leaves the field untouched.
    dlg->save(
        *this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult> &result) {
            Glib::RefPtr<Gio::File> f;
            try
            {
                f = dlg->save_finish(result);
            }
            catch (const Glib::Error &)
            {
                return; // cancelled
            }
            if (f && f->is_native())
            {
                m_file.set_text(std::string(f->get_path()));
                updatePathDisplay();
            }
        });
}

void
MainWindow::onFormatChanged()
{
    // Keep the user's directory and base name; only swap the extension so
    // the field always shows the full path for the chosen format.
    m_file.set_text(
        stripKnownExt(normalizeRecordPath(m_file.get_text())) + fileExt());
}

void
MainWindow::stopEscalation()
{
    m_stopTerm.stop();
    m_stopKill.stop();
}

void
MainWindow::onStart()
{
    if (m_proc != nullptr && m_proc->running())
        return;

    // Cancel any pending stop-escalation from a previous session so it can't
    // signal this fresh recording.
    stopEscalation();
    m_stopPid = 0;

    std::vector<std::string> args;
    if (m_haveArea)
    {
        int ox = 0, oy = 0;
        double sc = 1.0;
        if (!outputGeom(m_output, ox, oy, sc))
            g_warning("wf-recorder-lite: cannot determine position of %s; "
                       "region may be captured on the wrong output",
                       m_output.c_str());
        if (sc <= 0)
            sc = 1.0;
        const int gx = static_cast<int>(std::llround(ox + m_areaX / sc));
        const int gy = static_cast<int>(std::llround(oy + m_areaY / sc));
        const int gw = static_cast<int>(std::llround(m_areaW / sc));
        const int gh = static_cast<int>(std::llround(m_areaH / sc));
        args.push_back("-g");
        args.push_back(std::to_string(gx) + "," + std::to_string(gy) + " " +
                       std::to_string(gw) + "x" + std::to_string(gh));
        if (!m_output.empty())
        {
            args.push_back("-o");
            args.push_back(m_output);
        }
    }

    // Direct GIF output hits a wf-recorder 0.6.0 bug
    // (https://github.com/ammen99/wf-recorder/issues/356): the GIF muxer
    // uses a coarse (centisecond) timebase, so frames captured within the
    // same ~10ms window (damage events / high refresh) get an identical
    // DTS -> the filtergraph errors and wf-recorder exits early (clean, no
    // stderr).
    // We therefore record lossless H.264 (no such bug, and the best possible
    // GIF source) to a temp file, then convert it to the .gif on stop.
    if (currentFormat() == Format::Gif)
    {
        m_isGif = true;
        m_gifTempPath = makeGifTemp();
        if (m_gifTempPath.empty())
        {
            setStatus("failed to create temp file");
            g_warning("wf-recorder-lite: failed to create temp file in the "
                      "temp dir");
            m_isGif = false;
            return;
        }
        args.insert(args.end(), {"-c", "libx264", "-p", "qp=0"});
    }
    else
    {
        // A conversion from a previous session may still be running, but it
        // reads its own snapshot (m_gifTempSource), so the live path can be
        // reset for this session.
        m_isGif = false;
        m_gifTempPath.clear();
        const std::string extra = codecArgs();
        if (!extra.empty())
        {
            // codecArgs() is a fixed "a b" pair; split on the single space.
            const auto sp = extra.find(' ');
            args.insert(args.end(), {extra.substr(0, sp), extra.substr(sp + 1)});
        }
    }
    // -y: never prompt "Overwrite? Y/n:" when the file exists — that prompt
    // hangs (no stdin) and makes the recording self-stop. The file picker
    // already handles the overwrite warning.
    args.push_back("-y");
    args.push_back("-f");
    args.push_back(recPath());

    m_proc = new Child();
    m_stderr.clear();
    // Async start: the event loop stays responsive while wf-recorder execs
    // (a blocking waitForStarted() would freeze the UI for up to 3 s).
    m_proc->onStarted = [this] { onStarted(); };
    m_proc->onFinished = [this](int code, bool normalExit) {
        onFinished(code, normalExit);
    };
    m_proc->onStderr = [this](const std::string &chunk) {
        onStderrReady(chunk);
    };
    m_proc->onFailStart = [this] {
        // No process to manage — just restore the idle state and reset the
        // GIF-session state that was set up above for this launch.
        delete m_proc;
        m_proc = nullptr;
        m_isGif = false;
        std::remove(m_gifTempPath.c_str()); // temp file was created before start
        m_gifTempPath.clear();
        m_btnStart.set_sensitive(true);
        m_btnStop.set_sensitive(false);
        setStatus("failed to launch wf-recorder");
    };
    m_btnStart.set_sensitive(false);
    m_btnStop.set_sensitive(false);
    setStatus("starting…");
    m_proc->start("wf-recorder", args);
}

void
MainWindow::onStarted()
{
    m_btnStop.set_sensitive(true);
    m_elapsed = 0;
    m_tick.start(
        1, [this] {
            onTick();
            return true;
        });
    setStatus("recording…"); // full path is in the field's tooltip
}

void
MainWindow::onStop()
{
    if (m_proc == nullptr || !m_proc->running())
        return;
    m_stopPid = m_proc->pid();
    m_proc->signal(SIGINT);
    m_stopTerm.start(
        2, [this] {
            if (m_proc && m_proc->pid() == m_stopPid && m_proc->running())
                m_proc->signal(SIGTERM);
            return false; // single shot
        });
    m_stopKill.start(
        4, [this] {
            if (m_proc && m_proc->pid() == m_stopPid && m_proc->running())
                m_proc->signal(SIGKILL);
            return false; // single shot
        });
}

void
MainWindow::onTick()
{
    if (m_proc == nullptr)
        return;
    m_elapsed++;
    struct stat st;
    const long long size =
        (stat(recPath().c_str(), &st) == 0) ? st.st_size : 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "recording %dm%02ds — %s", m_elapsed / 60,
                  m_elapsed % 60, fmtSize(size).c_str());
    setStatus(buf);
}

void
MainWindow::onStderrReady(const std::string &chunk)
{
    m_stderr += chunk;
}

void
MainWindow::onFinished(int code, bool normalExit)
{
    (void)normalExit; // a killed recorder reports via its signal; the
                      // code-0-and-file-exists check below is what matters
    g_warning("wf-recorder finished: code=%d status=%d path=%s stderr=%s",
              code, static_cast<int>(normalExit), recPath().c_str(),
              m_stderr.c_str());
    stopEscalation();
    m_tick.stop();
    m_btnStart.set_sensitive(true);
    m_btnStop.set_sensitive(false);

    if (m_isGif)
    {
        // The lossless temp .mp4 is finalized; turn it into the .gif.
        convertGif();
    }
    else
    {
        const std::string path = outPath();
        struct stat st;
        const bool exists = (stat(path.c_str(), &st) == 0);
        std::string msg;
        if (code == 0 && exists)
            msg = "saved (" + fmtSize(st.st_size) + ")"; // path: field tooltip
        else
        {
            msg = "stopped with code " + std::to_string(code);
            // On failure, surface the most relevant stderr line so the cause
            // is visible. (The full log is also written to the app log via
            // g_warning; the whole stderr is far too verbose for the status.)
            const std::string last = lastStderrLine(m_stderr);
            if (!last.empty())
                msg += " — " + last;
        }
        setStatus(msg);
    }

    delete m_proc;
    m_proc = nullptr;
}

// Run the conversion as an async process: GIF encoding is slow (minutes for
// long high-res recordings) and a blocking wait would freeze the GUI for
// the whole time — including the "converting to GIF…" status.
void
MainWindow::convertGif()
{
    // If the recording produced no usable source (wf-recorder failed early),
    // say so — don't report a conversion failure for a file that doesn't
    // exist.
    struct stat st;
    if (stat(m_gifTempPath.c_str(), &st) != 0 || st.st_size == 0)
    {
        std::remove(m_gifTempPath.c_str());
        m_gifTempPath.clear();
        m_isGif = false;
        std::string msg = "recording produced no output";
        const std::string last = lastStderrLine(m_stderr);
        if (!last.empty())
            msg += " — " + last;
        setStatus(msg);
        return;
    }

    // If a conversion from a previous session is still running, kill it
    // first: it writes the same final .gif path this conversion targets,
    // and it owns no UI state to keep. (Its lossless source is kept.)
    if (m_ffmpeg && m_ffmpeg->running())
    {
        m_gifCap.stop();
        m_ffmpeg->detach();
        m_ffmpeg->signal(SIGKILL);
        m_ffmpeg->waitSync(5000);
        std::remove(m_gifFinalPath.c_str()); // incomplete .gif
        g_warning("wf-recorder-lite: previous GIF conversion superseded; "
                  "source kept: %s",
                  m_gifTempSource.c_str());
        delete m_ffmpeg;
        m_ffmpeg = nullptr;
    }

    const std::string finalPath = outPath();
    m_gifFinalPath = finalPath;
    // Snapshot the source: a new recording may start while this conversion
    // runs and replace m_gifTempPath, so the conversion must own its input.
    m_gifTempSource = m_gifTempPath;
    setStatus("converting to GIF…");

    m_ffmpeg = new Child();
    m_ffmpeg->onFinished = [this](int code, bool normalExit) {
        onGifConverted(code, normalExit);
    };
    m_ffmpeg->onStderr =
        [this](const std::string &chunk) { m_ffmpegErr += chunk; };
    // Cap on the conversion: a huge lossless source can take a long time to
    // encode. The timer is owned by this conversion (removed in
    // onGifConverted / the close path), so a second conversion can't extend
    // the first one's cap. The kill (not a blocking cap) means we never
    // delete the temp .mp4 out from under a live ffmpeg; the kill triggers
    // onFinished().
    m_gifCap.start(
        120, [this] {
            if (m_ffmpeg)
                m_ffmpeg->signal(SIGKILL);
            return false; // single shot
        });

    // Two caps, in this order:
    // fps=30 — the source is captured at the display's refresh rate (e.g.
    //   120 Hz -> 8.3 ms/frame), which the GIF format stores as a 1 cs
    //   delay/frame. Browsers enforce a 2 cs minimum frame delay, so a
    //   sub-20 ms GIF plays ~2x too slowly there (players that honour the
    //   raw delay look correct, which makes the discrepancy easy to spot).
    //   30 fps is the GIF convention and keeps playback speed consistent.
    // scale  — an unbounded conversion of a 2880x1800 source yields a
    //   multi-hundred-MB GIF nobody can use; min() leaves smaller sources
    //   untouched, -2 keeps the width even as the GIF scaler requires.
    const std::string filter =
        "fps=30,scale='min(" + std::to_string(kGifMaxWidth) + ",iw)':-2";
    m_ffmpegErr.clear();
    m_ffmpeg->start("ffmpeg",
                    {"-y", "-loglevel", "error", "-i", m_gifTempSource,
                     "-vf", filter, "-f", "gif", finalPath});
}

void
MainWindow::onGifConverted(int code, bool normalExit)
{
    const std::string ffErr = trimmed(m_ffmpegErr);
    delete m_ffmpeg;
    m_ffmpeg = nullptr;
    m_gifCap.stop();

    // A killed ffmpeg (timeout/close) reports code 0 with a signal exit, and
    // a partial .gif is on disk by then — never count that as success.
    struct stat st;
    const bool ok =
        code == 0 && normalExit && (stat(m_gifFinalPath.c_str(), &st) == 0);
    if (ok)
    {
        std::remove(m_gifTempSource.c_str());
        setStatus("saved (" + fmtSize(st.st_size) + ")");
    }
    else
    {
        // Keep the lossless .mp4: it is the most expensive artifact, and on
        // a conversion failure (missing ffmpeg, full disk, timeout) the user
        // should be able to convert it manually. The status label is fixed
        // width, so the full detail goes to the log. Drop the incomplete
        // .gif: it would silently overwrite a previously valid file at that
        // path, and the source above is enough to produce a real one.
        std::remove(m_gifFinalPath.c_str());
        g_warning("GIF conversion failed: path=%s source=%s code=%d ffmpeg: %s",
                  m_gifFinalPath.c_str(), m_gifTempSource.c_str(), code,
                  ffErr.c_str());
        std::string msg = "GIF conversion failed";
        if (!ffErr.empty())
            msg += " — " + ffErr;
        msg += " (source kept: " + m_gifTempSource + ")";
        setStatus(msg);
    }
    // m_gifTempPath / m_isGif describe the live recording session, which may
    // be a newer one that started while this conversion ran — leave them.
    m_gifTempSource.clear();
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

std::string
MainWindow::codecArgs() const
{
    switch (currentFormat())
    {
    case Format::Lossless: // lossless — bit-exact H.264, best source for GIF
        return "-c libx264 -p qp=0";
    default:
        return {};
    }
}

// The file wf-recorder is actually writing right now. For a GIF recording we
// record a lossless .mp4 to a temp file (converted to the .gif on stop), so
// this differs from the user-visible outPath() during a GIF session.
std::string
MainWindow::recPath() const
{
    return m_isGif ? m_gifTempPath : outPath();
}
