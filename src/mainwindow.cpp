#include "mainwindow.h"
#include "areaoverlay.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <csignal>

// wf-recorder builds its output list from the xdg-output protocol
// (logical_position/logical_size) and passes "-g minus output position" to
// wlroots, which interprets the region in the output's LOGICAL space (then
// scales it to the buffer). On this wayfire build the reported position is
// the PHYSICAL one while the size is logical, so the -g value that lands on
// a selection (px,py,W,H) of PHYSICAL local pixels is:
//   x = physPos.x + px/scale,  y = physPos.y + py/scale
//   w = W/scale,               h = H/scale
// (physPos from wlr-randr).
static bool outputPhysPos(const QString &name, int &x, int &y)
{
    QProcess proc;
    proc.start("wlr-randr", {});
    if (!proc.waitForFinished(2000))
        return false;

    QString cur;
    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    for (const QString &raw : out.split('\n'))
    {
        const QString line = raw.trimmed();
        if (raw.startsWith(' ') || raw.isEmpty())
        {
            if (cur == name && line.startsWith("Position:"))
            {
                const auto parts =
                    line.mid(9).trimmed().split(',', Qt::SkipEmptyParts);
                if (parts.size() == 2)
                {
                    bool okx = false, oky = false;
                    x = parts[0].toInt(&okx);
                    y = parts[1].toInt(&oky);
                    return okx && oky;
                }
            }
        }
        else
        {
            cur = raw.section(' ', 0, 0);
        }
    }
    return false;
}

static double outputScale(const QString &name)
{
    QProcess proc;
    proc.start("wlr-randr", {});
    if (!proc.waitForFinished(2000))
        return 1.0;

    QString cur;
    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    for (const QString &raw : out.split('\n'))
    {
        const QString line = raw.trimmed();
        if (raw.startsWith(' ') && cur == name && line.startsWith("Scale:"))
            return line.mid(6).trimmed().toDouble();
        if (!raw.isEmpty() && !raw.startsWith(' '))
            cur = raw.section(' ', 0, 0);
    }
    return 1.0;
}

MainWindow::MainWindow()
    : m_overlay(new AreaOverlay(this)),
      m_tick(new QTimer(this))
{
    setWindowTitle("wl-recorder");

    m_area = new QLabel("full output");

    m_format = new QComboBox();
    m_format->addItem("MP4 — H.264");
    m_format->addItem("MP4 — lossless");
    m_format->addItem("GIF");
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

QString MainWindow::fileExt() const
{
    return m_format->currentIndex() == 2 ? ".gif" : ".mp4";
}

QString MainWindow::defaultPath() const
{
    return QDir(defaultDir()).filePath("recording" + fileExt());
}

QStringList MainWindow::codecArgs() const
{
    switch (m_format->currentIndex())
    {
    case 1: // lossless — bit-exact H.264, best source for GIF conversion
        return {"-c", "libx264", "-p", "qp=0"};
    case 2: // direct animated GIF
        return {"-c", "gif"};
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

    const QStringList known = {".mp4", ".gif", ".mkv", ".webm", ".mov"};
    for (const QString &ext : known)
        if (p.endsWith(ext, Qt::CaseInsensitive))
        {
            p = p.left(p.size() - ext.size());
            break;
        }
    return p + fileExt();
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
        m_format->currentIndex() == 2 ? "GIF (*.gif)" : "MP4 (*.mp4)";
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

    const QStringList known = {".mp4", ".gif", ".mkv", ".webm", ".mov"};
    for (const QString &ext : known)
        if (p.endsWith(ext, Qt::CaseInsensitive))
        {
            p = p.left(p.size() - ext.size());
            break;
        }
    m_file->setText(p + fileExt());
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
        double sc = outputScale(m_output);
        if (sc <= 0)
            sc = 1.0;
        if (!outputPhysPos(m_output, ox, oy))
            qWarning() << "wl-recorder-ui: cannot determine position of"
                       << m_output << "; region may be captured on the wrong output";
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
    if (m_format->currentIndex() == 2)
    {
        m_isGif = true;
        m_gifTempPath =
            QDir(QDir::tempPath())
                .filePath(
                    QString("wlrec-%1-%2.mp4")
                        .arg(QCoreApplication::applicationPid())
                        .arg(QDateTime::currentMSecsSinceEpoch()));
        args << "-c" << "libx264" << "-p" << "qp=0";
    }
    else
    {
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
    connect(m_proc, &QProcess::finished, this, &MainWindow::onFinished);
    connect(m_proc, &QProcess::readyReadStandardError, this,
            &MainWindow::onStderrReady);
    m_proc->start("wf-recorder", args);
    if (!m_proc->waitForStarted(3000))
    {
        m_status->setText("failed to launch wf-recorder");
        m_proc->deleteLater();
        m_proc = nullptr;
        return;
    }

    m_btnStart->setEnabled(false);
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

void MainWindow::convertGif()
{
    const QString finalPath = outPath();
    m_status->setText("converting to GIF…");

    QProcess ff;
    ff.start("ffmpeg", {"-y", "-loglevel", "error", "-i", m_gifTempPath,
                        "-f", "gif", finalPath});
    ff.waitForFinished(120000);
    const int ffCode = ff.exitCode();
    const QString ffErr =
        QString::fromUtf8(ff.readAllStandardError()).trimmed();

    const bool ok = ffCode == 0 && QFileInfo(finalPath).exists();

    QFile::remove(m_gifTempPath);
    m_gifTempPath.clear();
    m_isGif = false;

    if (ok)
        m_status->setText("saved " + finalPath + " (" +
                          fmtSize(QFileInfo(finalPath).size()) + ")");
    else
    {
        QString msg = "GIF conversion failed";
        if (!ffErr.isEmpty())
            msg += " — " + ffErr;
        m_status->setText(msg);
    }
}
