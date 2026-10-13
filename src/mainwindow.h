#pragma once

#include <QRect>
#include <QWidget>

#include <QProcess>

class AreaOverlay;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

// Small control panel: area selection + filename + start/stop for
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
    void onStart();
    void onStop();
    void onTick();
    void onFinished(int code, QProcess::ExitStatus status);

private:
    QString outPath() const;
    QString fmtSize(qint64 bytes) const;

    QLabel *m_area;
    QLineEdit *m_name;
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
