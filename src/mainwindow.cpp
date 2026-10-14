#include "mainwindow.h"
#include "areaoverlay.h"

#include <QComboBox>
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
        "GIF: records straight to an animated GIF. Note a GIF is always "
        "limited to 256 colours per frame, so it can never be fully "
        "pixel-perfect.");

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
    args << codecArgs();
    args << "-f" << outPath();

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, &MainWindow::onFinished);
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
    ::kill(m_proc->processId(), SIGINT);
}

void MainWindow::onTick()
{
    if (m_proc == nullptr)
        return;
    m_elapsed++;
    const auto f = QFileInfo(outPath());
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

void MainWindow::onFinished(int code, QProcess::ExitStatus)
{
    m_tick->stop();
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);

    const auto f = QFileInfo(outPath());
    if (code == 0 && f.exists())
        m_status->setText("saved " + outPath() + " (" + fmtSize(f.size()) +
                          ")");
    else
        m_status->setText("stopped with code " + QString::number(code));

    m_proc->deleteLater();
    m_proc = nullptr;
}
