#include "mainwindow.h"
#include "areaoverlay.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <csignal>

// Max source width for the async GIF conversion; wider sources are scaled
// down, smaller ones pass through unchanged (see convertGif()).
static constexpr int kGifMaxWidth = 1280;

// wf-recorder builds its output list from the xdg-output protocol
// (logical_position/logical_size) and passes "-g minus output position" to
// wlroots, which interprets the region in the output's LOGICAL space (then
// scales it to the buffer). On this wayfire build the reported position is
// the PHYSICAL one while the size is logical, so the -g value that lands on
// a selection (px,py,W,H) of PHYSICAL local pixels is:
//   x = physPos.x + px/scale,  y = physPos.y + py/scale
//   w = W/scale,               h = H/scale
// (physPos from wlr-randr). One wlr-randr query yields both, so Start blocks
// at most once (~2 s) instead of parsing the same output twice.
static bool outputGeom(const QString &name, int &x, int &y, double &scale)
{
    QProcess proc;
    proc.start("wlr-randr", {});
    if (!proc.waitForFinished(2000))
        return false;

    x = 0;
    y = 0;
    scale = 1.0;
    bool found = false;
    QString cur;
    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    for (const QString &raw : out.split('\n'))
    {
        if (raw.isEmpty() || raw.startsWith(' '))
        {
            const QString line = raw.trimmed();
            if (cur != name)
                continue;
            found = true; // output is listed; missing fields keep the defaults
            if (line.startsWith("Position:"))
            {
                const auto parts =
                    line.mid(9).trimmed().split(',', Qt::SkipEmptyParts);
                if (parts.size() == 2)
                {
                    bool okx = false, oky = false;
                    x = parts[0].toInt(&okx);
                    y = parts[1].toInt(&oky);
                }
            }
            else if (line.startsWith("Scale:"))
                scale = line.mid(6).trimmed().toDouble();
        }
        else
        {
            cur = raw.section(' ', 0, 0);
        }
    }
    return found;
}

// Drop a known container extension (case-insensitive) so outPath() /
// onFormatChanged() can force the one matching the selected format.
static QString stripKnownExt(const QString &p)
{
    const QStringList known = {".mp4", ".gif", ".mkv", ".webm", ".mov"};
    for (const QString &ext : known)
        if (p.endsWith(ext, Qt::CaseInsensitive))
            return p.left(p.size() - ext.size());
    return p;
}

MainWindow::MainWindow()
    : m_overlay(new AreaOverlay(this)),
      m_tick(new QTimer(this))
{
    setWindowTitle("wf-recorder");

    m_area = new QLabel("full output");

    m_format = new QComboBox();
    m_format->addItem("MP4 — H.264", static_cast<int>(Format::H264));
    m_format->addItem("MP4 — lossless", static_cast<int>(Format::Lossless));
    m_format->addItem("GIF", static_cast<int>(Format::Gif));
    m_format->setToolTip(
        "H.264 (default): small, but lossy — colours are slightly shifted "
        "and edges get soft.\n"
        "Lossless: keeps every pixel exactly (H.264 qp=0); the best source "
        "for converting to a GIF. Files are larger.\n"
        "GIF: records losslessly and converts to an animated GIF when you "
        "stop (highest quality). A GIF is always limited to 256 colours per "
        "frame, so it can never be fully pixel-perfect.");

    m_file = new QLineEdit(defaultPath());
    m_btnBrowse = new QPushButton("Browse…");
    m_pathLabel = new QLabel();
    m_pathLabel->setStyleSheet("color: #888;");

    m_btnSelect = new QPushButton("Select Area");
    m_btnStart = new QPushButton("Start");
    m_btnStop = new QPushButton("Stop");
    m_btnStop->setEnabled(false);
    m_status = new QLabel("idle");
    m_status->setStyleSheet("color: #888;");

    auto *row1 = new QHBoxLayout();
    row1->addWidget(m_btnSelect);
    row1->addStretch();
    row1->addWidget(m_area);

    auto *rowFile = new QHBoxLayout();
    rowFile->addWidget(m_file, 1);
    rowFile->addWidget(m_btnBrowse);

    auto *rowFmt = new QHBoxLayout();
    rowFmt->addWidget(new QLabel("Format:"));
    rowFmt->addWidget(m_format);
    rowFmt->addStretch();

    auto *row2 = new QHBoxLayout();
    row2->addWidget(m_btnStart);
    row2->addWidget(m_btnStop);
    row2->addStretch();
    row2->addWidget(m_status);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(row1);
    layout->addLayout(rowFile);
    layout->addWidget(m_pathLabel);
    layout->addLayout(rowFmt);
    layout->addLayout(row2);

    connect(m_btnSelect, &QPushButton::clicked, this, &MainWindow::onSelectArea);
    connect(m_overlay, &AreaOverlay::selected, this,
            &MainWindow::onAreaSelected);
    connect(m_btnBrowse, &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(m_format, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFormatChanged);
    // Keep the tooltip and the always-visible path label in sync with the
    // field: the tooltip shows the full path on hover, the label shows it
    // persistently without hovering.
    connect(m_file, &QLineEdit::textChanged, this, [this](const QString &) {
        updatePathDisplay();
    });
    updatePathDisplay();
    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::onStop);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &MainWindow::onTick);

    // Stop escalation: SIGINT is the clean-stop. wf-recorder can hang after
    // it finalizes the output file (seen with GIF), leaving the process alive
    // so the UI stays on "recording…". Escalate so it always exits.
    // Guarded by m_stopPid so a stale timer from a previous session can never
    // signal a freshly started recording.
    m_stopTerm = new QTimer(this);
    m_stopTerm->setSingleShot(true);
    connect(m_stopTerm, &QTimer::timeout, this, [this]() {
        if (m_proc && m_proc->processId() == m_stopPid &&
            m_proc->state() != QProcess::NotRunning)
            ::kill(m_stopPid, SIGTERM);
    });
    m_stopKill = new QTimer(this);
    m_stopKill->setSingleShot(true);
    connect(m_stopKill, &QTimer::timeout, this, [this]() {
        if (m_proc && m_proc->processId() == m_stopPid &&
            m_proc->state() != QProcess::NotRunning)
            ::kill(m_stopPid, SIGKILL);
    });

    // Cap on the async GIF conversion: a huge lossless source can take a
    // long time to encode. On timeout we kill ffmpeg so the temp file is
    // never deleted out from under a live process (the failure path keeps
    // the .mp4 for a manual retry).
    m_gifTimeout = new QTimer(this);
    m_gifTimeout->setSingleShot(true);
    connect(m_gifTimeout, &QTimer::timeout, this, [this]() {
        if (m_ffmpeg && m_ffmpeg->state() != QProcess::NotRunning)
            m_ffmpeg->kill();
    });
}

void MainWindow::onSelectArea()
{
    // Do NOT use showFullScreen(): the compositor's fullscreen path is
    // unstable on this box and kills the session. A frameless top-level
    // window sized to the whole screen covers identically.
    m_overlay->setGeometry(m_overlay->screen()->geometry());
    m_overlay->show();
    m_overlay->raise();
    m_overlay->activateWindow();
}

void MainWindow::onAreaSelected(const QRect &phys)
{
    m_area_phys = phys;
    m_output = m_overlay->screen()->name();
    m_area->setText(QString("%1,%2 %3x%4")
                        .arg(phys.x())
                        .arg(phys.y())
                        .arg(phys.width())
                        .arg(phys.height()));
}

QString MainWindow::defaultDir() const
{
    const QString home = QDir::homePath();
    const QString videos = home + "/Videos";
    return QDir(videos).exists() ? videos : home;
}

MainWindow::Format MainWindow::currentFormat() const
{
    return static_cast<Format>(m_format->currentData().toInt());
}

QString MainWindow::fileExt() const
{
    return currentFormat() == Format::Gif ? ".gif" : ".mp4";
}

QString MainWindow::defaultPath() const
{
    return QDir(defaultDir()).filePath("recording" + fileExt());
}

QStringList MainWindow::codecArgs() const
{
    switch (currentFormat())
    {
    case Format::Lossless: // lossless — bit-exact H.264, best source for GIF
        return {"-c", "libx264", "-p", "qp=0"};
    default:
        return {};
    }
}

// Returns the full output path, with the extension forced to match the
// selected format (so the codec and the file can never disagree).
QString MainWindow::outPath() const
{
    QString p = m_file->text().trimmed();
    if (p.isEmpty())
        p = "recording";
    if (!p.contains('/'))
        p = QDir::homePath() + "/" + p;
    return stripKnownExt(p) + fileExt();
}

// The file wf-recorder is actually writing right now. For a GIF recording we
// record a lossless .mp4 to a temp file (converted to the .gif on stop), so
// this differs from the user-visible outPath() during a GIF session.
QString MainWindow::recPath() const
{
    return m_isGif ? m_gifTempPath : outPath();
}

void MainWindow::updatePathDisplay()
{
    const QString p = outPath();
    m_file->setToolTip(p);
    m_pathLabel->setText(p);
}

void MainWindow::onBrowse()
{
    const QString filter =
        currentFormat() == Format::Gif ? "GIF (*.gif)" : "MP4 (*.mp4)";
    const QString startIn =
        m_file->text().trimmed().isEmpty() ? defaultPath() : m_file->text();
    const QString path =
        QFileDialog::getSaveFileName(this, "Save recording as", startIn, filter);
    if (!path.isEmpty())
        m_file->setText(path);
}

void MainWindow::onFormatChanged(int)
{
    // Keep the user's directory and base name; only swap the extension so
    // the field always shows the full path for the chosen format.
    QString p = m_file->text().trimmed();
    if (p.isEmpty())
        p = "recording";
    if (!p.contains('/'))
        p = QDir::homePath() + "/" + p;
    m_file->setText(stripKnownExt(p) + fileExt());
}

void MainWindow::onStart()
{
    if (m_proc != nullptr && m_proc->state() != QProcess::NotRunning)
        return;

    // Cancel any pending stop-escalation from a previous session so it can't
    // signal this fresh recording.
    m_stopTerm->stop();
    m_stopKill->stop();
    m_stopPid = 0;

    QStringList args;
    if (!m_area_phys.isNull())
    {
        int ox = 0, oy = 0;
        double sc = 1.0;
        if (!outputGeom(m_output, ox, oy, sc))
            qWarning() << "wf-recorder-ui: cannot determine position of"
                       << m_output << "; region may be captured on the wrong output";
        if (sc <= 0)
            sc = 1.0;
        args << "-g"
             << QString("%1,%2 %3x%4").arg(qRound(ox + m_area_phys.x() / sc))
                                   .arg(qRound(oy + m_area_phys.y() / sc))
                                   .arg(qRound(m_area_phys.width() / sc))
                                   .arg(qRound(m_area_phys.height() / sc));
    }
    if (!m_output.isEmpty())
        args << "-o" << m_output;

    // Direct GIF output hits a wf-recorder 0.6.0 bug: the GIF muxer uses a
    // coarse (centisecond) timebase, so frames captured within the same ~10ms
    // window (damage events / high refresh) get an identical DTS -> the
    // filtergraph errors and wf-recorder exits early (clean, no stderr).
    // We therefore record lossless H.264 (no such bug, and the best possible
    // GIF source) to a temp file, then convert it to the .gif on stop.
    if (currentFormat() == Format::Gif)
    {
        m_isGif = true;
        // Collision-proof temp name (random suffix) in $TMPDIR. An empty
        // placeholder is created; wf-recorder overwrites it (we pass -y, so
        // no "Overwrite?" prompt). autoRemove is off: the file's lifecycle
        // belongs to the recording session, and onGifConverted() (or the
        // no-output branch of it) removes it. (Note: the static
        // createNativeFile() is a no-op for already-native paths — it only
        // materializes Qt resources — so use an instance + open() here.)
        QTemporaryFile gifTemp(QDir::tempPath() + "/wfrec-XXXXXX.mp4");
        gifTemp.setAutoRemove(false);
        if (!gifTemp.open())
        {
            const QString err = gifTemp.errorString();
            m_status->setText("failed to create temp file: " + err);
            qWarning() << "wf-recorder-ui: failed to create temp file in"
                       << QDir::tempPath() << "—" << err;
            m_isGif = false;
            return;
        }
        m_gifTempPath = gifTemp.fileName();
        args << "-c" << "libx264" << "-p" << "qp=0";
    }
    else
    {
        // A conversion from a previous session may still be running, but it
        // reads its own snapshot (m_gifTempSource), so the live path can be
        // reset for this session.
        m_isGif = false;
        m_gifTempPath.clear();
        args << codecArgs();
    }
    // -y: never prompt "Overwrite? Y/n:" when the file exists — that prompt
    // hangs (no stdin) and makes the recording self-stop. The file picker
    // already handles the overwrite warning.
    args << "-y";
    args << "-f" << recPath();

    m_proc = new QProcess(this);
    m_stderr.clear();
    // Async start: the event loop stays responsive while wf-recorder execs
    // (a blocking waitForStarted() would freeze the UI for up to 3 s).
    connect(m_proc, &QProcess::started, this, &MainWindow::onStarted);
    connect(m_proc, &QProcess::finished, this, &MainWindow::onFinished);
    connect(m_proc, &QProcess::readyReadStandardError, this,
            &MainWindow::onStderrReady);
    connect(m_proc, &QProcess::errorOccurred, this, [this](auto err) {
        if (err != QProcess::FailedToStart)
            return; // a started process that fails later reaches onFinished()
        // No process to manage — just restore the idle state and reset the
        // GIF-session state that was set up above for this launch.
        m_proc->deleteLater();
        m_proc = nullptr;
        m_isGif = false;
        m_gifTempPath.clear();
        m_btnStart->setEnabled(true);
        m_btnStop->setEnabled(false);
        m_status->setText("failed to launch wf-recorder");
    });
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(false);
    m_status->setText("starting…");
    m_proc->start("wf-recorder", args);
}

void MainWindow::onStarted()
{
    m_btnStop->setEnabled(true);
    m_elapsed = 0;
    m_tick->start();
    m_status->setText("recording " + outPath() + "…");
}

void MainWindow::onStop()
{
    if (m_proc == nullptr || m_proc->state() == QProcess::NotRunning)
        return;
    m_stopPid = m_proc->processId();
    ::kill(m_stopPid, SIGINT);
    m_stopTerm->start(2000);
    m_stopKill->start(4000);
}

void MainWindow::onTick()
{
    if (m_proc == nullptr)
        return;
    m_elapsed++;
    const auto f = QFileInfo(recPath());
    m_status->setText(
        QString("recording %1m%2s — %3")
            .arg(m_elapsed / 60)
            .arg(m_elapsed % 60, 2, 10, QChar('0'))
            .arg(fmtSize(f.exists() ? f.size() : 0)));
}

QString MainWindow::fmtSize(qint64 bytes) const
{
    if (bytes >= 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    if (bytes >= 1024)
        return QString::number(bytes / 1024.0, 'f', 0) + " KB";
    return QString::number(bytes) + " B";
}

void MainWindow::onStderrReady()
{
    if (m_proc)
        m_stderr += QString::fromUtf8(m_proc->readAllStandardError());
}

void MainWindow::onFinished(int code, QProcess::ExitStatus exitStatus)
{
    qWarning() << "wf-recorder finished: code=" << code
               << "status=" << (int)exitStatus
               << "stderr=" << m_stderr;
    m_stopTerm->stop();
    m_stopKill->stop();
    m_tick->stop();
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);

    if (m_isGif)
    {
        // The lossless temp .mp4 is finalized; turn it into the .gif.
        convertGif();
    }
    else
    {
        const auto f = QFileInfo(outPath());
        QString msg;
        if (code == 0 && f.exists())
            msg = "saved " + outPath() + " (" + fmtSize(f.size()) + ")";
        else
        {
            msg = "stopped with code " + QString::number(code);
            // On failure, surface the most relevant stderr line so the cause
            // is visible. (The full log is also written to the app log via
            // qWarning; the whole stderr is far too verbose for the status.)
            const QStringList lines =
                m_stderr.trimmed().split('\n', Qt::SkipEmptyParts);
            if (!lines.isEmpty())
                msg += " — " + lines.last();
        }
        m_status->setText(msg);
    }

    m_proc->deleteLater();
    m_proc = nullptr;
}

// Run the conversion as an async QProcess: GIF encoding is slow (minutes for
// long high-res recordings) and a blocking waitForFinished() would freeze the
// GUI for the whole time — including the "converting to GIF…" status, which
// can't repaint while the event loop is blocked.
void MainWindow::convertGif()
{
    // If the recording produced no usable source (wf-recorder failed early),
    // say so — don't report a conversion failure for a file that doesn't
    // exist.
    const auto src = QFileInfo(m_gifTempPath);
    if (!src.exists() || src.size() == 0)
    {
        QFile::remove(m_gifTempPath);
        m_gifTempPath.clear();
        m_isGif = false;
        QString msg = "recording produced no output";
        const QStringList lines =
            m_stderr.trimmed().split('\n', Qt::SkipEmptyParts);
        if (!lines.isEmpty())
            msg += " — " + lines.last();
        m_status->setText(msg);
        return;
    }

    const QString finalPath = outPath();
    m_gifFinalPath = finalPath;
    // Snapshot the source: a new recording may start while this conversion
    // runs and replace m_gifTempPath, so the conversion must own its input.
    m_gifTempSource = m_gifTempPath;
    m_status->setText("converting to GIF…");

    m_ffmpeg = new QProcess(this);
    connect(m_ffmpeg, &QProcess::finished, this,
            &MainWindow::onGifConverted);
    // Kill on timeout (instead of a blocking cap) so we never delete the
    // temp .mp4 out from under a live ffmpeg; the kill triggers finished().
    m_gifTimeout->start(120000);

    // Cap the GIF width: an unbounded conversion of e.g. a 2880x1800 source
    // yields a multi-hundred-MB GIF nobody can use. min() leaves smaller
    // sources untouched; -2 keeps the width even, as the GIF scaler requires.
    const QString scale =
        QString("scale='min(%1,iw)':-2").arg(kGifMaxWidth);
    m_ffmpeg->start("ffmpeg",
                    {"-y", "-loglevel", "error", "-i", m_gifTempSource,
                     "-vf", scale, "-f", "gif", finalPath});
}

void MainWindow::onGifConverted(int code, QProcess::ExitStatus)
{
    m_gifTimeout->stop();
    const QString ffErr =
        QString::fromUtf8(m_ffmpeg->readAllStandardError()).trimmed();
    m_ffmpeg->deleteLater();
    m_ffmpeg = nullptr;

    const bool ok = code == 0 && QFileInfo(m_gifFinalPath).exists();
    if (ok)
        QFile::remove(m_gifTempSource);
    else
    {
        // Keep the lossless .mp4: it is the most expensive artifact, and on
        // a conversion failure (missing ffmpeg, full disk, timeout) the user
        // should be able to convert it manually.
        QString msg = "GIF conversion failed";
        if (!ffErr.isEmpty())
            msg += " — " + ffErr;
        msg += " (source kept: " + m_gifTempSource + ")";
        m_status->setText(msg);
    }
    // m_gifTempPath / m_isGif describe the live recording session, which may
    // be a newer one that started while this conversion ran — leave them.
    m_gifTempSource.clear();
}
