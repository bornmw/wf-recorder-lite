#pragma once

#include <QRect>
#include <QWidget>

#include <QProcess>

class AreaOverlay;
class QCloseEvent;
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
    // Formats in the drop-down. Stored as item data, so inserting a new
    // item can't silently shift the indices the logic below switches on.
    enum class Format { H264, Lossless, Gif };
    Q_ENUM(Format)

    MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSelectArea();
    void onAreaSelected(const QRect &phys);
    void onBrowse();
    void onFormatChanged(int index);
    void onStart();
    void onStop();
    void onStarted();
    void onTick();
    void onStderrReady();
    void onFinished(int code, QProcess::ExitStatus status);
    void onGifConverted(int code, QProcess::ExitStatus);

private:
    Format currentFormat() const;   // the selected drop-down format
    QString defaultDir() const;     // ~/Videos if it exists, else ~
    QString defaultPath() const;    // defaultDir() + "recording" + fileExt()
    QString fileExt() const;        // ".mp4" / ".gif" for the selected format
    QStringList codecArgs() const;  // extra wf-recorder args for the format
    QString outPath() const;        // full path, extension matched to format
    QString recPath() const;        // file currently being written (temp for GIF)
    void updatePathDisplay();       // refresh the field's full-path tooltip
    void setStatus(const QString &text); // fixed-width status, elided + tooltip
    QString fmtSize(qint64 bytes) const;
    void convertGif();              // start the async temp .mp4 -> final .gif conversion

    QLabel *m_area;
    QLineEdit *m_file;
    QPushButton *m_btnBrowse;
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
    QTimer *m_stopTerm;  // escalate to SIGTERM if wf-recorder hangs after SIGINT
    QTimer *m_stopKill;  // last resort: SIGKILL
    int m_elapsed = 0;
    QString m_stderr;    // captured wf-recorder stderr (failure diagnostics)
    qint64 m_stopPid = 0; // wf-recorder PID the stop escalation targets
    bool m_isGif = false;      // current recording is a GIF (via lossless temp .mp4)
    QString m_gifTempPath;     // lossless .mp4 being recorded, converted to .gif on stop
    QProcess *m_ffmpeg = nullptr; // async .mp4 -> .gif conversion (null = idle)
    QTimer *m_gifTimeout;         // kills m_ffmpeg if conversion exceeds the cap
    QString m_gifFinalPath;       // where m_ffmpeg is writing the .gif
    QString m_gifTempSource;      // the .mp4 this conversion reads (owned by the
                                  // conversion, not by m_gifTempPath — a new
                                  // recording may have replaced that one)
};
