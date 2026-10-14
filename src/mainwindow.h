#pragma once

#include <QRect>
#include <QWidget>

#include <QProcess>

class AreaOverlay;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

// Small control panel: area selection + file path + start/stop for
// wf-recorder (spawned with -g for the selected region, stopped with
// SIGINT — the clean-stop signal per the wf-recorder man page).
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    MainWindow();

private slots:
    void onSelectArea();
    void onAreaSelected(const QRect &phys);
    void onBrowse();
    void onFormatChanged(int index);
    void onStart();
    void onStop();
    void onTick();
    void onFinished(int code, QProcess::ExitStatus status);

private:
    QString defaultDir() const;     // ~/Videos if it exists, else ~
    QString defaultPath() const;    // defaultDir() + "recording" + fileExt()
    QString fileExt() const;        // ".mp4" / ".gif" for the selected format
    QStringList codecArgs() const;  // extra wf-recorder args for the format
    QString outPath() const;        // full path, extension matched to format
    void updatePathDisplay();       // refresh tooltip + always-visible full path
    QString fmtSize(qint64 bytes) const;

    QLabel *m_area;
    QLineEdit *m_file;
    QPushButton *m_btnBrowse;
    QLabel *m_pathLabel;  // always-visible full path (QToolTip is dead on Wayland)
    QComboBox *m_format;
    QPushButton *m_btnSelect;
    QPushButton *m_btnStart;
    QPushButton *m_btnStop;
    QLabel *m_status;

    AreaOverlay *m_overlay;
    QRect m_area_phys; // device pixels; null = record the full output
    QString m_output;  // output name the area was selected on (empty = default)

    QProcess *m_proc = nullptr;
    QTimer *m_tick;
    int m_elapsed = 0;
};
