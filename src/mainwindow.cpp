#include "mainwindow.h"
#include "areaoverlay.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <csignal>

MainWindow::MainWindow()
    : m_overlay(new AreaOverlay(this)),
      m_tick(new QTimer(this))
{
    setWindowTitle("wl-recorder");

    m_area = new QLabel("full output");
    m_name = new QLineEdit("recording");
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

    auto *row2 = new QHBoxLayout();
    row2->addWidget(m_btnStart);
    row2->addWidget(m_btnStop);
    row2->addStretch();
    row2->addWidget(m_status);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(row1);
    layout->addWidget(m_name);
    layout->addLayout(row2);

    connect(m_btnSelect, &QPushButton::clicked, this, &MainWindow::onSelectArea);
    connect(m_overlay, &AreaOverlay::selected, this,
            &MainWindow::onAreaSelected);
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

QString MainWindow::outPath() const
{
    QString name = m_name->text().trimmed();
    if (name.isEmpty())
        name = "recording";
    if (!name.contains('.'))
        name += ".mp4";
    return name;
}

void MainWindow::onStart()
{
    if (m_proc != nullptr && m_proc->state() != QProcess::NotRunning)
        return;

    QStringList args;
    if (!m_area_phys.isNull())
        args << "-g"
             << QString("%1,%2 %3x%4").arg(m_area_phys.x())
                                   .arg(m_area_phys.y())
                                   .arg(m_area_phys.width())
                                   .arg(m_area_phys.height());
    if (!m_output.isEmpty())
        args << "-o" << m_output;
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
    m_status->setText("recording…");
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
